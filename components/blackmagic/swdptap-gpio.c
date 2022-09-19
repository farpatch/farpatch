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

#include "general.h"
#include "timing.h"
#include "adiv5.h"

#if SWDPTAP_MODE_GPIO == 1

uint32_t swd_delay_cnt = 0;

enum {
	SWDIO_STATUS_FLOAT = 0,
	SWDIO_STATUS_DRIVE
};
static IRAM_ATTR void swdptap_turnaround(int dir) __attribute__((optimize(3)));
static IRAM_ATTR uint32_t swdptap_seq_in(size_t ticks) __attribute__((optimize(3)));
static IRAM_ATTR bool swdptap_seq_in_parity(uint32_t *ret, size_t ticks) __attribute__((optimize(3)));
static IRAM_ATTR void swdptap_seq_out(uint32_t MS, size_t ticks) __attribute__((optimize(3)));
static IRAM_ATTR void swdptap_seq_out_parity(uint32_t MS, size_t ticks) __attribute__((optimize(3)));

static inline void swdio_high(void)
{
#if SWDIO_PIN < 32
	GPIO.out_w1ts = (1 << SWDIO_PIN);
#else
	GPIO.out1_w1ts.data = (1 << (SWDIO_PIN - 32));
#endif
}

static inline void swdio_low(void)
{
#if SWDIO_PIN < 32
	GPIO.out_w1tc = (1 << SWDIO_PIN);
#else
	GPIO.out1_w1tc.data = (1 << (SWDIO_PIN - 32));
#endif
}

static inline void swdio_set(uint32_t val)
{
	if (val) {
		swdio_high();
	} else {
		swdio_low();
	}
}

static inline uint32_t swdio_get(void)
{
#if SWDIO_PIN < 32
	return GPIO.in & (1 << SWDIO_PIN);
#else
	return GPIO.in1.data & (1 << (SWDIO_PIN - 32));
#endif
}

static inline void swclk_high(void)
{
#if SWCLK_PIN < 32
	GPIO.out_w1ts = (1 << SWCLK_PIN);
#else
	GPIO.out1_w1ts.data = (1 << (SWCLK_PIN - 32));
#endif
}

static inline void swclk_low(void)
{
#if SWCLK_PIN < 32
	GPIO.out_w1tc = (1 << SWCLK_PIN);
#else
	GPIO.out1_w1tc.data = (1 << (SWCLK_PIN - 32));
#endif
}

static inline void swdio_mode_float(void)
{
	gpio_set_direction(CONFIG_TMS_SWDIO_GPIO, GPIO_MODE_INPUT);
	// #if CONFIG_TMS_SWDIO_GPIO < 32
	// 	GPIO.enable_w1tc = (0x1 << CONFIG_TMS_SWDIO_GPIO);
	// #else
	// 	GPIO.enable1_w1tc.data = (0x1 << (CONFIG_TMS_SWDIO_GPIO - 32));
	// #endif

#if CONFIG_TMS_SWDIO_DIR_GPIO < 0
	// Do nothing if the DIR GPIO is -1
#elif CONFIG_TMS_SWDIO_DIR_GPIO < 32
	GPIO.out_w1ts = (1 << CONFIG_TMS_SWDIO_DIR_GPIO);
#else
	GPIO.out1_w1ts.data = (1 << (CONFIG_TMS_SWDIO_DIR_GPIO - 32));
#endif
}

static inline void swdio_mode_drive(void)
{
#if CONFIG_TMS_SWDIO_DIR_GPIO < 0
	// Do nothing if the DIR GPIO is -1
#elif CONFIG_TMS_SWDIO_DIR_GPIO < 32
	GPIO.out_w1tc = (1 << CONFIG_TMS_SWDIO_DIR_GPIO);
#else
	GPIO.out1_w1tc.data = (1 << (CONFIG_TMS_SWDIO_DIR_GPIO - 32));
#endif

	// #if CONFIG_TMS_SWDIO_GPIO < 32
	// 	GPIO.enable_w1ts = (0x1 << CONFIG_TMS_SWDIO_GPIO);
	// #else
	// 	GPIO.enable1_w1ts.data = (0x1 << (CONFIG_TMS_SWDIO_GPIO - 32));
	// #endif
	gpio_ll_output_enable(GPIO_HAL_GET_HW(GPIO_PORT_0), CONFIG_TMS_SWDIO_GPIO);
}

static void swdptap_turnaround(int dir)
{
	static int olddir = SWDIO_STATUS_FLOAT;
	register int32_t cnt;

	// Throw in a sleep every now and then, in order to allow
	// for other tasks to run.
	static int rest_counter = 0;
	rest_counter += 1;
	if (rest_counter > 1000) {
		rest_counter = 0;
		vTaskDelay(1);
	}

	/* Don't turnaround if direction not changing */
	if (dir == olddir)
		return;
	olddir = dir;

#ifdef DEBUG_SWD_BITS
	DEBUG("%s", dir ? "\n-> " : "\n<- ");
#endif
	if (dir == SWDIO_STATUS_FLOAT)
		swdio_mode_float();
	swclk_high();
	for (cnt = swd_delay_cnt; --cnt > 0;)
		;
	swclk_low();
	for (cnt = swd_delay_cnt; --cnt > 0;)
		;
	if (dir == SWDIO_STATUS_DRIVE)
		swdio_mode_drive();
}

static uint32_t swdptap_seq_in(size_t ticks)
{
	uint32_t index = 1;
	uint32_t ret = 0;
	int len = ticks;
	register int32_t cnt;
	swdptap_turnaround(SWDIO_STATUS_FLOAT);
	if (swd_delay_cnt) {
		while (len--) {
			int res;
			res = swdio_get();
			swclk_high();
			for (cnt = swd_delay_cnt; --cnt > 0;)
				;
			ret |= (res) ? index : 0;
			index <<= 1;
			swclk_low();
			for (cnt = swd_delay_cnt; --cnt > 0;)
				;
		}
	} else {
		int res;
		while (len--) {
			res = swdio_get();
			swclk_high();
			ret |= (res) ? index : 0;
			index <<= 1;
			swclk_low();
		}
	}
#ifdef DEBUG_SWD_BITS
	for (int i = 0; i < len; i++)
		DEBUG("%d", (ret & (1 << i)) ? 1 : 0);
#endif
	return ret;
}

static bool swdptap_seq_in_parity(uint32_t *ret, size_t ticks)
{
	uint32_t index = 1;
	uint32_t res = 0;
	bool bit;
	int len = ticks;
	register int32_t cnt;

	swdptap_turnaround(SWDIO_STATUS_FLOAT);
	if (swd_delay_cnt) {
		while (len--) {
			bit = swdio_get();
			swclk_high();
			for (cnt = swd_delay_cnt; --cnt > 0;)
				;
			res |= (bit) ? index : 0;
			index <<= 1;
			swclk_low();
			for (cnt = swd_delay_cnt; --cnt > 0;)
				;
		}
	} else {
		while (len--) {
			bit = swdio_get();
			swclk_high();
			res |= (bit) ? index : 0;
			index <<= 1;
			swclk_low();
		}
	}
	int parity = __builtin_popcount(res);
	bit = swdio_get();
	swclk_high();
	for (cnt = swd_delay_cnt; --cnt > 0;)
		;
	parity += (bit) ? 1 : 0;
	swclk_low();
	for (cnt = swd_delay_cnt; --cnt > 0;)
		;
#ifdef DEBUG_SWD_BITS
	for (int i = 0; i < len; i++)
		DEBUG("%d", (res & (1 << i)) ? 1 : 0);
#endif
	*ret = res;
	/* Terminate the read cycle now */
	swdptap_turnaround(SWDIO_STATUS_DRIVE);
	return (parity & 1);
}

static void swdptap_seq_out(uint32_t MS, size_t ticks)
{
#ifdef DEBUG_SWD_BITS
	for (int i = 0; i < ticks; i++)
		DEBUG("%d", (MS & (1 << i)) ? 1 : 0);
#endif
	register int32_t cnt;
	swdptap_turnaround(SWDIO_STATUS_DRIVE);
	swdio_set(MS & 1);
	if (swd_delay_cnt) {
		while (ticks--) {
			swclk_high();
			for (cnt = swd_delay_cnt; --cnt > 0;)
				;
			MS >>= 1;
			swdio_set(MS & 1);
			swclk_low();
			for (cnt = swd_delay_cnt; --cnt > 0;)
				;
		}
	} else {
		while (ticks--) {
			swclk_high();
			MS >>= 1;
			swdio_set(MS & 1);
			swclk_low();
		}
	}
}

static void swdptap_seq_out_parity(uint32_t MS, size_t ticks)
{
	int parity = __builtin_popcount(MS);
#ifdef DEBUG_SWD_BITS
	for (int i = 0; i < ticks; i++)
		DEBUG("%d", (MS & (1 << i)) ? 1 : 0);
#endif
	register int32_t cnt;
	swdptap_turnaround(SWDIO_STATUS_DRIVE);
	swdio_set(MS & 1);
	MS >>= 1;
	if (swd_delay_cnt) {
		while (ticks--) {
			swclk_high();
			for (cnt = swd_delay_cnt; --cnt > 0;)
				;
			swdio_set(MS & 1);
			MS >>= 1;
			swclk_low();
			for (cnt = swd_delay_cnt; --cnt > 0;)
				;
		}
	} else {
		while (ticks--) {
			swclk_high();
			swdio_set(MS & 1);
			MS >>= 1;
			swclk_low();
		}
	}
	gpio_set_val(SWDIO_PORT, SWDIO_PIN, parity & 1);
	swclk_high();
	for (cnt = swd_delay_cnt; --cnt > 0;)
		;
	swclk_low();
	for (cnt = swd_delay_cnt; --cnt > 0;)
		;
}

int swdptap_init(ADIv5_DP_t *dp)
{
	dp->seq_in = swdptap_seq_in;
	dp->seq_in_parity = swdptap_seq_in_parity;
	dp->seq_out = swdptap_seq_out;
	dp->seq_out_parity = swdptap_seq_out_parity;

	gpio_reset_pin(CONFIG_TDI_GPIO);
	gpio_reset_pin(CONFIG_TDO_GPIO);
	gpio_reset_pin(CONFIG_TMS_SWDIO_GPIO);
	gpio_reset_pin(CONFIG_TCK_SWCLK_GPIO);
#if CONFIG_TMS_SWDIO_DIR_GPIO >= 0
	gpio_reset_pin(CONFIG_TMS_SWDIO_DIR_GPIO);
#endif

	gpio_set_direction(CONFIG_TDI_GPIO, GPIO_MODE_OUTPUT);
	gpio_set_direction(CONFIG_TDO_GPIO, GPIO_MODE_INPUT);
	gpio_set_direction(CONFIG_TMS_SWDIO_GPIO, GPIO_MODE_OUTPUT);
	gpio_set_direction(CONFIG_TCK_SWCLK_GPIO, GPIO_MODE_OUTPUT);
#if CONFIG_TMS_SWDIO_DIR_GPIO >= 0
	gpio_set_direction(CONFIG_TMS_SWDIO_DIR_GPIO, GPIO_MODE_OUTPUT);
	gpio_set_level(CONFIG_TMS_SWDIO_DIR_GPIO, 0);
#endif
	return 0;
}
#endif