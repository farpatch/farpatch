#include <freertos/FreeRTOS.h>
#include "esp_attr.h"

// Insert a sleep statement at least once every 1.5 seconds in order
// to ensure non-critical tasks can run.
void IRAM_ATTR platform_maybe_delay(void)
{
	static TickType_t last_sleep = 0;
	static TickType_t current_sleep = 0;

	current_sleep = xTaskGetTickCount();
	if ((current_sleep - last_sleep) > pdMS_TO_TICKS(3500)) {
		last_sleep = current_sleep;
		vTaskDelay(2);
	}
}
