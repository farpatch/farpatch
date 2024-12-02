#include <driver/uart.h>
#include <esp_attr.h>
#include <esp_http_server.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include "exception.h"
#include "farpatch_adc.h"
#include "general.h"
#include <hashmap.h>
#include <inttypes.h>
#include <nvs_flash.h>
#include <string.h>
#include <stdio.h>
#include "swo.h"
#include "target_internal.h"
#include "version.h"
#include "wilma/wilma.h"

static const char *TAG = "http_api";

const static char http_content_type_json[] = "application/json";
const static char http_cache_control_hdr[] = "Cache-Control";
const static char http_cache_control_no_cache[] = "no-store, no-cache, must-revalidate, max-age=0";
const static char http_pragma_hdr[] = "Pragma";
const static char http_pragma_no_cache[] = "no-cache";
const static char http_connection_hdr[] = "Connection";
const static char http_connection_close[] = "Close";

static const char *ESP_RESET_REASONS[] = {
	"ESP_RST_UNKNOWN",
	"ESP_RST_POWERON",
	"ESP_RST_EXT",
	"ESP_RST_SW",
	"ESP_RST_PANIC",
	"ESP_RST_INT_WDT",
	"ESP_RST_TASK_WDT",
	"ESP_RST_WDT",
	"ESP_RST_DEEPSLEEP",
	"ESP_RST_BROWNOUT",
	"ESP_RST_SDIO",
	"ESP_RST_USB",
	"ESP_RST_JTAG",
};

static const char *ESP_RESET_DESCRIPTIONS[] = {
	"Reset reason can not be determined",
	"Reset due to power-on event",
	"Reset by external pin (not applicable for ESP32)",
	"Software reset via esp_restart",
	"Software reset due to exception/panic",
	"Reset (software or hardware) due to interrupt watchdog",
	"Reset due to task watchdog",
	"Reset due to other watchdogs",
	"Reset after exiting deep sleep mode",
	"Brownout reset (software or hardware)",
	"Reset over SDIO",
	"Reset by USB peripheral",
	"Reset by JTAG",
};

struct output_context {
	char *buffer;
	size_t buffer_size;
	int first;
};

static esp_err_t http_resp_empty_json(httpd_req_t *req)
{
	httpd_resp_set_type(req, http_content_type_json);
	httpd_resp_set_hdr(req, http_cache_control_hdr, http_cache_control_no_cache);
	httpd_resp_set_hdr(req, http_pragma_hdr, http_pragma_no_cache);
	httpd_resp_set_hdr(req, http_connection_hdr, http_connection_close);
	httpd_resp_send(req, "{}", 2);

	return ESP_OK;
}

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
static int task_status_cmp(const void *a, const void *b)
{
	TaskStatus_t *ta = (TaskStatus_t *)a;
	TaskStatus_t *tb = (TaskStatus_t *)b;
	return ta->xTaskNumber - tb->xTaskNumber;
}
#endif

#if CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
static const char *core_str(int core_id)
{
	switch (core_id) {
	case 0:
		return "0";
	case 1:
		return "1";
	case 2147483647:
		return "ANY";
	case -1:
		return "any";
	default:
		return "???";
	}
}
#endif

static const char *const task_state_name[] = {
	"eRunning", /* A task is querying the state of itself, so must be running. */
	"eReady",   /* The task being queried is in a read or pending ready list. */
	"eBlocked", /* The task being queried is in the Blocked state. */
	"eSuspended", /* The task being queried is in the Suspended state, or is in the Blocked state with an infinite time out. */
	"eDeleted", /* The task being queried has been deleted, but its TCB has not yet been freed. */
	"eInvalid"  /* Used as an 'invalid state' value. */
};

static void append_tasks_to_output(httpd_req_t *req)
{
	TaskStatus_t *pxTaskStatusArray;
	int i;
	int uxArraySize;
	uint32_t totalRuntime;

	static hashmap *task_times;
	if (!task_times) {
		task_times = hashmap_new();
	}

	httpd_resp_sendstr_chunk(req, ",\"tasks\":[");

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
	uxArraySize = uxTaskGetNumberOfTasks();
	pxTaskStatusArray = malloc(uxArraySize * sizeof(TaskStatus_t));
	uxArraySize = uxTaskGetSystemState(pxTaskStatusArray, uxArraySize, &totalRuntime);
	qsort(pxTaskStatusArray, uxArraySize, sizeof(TaskStatus_t), task_status_cmp);
#else
	pxTaskStatusArray = NULL;
	uxArraySize = 0;
	totalRuntime = 0;
#endif

	for (i = 0; (pxTaskStatusArray != NULL) && (i < uxArraySize); i++) {
		int len;
		char buff[256];
		TaskStatus_t *tsk = &pxTaskStatusArray[i];

		uint32_t last_task_time = tsk->ulRunTimeCounter;
		hashmap_get(task_times, tsk->xTaskNumber, &last_task_time);
		hashmap_set(task_times, tsk->xTaskNumber, tsk->ulRunTimeCounter);
		tsk->ulRunTimeCounter -= last_task_time;

		len = snprintf(buff, sizeof(buff),
			"{\"id\":%u,\"name\":\"%s\",\"prio\":%d,\"state\":\"%s\",\"stack_hwm\":%" PRIu32 ","
#if CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
			"\"core\":\"%s\","
#endif
			"\"cpu\":%" PRId32 ", \"pc\":%" PRId32 "}%c",
			tsk->xTaskNumber, tsk->pcTaskName, tsk->uxCurrentPriority, task_state_name[tsk->eCurrentState],
			tsk->usStackHighWaterMark,
#if CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
			core_str((int)tsk->xCoreID),
#endif
			tsk->ulRunTimeCounter / totalRuntime, (*((uint32_t **)tsk->xHandle))[1], i == uxArraySize - 1 ? ' ' : ',');
		httpd_resp_send_chunk(req, buff, len);
	}
	httpd_resp_sendstr_chunk(req, "]");

	if (pxTaskStatusArray != NULL) {
		free(pxTaskStatusArray);
	}
}

static void append_ota_status(httpd_req_t *req)
{
	char buffer[256];
	const esp_partition_t *current_partition = esp_ota_get_running_partition();
	const esp_partition_t *next_partition = NULL;
	if (current_partition != NULL) {
		next_partition = esp_ota_get_next_update_partition(current_partition);
	}
	esp_ota_img_states_t current_partition_state = ESP_OTA_IMG_UNDEFINED;
	esp_ota_img_states_t next_partition_state = ESP_OTA_IMG_UNDEFINED;
	uint32_t next_partition_address = 0;

	esp_ota_get_state_partition(current_partition, &current_partition_state);
	esp_ota_get_state_partition(next_partition, &next_partition_state);

	if (next_partition != NULL) {
		next_partition_address = next_partition->address;
	}
	const char *update_status = "valid";
	if (next_partition_state != ESP_OTA_IMG_VALID) {
		update_status = "fail";
	}

	snprintf(buffer, sizeof(buffer),
		",\"ota\":{"
		"\"current\":{"
		"\"address\":%" PRId32 ","
		"\"state\":%d"
		"},"
		"\"next\":{"
		"\"address\":%" PRId32 ","
		"\"state\":%d"
		"},"
		"\"status\":\"%s\""
		"}",
		current_partition->address, current_partition_state, next_partition_address, next_partition_state,
		update_status);
	httpd_resp_sendstr_chunk(req, buffer);
}

static void append_reset_reason(httpd_req_t *req)
{
	char buffer[256];
	snprintf(buffer, sizeof(buffer),
		",\"reset\":{"
		"\"code\":%d,"
		"\"reason\":\"%s\","
		"\"description\":\"%s\""
		"}",
		esp_reset_reason(), ESP_RESET_REASONS[esp_reset_reason()], ESP_RESET_DESCRIPTIONS[esp_reset_reason()]);
	httpd_resp_sendstr_chunk(req, buffer);
}

static void append_networking_to_output(httpd_req_t *req)
{
	char buffer[512];
	esp_netif_ip_info_t ip_info = wilma_get_ip_info();
	char ip[IP4ADDR_STRLEN_MAX]; /* note: IP4ADDR_STRLEN_MAX is defined in lwip */
	char gw[IP4ADDR_STRLEN_MAX];
	char netmask[IP4ADDR_STRLEN_MAX];

	esp_ip4addr_ntoa(&ip_info.ip, ip, IP4ADDR_STRLEN_MAX);
	esp_ip4addr_ntoa(&ip_info.gw, gw, IP4ADDR_STRLEN_MAX);
	esp_ip4addr_ntoa(&ip_info.netmask, netmask, IP4ADDR_STRLEN_MAX);

	char generated_hostname[32];
	const char *name1;
	const char *name2;
	wilma_unique_words(&name1, &name2);
	snprintf(generated_hostname, sizeof(generated_hostname) - 1, CONFIG_PRODUCT_NAME "-%s-%s.local", name1, name2);
	snprintf(buffer, sizeof(buffer) - 1,
		",\"networking\": {"
		"\"hostname\": \"%s\","
		"\"ip\":\"%s\","
		"\"netmask\":\"%s\","
		"\"gw\":\"%s\","
		"\"ssid\":\"%s\","
		"\"gdb\": %d,"
		"\"rtt-tcp\": %d,"
		"\"rtt-count\": %d,"
		"\"rtt-udp\": %d,"
		"\"uart-tcp\": %d,"
		"\"uart-udp\": %d,"
		"\"tftp\": \"farpatch.bin\""
		"}",
		generated_hostname, ip, netmask, gw, wilma_current_ssid(), CONFIG_GDB_TCP_PORT, CONFIG_RTT_TCP_PORT,
		CONFIG_RTT_MAX_CHANNELS, CONFIG_RTT_UDP_PORT, CONFIG_UART_TCP_PORT, CONFIG_UART_UDP_PORT);
	httpd_resp_sendstr_chunk(req, buffer);
}

static void append_target_to_output(size_t idx, target_s *target, void *context)
{
	(void)idx;
	struct output_context *ctx = (struct output_context *)context;

	if (ctx->buffer_size == 0) {
		return;
	}

	const char *const attached = target->attached ? "true" : "false";
	const char *const core_name = target->core;
	// Append a comma if this isn't the first target
	if (ctx->first) {
		ctx->first = 0;
	} else {
		ctx->buffer[0] = ',';
		ctx->buffer += 1;
		ctx->buffer[0] = '\0';
		ctx->buffer_size -= 1;
	}
	if (ctx->buffer_size == 0) {
		return;
	}

	int len;
	ESP_LOGI(TAG, "Driver name: %s", target->driver);
	if (!strcmp(target->driver, "ARM Cortex-M")) {
		len = snprintf(ctx->buffer, ctx->buffer_size - 1,
			"{\"attached\":%s,\"driver\":\"%s\",\"designer\":%d,\"part_id\":%d,\"core_name\":\"%s\"}", attached,
			target->driver, target->designer_code, target->part_id, core_name ? core_name : "");
	} else {
		len = snprintf(ctx->buffer, ctx->buffer_size - 1, "{\"attached\":%s,\"driver\":\"%s\",\"core_name\":\"%s\"}",
			attached, target->driver, core_name ? core_name : "");
	}
	ctx->buffer += len;
	ctx->buffer_size -= len;
}

static void append_version_to_output(httpd_req_t *req)
{
	char buffer[256];
	// Open the JSON tag and send version information
	snprintf(buffer, sizeof(buffer) - 1,
		",\"version\":{"
		"\"farpatch\":\"" FARPATCH_VERSION "\", "
		"\"esp-idf\":\"%s\","
		"\"bmp\":\"" BMP_VERSION "\","
		"\"build-time\":\"" BUILD_TIMESTAMP "\","
		"\"hardware\":\"" HARDWARE_VERSION "\""
		"}",
		esp_get_idf_version());
	httpd_resp_sendstr_chunk(req, buffer);
}

static void append_sysinfo_to_output(httpd_req_t *req)
{
	char buffer[256];
	// System information
	snprintf(buffer, sizeof(buffer) - 1,
		",\"system\": {"
		"\"heap\": %" PRIu32 ","
		"\"uptime\": %" PRIu32 "}",
		esp_get_free_heap_size(), xTaskGetTickCount() * portTICK_PERIOD_MS);
	httpd_resp_sendstr_chunk(req, buffer);
}

static void append_voltages_to_output(httpd_req_t *req, const char *prefix, const char *suffix)
{
	char buffer[256];
	// Voltages
	httpd_resp_sendstr_chunk(req, prefix);
	snprintf(buffer, sizeof(buffer) - 1,
#if SOC_TEMP_SENSOR_SUPPORTED
		"\"temperature\": %f,"
#endif
#if defined(CONFIG_ADC_VREF_CHANNEL) && CONFIG_ADC_VREF_CHANNEL >= 0
		"\"target\": %" PRIu32 ","
#endif
#if defined(CONFIG_ADC_USB_CHANNEL) && CONFIG_ADC_USB_CHANNEL >= 0
		"\"usb\": %" PRIu32 ","
#endif
#if defined(CONFIG_ADC_DEBUG_CHANNEL) && CONFIG_ADC_DEBUG_CHANNEL >= 0
		"\"debug\": %" PRIu32 ","
#endif
#if defined(CONFIG_ADC_EXT_CHANNEL) && CONFIG_ADC_EXT_CHANNEL >= 0
		"\"ext\": %" PRIu32 ","
#endif
#if defined(CONFIG_ADC_SYSTEM_CHANNEL) && CONFIG_ADC_SYSTEM_CHANNEL >= 0
		"\"system\": %" PRIu32 ","
#endif
		"\"core\": %" PRIu32 "",
#if SOC_TEMP_SENSOR_SUPPORTED
		temperature,
#endif
#if defined(CONFIG_ADC_VREF_CHANNEL) && CONFIG_ADC_VREF_CHANNEL >= 0
		voltages_mv[ADC_VREF_VOLTAGE],
#endif
#if defined(CONFIG_ADC_USB_CHANNEL) && CONFIG_ADC_USB_CHANNEL >= 0
		voltages_mv[ADC_USB_VOLTAGE],
#endif
#if defined(CONFIG_ADC_DEBUG_CHANNEL) && CONFIG_ADC_DEBUG_CHANNEL >= 0
		voltages_mv[ADC_DEBUG_VOLTAGE],
#endif
#if defined(CONFIG_ADC_EXT_CHANNEL) && CONFIG_ADC_EXT_CHANNEL >= 0
		voltages_mv[ADC_EXT_VOLTAGE],
#endif
#if defined(CONFIG_ADC_SYSTEM_CHANNEL) && CONFIG_ADC_SYSTEM_CHANNEL >= 0
		voltages_mv[ADC_SYSTEM_VOLTAGE],
#endif
		voltages_mv[ADC_CORE_VOLTAGE]);
	httpd_resp_sendstr_chunk(req, buffer);
	httpd_resp_sendstr_chunk(req, suffix);
}

static void append_targets_to_output(httpd_req_t *req)
{
	char buffer[256];
	// Targets
	snprintf(buffer, sizeof(buffer) - 1,
		",\"targets\": {"
		"\"available\":[");
	bmp_core_lock();
	{
		struct output_context ctx = {
			.buffer = buffer + strlen(buffer),
			.buffer_size = sizeof(buffer) - strlen(buffer),
			.first = 1,
		};
		target_foreach(append_target_to_output, &ctx);
	}
	bmp_core_unlock();
	snprintf(buffer + strlen(buffer), sizeof(buffer) - strlen(buffer) - 1, "]}");
	httpd_resp_sendstr_chunk(req, buffer);
}

static void append_uart_to_output(httpd_req_t *req)
{
	char buffer[256];
	// UART status
	uint32_t target_baud = 0;
	uint32_t swo_baud = 0;
	uart_get_baudrate(TARGET_UART_IDX, &target_baud);
	if (swo_current_mode == swo_nrz_uart) {
		uart_get_baudrate(SWO_UART_IDX, &swo_baud);
	}
	snprintf(buffer, sizeof(buffer) - 1,
		",\"ports\": {\"target\": {\"baudrate\": %" PRIu32 "}, \"swo\": {\"baudrate\": %" PRIu32 "}}", target_baud,
		swo_baud);
	httpd_resp_sendstr_chunk(req, buffer);
}

esp_err_t cgi_status(httpd_req_t *req)
{
	httpd_resp_set_type(req, http_content_type_json);
	httpd_resp_set_hdr(req, http_cache_control_hdr, http_cache_control_no_cache);
	httpd_resp_set_hdr(req, http_pragma_hdr, http_pragma_no_cache);
	httpd_resp_set_hdr(req, http_connection_hdr, http_connection_close);

	httpd_resp_sendstr_chunk(req, "{\"status\":\"ok\"");
	append_version_to_output(req);
	append_sysinfo_to_output(req);
	append_voltages_to_output(req, ",\"voltages\": {", "}");
	append_networking_to_output(req);
	append_targets_to_output(req);
	append_uart_to_output(req);
	append_tasks_to_output(req);
	append_reset_reason(req);
	append_ota_status(req);

	// Close the JSON tag
	httpd_resp_sendstr_chunk(req, "}");

	// Close the connection
	httpd_resp_sendstr_chunk(req, NULL);

	return ESP_OK;
}

esp_err_t cgi_voltages(httpd_req_t *req)
{
	append_voltages_to_output(req, "{", "}");
	httpd_resp_sendstr_chunk(req, NULL);
	return ESP_OK;
}

esp_err_t cgi_targets(httpd_req_t *req)
{
	httpd_resp_sendstr(req, "{\"targets\":{\"current\":{\"name\":\"nrf52840\",\"ram\":262144,\"flash\":524188,\"cpu\":"
							"\"ARM Cortex-M4F\"},\"available\":[\"nrf52840\",\"mdf\"]}}");
	return ESP_OK;
}

static void maybe_init_exceptions(void)
{
	static int initialized = 0;
	static void *tls[2] = {};
	if (initialized) {
		return;
	}
	vTaskSetThreadLocalStoragePointer(NULL, 1 /* GDB_TLS_INDEX */, tls); // used for exception handling
	initialized = 1;
}

esp_err_t cgi_scan_jtag(httpd_req_t *req)
{
	maybe_init_exceptions();
	bmp_core_lock();
	TRY(EXCEPTION_ALL)
	{
		jtag_scan();
	}
	CATCH()
	{
	case EXCEPTION_TIMEOUT:
		ESP_LOGE(TAG, "Timeout during scan. Is target stuck in WFI?\n");
		break;
	case EXCEPTION_ERROR:
		ESP_LOGE(TAG, "Exception: %s\n", exception_frame.msg);
		break;
	}
	bmp_core_unlock();

	return http_resp_empty_json(req);
}

esp_err_t cgi_scan_swd(httpd_req_t *req)
{
	maybe_init_exceptions();
	bmp_core_lock();
	TRY(EXCEPTION_ALL)
	{
		adiv5_swd_scan(0);
	}
	CATCH()
	{
	case EXCEPTION_TIMEOUT:
		ESP_LOGE(TAG, "Timeout during scan. Is target stuck in WFI?\n");
		break;
	case EXCEPTION_ERROR:
		ESP_LOGE(TAG, "Exception: %s\n", exception_frame.msg);
		break;
	}
	bmp_core_unlock();

	return http_resp_empty_json(req);
}

esp_err_t cgi_storage_delete(httpd_req_t *req)
{
	switch (req->method) {
	case HTTP_DELETE:
		http_resp_empty_json(req);
		break;
	default:
		return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "request type not allowed");
	}

	ESP_LOGI(TAG, "erasing NVS");
	ESP_ERROR_CHECK(nvs_flash_erase());
	ESP_ERROR_CHECK(nvs_flash_init());
	ESP_LOGI(TAG, "preparing to reboot in 500ms...");
	vTaskDelay(pdMS_TO_TICKS(500));
	ESP_LOGI(TAG, "initiating a reboot");
	esp_restart();

	return ESP_OK;
}
