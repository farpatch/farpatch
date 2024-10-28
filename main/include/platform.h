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

#ifndef FARPATCH_PLATFORM_H
#define FARPATCH_PLATFORM_H

#include "esp_log.h"
#include "esp_attr.h"
#include "timing.h"
#include "driver/gpio.h"
#include "hal/gpio_hal.h"
#include "hal/gpio_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PLATFORM_HAS_DEBUG
extern bool debug_bmp;

void platform_buffer_flush(void);
void platform_set_baud(uint32_t baud);

#define SET_RUN_STATE(state)
#define SET_IDLE_STATE(state)
#define SET_ERROR_STATE(state) gpio_set_level(CONFIG_LED_GPIO, !state)

#ifndef NO_LIBOPENCM3
#define NO_LIBOPENCM3
#endif

#ifndef PC_HOSTED
#define PC_HOSTED 0
#endif

#if 1
#define ENABLE_DEBUG 1
#define DEBUG(x, ...)                        \
	do {                                     \
		TRIM(out, x);                        \
		ESP_LOGD("BMP", out, ##__VA_ARGS__); \
	} while (0)
#else
#define DEBUG(x, ...)
#endif

#define SWDIO_MODE_FLOAT()                                          \
	do {                                                            \
		gpio_set_direction(CONFIG_TMS_SWDIO_GPIO, GPIO_MODE_INPUT); \
		if (CONFIG_TMS_SWDIO_DIR_GPIO >= 0)                         \
			gpio_set_level(CONFIG_TMS_SWDIO_DIR_GPIO, 1);           \
	} while (0)

#define SWDIO_MODE_DRIVE()                                                          \
	do {                                                                            \
		if (CONFIG_TMS_SWDIO_DIR_GPIO >= 0)                                         \
			gpio_set_level(CONFIG_TMS_SWDIO_DIR_GPIO, 0);                           \
		gpio_ll_output_enable(GPIO_HAL_GET_HW(GPIO_PORT_0), CONFIG_TMS_SWDIO_GPIO); \
	} while (0)

#define TMS_PIN CONFIG_TMS_SWDIO_GPIO
#define TCK_PIN CONFIG_TCK_SWCLK_GPIO
#define TDI_PIN CONFIG_TDI_GPIO
#define TDO_PIN CONFIG_TDO_GPIO

#define SWDIO_PIN CONFIG_TMS_SWDIO_GPIO
#define SWDIO_IN_PIN CONFIG_TMS_SWDIO_GPIO
#define SWCLK_PIN CONFIG_TCK_SWCLK_GPIO
#define SRST_PIN  CONFIG_SRST_GPIO

#define SWCLK_PORT 0
#define SWDIO_PORT 0

#if defined(CONFIG_IDF_TARGET_ESP32C3)
#define gpio_set(port, pin)                            \
	do {                                               \
		GPIO.out_w1ts.out_w1ts = (1 << (uint32_t)pin); \
	} while (0)
#define gpio_clear(port, pin)                          \
	do {                                               \
		GPIO.out_w1tc.out_w1tc = (1 << (uint32_t)pin); \
	} while (0)
#define gpio_get(port, pin) ((GPIO.in.data >> pin) & 0x1)
/* CONFIG_IDF_TARGET_ESP32C3 */

#elif defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32)
#define gpio_set(port, pin)                              \
	do {                                                 \
		if (pin < 0) {                                   \
		} else if (pin < 32) {                           \
			GPIO.out_w1ts = (1 << (uint32_t)(pin & 31)); \
		} else if (pin < 64) {                           \
			uint32_t p = pin - 32;                       \
			GPIO.out1_w1ts.data = (1 << p);              \
		}                                                \
	} while (0)
#define gpio_clear(port, pin)                            \
	do {                                                 \
		if (pin < 0) {                                   \
		} else if (pin < 32) {                           \
			GPIO.out_w1tc = (1 << (uint32_t)(pin & 31)); \
		} else if (pin < 64) {                           \
			uint32_t p = pin - 32;                       \
			GPIO.out1_w1tc.data = (1 << p);              \
		}                                                \
	} while (0)
#define gpio_get(port, pin) ((pin < 0 ? 0 : pin < 32 ? (GPIO.in >> pin) : (GPIO.in1.data >> (pin - 32))) & 0x1)
/* CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32 */

#else
#define gpio_set(port, pin)     \
	do {                        \
		gpio_set_level(pin, 1); \
	} while (0)
#define gpio_clear(port, pin)   \
	do {                        \
		gpio_set_level(pin, 0); \
	} while (0)
#define gpio_get(port, pin) gpio_get_level(pin)
#endif

#define gpio_set_val(port, pin, value) \
	if (value) {                       \
		gpio_set(port, pin);           \
	} else {                           \
		gpio_clear(port, pin);         \
	}

#define GPIO_INPUT  GPIO_MODE_INPUT
#define GPIO_OUTPUT GPIO_MODE_OUTPUT

#define PLATFORM_HAS_DEBUG
#define PLATFORM_IDENT CONFIG_IDF_TARGET

#define PLATFORM_HAS_TRACESWO
#define NUM_TRACE_PACKETS (128) /* This is an 8K buffer */
#define SWO_ENCODING 2     /* 1 = Manchester, 2 = NRZ / async, 3 = Both */
#define SWO_ENDPOINT 0U /* Dummy value -- not used */

extern uint32_t target_delay_us;

#endif /* FARPATCH_PLATFORM_H */
