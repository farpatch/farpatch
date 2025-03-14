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

#define TAG    "swd"
#define TAG_LL "swd-ll"
#define DEBUG_SWD_TRANSACTIONS
/* This file implements the SW-DP interface. */

#include "adiv5.h"
#include "general.h"
#include "platform.h"
#include "timing.h"
#include "maths_utils.h"

#if SWDPTAP_MODE_GPIO == 1

void IRAM_ATTR platform_maybe_delay(void);

swd_proc_s swd_proc;

typedef enum swdio_status_e {
	SWDIO_STATUS_FLOAT = 0,
	SWDIO_STATUS_DRIVE
} swdio_status_t;

static void IRAM_ATTR swdptap_turnaround(swdio_status_t dir) __attribute__((optimize(3)));
static uint32_t IRAM_ATTR swdptap_seq_in(size_t clock_cycles) __attribute__((optimize(3)));
static bool IRAM_ATTR swdptap_seq_in_parity(uint32_t *ret, size_t clock_cycles) __attribute__((optimize(3)));
static void IRAM_ATTR swdptap_seq_out(uint32_t tms_states, size_t clock_cycles) __attribute__((optimize(3)));
static void IRAM_ATTR swdptap_seq_out_parity(uint32_t tms_states, size_t clock_cycles) __attribute__((optimize(3)));

static uint32_t IRAM_ATTR swdptap_seq_in_clk_delay(size_t clock_cycles) __attribute__((optimize(3)));
static uint32_t IRAM_ATTR swdptap_seq_in_no_delay(size_t clock_cycles) __attribute__((optimize(3)));
static void IRAM_ATTR swdptap_seq_out_clk_delay(uint32_t tms_states, size_t clock_cycles) __attribute__((optimize(3)));
static void IRAM_ATTR swdptap_seq_out_no_delay(uint32_t tms_states, size_t clock_cycles) __attribute__((optimize(3)));

static void swdptap_turnaround(const swdio_status_t dir)
{
	static swdio_status_t olddir = SWDIO_STATUS_FLOAT;
	/* Don't turnaround if direction not changing */
	if (dir == olddir)
		return;
	olddir = dir;

#ifdef DEBUG_SWD_BITS
	DEBUG_INFO("%s", dir ? "\n-> " : "\n<- ");
#endif

	if (dir == SWDIO_STATUS_FLOAT) {
		SWDIO_MODE_FLOAT();
	}
	esp_rom_delay_us(target_delay_us);

	gpio_set(SWCLK_PORT, SWCLK_PIN);
	esp_rom_delay_us(target_delay_us);

	gpio_clear(SWCLK_PORT, SWCLK_PIN);
	if (dir == SWDIO_STATUS_DRIVE) {
		SWDIO_MODE_DRIVE();
	}
}

static uint32_t swdptap_seq_in_clk_delay(const size_t clock_cycles)
{
	uint32_t value = 0;
	if (!clock_cycles)
		return 0;
	/*
	 * Count down instead of up, because with an up-count, some ARM-GCC
	 * versions use an explicit CMP, missing the optimization of converting
	 * to a faster down-count that uses SUBS followed by BCS/BCC.
	 */
	for (size_t cycle = clock_cycles; cycle--;) {
		esp_rom_delay_us(target_delay_us);
		const bool bit = gpio_get(SWDIO_IN_PORT, SWDIO_IN_PIN);
		gpio_set(SWCLK_PORT, SWCLK_PIN);
		esp_rom_delay_us(target_delay_us);
		value >>= 1U;
		value |= (uint32_t)bit << 31U;
		gpio_clear(SWCLK_PORT, SWCLK_PIN);
	}
	value >>= (32U - clock_cycles);
	return value;
}

static uint32_t swdptap_seq_in_no_delay(const size_t clock_cycles)
{
	if (!clock_cycles)
		return 0;
	uint32_t value = 0;
	/*
	 * Count down instead of up, because with an up-count, some ARM-GCC
	 * versions use an explicit CMP, missing the optimization of converting
	 * to a faster down-count that uses SUBS followed by BCS/BCC.
	 */
	for (size_t cycle = clock_cycles; cycle--;) {
		bool bit = gpio_get(SWDIO_IN_PORT, SWDIO_IN_PIN);
		gpio_set(SWCLK_PORT, SWCLK_PIN);
		value >>= 1U;
		value |= (uint32_t)bit << 31U;
		gpio_clear(SWCLK_PORT, SWCLK_PIN);
	}
	value >>= (32U - clock_cycles);
	return value;

	// This version processes bytes without reversing them, but doesn't
	// seem to be any faster:
	// uint32_t value = 0;
	// for (size_t cycle = 0; cycle < clock_cycles;) {
	// 	if (gpio_get(SWDIO_PORT, SWDIO_PIN))
	// 		value |= (1U << cycle);
	// 	gpio_set(SWCLK_PORT, SWCLK_PIN);
	// 	++cycle;
	// 	gpio_clear(SWCLK_PORT, SWCLK_PIN);
	// }
	// return value;
}

static uint32_t swdptap_seq_in(size_t clock_cycles)
{
	platform_maybe_delay();
	swdptap_turnaround(SWDIO_STATUS_FLOAT);
	if (target_delay_us != 0)
		return swdptap_seq_in_clk_delay(clock_cycles);
	else // NOLINT(readability-else-after-return)
		return swdptap_seq_in_no_delay(clock_cycles);
}

static bool swdptap_seq_in_parity(uint32_t *ret, size_t clock_cycles)
{
	platform_maybe_delay();
	const uint32_t result = swdptap_seq_in(clock_cycles);
	esp_rom_delay_us(target_delay_us);

	const bool bit = gpio_get(SWDIO_IN_PORT, SWDIO_IN_PIN);

	gpio_set(SWCLK_PORT, SWCLK_PIN);
	esp_rom_delay_us(target_delay_us);

	gpio_clear(SWCLK_PORT, SWCLK_PIN);
	/* Terminate the read cycle now */
	swdptap_turnaround(SWDIO_STATUS_DRIVE);

	const bool parity = calculate_odd_parity(result);
	*ret = result;
	return parity == bit;
}

static void swdptap_seq_out_clk_delay(const uint32_t tms_states, const size_t clock_cycles)
{
	uint32_t value = tms_states;
	bool bit = value & 1U;
	if (!clock_cycles)
		return;
	/*
	 * Count down instead of up, because with an up-count, some ARM-GCC
	 * versions use an explicit CMP, missing the optimization of converting
	 * to a faster down-count that uses SUBS followed by BCS/BCC.
	 */
	for (size_t cycle = clock_cycles; cycle--;) {
		gpio_set_val(SWDIO_PORT, SWDIO_PIN, bit);
		esp_rom_delay_us(target_delay_us);
		gpio_set(SWCLK_PORT, SWCLK_PIN);
		esp_rom_delay_us(target_delay_us);
		value >>= 1U;
		bit = value & 1U;
		gpio_clear(SWCLK_PORT, SWCLK_PIN);
	}
}

static void swdptap_seq_out_no_delay(const uint32_t tms_states, const size_t clock_cycles)
{
	uint32_t value = tms_states;
	bool bit = value & 1U;
	if (!clock_cycles)
		return;
	/*
	 * Count down instead of up, because with an up-count, some ARM-GCC
	 * versions use an explicit CMP, missing the optimization of converting
	 * to a faster down-count that uses SUBS followed by BCS/BCC.
	 */
	for (size_t cycle = clock_cycles; cycle--;) {
		gpio_clear(SWCLK_PORT, SWCLK_PIN);
		gpio_set_val(SWDIO_PORT, SWDIO_PIN, bit);
		gpio_set(SWCLK_PORT, SWCLK_PIN);
		value >>= 1U;
		bit = value & 1U;
	}
	gpio_clear(SWCLK_PORT, SWCLK_PIN);

	// For unknown reasons, this non-reversed version no longer works.
	// for (size_t cycle = 0; cycle < clock_cycles;) {
	// 	++cycle;
	// 	gpio_set(SWCLK_PORT, SWCLK_PIN);
	// 	gpio_set_val(SWDIO_PORT, SWDIO_PIN, tms_states & (1 << cycle));
	// 	gpio_clear(SWCLK_PORT, SWCLK_PIN);
	// }
}

static void swdptap_seq_out(const uint32_t tms_states, const size_t clock_cycles)
{
	platform_maybe_delay();
	swdptap_turnaround(SWDIO_STATUS_DRIVE);
	if (target_delay_us != 0)
		swdptap_seq_out_clk_delay(tms_states, clock_cycles);
	else
		swdptap_seq_out_no_delay(tms_states, clock_cycles);
}

static void swdptap_seq_out_parity(const uint32_t tms_states, const size_t clock_cycles)
{
	platform_maybe_delay();
	const bool parity = calculate_odd_parity(tms_states);
	swdptap_seq_out(tms_states, clock_cycles);
	gpio_set_val(SWDIO_PORT, SWDIO_PIN, parity);
	esp_rom_delay_us(target_delay_us);
	gpio_set(SWCLK_PORT, SWCLK_PIN);
	esp_rom_delay_us(target_delay_us);
	gpio_clear(SWCLK_PORT, SWCLK_PIN);
}

void swdptap_init(void)
{
	swd_proc.seq_in = swdptap_seq_in;
	swd_proc.seq_in_parity = swdptap_seq_in_parity;
	swd_proc.seq_out = swdptap_seq_out;
	swd_proc.seq_out_parity = swdptap_seq_out_parity;
}

#endif
