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

static void configure_gpio(int gpio)
{
	if (gpio == -1) {
		return;
	}

	gpio_config_t gpio_configuration = {
		.pin_bit_mask = (1ULL << gpio),
		.mode = GPIO_MODE_INPUT_OUTPUT,
		.pull_up_en = 0,
		.pull_down_en = 0,
		.intr_type = GPIO_INTR_DISABLE,
	};
	ESP_ERROR_CHECK(gpio_config(&gpio_configuration));
}

#if (CONFIG_TDI_GPIO != -1) && (CONFIG_TDO_GPIO == -1)
#error "If TDI is defined then TDO must also be defined"
#endif
#if (CONFIG_TMS_SWDIO_DIR_GPIO != -1) && (CONFIG_TDO_GPIO == -1)
#error "Cannot define TMS_SWDIO_DIR without JTAG support"
#endif
#if (CONFIG_TCK_TDI_DIR_GPIO != -1) && (CONFIG_TMS_SWDIO_DIR_GPIO == -1)
#error "Having tristatable TCK without tristatable TMS is not supported"
#endif

void gpio_dedic_init(void)
{
	static bool initialized = false;
	if (initialized)
		return;
	initialized = true;

	// The order of pins in this array is important, as it must match up with
	// the definitions at the top of gpio-dedic.h.
	int dedic_pin_array[] = {
		CONFIG_TMS_SWDIO_GPIO,
		CONFIG_TCK_SWCLK_GPIO,
		CONFIG_TDO_GPIO,
		CONFIG_TDI_GPIO,
		CONFIG_TMS_SWDIO_DIR_GPIO,
		CONFIG_TCK_TDI_DIR_GPIO,
	};

	configure_gpio(CONFIG_TMS_SWDIO_GPIO);
	configure_gpio(CONFIG_TCK_SWCLK_GPIO);
	configure_gpio(CONFIG_TDO_GPIO);
	configure_gpio(CONFIG_TDI_GPIO);
	configure_gpio(CONFIG_TMS_SWDIO_DIR_GPIO);
	configure_gpio(CONFIG_TCK_TDI_DIR_GPIO);

	dedic_gpio_bundle_config_t dedic_config = {
		.gpio_array = dedic_pin_array,
		.array_size = 2, // Omit JTAG, TCK_TDI_DIR, and TCK_DIF for now
		.flags = {.out_en = 1, .in_en = 1},
	};

	if ((CONFIG_TDI_GPIO != -1) && (CONFIG_TDO_GPIO != -1)) {
		dedic_config.array_size += 2;
		if (CONFIG_TMS_SWDIO_DIR_GPIO != -1) {
			dedic_config.array_size += 1;
			if (CONFIG_TCK_TDI_DIR_GPIO != -1) {
				dedic_config.array_size += 1;
			}
		}
	}

	ESP_ERROR_CHECK(dedic_gpio_new_bundle(&dedic_config, &dedic_gpio_bundle));

	if (CONFIG_TMS_SWDIO_DIR_GPIO != -1) {
		// Enable driving TMS/SWDIO
		dedic_gpio_cpu_ll_write_mask(SWDIO_TMS_DIR_DEDIC_MASK, 0);
		gpio_ll_input_enable(GPIO_HAL_GET_HW(GPIO_PORT_0), CONFIG_TMS_SWDIO_GPIO);

		// Enable the clock, if that pin exists
		if (CONFIG_TCK_TDI_DIR_GPIO != -1) {
			dedic_gpio_cpu_ll_write_mask(SWCLK_DIR_DEDIC_MASK, 0);
		}
	}
}

#endif /* SWDPTAP_MODE_DEDIC == 1 */
