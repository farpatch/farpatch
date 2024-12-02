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
#include "farpatch_adc.h"
#include "general.h"
#include "gdb_if.h"
#include "version.h"

#include "gdb_packet.h"
#include "gdb_main.h"
#include "target.h"
#include "exception.h"
#include "gdb_packet.h"
#include "morse.h"
#include "platform.h"
#include "CBUF.h"

#include <assert.h>
#include <sys/time.h>
#include <sys/unistd.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "dhcpserver/dhcpserver.h"
#include "http.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "lwip/api.h"
#include "lwip/tcp.h"

// #include "esp32/rom/ets_sys.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "soc/gpio_sig_map.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/uart.h"

#include "uart.h"
#include "wifi.h"
#include "wilma/wilma.h"

#include <lwip/sockets.h>

#include "ota-tftp.h"

#define TAG "farpatch"

nvs_handle_t h_nvs_conf;

// This value is used to spread out service startup,
// which is useful for keeping the power consumption low.
const uint32_t STARTUP_SERVICE_DELAY_MS = 400;
uint32_t target_delay_us = 0;

#if defined(CONFIG_VSEL_PRESENT)
static const char *power_source_name = "unknown";
#endif

void initialise_mdns(const char *hostname);
void rtt_init(void);

void platform_max_frequency_set(uint32_t freq)
{
	// Delay is done with a `sleep_us()`, so 1 MHz is the most
	// we can reliably delay.
	if ((freq == 0) || (freq > 1000000)) {
		target_delay_us = 0;
		return;
	}
	target_delay_us = 1000000 / freq;
}

uint32_t platform_max_frequency_get(void)
{
	// With no delay we run about 12 MHz.
	if (target_delay_us == 0) {
		return 12000000;
	}
	return 1000000 / target_delay_us;
}

void platform_init(void)
{
#if defined(CONFIG_TDI_GPIO) && CONFIG_TDI_GPIO >= 0
	gpio_reset_pin(CONFIG_TDI_GPIO);
#endif
#if defined(CONFIG_TDO_GPIO) && CONFIG_TDO_GPIO >= 0
	gpio_reset_pin(CONFIG_TDO_GPIO);
#endif
	gpio_reset_pin(CONFIG_TMS_SWDIO_GPIO);
	gpio_reset_pin(CONFIG_TCK_SWCLK_GPIO);
#if defined(CONFIG_TMS_SWDIO_DIR_GPIO) && CONFIG_TMS_SWDIO_DIR_GPIO >= 0
	gpio_reset_pin(CONFIG_TMS_SWDIO_DIR_GPIO);
#endif
#if defined(CONFIG_TCK_TDI_DIR_GPIO) && CONFIG_TCK_TDI_DIR_GPIO >= 0
	gpio_reset_pin(CONFIG_TCK_TDI_DIR_GPIO);
#endif

	// Reset Button
#if defined(CONFIG_RESET_BUTTON_GPIO) && CONFIG_RESET_BUTTON_GPIO >= 0
	{
		void handle_wifi_reset(void *parameter);
		void setup_wifi_reset(void);
		const gpio_config_t gpio_conf = {
			.pin_bit_mask = BIT64(CONFIG_RESET_BUTTON_GPIO),
			.mode = GPIO_MODE_INPUT,
			.pull_up_en = GPIO_PULLUP_ENABLE,
			.pull_down_en = 0,
			.intr_type = GPIO_INTR_NEGEDGE,
		};
		setup_wifi_reset();
		gpio_config(&gpio_conf);
		gpio_install_isr_service(0);
		gpio_intr_enable(CONFIG_RESET_BUTTON_GPIO);
		gpio_isr_handler_add(CONFIG_RESET_BUTTON_GPIO, handle_wifi_reset, NULL);
	}
#endif

#if defined(CONFIG_TDO_GPIO) && CONFIG_TDO_GPIO >= 0
	// TDO / SWO
	{
		const gpio_config_t gpio_conf = {
			.pin_bit_mask = BIT64(CONFIG_TDO_GPIO),
			.mode = GPIO_MODE_INPUT,
			.pull_up_en = 0,
			.pull_down_en = 0,
			.intr_type = GPIO_INTR_DISABLE,
		};
		gpio_config(&gpio_conf);
	}
#endif

	// TMS / SWDIO
	{
		const gpio_config_t gpio_conf = {
			.pin_bit_mask = BIT64(CONFIG_TMS_SWDIO_GPIO),
			.mode = GPIO_MODE_OUTPUT,
			.pull_up_en = 0,
			.pull_down_en = 0,
			.intr_type = GPIO_INTR_DISABLE,
		};
		gpio_config(&gpio_conf);
		gpio_set_level(CONFIG_TMS_SWDIO_GPIO, 1);
	}

	// TCK / SWCLK
	{
		const gpio_config_t gpio_conf = {
			.pin_bit_mask = BIT64(CONFIG_TCK_SWCLK_GPIO),
			.mode = GPIO_MODE_OUTPUT,
			.pull_up_en = 0,
			.pull_down_en = 0,
			.intr_type = GPIO_INTR_DISABLE,
		};
		gpio_config(&gpio_conf);
		gpio_set_level(CONFIG_TCK_SWCLK_GPIO, 1);
	}

#if defined(CONFIG_NRST_GPIO) && CONFIG_NRST_GPIO >= 0
	// NRST
	{
		const gpio_config_t gpio_conf = {
			.pin_bit_mask = BIT64(CONFIG_NRST_GPIO),
			.mode = GPIO_MODE_OUTPUT,
			.pull_up_en = 0,
			.pull_down_en = 0,
			.intr_type = GPIO_INTR_DISABLE,
		};
#if defined(CONFIG_RESET_PUSHPULL)
		gpio_set_level(CONFIG_NRST_GPIO, 1);
#endif
#if defined(CONFIG_RESET_OPENDRAIN)
		gpio_set_level(CONFIG_NRST_GPIO, 0);
#endif
		gpio_config(&gpio_conf);
	}
#endif

#if defined(CONFIG_RESET_SENSE_GPIO) && CONFIG_RESET_SENSE_GPIO >= 0
	// TMS/SWDIO level shifter direction
	{
		const gpio_config_t gpio_conf = {
			.pin_bit_mask = BIT64(CONFIG_RESET_SENSE_GPIO),
			.mode = GPIO_MODE_INPUT,
			.pull_up_en = 0,
			.pull_down_en = 0,
			.intr_type = GPIO_INTR_DISABLE,
		};
		gpio_config(&gpio_conf);
	}
#endif

	// TDI
#if defined(CONFIG_TDI_GPIO) && CONFIG_TDI_GPIO >= 0
	{
		const gpio_config_t gpio_conf = {
			.pin_bit_mask = BIT64(CONFIG_TDI_GPIO),
			.mode = GPIO_MODE_OUTPUT,
			.pull_up_en = 0,
			.pull_down_en = 0,
			.intr_type = GPIO_INTR_DISABLE,
		};
		gpio_config(&gpio_conf);
		gpio_set_level(CONFIG_TDI_GPIO, 1);
	}
#endif

#if defined(CONFIG_TMS_SWDIO_DIR_GPIO) && CONFIG_TMS_SWDIO_DIR_GPIO >= 0
	// TMS/SWDIO level shifter direction
	{
		const gpio_config_t gpio_conf = {
			.pin_bit_mask = BIT64(CONFIG_TMS_SWDIO_DIR_GPIO),
			.mode = GPIO_MODE_OUTPUT,
			.pull_up_en = 0,
			.pull_down_en = 0,
			.intr_type = GPIO_INTR_DISABLE,
		};
		gpio_config(&gpio_conf);
		gpio_set_level(CONFIG_TMS_SWDIO_DIR_GPIO, 1);
	}
#endif

#if defined(CONFIG_TCK_TDI_DIR_GPIO) && CONFIG_TCK_TDI_DIR_GPIO >= 0
	// TCK/TDI level shifter direction
	{
		const gpio_config_t gpio_conf = {
			.pin_bit_mask = BIT64(CONFIG_TCK_TDI_DIR_GPIO),
			.mode = GPIO_MODE_OUTPUT,
			.pull_up_en = 0,
			.pull_down_en = 0,
			.intr_type = GPIO_INTR_DISABLE,
		};
		gpio_config(&gpio_conf);
		gpio_set_level(CONFIG_TCK_TDI_DIR_GPIO, 0);
	}
#endif

#if CONFIG_VTARGET_EN_PRESENT
	gpio_reset_pin(CONFIG_VTARGET_EN_GPIO);
#if defined(CONFIG_FARPATCH_DVT4)
	gpio_set_level(CONFIG_VTARGET_EN_GPIO, 0);
#else
	gpio_set_level(CONFIG_VTARGET_EN_GPIO, 1);
#endif
	gpio_set_direction(CONFIG_VTARGET_EN_GPIO, GPIO_MODE_OUTPUT);
#endif

#if defined(CONFIG_VSEL_PRESENT)
	{
		gpio_reset_pin(CONFIG_VSEL_TARGET_GPIO);
		gpio_reset_pin(CONFIG_VSEL_USB_GPIO);
		gpio_reset_pin(CONFIG_VSEL_EXTRA_GPIO);
		gpio_config_t gpio_conf = {
			.pin_bit_mask = BIT64(CONFIG_VSEL_TARGET_GPIO),
			.mode = GPIO_MODE_INPUT,
			.pull_up_en = 1,
			.pull_down_en = 0,
			.intr_type = GPIO_INTR_DISABLE,
		};
		gpio_config(&gpio_conf);

		gpio_conf.pin_bit_mask = BIT64(CONFIG_VSEL_USB_GPIO);
		gpio_config(&gpio_conf);

		gpio_conf.pin_bit_mask = BIT64(CONFIG_VSEL_EXTRA_GPIO);
		gpio_config(&gpio_conf);

		uint32_t power_source = ((!gpio_get_level(CONFIG_VSEL_TARGET_GPIO)) << 0) |
		                        ((!gpio_get_level(CONFIG_VSEL_USB_GPIO)) << 1) |
		                        ((!gpio_get_level(CONFIG_VSEL_EXTRA_GPIO)) << 2);
		if (power_source == 1) {
			power_source_name = "VREF";
		} else if (power_source == 2) {
			power_source_name = "USB";
		} else if (power_source == 4) {
			power_source_name = "EXTRA";
		} else {
			power_source_name = "invalid";
		}
		ESP_LOGI(TAG, "power source: %s", power_source_name);
	}
#endif
}

#ifdef PLATFORM_HAS_POWER_SWITCH
static bool tpwr_enabled = false;
bool platform_target_get_power(void)
{
	return tpwr_enabled;
}

bool platform_target_set_power(bool power)
{
	tpwr_enabled = power;
#if defined(CONFIG_FARPATCH_DVT4)
	gpio_set_level(CONFIG_VTARGET_EN_GPIO, power);
#else
	gpio_set_level(CONFIG_VTARGET_EN_GPIO, !power);
#endif
	return true;
}
#endif /* PLATFORM_HAS_POWER_SWITCH */

void platform_buffer_flush(void)
{
	;
}

void platform_nrst_set_val(bool assert)
{
#if defined(CONFIG_RESET_PUSHPULL)
	gpio_set_level(CONFIG_NRST_GPIO, !assert);
#endif
#if defined(CONFIG_RESET_OPENDRAIN)
	gpio_set_level(CONFIG_NRST_GPIO, assert);
#endif
}

bool platform_nrst_get_val(void)
{
#if defined(CONFIG_RESET_PUSHPULL)
	return !gpio_get_level(CONFIG_NRST_GPIO);
#endif
#if defined(CONFIG_RESET_OPENDRAIN)
	return gpio_get_level(CONFIG_RESET_SENSE_GPIO);
#endif
}

uint32_t platform_target_voltage_sense(void)
{
	// Convert mV to dV (e.g. 3300 -> 33 for 3.3V)
	return voltages_mv[ADC_VREF_VOLTAGE] / 100;
}

const char *platform_target_voltage(void)
{
	static char voltage[48];

	int32_t adjusted_voltage = voltages_mv[ADC_VREF_VOLTAGE];
	if (adjusted_voltage == -1) {
		snprintf(voltage, sizeof(voltage) - 1, "unknown");
		return voltage;
	}

	snprintf(voltage, sizeof(voltage) - 1, "%ldmV", adjusted_voltage);
	return voltage;
}

uint32_t platform_time_ms(void)
{
	return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

#define vTaskDelayMs(ms) vTaskDelay((ms) / portTICK_PERIOD_MS)

void platform_delay(uint32_t ms)
{
	vTaskDelayMs(ms);
}

int platform_hwversion(void)
{
	return 0;
}

void platform_set_baud(uint32_t baud)
{
	uart_set_baudrate(TARGET_UART_IDX, baud);
	nvs_set_u32(h_nvs_conf, "uartbaud", baud);
}

bool cmd_setbaud(target_s *t, int argc, const char **argv)
{
	uint32_t baud;
	if (argc == 1) {
		uart_get_baudrate(TARGET_UART_IDX, &baud);
		gdb_outf("Current baud: %" PRIu32 "\n", baud);
	}
	if (argc == 2) {
		baud = strtoul(argv[1], NULL, 0);
		gdb_outf("Setting baud: %" PRIu32 "\n", baud);
		platform_set_baud(baud);
	}

	return 1;
}

/// Enable or disable the clock output pin. This is not configured on
/// current Farpatch designs, but will be used in a future model.
void platform_target_clk_output_enable(bool enabled)
{
#if defined(CONFIG_TCK_TDI_DIR_GPIO) && CONFIG_TCK_TDI_DIR_GPIO >= 0
	gpio_set_level(CONFIG_NRST_GPIO, !enabled);
#else
	(void)enabled;
#endif
}

int vprintf_noop(const char *s, va_list va)
{
	return 1;
}

extern void gdb_net_task();
#ifdef CONFIG_RTT_ON_BOOT
void rtt_monitor_task(void *params);
#endif /* CONFIG_RTT_ON_BOOT */

void app_main(void)
{
	esp_err_t ret;

	ESP_LOGI(__func__, "starting farpatch");
#if CONFIG_LED_GPIO >= 0
	gpio_reset_pin(CONFIG_LED_GPIO);
	gpio_set_direction(CONFIG_LED_GPIO, GPIO_MODE_OUTPUT);
	gpio_set_level(CONFIG_LED_GPIO, 1);
#endif
#if CONFIG_LED2_GPIO >= 0
	gpio_reset_pin(CONFIG_LED2_GPIO);
	gpio_set_direction(CONFIG_LED2_GPIO, GPIO_MODE_OUTPUT);
	gpio_set_level(CONFIG_LED2_GPIO, 1);
#endif
#if CONFIG_LED3_GPIO >= 0
	gpio_reset_pin(CONFIG_LED3_GPIO);
	gpio_set_direction(CONFIG_LED3_GPIO, GPIO_MODE_OUTPUT);
	gpio_set_level(CONFIG_LED3_GPIO, 1);
#endif
#if CONFIG_LED4_GPIO >= 0
	// Turn LED4 ("power LED") on
	gpio_reset_pin(CONFIG_LED4_GPIO);
	gpio_set_direction(CONFIG_LED4_GPIO, GPIO_MODE_OUTPUT);
	gpio_set_level(CONFIG_LED4_GPIO, 0);
#endif
#if defined(CONFIG_UART_TX_DIR_GPIO) && CONFIG_UART_TX_DIR_GPIO >= 0
	gpio_reset_pin(CONFIG_UART_TX_DIR_GPIO);
	gpio_set_direction(CONFIG_UART_TX_DIR_GPIO, GPIO_MODE_OUTPUT);
	gpio_set_level(CONFIG_UART_TX_DIR_GPIO, 0);
#endif
	vTaskDelay(pdMS_TO_TICKS(STARTUP_SERVICE_DELAY_MS));

#ifdef CONFIG_ESP_DEBUG_LOGS
	uart_dbg_install();
#else /* !CONFIG_ESP_DEBUG_LOGS */
	ESP_LOGI(TAG, "deactivating debug");
	esp_log_set_vprintf(vprintf_noop);
#endif

	ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	ESP_ERROR_CHECK(nvs_open("config", NVS_READWRITE, &h_nvs_conf));

	ESP_LOGI(TAG, "starting adc");
	vTaskDelay(pdMS_TO_TICKS(STARTUP_SERVICE_DELAY_MS));
	xTaskCreate(adc_task, "adc", 2048, NULL, 10, NULL);

	ESP_LOGI(TAG, "starting WiLma wifi manager");
	vTaskDelay(pdMS_TO_TICKS(STARTUP_SERVICE_DELAY_MS));
	wilma_start();

	ESP_LOGI(TAG, "starting web server");
	vTaskDelay(pdMS_TO_TICKS(STARTUP_SERVICE_DELAY_MS));
	webserver_start();

	ESP_LOGI(TAG, "starting mdns broadcaster");
	vTaskDelay(pdMS_TO_TICKS(STARTUP_SERVICE_DELAY_MS));
	initialise_mdns(NULL);

	ESP_LOGI(TAG, "initializing platform");
	vTaskDelay(pdMS_TO_TICKS(STARTUP_SERVICE_DELAY_MS));
	platform_init();

	ESP_LOGI(TAG, "starting uart monitor");
	vTaskDelay(pdMS_TO_TICKS(STARTUP_SERVICE_DELAY_MS));
	uart_init();

	ESP_LOGI(TAG, "starting rtt receiver for tcp");
	vTaskDelay(pdMS_TO_TICKS(STARTUP_SERVICE_DELAY_MS));
	rtt_init();

	ESP_LOGI(TAG, "starting gdb server");
	vTaskDelay(pdMS_TO_TICKS(STARTUP_SERVICE_DELAY_MS));
	xTaskCreate(gdb_net_task, "gdb_net", 2000, NULL, 3, NULL);

	ESP_LOGI(TAG, "starting tftp server");
	vTaskDelay(pdMS_TO_TICKS(STARTUP_SERVICE_DELAY_MS));
	ota_tftp_init_server(69, 4);

	ESP_LOGI(TAG, "starting swo server");
	vTaskDelay(pdMS_TO_TICKS(STARTUP_SERVICE_DELAY_MS));
	void swo_listen_task(void *);
	xTaskCreate(swo_listen_task, "swo listen", 2000, NULL, 4, NULL);

#ifdef CONFIG_RESET_TARGET_ON_BOOT
	ESP_LOGI(TAG, "resetting target on boot");
	platform_nrst_set_val(true);
	vTaskDelay(pdMS_TO_TICKS(STARTUP_SERVICE_DELAY_MS));
	platform_nrst_set_val(false);
#endif /* CONFIG_RESET_TARGET_ON_BOOT */

#ifdef CONFIG_RTT_ON_BOOT
	ESP_LOGI(TAG, "starting rtt monitor thread");
	vTaskDelay(pdMS_TO_TICKS(STARTUP_SERVICE_DELAY_MS));
	xTaskCreate(rtt_monitor_task, "rtt_monitor", 3000, NULL, tskIDLE_PRIORITY + 1, NULL);
#endif /* CONFIG_RTT_ON_BOOT */

	ESP_LOGI(__func__, "Free heap %" PRId32, esp_get_free_heap_size());

	// Wait a few seconds for the system to stabilize before confirming the
	// new firmware image works. This gives us time to ensure the new
	// environment works well.
	vTaskDelay(pdMS_TO_TICKS(5000));
	esp_ota_mark_app_valid_cancel_rollback();
}
