#include "general.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "sdkconfig.h"

#define TAG "bmp-adc"

static adc_cali_handle_t adc_cali_handle;
static adc_oneshot_unit_handle_t adc_handle;
static const adc_oneshot_chan_cfg_t channel_config = {
	.bitwidth = ADC_BITWIDTH_DEFAULT,
	.atten = ADC_ATTEN_DB_2_5,
};

static bool adc_calibration_init(adc_unit_t unit, adc_atten_t atten, adc_cali_handle_t *out_handle)
{
	adc_cali_handle_t handle = NULL;
	esp_err_t ret = ESP_FAIL;
	bool calibrated[2] = {false, false};

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
	if (!calibrated[unit]) {
		ESP_LOGI(TAG, "calibration scheme version is %s", "Curve Fitting");
		adc_cali_curve_fitting_config_t cali_config = {
			.unit_id = unit,
			.atten = atten,
			.bitwidth = ADC_BITWIDTH_DEFAULT,
		};
		ret = adc_cali_create_scheme_curve_fitting(&cali_config, &handle);
		if (ret == ESP_OK) {
			calibrated[unit] = true;
		}
	}
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
	if (!calibrated[unit]) {
		ESP_LOGI(TAG, "calibration scheme version is %s", "Line Fitting");
		adc_cali_line_fitting_config_t cali_config = {
			.unit_id = unit,
			.atten = atten,
			.bitwidth = ADC_BITWIDTH_DEFAULT,
		};
		ret = adc_cali_create_scheme_line_fitting(&cali_config, &handle);
		if (ret == ESP_OK) {
			calibrated[unit] = true;
		}
	}
#endif

	*out_handle = handle;
	if (ret == ESP_OK) {
		ESP_LOGI(TAG, "Calibration Success");
	} else if (ret == ESP_ERR_NOT_SUPPORTED || !calibrated[unit]) {
		ESP_LOGW(TAG, "eFuse not burnt, skip software calibration");
	} else {
		ESP_LOGE(TAG, "Invalid arg or no memory");
	}

	return calibrated[unit];
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

int32_t adc_read_system_voltage(void)
{
	int adc_reading;
	int voltage_reading;
	static bool channel_configured = false;

	if (adc_handle == NULL && !adc_init(&adc_cali_handle, &adc_handle, CONFIG_VREF_ADC_UNIT)) {
		return -1;
	}
	if (!channel_configured) {
		ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, CONFIG_VREF_ADC_CHANNEL, &channel_config));
	}

	ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, CONFIG_VREF_ADC_CHANNEL, &adc_reading));
	ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, CONFIG_VREF_ADC_CHANNEL, &adc_reading));
	// ESP_LOGI(TAG, "ADC%d Channel[%d] Raw Data: %d", CONFIG_VREF_ADC_UNIT + 1, CONFIG_VREF_ADC_CHANNEL, adc_reading);
	// ESP_LOGD(TAG, "raw  data: %d", adc_reading);

	ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc_cali_handle, adc_reading, &voltage_reading));
	// ESP_LOGI(TAG, "ADC%d Channel[%d] Cali Voltage: %d mV", CONFIG_VREF_ADC_UNIT + 1, CONFIG_VREF_ADC_CHANNEL, voltage_reading);

	// Farpatch has a divider that's 82k on top and 20k on the bottom. We're using 2.5 dB
	// attenuation, so also multiply it by 1.33 (aka 4/3).
	int adjusted_voltage = (voltage_reading * 4 * 82) / 20 / 3;
	ESP_LOGD(TAG, "cal data: %d mV, adjusted: %d mV", voltage_reading, adjusted_voltage);

	return adjusted_voltage;
}

#if CONFIG_TMS_ADC_UNIT >= 0
int32_t adc_read_tms_voltage(void)
{
	int adc_reading;
	int voltage_reading;
	static bool channel_configured = false;

	if (adc_handle == NULL && !adc_init(&adc_cali_handle, &adc_handle, CONFIG_TMS_ADC_UNIT)) {
		return -1;
	}
	if (!channel_configured) {
		ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, CONFIG_TMS_ADC_CHANNEL, &channel_config));
	}

	ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, CONFIG_TMS_ADC_CHANNEL, &adc_reading));
	ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, CONFIG_TMS_ADC_CHANNEL, &adc_reading));
	ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc_cali_handle, adc_reading, &voltage_reading));
	int adjusted_voltage = (voltage_reading * 4 * 82) / 20 / 3;
	ESP_LOGD(TAG, "cal data: %d mV, adjusted: %d mV", voltage_reading, adjusted_voltage);

	return adjusted_voltage;
}
#endif

#if CONFIG_TDO_ADC_UNIT >= 0
int32_t adc_read_tdo_voltage(void)
{
	int adc_reading;
	int voltage_reading;
	static bool channel_configured = false;

	if (adc_handle == NULL && !adc_init(&adc_cali_handle, &adc_handle, CONFIG_TDO_ADC_UNIT)) {
		return -1;
	}
	if (!channel_configured) {
		ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, CONFIG_TDO_ADC_CHANNEL, &channel_config));
	}

	ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, CONFIG_TDO_ADC_CHANNEL, &adc_reading));
	ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, CONFIG_TDO_ADC_CHANNEL, &adc_reading));
	ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc_cali_handle, adc_reading, &voltage_reading));
	int adjusted_voltage = (voltage_reading * 4 * 82) / 20 / 3;
	ESP_LOGD(TAG, "cal data: %d mV, adjusted: %d mV", voltage_reading, adjusted_voltage);

	return adjusted_voltage;
}
#endif
