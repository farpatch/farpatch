#include "exception.h"
#include <stdlib.h>
#include "general.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esp_log.h"

void raise_exception(const uint32_t type, const char *const msg)
{
	ESP_LOGW("EX", "Exception: %s", msg);
	for (exception_s *exception = innermost_exception; exception; exception = exception->outer) {
		if (exception->mask & type) {
			exception->type = type;
			exception->msg = msg;
			innermost_exception = exception->outer;
			longjmp(exception->jmpbuf, type);
		}
	}
	ESP_LOGE("EX", "Unhandled exception %" PRId32 ": %s", type, msg);

	abort();
}
