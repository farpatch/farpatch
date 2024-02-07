#ifndef FARPATCH_ADC_H__
#define FARPATCH_ADC_H__

#include <stdint.h>

enum ADC_VOLTAGES {
	ADC_SYSTEM_VOLTAGE,
	ADC_TARGET_VOLTAGE,
	ADC_USB_VOLTAGE,
	ADC_EXT_VOLTAGE,
	ADC_DEBUG_VOLTAGE,
	ADC_VOLTAGE_COUNT,
};

extern int32_t voltages_mv[ADC_VOLTAGE_COUNT];
void adc_task(void *ignored);

#endif /* FARPATCH_ADC_H__ */
