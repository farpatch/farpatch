#include "farpatch_adc.h"
#include "general.h"
#include "driver/temperature_sensor.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "sdkconfig.h"

#define TAG "farpatch-adc"

#define ATTENUATION         ADC_ATTEN_DB_2_5
#define ADC_CONSTANT_OFFSET 50

static adc_oneshot_unit_handle_t adc_handle;
static const adc_oneshot_chan_cfg_t channel_config = {
	.bitwidth = ADC_BITWIDTH_DEFAULT,
	.atten = ATTENUATION,
};

int32_t voltages_mv[ADC_VOLTAGE_COUNT] = {};

static adc_channel_t channel_index[] = {
	CONFIG_ADC_SYSTEM_CHANNEL,
	CONFIG_ADC_VREF_CHANNEL,
	CONFIG_ADC_USB_CHANNEL,
	CONFIG_ADC_EXT_CHANNEL,
	CONFIG_ADC_DEBUG_CHANNEL,
};
const char *channel_names[] = {
	"system",
	"vref",
	"usb",
	"ext",
	"debug",
};

#if SOC_TEMP_SENSOR_SUPPORTED
static temperature_sensor_handle_t temp_sensor = NULL;
static const temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 70);
float temperature;
#endif

static adc_cali_handle_t adc_cali_handle[ADC_VOLTAGE_COUNT];
#define ADC_POLL_RATE_MS 100

static bool adc_calibration_init(
	adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t *out_handle)
{
	adc_cali_handle_t handle = NULL;
	esp_err_t ret = ESP_FAIL;
	bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
	if (!calibrated) {
		ESP_LOGI(TAG, "calibration scheme version is %s", "Curve Fitting");
		adc_cali_curve_fitting_config_t cali_config = {
			.unit_id = unit,
			.chan = channel,
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

	return calibrated;
}

static bool adc_init(void)
{
	const adc_oneshot_unit_init_cfg_t init_config = {
		.unit_id = CONFIG_ADC_UNIT,
	};
	ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));
	return true;
}

static void channel_init(enum AdcVoltageChannel voltage_index)
{
	adc_channel_t channel = channel_index[voltage_index];
	ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, channel, &channel_config));
	adc_calibration_init(CONFIG_ADC_UNIT, channel, ATTENUATION, &adc_cali_handle[voltage_index]);
}

static void update_voltage(enum AdcVoltageChannel voltage_index)
{
	adc_channel_t channel = channel_index[voltage_index];
	int adc_reading;
	int voltage_reading;
	ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, channel, &adc_reading));
	ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc_cali_handle[voltage_index], adc_reading, &voltage_reading));
	// ESP_LOGI(TAG, "channel %d %s adc_reading: %d  voltage_reading: %d mV (%d adjusted)", channel, channel_names[voltage_index],
	// 	adc_reading, voltage_reading, voltage_reading * 51 / 10);

	// Farpatch has a divider that's 82k on top (R1) and 20k on the bottom. (R2).
	// To figure out the voltage Vs, we need to calculate:
	//    Vout = (Vs*R2)/(R1+R2)
	// Or, rearranging it,
	//    Vs = (Vout * (R1 + R2)) / R2
	//    Vs = Vout * (R1+R2)/R2
	// for our purposes, (R1+R2)/R2 is a constant 5.1. Hence,
	// we can simply multiply Vout by 10 and divide by 51.
	//
	// Additionally, there seems to be a constant offset. It's unclear what's causing this.
	// Perhaps the bottom of the ADC range isn't 0 mV, but 50 mV.
	voltages_mv[voltage_index] = (voltage_reading * 51) / 10 + ADC_CONSTANT_OFFSET;
}

void adc_task(void *ignored)
{
	(void)ignored;

	if (CONFIG_ADC_UNIT == -1) {
		ESP_LOGI(TAG, "adc is disabled");
		vTaskDelete(NULL);
		return;
	}

	ESP_LOGI(TAG, "ADC task started");
	if (!adc_init()) {
		ESP_LOGE(TAG, "ADC init failed");
		vTaskDelete(NULL);
		return;
	}

#if SOC_TEMP_SENSOR_SUPPORTED
	ESP_ERROR_CHECK(temperature_sensor_install(&temp_sensor_config, &temp_sensor));
	ESP_ERROR_CHECK(temperature_sensor_enable(temp_sensor));
#endif

	if (CONFIG_ADC_SYSTEM_CHANNEL >= 0) {
		channel_init(ADC_SYSTEM_VOLTAGE);
	}
	if (CONFIG_ADC_VREF_CHANNEL >= 0) {
		channel_init(ADC_VREF_VOLTAGE);
	}
	if (CONFIG_ADC_USB_CHANNEL >= 0) {
		channel_init(ADC_USB_VOLTAGE);
	}
	if (CONFIG_ADC_EXT_CHANNEL >= 0) {
		channel_init(ADC_EXT_VOLTAGE);
	}
	if (CONFIG_ADC_DEBUG_CHANNEL >= 0) {
		channel_init(ADC_DEBUG_VOLTAGE);
	}

	while (true) {
		if (CONFIG_ADC_SYSTEM_CHANNEL >= 0) {
			update_voltage(ADC_SYSTEM_VOLTAGE);
		}
		if (CONFIG_ADC_VREF_CHANNEL >= 0) {
			update_voltage(ADC_VREF_VOLTAGE);
		}
		if (CONFIG_ADC_USB_CHANNEL >= 0) {
			update_voltage(ADC_USB_VOLTAGE);
		}
		if (CONFIG_ADC_EXT_CHANNEL >= 0) {
			update_voltage(ADC_EXT_VOLTAGE);
		}
		if (CONFIG_ADC_DEBUG_CHANNEL >= 0) {
			update_voltage(ADC_DEBUG_VOLTAGE);
		}

		// TODO: The docs say that " It is also possible to measure internal signals, such as VDD33."
		// However, they do not elaborate on this statement. It's probably the case that VDD33 is
		// available as a separate signal, however that isn't stated anywhere.
		voltages_mv[ADC_CORE_VOLTAGE] = 3270;

#if SOC_TEMP_SENSOR_SUPPORTED
		ESP_ERROR_CHECK(temperature_sensor_get_celsius(temp_sensor, &temperature));
#endif

		vTaskDelay(ADC_POLL_RATE_MS / portTICK_PERIOD_MS);
	}
}
