#include <freertos/FreeRTOS.h>
#include "esp_attr.h"

// Insert a sleep statement 500 ms before the WDT timeout starts,
// to ensure non-critical tasks can run.
void IRAM_ATTR platform_maybe_delay(void)
{
	static TickType_t last_sleep = 0;
	static TickType_t current_sleep = 0;

	current_sleep = xTaskGetTickCount();
	if ((current_sleep - last_sleep) > pdMS_TO_TICKS((CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000) - 500)) {
		last_sleep = current_sleep;
		vTaskDelay(2);
	}
}
