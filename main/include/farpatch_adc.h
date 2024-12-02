#ifndef FARPATCH_ADC_H__
#define FARPATCH_ADC_H__

#include <stdint.h>

enum AdcVoltageChannel {
	ADC_SYSTEM_VOLTAGE,
	ADC_VREF_VOLTAGE,
	ADC_USB_VOLTAGE,
	ADC_EXT_VOLTAGE,
	ADC_DEBUG_VOLTAGE,
	ADC_CORE_VOLTAGE,
	ADC_VOLTAGE_COUNT,
};

extern int32_t voltages_mv[ADC_VOLTAGE_COUNT];
void adc_task(void *ignored);

#if SOC_TEMP_SENSOR_SUPPORTED
extern float temperature;
#endif

#endif /* FARPATCH_ADC_H__ */
