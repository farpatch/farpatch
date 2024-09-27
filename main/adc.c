#include "farpatch_adc.h"
#include "general.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "sdkconfig.h"

#define TAG "farpatch-adc"

static adc_cali_handle_t adc_cali_handle;
static adc_oneshot_unit_handle_t adc_handle;
static const adc_oneshot_chan_cfg_t channel_config = {
	.bitwidth = ADC_BITWIDTH_DEFAULT,
	.atten = ADC_ATTEN_DB_2_5,
};

int32_t voltages_mv[ADC_VOLTAGE_COUNT] = {};
#define ADC_POLL_RATE_MS 100

static bool adc_calibration_init(adc_unit_t unit, adc_atten_t atten, adc_cali_handle_t *out_handle)
{
	adc_cali_handle_t handle = NULL;
	esp_err_t ret = ESP_FAIL;
	bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
	if (!calibrated) {
		ESP_LOGI(TAG, "calibration scheme version is %s", "Curve Fitting");
		adc_cali_curve_fitting_config_t cali_config = {
			.unit_id = unit,
			.atten = atten,
			.bitwidth = ADC_BITWIDTH_DEFAULT,
		};
		ret = adc_cali_create_scheme_curve_fitting(&cali_config, &handle);
		if (ret == ESP_OK) {
			calibrated = true;
		}
	}
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
	if (!calibrated) {
		ESP_LOGI(TAG, "calibration scheme version is %s", "Line Fitting");
		adc_cali_line_fitting_config_t cali_config = {
			.unit_id = unit,
			.atten = atten,
			.bitwidth = ADC_BITWIDTH_DEFAULT,
		};
		ret = adc_cali_create_scheme_line_fitting(&cali_config, &handle);
		if (ret == ESP_OK) {
			calibrated = true;
		}
	}
#endif

	*out_handle = handle;
	if (ret == ESP_OK) {
		ESP_LOGI(TAG, "Calibration Success");
	} else if (ret == ESP_ERR_NOT_SUPPORTED || !calibrated) {
		ESP_LOGW(TAG, "eFuse not burnt, skip software calibration");
	} else {
		ESP_LOGE(TAG, "Invalid arg or no memory");
	}

	// If calibration failed, then this board just doesn't support it.
	// Return `true` in order to prevent a memory leak from constantly
	// reinitializing the ADC.
	return true;
}

static bool adc_init(adc_cali_handle_t *adc_cali_handle, adc_oneshot_unit_handle_t *adc_handle, adc_unit_t unit)
{
	const adc_oneshot_unit_init_cfg_t init_config = {
		.unit_id = unit,
	};
	if (!adc_calibration_init(unit, ADC_ATTEN_DB_2_5, adc_cali_handle)) {
		return false;
	}
	ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, adc_handle));
	return true;
}

int32_t adc_read_voltage(adc_channel_t channel)
{
	int adc_reading;
	int voltage_reading;
	ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, channel, &adc_reading));
	ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc_cali_handle, adc_reading, &voltage_reading));

	// Farpatch has a divider that's 82k on top and 20k on the bottom. We're using 2.5 dB
	// attenuation, so also multiply it by 1.33 (aka 4/3).
	return (voltage_reading * 4 * 82) / 20 / 3;
}

void adc_task(void *ignored)
{
	(void)ignored;

	if (CONFIG_VREF_ADC_UNIT == -1) {
		ESP_LOGI(TAG, "adc is disabled");
		vTaskDelete(NULL);
		return;
	}

	ESP_LOGI(TAG, "ADC task started");
	if (!adc_init(&adc_cali_handle, &adc_handle, CONFIG_VREF_ADC_UNIT)) {
		ESP_LOGE(TAG, "ADC init failed");
		vTaskDelete(NULL);
		return;
	}

	if (CONFIG_ADC_SYSTEM_CHANNEL >= 0) {
		ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, CONFIG_ADC_SYSTEM_CHANNEL, &channel_config));
	}
	if (CONFIG_VREF_ADC_CHANNEL >= 0) {
		ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, CONFIG_VREF_ADC_CHANNEL, &channel_config));
	}
	if (CONFIG_ADC_USB_CHANNEL >= 0) {
		ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, CONFIG_ADC_USB_CHANNEL, &channel_config));
	}
	if (CONFIG_ADC_EXT_CHANNEL >= 0) {
		ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, CONFIG_ADC_EXT_CHANNEL, &channel_config));
	}
	if (CONFIG_ADC_DEBUG_CHANNEL >= 0) {
		ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, CONFIG_ADC_DEBUG_CHANNEL, &channel_config));
	}

	while (true) {
		if (CONFIG_ADC_SYSTEM_CHANNEL >= 0) {
			voltages_mv[ADC_SYSTEM_VOLTAGE] = adc_read_voltage(CONFIG_ADC_SYSTEM_CHANNEL);
		}
		if (CONFIG_VREF_ADC_CHANNEL >= 0) {
			voltages_mv[ADC_TARGET_VOLTAGE] = adc_read_voltage(CONFIG_VREF_ADC_CHANNEL);
		}
		if (CONFIG_ADC_USB_CHANNEL >= 0) {
			voltages_mv[ADC_USB_VOLTAGE] = adc_read_voltage(CONFIG_ADC_USB_CHANNEL);
		}
		if (CONFIG_ADC_EXT_CHANNEL >= 0) {
			voltages_mv[ADC_EXT_VOLTAGE] = adc_read_voltage(CONFIG_ADC_EXT_CHANNEL);
		}
		if (CONFIG_ADC_DEBUG_CHANNEL >= 0) {
			voltages_mv[ADC_DEBUG_VOLTAGE] = adc_read_voltage(CONFIG_ADC_DEBUG_CHANNEL);
		}
		vTaskDelay(ADC_POLL_RATE_MS / portTICK_PERIOD_MS);
	}
}
