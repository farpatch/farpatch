/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2011  Black Sphere Technologies Ltd.
 * Written by Gareth McMullin <gareth@blacksphere.co.nz>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* This file implements the low-level JTAG TAP interface.  */

#include <stdio.h>

#include "gdb_packet.h"
#include "general.h"
#include "jtagtap.h"
#include "platform.h"

#if JTAGTAP_MODE_DEDIC == 1
#include "driver/dedic_gpio.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "hal/dedic_gpio_cpu_ll.h"
#include "gpio-dedic.h"

jtag_proc_s jtag_proc;

#define CLK_HIGH()   dedic_gpio_cpu_ll_write_mask(SWCLK_DEDIC_MASK, SWCLK_DEDIC_MASK)
#define CLK_LOW()    dedic_gpio_cpu_ll_write_mask(SWCLK_DEDIC_MASK, 0)
#define SET_TMS(val) dedic_gpio_cpu_ll_write_mask(SWDIO_TMS_DEDIC_MASK, (val) << SWDIO_TMS_DEDIC_PIN)
#define SET_TDI(val) dedic_gpio_cpu_ll_write_mask(JTAG_TDI_DEDIC_MASK, (val) << JTAG_TDI_DEDIC_PIN)
#define GET_TDO()    (dedic_gpio_cpu_ll_read_in() & JTAG_TDO_DEDIC_MASK)

void IRAM_ATTR platform_maybe_delay(void);

static void jtagtap_reset(void);
static IRAM_ATTR void jtagtap_tms_seq(uint32_t MS, size_t ticks);
static IRAM_ATTR void jtagtap_tdi_tdo_seq(uint8_t *DO, const bool final_tms, const uint8_t *DI, size_t ticks);
static IRAM_ATTR void jtagtap_tdi_seq(const bool final_tms, const uint8_t *DI, size_t ticks);
static IRAM_ATTR bool jtagtap_next(const bool dTMS, const bool dTDI);
static IRAM_ATTR void jtagtap_cycle(const bool tms, const bool tdi, const size_t clock_cycles);

void jtagtap_init(void)
{
	gpio_dedic_init();

	ESP_LOGI("jtag", "initializing jtag GPIO");

// Ensure the TMS pin is driven as an output, and that TDO is an input
#if SOC_DEDIC_GPIO_OUT_AUTO_ENABLE
	REG_WRITE(GPIO_FUNC0_OUT_SEL_CFG_REG + (CONFIG_TMS_SWDIO_GPIO * 4), CORE1_GPIO_OUT0_IDX);
	gpio_ll_output_enable(GPIO_HAL_GET_HW(GPIO_PORT_0), CONFIG_TMS_SWDIO_GPIO);

	gpio_ll_output_disable(GPIO_HAL_GET_HW(GPIO_PORT_0), CONFIG_TDO_GPIO);
#else
	dedic_gpio_cpu_ll_enable_output(ALL_OUTPUT_MASK | SWDIO_TMS_DEDIC_MASK);
#endif

	if (CONFIG_TMS_SWDIO_DIR_GPIO >= 0)
		dedic_gpio_cpu_ll_write_mask(SWDIO_TMS_DIR_DEDIC_MASK, 0);
	if (CONFIG_TCK_TDI_DIR_GPIO >= 0)
		dedic_gpio_cpu_ll_write_mask(SWCLK_DIR_DEDIC_MASK, 0);

	jtag_proc.jtagtap_reset = jtagtap_reset;
	jtag_proc.jtagtap_next = jtagtap_next;
	jtag_proc.jtagtap_tms_seq = jtagtap_tms_seq;
	jtag_proc.jtagtap_tdi_tdo_seq = jtagtap_tdi_tdo_seq;
	jtag_proc.jtagtap_tdi_seq = jtagtap_tdi_seq;
	jtag_proc.jtagtap_cycle = jtagtap_cycle;
	jtag_proc.tap_idle_cycles = 1;

	/* Ensure we're in JTAG mode */
	for (size_t i = 0; i <= 50U; ++i)
		jtagtap_next(true, false); /* 50 + 1 idle cycles for SWD reset */
	jtagtap_tms_seq(0xe73cU, 16U); /* SWD to JTAG sequence */
}

static void jtagtap_reset(void)
{
	jtagtap_soft_reset();
}

static bool jtagtap_next_clk_delay()
{
	CLK_HIGH();
	esp_rom_delay_us(target_delay_us);
	const uint16_t result = GET_TDO();
	CLK_LOW();
	esp_rom_delay_us(target_delay_us);
	return result != 0;
}

static bool jtagtap_next_no_delay()
{
	CLK_HIGH();
	const uint16_t result = GET_TDO();
	CLK_LOW();
	return result != 0;
}

static bool jtagtap_next(const bool tms, const bool tdi)
{
	platform_maybe_delay();
	SET_TMS(tms);
	SET_TDI(tdi);
	if (target_delay_us)
		return jtagtap_next_clk_delay();
	else // NOLINT(readability-else-after-return)
		return jtagtap_next_no_delay();
}

static void jtagtap_tms_seq_clk_delay(uint32_t tms_states, const size_t clock_cycles)
{
	for (size_t cycle = 0; cycle < clock_cycles; ++cycle) {
		const bool state = tms_states & 1U;
		SET_TMS(state);
		CLK_HIGH();
		esp_rom_delay_us(target_delay_us);
		tms_states >>= 1U;
		CLK_LOW();
		esp_rom_delay_us(target_delay_us);
	}
}

static void jtagtap_tms_seq_no_delay(uint32_t tms_states, const size_t clock_cycles)
{
	bool state = tms_states & 1U;
	for (size_t cycle = 0; cycle < clock_cycles; ++cycle) {
		SET_TMS(state);
		CLK_HIGH();
		/* Block the compiler from re-ordering the TMS states calculation to preserve timings */
		__asm__ volatile("" ::: "memory");
		tms_states >>= 1U;
		state = tms_states & 1U;
		CLK_LOW();
	}
}

static void jtagtap_tms_seq(const uint32_t tms_states, const size_t ticks)
{
	SET_TDI(1);
	if (target_delay_us)
		jtagtap_tms_seq_clk_delay(tms_states, ticks);
	else
		jtagtap_tms_seq_no_delay(tms_states, ticks);
}

static void jtagtap_tdi_tdo_seq_clk_delay(
	const uint8_t *const data_in, uint8_t *const data_out, const bool final_tms, const size_t clock_cycles)
{
	uint8_t value = 0;
	for (size_t cycle = 0; cycle < clock_cycles; ++cycle) {
		/* Calculate the next bit and byte to consume data from */
		const uint8_t bit = cycle & 7U;
		const size_t byte = cycle >> 3U;
		/* On the last cycle, assert final_tms to TMS_PIN */
		SET_TMS(cycle + 1U >= clock_cycles && final_tms);
		/* Set up the TDI pin and start the clock cycle */
		SET_TDI(!!(data_in[byte] & (1U << bit)));
		/* Start the clock cycle */
		CLK_HIGH();
		esp_rom_delay_us(target_delay_us);
		/* If TDO is high, store a 1 in the appropriate position in the value being accumulated */
		if (GET_TDO())
			value |= 1U << bit;
		if (bit == 7U) {
			data_out[byte] = value;
			value = 0;
		}
		/* Finish the clock cycle */
		CLK_LOW();
		esp_rom_delay_us(target_delay_us);
	}
	/* If clock_cycles is not divisible by 8, we have some extra data to write back here. */
	if (clock_cycles & 7U) {
		const size_t byte = (clock_cycles - 1U) >> 3U;
		data_out[byte] = value;
	}
}

static void jtagtap_tdi_tdo_seq_no_delay(
	const uint8_t *const data_in, uint8_t *const data_out, const bool final_tms, const size_t clock_cycles)
{
	uint8_t value = 0;
	for (size_t cycle = 0; cycle < clock_cycles;) {
		/* Calculate the next bit and byte to consume data from */
		const uint8_t bit = cycle & 7U;
		const size_t byte = cycle >> 3U;
		const bool tms = cycle + 1U >= clock_cycles && final_tms;
		const bool tdi = data_in[byte] & (1U << bit);
		/* Block the compiler from re-ordering the calculations to preserve timings */
		__asm__ volatile("" ::: "memory");
		CLK_LOW();
		/* Block the compiler from re-ordering the calculations to preserve timings */
		__asm__ volatile("" ::: "memory");
		/* Configure the bus for the next cycle */
		SET_TDI(tdi);
		SET_TMS(tms);
		/* Block the compiler from re-ordering the calculations to preserve timings */
		__asm__ volatile("" ::: "memory");
		/* Increment the cycle counter */
		++cycle;
		/* Block the compiler from re-ordering the calculations to preserve timings */
		__asm__ volatile("nop" ::: "memory");
		/* Start the clock cycle */
		CLK_HIGH();
		/* If TDO is high, store a 1 in the appropriate position in the value being accumulated */
		if (GET_TDO()) /* XXX: Try to remove the need for the if here */
			value |= 1U << bit;
		/* If we've got the next whole byte, store the accumulated value and reset state */
		if (bit == 7U) {
			data_out[byte] = value;
			value = 0;
		}
		/* Finish the clock cycle */
	}
	/* If clock_cycles is not divisible by 8, we have some extra data to write back here. */
	if (clock_cycles & 7U) {
		const size_t byte = (clock_cycles - 1U) >> 3U;
		data_out[byte] = value;
	}
	CLK_LOW();
}

static void jtagtap_tdi_tdo_seq(
	uint8_t *const data_out, const bool final_tms, const uint8_t *const data_in, size_t clock_cycles)
{
	SET_TMS(0);
	SET_TDI(0);
	if (target_delay_us != 0)
		jtagtap_tdi_tdo_seq_clk_delay(data_in, data_out, final_tms, clock_cycles);
	else
		jtagtap_tdi_tdo_seq_no_delay(data_in, data_out, final_tms, clock_cycles);
}

static void jtagtap_tdi_seq_clk_delay(const uint8_t *const data_in, const bool final_tms, size_t clock_cycles)
{
	for (size_t cycle = 0; cycle < clock_cycles; ++cycle) {
		const uint8_t bit = cycle & 7U;
		const size_t byte = cycle >> 3U;
		/* On the last tick, assert final_tms to TMS_PIN */
		SET_TMS(cycle + 1U >= clock_cycles && final_tms);
		/* Set up the TDI pin and start the clock cycle */
		SET_TDI(!!(data_in[byte] & (1U << bit)));
		CLK_HIGH();
		esp_rom_delay_us(target_delay_us);
		/* Finish the clock cycle */
		CLK_LOW();
		esp_rom_delay_us(target_delay_us);
	}
}

static void jtagtap_tdi_seq_no_delay(const uint8_t *const data_in, const bool final_tms, size_t clock_cycles)
{
	for (size_t cycle = 0; cycle < clock_cycles;) {
		const uint8_t bit = cycle & 7U;
		const size_t byte = cycle >> 3U;
		const bool tms = cycle + 1U >= clock_cycles && final_tms;
		const bool tdi = data_in[byte] & (1U << bit);
		/* Block the compiler from re-ordering the calculations to preserve timings */
		__asm__ volatile("" ::: "memory");
		CLK_LOW();
		/* On the last tick, assert final_tms to TMS_PIN */
		SET_TMS(tms);
		/* Set up the TDI pin and start the clock cycle */
		SET_TDI(tdi);
		/* Block the compiler from re-ordering the calculations to preserve timings */
		__asm__ volatile("" ::: "memory");
		/* Increment the cycle counter */
		++cycle;
		/* Block the compiler from re-ordering the calculations to preserve timings */
		__asm__ volatile("nop" ::: "memory");
		/* Start the clock cycle */
		CLK_HIGH();
		/* Finish the clock cycle */
	}
	CLK_LOW();
}

static void jtagtap_tdi_seq(const bool final_tms, const uint8_t *const data_in, const size_t clock_cycles)
{
	SET_TMS(0);
	if (target_delay_us)
		jtagtap_tdi_seq_clk_delay(data_in, final_tms, clock_cycles);
	else
		jtagtap_tdi_seq_no_delay(data_in, final_tms, clock_cycles);
}

static void jtagtap_cycle_clk_delay(const size_t clock_cycles)
{
	for (size_t cycle = 0; cycle < clock_cycles; ++cycle) {
		CLK_HIGH();
		esp_rom_delay_us(target_delay_us);
		CLK_LOW();
		esp_rom_delay_us(target_delay_us);
	}
}

static void jtagtap_cycle_no_delay(const size_t clock_cycles)
{
	for (size_t cycle = 0; cycle < clock_cycles; ++cycle) {
		CLK_HIGH();
		__asm__ volatile("nop" ::: "memory");
		CLK_LOW();
	}
}

static void jtagtap_cycle(const bool tms, const bool tdi, const size_t clock_cycles)
{
	jtagtap_next(tms, tdi);
	if (target_delay_us)
		jtagtap_cycle_clk_delay(clock_cycles - 1U);
	else
		jtagtap_cycle_no_delay(clock_cycles - 1U);
}

#endif /* JTAGTAP_MODE_DEDIC == 1 */
