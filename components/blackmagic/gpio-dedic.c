#define TAG    "swd"
#define TAG_LL "swd-ll"
#define DEBUG_SWD_TRANSACTIONS
/* This file implements the SW-DP interface. */

#include "adiv5.h"
#include "general.h"
#include "platform.h"

#if SWDPTAP_MODE_DEDIC == 1

#include "driver/dedic_gpio.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "hal/dedic_gpio_cpu_ll.h"
#include "gpio-dedic.h"

static dedic_gpio_bundle_handle_t dedic_gpio_bundle;

void gpio_dedic_init(void)
{
	static bool initialized = false;
	if (initialized)
		return;
	initialized = true;

	gpio_config_t gpio_test_conf = {
		.pin_bit_mask = (1ULL << CONFIG_TMS_SWDIO_GPIO) | (1ULL << CONFIG_TCK_SWCLK_GPIO) | (1ULL << CONFIG_TDI_GPIO) |
	                    (1ULL << CONFIG_TDO_GPIO),
		.mode = GPIO_MODE_INPUT_OUTPUT,
		.pull_up_en = 0,
		.pull_down_en = 0,
		.intr_type = GPIO_INTR_DISABLE,
	};

	// The order of pins in this array is important, as it must match up with
	// the definitions at the top of this file.
	int dedic_pin_array[] = {
		CONFIG_TMS_SWDIO_GPIO,
		CONFIG_TCK_SWCLK_GPIO,
		CONFIG_TDO_GPIO,
		CONFIG_TDI_GPIO,
		CONFIG_TMS_SWDIO_DIR_GPIO,
		CONFIG_TCK_TDI_DIR_GPIO,
	};

	dedic_gpio_bundle_config_t dedic_config = {
		.gpio_array = dedic_pin_array,
		.array_size = 4, // Omit TCK_TDI_DIR for now
		.flags = {.out_en = 1, .in_en = 1},
	};

	if (CONFIG_TMS_SWDIO_DIR_GPIO != -1) {
		gpio_test_conf.pin_bit_mask |= (1ULL << (CONFIG_TMS_SWDIO_DIR_GPIO & 63));
		dedic_config.array_size += 1;
		if (CONFIG_TCK_TDI_DIR_GPIO != -1) {
			gpio_test_conf.pin_bit_mask |= (1ULL << (CONFIG_TCK_TDI_DIR_GPIO & 63));
			dedic_config.array_size += 1;
		}
	}

	ESP_ERROR_CHECK(gpio_config(&gpio_test_conf));
	ESP_ERROR_CHECK(dedic_gpio_new_bundle(&dedic_config, &dedic_gpio_bundle));

	if (CONFIG_TMS_SWDIO_DIR_GPIO != -1) {
		dedic_gpio_cpu_ll_write_mask(SWDIO_TMS_DIR_DEDIC_MASK, 0);
		gpio_ll_input_enable(GPIO_HAL_GET_HW(GPIO_PORT_0), CONFIG_TMS_SWDIO_GPIO);

		// Enable the clock, if that pin is connected
		if (CONFIG_TCK_TDI_DIR_GPIO != -1) {
			dedic_gpio_cpu_ll_write_mask(SWCLK_DIR_DEDIC_MASK, 0);
		}
	}
}

#endif /* SWDPTAP_MODE_DEDIC == 1 */
