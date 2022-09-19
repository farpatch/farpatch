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

#if SWDPTAP_MODE_ULP == 1

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

/* This file implements the SW-DP interface. */

#include "timing.h"
#include "adiv5.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "ulp_riscv.h"
#include "soc/rtc.h"
// #include "ulp_bmp.h"

extern volatile uint32_t ulp___stack_top;
extern volatile uint32_t ulp_irq_vector;
extern volatile uint32_t ulp_main;
extern volatile uint32_t ulp_reset_vector;
extern volatile uint32_t ulp_SENS;
extern volatile uint32_t ulp_swd_direction_is_output;
extern volatile uint32_t ulp_swd_has_parity;
extern volatile uint32_t ulp_swd_length;
extern volatile uint32_t ulp_swd_parity;
extern volatile uint32_t ulp_swd_value;
extern volatile uint32_t ulp_ulp_riscv_halt;
extern volatile uint32_t ulp_ulp_riscv_rescue_from_monitor;
#if 0
static uint32_t swdptap_seq_in(size_t clock_cycles)
{
	ulp_swd_has_parity = false;
	ulp_swd_direction_is_output = false;
	ulp_swd_length = clock_cycles;
	while (ulp_swd_length) {
	}
	return ulp_swd_value;
}

static bool swdptap_seq_in_parity(uint32_t *ret, size_t clock_cycles)
{
	ulp_swd_has_parity = true;
	ulp_swd_direction_is_output = false;
	ulp_swd_length = clock_cycles;
	while (ulp_swd_length) {
	}
	*ret = ulp_swd_value;
	return ulp_swd_parity;
}

static void swdptap_seq_out(const uint32_t tms_states, const size_t clock_cycles)
{
	ulp_swd_direction_is_output = true;
	ulp_swd_has_parity = false;
	ulp_swd_value = tms_states;
	ulp_swd_length = clock_cycles;
	// TODO: Remove this?
	while (ulp_swd_length) {
	}
}

static void swdptap_seq_out_parity(const uint32_t tms_states, const size_t clock_cycles)
{
	ulp_swd_parity = __builtin_popcount(tms_states) & 1;
	ulp_swd_direction_is_output = true;
	ulp_swd_has_parity = true;
	ulp_swd_value = tms_states;
	ulp_swd_length = clock_cycles;
	// TODO: Remove this?
	while (ulp_swd_length) {
	}
}
#endif

void init_ulp_program(void)
{
	ulp_riscv_halt();

	extern const uint8_t ulp_main_bin_start[] asm("_binary_ulp_bmp_bin_start");
	extern const uint8_t ulp_main_bin_end[] asm("_binary_ulp_bmp_bin_end");
	esp_err_t err = ulp_riscv_load_binary(ulp_main_bin_start, (ulp_main_bin_end - ulp_main_bin_start));
	ESP_ERROR_CHECK(err);

	/* The first argument is the period index, which is not used by the ULP-RISC-V timer
     * The second argument is the period in microseconds, which gives a wakeup time period of: 200ms
     */
	ulp_set_wakeup_period(0, 20000);

	/* Start the program */
	err = ulp_riscv_run();
	ESP_ERROR_CHECK(err);
}
static void configure_clock(void)
{
	// rtc_clk_8m_enable(true, true);
	// rtc_clk_slow_src_set(SOC_RTC_SLOW_CLK_SRC_RC_FAST_D256);
}

int swdptap_init(ADIv5_DP_t *dp)
{
#if 0
	configure_clock();
	rtc_gpio_init(CONFIG_TMS_SWDIO_GPIO);
	rtc_gpio_set_direction(CONFIG_TMS_SWDIO_GPIO, RTC_GPIO_MODE_INPUT_OUTPUT);
	rtc_gpio_pulldown_dis(CONFIG_TMS_SWDIO_GPIO);
	rtc_gpio_pullup_dis(CONFIG_TMS_SWDIO_GPIO);

	rtc_gpio_init(CONFIG_TMS_SWDIO_DIR_GPIO);
	rtc_gpio_set_direction(CONFIG_TMS_SWDIO_GPIO, RTC_GPIO_MODE_OUTPUT_ONLY);
	rtc_gpio_pulldown_dis(CONFIG_TMS_SWDIO_GPIO);
	rtc_gpio_pullup_dis(CONFIG_TMS_SWDIO_GPIO);

	rtc_gpio_init(CONFIG_TCK_SWCLK_GPIO);
	rtc_gpio_set_direction(CONFIG_TCK_SWCLK_GPIO, RTC_GPIO_MODE_OUTPUT_ONLY);
	rtc_gpio_pulldown_dis(CONFIG_TCK_SWCLK_GPIO);
	rtc_gpio_pullup_dis(CONFIG_TCK_SWCLK_GPIO);

	ulp_swd_has_parity = false;
	init_ulp_program();

	ESP_LOGI("swd", "Waiting for has_parity to go true...");
	while (!ulp_swd_has_parity) {
	}
	ESP_LOGI("swd", "Parity is 1");

	dp->seq_in = swdptap_seq_in;
	dp->seq_in_parity = swdptap_seq_in_parity;
	dp->seq_out = swdptap_seq_out;
	dp->seq_out_parity = swdptap_seq_out_parity;
#endif
	return 0;
}
#endif