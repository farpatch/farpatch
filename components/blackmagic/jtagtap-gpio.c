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

#if JTAGTAP_MODE_GPIO == 1

#include "esp_attr.h"

jtag_proc_s jtag_proc;

void IRAM_ATTR platform_maybe_delay(void);

static void jtagtap_reset(void);
static IRAM_ATTR void jtagtap_tms_seq(uint32_t MS, size_t ticks);
static IRAM_ATTR void jtagtap_tdi_tdo_seq(uint8_t *DO, const bool final_tms, const uint8_t *DI, size_t ticks);
static IRAM_ATTR void jtagtap_tdi_seq(const bool final_tms, const uint8_t *DI, size_t ticks);
static IRAM_ATTR bool jtagtap_next(const bool dTMS, const bool dTDI);
static IRAM_ATTR void jtagtap_cycle(const bool tms, const bool tdi, const size_t clock_cycles);

static void dummy_reset(void)
{
}

static void dummy_tms_seq(uint32_t MS, size_t ticks)
{
	(void)MS;
	(void)ticks;
}
static void dummy_tdi_tdo_seq(uint8_t *DO, const bool final_tms, const uint8_t *DI, size_t ticks)
{
	(void)DO;
	(void)final_tms;
	(void)DI;
	(void)ticks;
}

static void dummy_tdi_seq(const bool final_tms, const uint8_t *DI, size_t ticks)
{
	(void)final_tms;
	(void)DI;
	(void)ticks;
}

static bool dummy_next(const bool dTMS, const bool dTDI)
{
	(void)dTMS;
	(void)dTDI;
	return false;
}

static void dummy_cycle(const bool tms, const bool tdi, const size_t clock_cycles)
{
	(void)tms;
	(void)tdi;
	(void)clock_cycles;
}

void jtagtap_init(void)
{
	if ((CONFIG_TDI_GPIO == -1) || (CONFIG_TDO_GPIO == -1)) {
		gdb_outf("jtag not supported on this board\n");
		ESP_LOGE("jtag", "TDI and TDO pins are not configured on this board");
		jtag_proc.jtagtap_reset = dummy_reset;
		jtag_proc.jtagtap_next = dummy_next;
		jtag_proc.jtagtap_tms_seq = dummy_tms_seq;
		jtag_proc.jtagtap_tdi_tdo_seq = dummy_tdi_tdo_seq;
		jtag_proc.jtagtap_tdi_seq = dummy_tdi_seq;
		jtag_proc.jtagtap_cycle = dummy_cycle;
		jtag_proc.tap_idle_cycles = 1;
		return;
	}

	gpio_reset_pin(CONFIG_TDI_GPIO);
	gpio_reset_pin(CONFIG_TDO_GPIO);
	gpio_reset_pin(CONFIG_TMS_SWDIO_GPIO);
	gpio_reset_pin(CONFIG_TCK_SWCLK_GPIO);
	if (CONFIG_TMS_SWDIO_DIR_GPIO >= 0)
		gpio_reset_pin(CONFIG_TMS_SWDIO_DIR_GPIO);

	gpio_set_direction(CONFIG_TDI_GPIO, GPIO_MODE_OUTPUT);
	gpio_set_direction(CONFIG_TDO_GPIO, GPIO_MODE_INPUT);
	gpio_set_direction(CONFIG_TMS_SWDIO_GPIO, GPIO_MODE_OUTPUT);
	gpio_set_direction(CONFIG_TCK_SWCLK_GPIO, GPIO_MODE_OUTPUT);
	if (CONFIG_TMS_SWDIO_DIR_GPIO >= 0)
		gpio_set_direction(CONFIG_TMS_SWDIO_DIR_GPIO, GPIO_MODE_OUTPUT);
	if (CONFIG_TMS_SWDIO_DIR_GPIO >= 0)
		gpio_set_level(CONFIG_TMS_SWDIO_DIR_GPIO, 0);

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
#ifdef TRST_PORT
	if (platform_hwversion() == 0) {
		gpio_clear(TRST_PORT, TRST_PIN);
		for (volatile size_t i = 0; i < 10000U; i++)
			continue;
		gpio_set(TRST_PORT, TRST_PIN);
	}
#endif
	jtagtap_soft_reset();
}

static bool jtagtap_next_clk_delay()
{
	gpio_set(TCK_PORT, TCK_PIN);
	esp_rom_delay_us(target_delay_us);
	const uint16_t result = gpio_get(TDO_PORT, TDO_PIN);
	gpio_clear(TCK_PORT, TCK_PIN);
	esp_rom_delay_us(target_delay_us);
	return result != 0;
}

static bool jtagtap_next_no_delay()
{
	gpio_set(TCK_PORT, TCK_PIN);
	const uint16_t result = gpio_get(TDO_PORT, TDO_PIN);
	gpio_clear(TCK_PORT, TCK_PIN);
	return result != 0;
}

static bool jtagtap_next(const bool tms, const bool tdi)
{
	platform_maybe_delay();
	gpio_set_val(TMS_PORT, TMS_PIN, tms);
	gpio_set_val(TDI_PORT, TDI_PIN, tdi);
	if (target_delay_us != 0)
		return jtagtap_next_clk_delay();
	else // NOLINT(readability-else-after-return)
		return jtagtap_next_no_delay();
}

static void jtagtap_tms_seq_clk_delay(uint32_t tms_states, const size_t clock_cycles)
{
	for (size_t cycle = 0; cycle < clock_cycles; ++cycle) {
		const bool state = tms_states & 1U;
		gpio_set_val(TMS_PORT, TMS_PIN, state);
		gpio_set(TCK_PORT, TCK_PIN);
		esp_rom_delay_us(target_delay_us);
		tms_states >>= 1U;
		gpio_clear(TCK_PORT, TCK_PIN);
		esp_rom_delay_us(target_delay_us);
	}
}

static void jtagtap_tms_seq_no_delay(uint32_t tms_states, const size_t clock_cycles)
{
	bool state = tms_states & 1U;
	for (size_t cycle = 0; cycle < clock_cycles; ++cycle) {
		gpio_set_val(TMS_PORT, TMS_PIN, state);
		gpio_set(TCK_PORT, TCK_PIN);
		/* Block the compiler from re-ordering the TMS states calculation to preserve timings */
		__asm__ volatile("" ::: "memory");
		tms_states >>= 1U;
		state = tms_states & 1U;
		__asm__("nop");
		__asm__("nop");
		gpio_clear(TCK_PORT, TCK_PIN);
	}
}

static void jtagtap_tms_seq(const uint32_t tms_states, const size_t ticks)
{
	gpio_set(TDI_PORT, TDI_PIN);
	if (target_delay_us != 0)
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
		gpio_set_val(TMS_PORT, TMS_PIN, cycle + 1U >= clock_cycles && final_tms);
		/* Set up the TDI pin and start the clock cycle */
		gpio_set_val(TDI_PORT, TDI_PIN, data_in[byte] & (1U << bit));
		/* Start the clock cycle */
		gpio_set(TCK_PORT, TCK_PIN);
		esp_rom_delay_us(target_delay_us);
		/* If TDO is high, store a 1 in the appropriate position in the value being accumulated */
		if (gpio_get(TDO_PORT, TDO_PIN))
			value |= 1U << bit;
		if (bit == 7U) {
			data_out[byte] = value;
			value = 0;
		}
		/* Finish the clock cycle */
		gpio_clear(TCK_PORT, TCK_PIN);
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
		gpio_clear(TCK_PORT, TCK_PIN);
		/* Block the compiler from re-ordering the calculations to preserve timings */
		__asm__ volatile("" ::: "memory");
		/* Configure the bus for the next cycle */
		gpio_set_val(TDI_PORT, TDI_PIN, tdi);
		gpio_set_val(TMS_PORT, TMS_PIN, tms);
		/* Block the compiler from re-ordering the calculations to preserve timings */
		__asm__ volatile("" ::: "memory");
		/* Increment the cycle counter */
		++cycle;
		__asm__("nop");
		__asm__("nop");
		/* Block the compiler from re-ordering the calculations to preserve timings */
		__asm__ volatile("nop" ::: "memory");
		/* Start the clock cycle */
		gpio_set(TCK_PORT, TCK_PIN);
		/* If TDO is high, store a 1 in the appropriate position in the value being accumulated */
		if (gpio_get(TDO_PORT, TDO_PIN)) /* XXX: Try to remove the need for the if here */
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
	gpio_clear(TCK_PORT, TCK_PIN);
}

static void jtagtap_tdi_tdo_seq(
	uint8_t *const data_out, const bool final_tms, const uint8_t *const data_in, size_t clock_cycles)
{
	gpio_clear(TMS_PORT, TMS_PIN);
	gpio_clear(TDI_PORT, TDI_PIN);
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
		gpio_set_val(TMS_PORT, TMS_PIN, cycle + 1U >= clock_cycles && final_tms);
		/* Set up the TDI pin and start the clock cycle */
		gpio_set_val(TDI_PORT, TDI_PIN, data_in[byte] & (1U << bit));
		gpio_set(TCK_PORT, TCK_PIN);
		esp_rom_delay_us(target_delay_us);
		/* Finish the clock cycle */
		gpio_clear(TCK_PORT, TCK_PIN);
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
		gpio_clear(TCK_PORT, TCK_PIN);
		/* On the last tick, assert final_tms to TMS_PIN */
		gpio_set_val(TMS_PORT, TMS_PIN, tms);
		/* Set up the TDI pin and start the clock cycle */
		gpio_set_val(TDI_PORT, TDI_PIN, tdi);
		/* Block the compiler from re-ordering the calculations to preserve timings */
		__asm__ volatile("" ::: "memory");
		/* Increment the cycle counter */
		++cycle;
		__asm__("nop");
		__asm__("nop");
		/* Block the compiler from re-ordering the calculations to preserve timings */
		__asm__ volatile("nop" ::: "memory");
		/* Start the clock cycle */
		gpio_set(TCK_PORT, TCK_PIN);
		/* Finish the clock cycle */
	}
	__asm__("nop");
	__asm__("nop");
	gpio_clear(TCK_PORT, TCK_PIN);
}

static void jtagtap_tdi_seq(const bool final_tms, const uint8_t *const data_in, const size_t clock_cycles)
{
	gpio_clear(TMS_PORT, TMS_PIN);
	if (target_delay_us != 0)
		jtagtap_tdi_seq_clk_delay(data_in, final_tms, clock_cycles);
	else
		jtagtap_tdi_seq_no_delay(data_in, final_tms, clock_cycles);
}

static void jtagtap_cycle_clk_delay(const size_t clock_cycles)
{
	for (size_t cycle = 0; cycle < clock_cycles; ++cycle) {
		gpio_set(TCK_PORT, TCK_PIN);
		esp_rom_delay_us(target_delay_us);
		gpio_clear(TCK_PORT, TCK_PIN);
		esp_rom_delay_us(target_delay_us);
	}
}

static void jtagtap_cycle_no_delay(const size_t clock_cycles)
{
	for (size_t cycle = 0; cycle < clock_cycles; ++cycle) {
		gpio_set(TCK_PORT, TCK_PIN);
		__asm__ volatile("nop" ::: "memory");
		gpio_clear(TCK_PORT, TCK_PIN);
	}
}

static void jtagtap_cycle(const bool tms, const bool tdi, const size_t clock_cycles)
{
	jtagtap_next(tms, tdi);
	if (target_delay_us != 0)
		jtagtap_cycle_clk_delay(clock_cycles - 1U);
	else
		jtagtap_cycle_no_delay(clock_cycles - 1U);
}

#endif /* JTAGTAP_MODE_GPIO == 1 */
