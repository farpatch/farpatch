#include <esp_attr.h>
#include <freertos/FreeRTOS.h>
#include "sdkconfig.h"

// Insert a sleep statement 500 ms before the WDT timeout starts,
// to ensure non-critical tasks can run.
void IRAM_ATTR platform_maybe_delay(void)
{
#if CONFIG_ESP_TASK_WDT_INIT
	static TickType_t last_sleep = 0;
	TickType_t current_sleep = xTaskGetTickCount();

	if ((current_sleep - last_sleep) > pdMS_TO_TICKS((CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000) - 500)) {
		last_sleep = current_sleep;
		vTaskDelay(1);
	}
#endif
}
