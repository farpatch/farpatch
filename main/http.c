#include <string.h>
#include <stdio.h>
// typedef uint8_t uint8;

#include "frogfs/frogfs.h"
#include <esp_http_server.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <freertos/list.h>
#include "platform.h"
#include "hashmap.h"
#include "http_api.h"
#include "ota-http.h"
#include "farpatch_adc.h"
#include "swo.h"
#include "websocket.h"
#include "wifi.h"
#include "driver/uart.h"
#include "uart.h"
#include "version.h"

#include "esp_attr.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

const static char http_cache_control_hdr[] = "Cache-Control";
const static char http_cache_control_no_cache[] = "no-store, no-cache, must-revalidate, max-age=0";
const static char http_content_type_json[] = "application/json";
const static char http_pragma_hdr[] = "Pragma";
const static char http_pragma_no_cache[] = "no-cache";

extern const uint8_t frogfs_bin[];
extern const size_t frogfs_bin_len;
extern void platform_set_baud(uint32_t);
static frogfs_fs_t *frog_fs;
httpd_handle_t http_daemon;
extern esp_err_t cgi_rtt_status(httpd_req_t *req);

#define TAG "httpd"

esp_err_t cgi_uart_break(httpd_req_t *req)
{
	void uart_send_break();
	uart_send_break();

	httpd_resp_set_type(req, http_content_type_json);
	httpd_resp_send(req, "{}", 2);

	return ESP_OK;
}

static esp_err_t cgi_baud(httpd_req_t *req)
{
	int len;
	char buff[24];
	char querystring[64];

	httpd_req_get_url_query_str(req, querystring, sizeof(querystring));
	if (ESP_OK == httpd_query_key_value(querystring, "set", buff, sizeof(buff))) {
		int baud = atoi(buff);
		// printf("baud %d\n", baud);
		if (baud) {
			platform_set_baud(baud);
		}
	}

	uint32_t baud = 0;
	uart_get_baudrate(TARGET_UART_IDX, &baud);

	len = snprintf(buff, sizeof(buff), "{\"baudrate\": %lu }", baud);
	httpd_resp_set_type(req, http_content_type_json);
	httpd_resp_send(req, buff, len);

	return ESP_OK;
}

extern uint32_t uart_overrun_cnt;
extern uint32_t uart_frame_error_cnt;
extern uint32_t uart_queue_full_cnt;
extern uint32_t uart_rx_count;
extern uint32_t uart_tx_count;
extern uint32_t uart_irq_count;
extern uint32_t uart_rx_data_relay;

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
static int task_status_cmp(const void *a, const void *b)
{
	TaskStatus_t *ta = (TaskStatus_t *)a;
	TaskStatus_t *tb = (TaskStatus_t *)b;
	return ta->xTaskNumber - tb->xTaskNumber;
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

static esp_err_t cgi_system_status_header(httpd_req_t *req)
{
	char buffer[256];

	uint32_t target_baud = 0;
	uint32_t swo_baud = 0;
	uart_get_baudrate(TARGET_UART_IDX, &target_baud);
	if (swo_current_mode == swo_nrz_uart) {
		uart_get_baudrate(SWO_UART_IDX, &swo_baud);
	}

	snprintf(buffer, sizeof(buffer),
		"free_heap: %" PRIu32 "\n"
		"uptime: %" PRIu32 "\n",
		esp_get_free_heap_size(), xTaskGetTickCount() * portTICK_PERIOD_MS);
	httpd_resp_sendstr_chunk(req, buffer);

	snprintf(buffer, sizeof(buffer), "reset_reason: %d %s -- %s\n", esp_reset_reason(),
		ESP_RESET_REASONS[esp_reset_reason()], ESP_RESET_DESCRIPTIONS[esp_reset_reason()]);
	httpd_resp_sendstr_chunk(req, buffer);

	snprintf(buffer, sizeof(buffer),
		"target_baud_rate: %" PRIu32 "\n"
		"swo_baud_rate: %" PRIu32 "\n",
		target_baud, swo_baud);
	httpd_resp_sendstr_chunk(req, buffer);

	snprintf(buffer, sizeof(buffer), "target voltage: %" PRIu32 " mV\n", voltages_mv[ADC_VREF_VOLTAGE]);
	httpd_resp_sendstr_chunk(req, buffer);

	snprintf(buffer, sizeof(buffer),
		"uart_overruns: %" PRIu32 "\n"
		"uart_frame_errors: %" PRIu32 "\n"
		"uart_queue_full_cnt: %" PRIu32 "\n"
		"uart_rx_count: %" PRIu32 "\n"
		"uart_tx_count: %" PRIu32 "\n"
		"uart_irq_count: %" PRIu32 "\n"
		"uart_rx_data_relay: %" PRIu32 "\n",
		uart_overrun_cnt, uart_frame_error_cnt, uart_queue_full_cnt, uart_rx_count, uart_tx_count, uart_irq_count,
		uart_rx_data_relay);
	httpd_resp_sendstr_chunk(req, buffer);

	const esp_partition_t *current_partition = esp_ota_get_running_partition();
	const esp_partition_t *next_partition = NULL;
	if (current_partition != NULL) {
		next_partition = esp_ota_get_next_update_partition(current_partition);
	}
	esp_ota_img_states_t current_partition_state = ESP_OTA_IMG_UNDEFINED;
	esp_ota_img_states_t next_partition_state = ESP_OTA_IMG_UNDEFINED;
	uint32_t next_partition_address = 0;

	esp_ota_get_state_partition(current_partition, &current_partition_state);
	// if (ret != ESP_OK) {
	// 	ESP_LOGE(__func__, "unable to get current partition state: %08x", ret);
	// }

	esp_ota_get_state_partition(next_partition, &next_partition_state);
	// if (ret != ESP_OK) {
	// 	ESP_LOGE(__func__, "unable to get next partition state: %08x", ret);
	// }

	if (next_partition != NULL) {
		next_partition_address = next_partition->address;
	}
	const char *update_status = "update valid\n";
	if (next_partition_state != ESP_OTA_IMG_VALID) {
		update_status = "UPDATE FAILED\n";
	}

	snprintf(buffer, sizeof(buffer),
		"current partition: 0x%08" PRIx32 " %d\n"
		"next partition: 0x%08" PRIx32 " %d\n"
		"%s",
		current_partition->address, current_partition_state, next_partition_address, next_partition_state,
		update_status);
	httpd_resp_sendstr_chunk(req, buffer);

	httpd_resp_sendstr_chunk(req, "tasks:\n");

	return ESP_OK;
}

static esp_err_t cgi_system_status(httpd_req_t *req)
{
	TaskStatus_t *pxTaskStatusArray;
	int i;
	int uxArraySize;
	uint32_t totalRuntime;

	static hashmap *task_times;
	if (!task_times) {
		task_times = hashmap_new();
	}

	httpd_resp_set_type(req, "text/plain");
	httpd_resp_set_hdr(req, http_cache_control_hdr, http_cache_control_no_cache);
	httpd_resp_set_hdr(req, http_pragma_hdr, http_pragma_no_cache);
	httpd_resp_set_hdr(req, "Refresh", "1");

	cgi_system_status_header(req);

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
	i = 0;

	uint32_t tmp;
	static uint32_t lastTotalRuntime;
	/* Generate the (binary) data. */
	tmp = totalRuntime;
	totalRuntime = totalRuntime - lastTotalRuntime;
	lastTotalRuntime = tmp;
	totalRuntime /= 100;
	if (totalRuntime == 0) {
		totalRuntime = 1;
	}

	for (i = 0; (pxTaskStatusArray != NULL) && (i < uxArraySize); i++) {
		int len;
		char buff[256];
		TaskStatus_t *tsk = &pxTaskStatusArray[i];

		uint32_t last_task_time = tsk->ulRunTimeCounter;
		hashmap_get(task_times, tsk->xTaskNumber, &last_task_time);
		hashmap_set(task_times, tsk->xTaskNumber, tsk->ulRunTimeCounter);
		tsk->ulRunTimeCounter -= last_task_time;

		len = snprintf(buff, sizeof(buff),
			"\tid: %3u, name: %16s, prio: %3d, state: %10s, stack_hwm: %5" PRIu32 ", "
#if CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
			"core: %3s, "
#endif
			"cpu: %3" PRId32 "%%, pc: 0x%08" PRIx32 "\n",
			tsk->xTaskNumber, tsk->pcTaskName, tsk->uxCurrentPriority, task_state_name[tsk->eCurrentState],
			tsk->usStackHighWaterMark,
#if CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
			core_str((int)tsk->xCoreID),
#endif
			tsk->ulRunTimeCounter / totalRuntime, (*((uint32_t **)tsk->xHandle))[1]);
		httpd_resp_send_chunk(req, buff, len);
	}

	if (pxTaskStatusArray != NULL) {
		free(pxTaskStatusArray);
	}

	// ESP_LOGI(__func__, "finishing connection");
	httpd_resp_sendstr_chunk(req, NULL);
	return ESP_OK;
}

// Use this as a cgi function to redirect one url to another.
static esp_err_t cgi_redirect(httpd_req_t *req)
{
	httpd_resp_set_type(req, "text/html");
	httpd_resp_set_status(req, "302 Moved");
	httpd_resp_set_hdr(req, "Location", (const char *)req->user_ctx);
	httpd_resp_send(req, NULL, 0);
	return ESP_OK;
}

// Struct to keep extension->mime data in
typedef struct {
	const char *ext;
	const char *mimetype;
} MimeMap;

// The mappings from file extensions to mime types. If you need an extra mime type,
// add it here.
static const MimeMap mimeTypes[] = {
	{"htm", "text/html"}, {"html", "text/html"}, {"css", "text/css"}, {"js", "text/javascript"}, {"txt", "text/plain"},
	{"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"png", "image/png"}, {"svg", "image/svg+xml"}, {"xml", "text/xml"},
	{"json", "application/json"}, {"ico", "image/x-icon"}, {NULL, "text/html"}, // default value
};

// Returns a static char* to a mime type for a given url to a file.
const char *frogfs_get_mime_type(const char *url)
{
	char *urlp = (char *)url;
	int i = 0;
	// Go find the extension
	const char *ext = urlp + (strlen(urlp) - 1);
	while (ext != urlp && *ext != '.')
		ext--;
	if (*ext == '.')
		ext++;

	while (mimeTypes[i].ext != NULL && strcasecmp(ext, mimeTypes[i].ext) != 0)
		i++;
	return mimeTypes[i].mimetype;
}

static bool frogfs_is_gzip(frogfs_file_t *file)
{
	frogfs_stat_t st;
	frogfs_fstat(file, &st);
	return (st.flags & FROGFS_FLAG_GZIP) != 0;
}

static esp_err_t cgi_frog_fs_hook(httpd_req_t *req)
{
	char chunk[525];
	int chunk_bytes;

	ESP_LOGI(__func__, "uri: %s", req->uri);
	bool is_gzip;
	// Temporarily reuse the chunk buffer to store the URI,
	// minus any extraneous bits
	memset(chunk, 0, sizeof(chunk));
	strncpy(chunk, req->uri, sizeof(chunk));
	char *param;
	param = (char *)strstr(chunk, "?");
	if (param != NULL) {
		/* separate uri from parameters for now, set back later */
		*param = 0;
	}

	frogfs_file_t *file = frogfs_fopen(frog_fs, chunk);
	httpd_resp_set_hdr(req, "Connection", "Close");

	if (file != NULL) {
		httpd_resp_set_type(req, frogfs_get_mime_type(req->uri));
	} else {
		size_t uri_len = strlen(req->uri);
		// If the URI ends in a `/`, don't add an extra one.
		if (req->uri[uri_len - 1] == '/') {
			snprintf(chunk, sizeof(chunk) - 1, "%sindex.html", req->uri);
		} else {
			snprintf(chunk, sizeof(chunk) - 1, "%s/index.html", req->uri);
		}
		file = frogfs_fopen(frog_fs, chunk);

		if (file == NULL) {
			return httpd_resp_send_404(req);
		}
		httpd_resp_set_type(req, frogfs_get_mime_type(chunk));
	}

	is_gzip = frogfs_is_gzip(file);

	// Check the browser's "Accept-Encoding" header. If the client does not
	// advertise that he accepts GZIP send a warning message (telnet users for e.g.)
	{
		char accept_encoding_buffer[64];
		bool found = (httpd_req_get_hdr_value_str(
						  req, "Accept-Encoding", accept_encoding_buffer, sizeof(accept_encoding_buffer)) == ESP_OK);
		if (!found || (strstr(accept_encoding_buffer, "gzip") == NULL)) {
			// No Accept-Encoding: gzip header present
			frogfs_fclose(file);
			return httpd_resp_send_err(
				req, HTTPD_501_METHOD_NOT_IMPLEMENTED, "your browser does not support gzip-compressed data");
		}
	}

	if (is_gzip) {
		httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
	}

	// httpd_resp_send(req, "testing", strlen("testing"));

	while ((chunk_bytes = frogfs_fread(file, chunk, sizeof(chunk))) > 0) {
		httpd_resp_send_chunk(req, chunk, chunk_bytes);
	}
	// Empty chunk closes the connection
	httpd_resp_sendstr_chunk(req, NULL);
	frogfs_fclose(file);

	return ESP_OK;
}

static const httpd_uri_t basic_handlers[] = {
	// New API style
	{
		.uri = "/fp/status",
		.method = HTTP_GET,
		.handler = cgi_status,
	},
	{
		.uri = "/fp/voltages",
		.method = HTTP_GET,
		.handler = cgi_voltages,
	},
	{
		.uri = "/fp/targets",
		.method = HTTP_GET,
		.handler = cgi_targets,
	},
	{
		.uri = "/fp/scan/jtag",
		.method = HTTP_GET,
		.handler = cgi_scan_jtag,
	},
	{
		.uri = "/fp/scan/swd",
		.method = HTTP_GET,
		.handler = cgi_scan_swd,
	},
	{
		.uri = "/wifi", // Legacy
		.method = HTTP_GET,
		.handler = cgi_redirect,
		.user_ctx = (void *)"/wifi.html",
	},

	// OTA updates
	{
		.uri = "/flash", // Legacy
		.method = HTTP_GET,
		.handler = cgi_redirect,
		.user_ctx = (void *)"/flash/",
	},
	{
		.uri = "/flash/init", // Legacy
		.method = HTTP_GET,
		.handler = cgi_flash_init,
	},
	{
		.uri = "/flash/upload", // Legacy
		.method = HTTP_POST,
		.handler = cgi_flash_upload,
	},
	{
		.uri = "/flash/reboot", // Legacy
		.method = HTTP_GET,
		.handler = cgi_flash_reboot,
	},
	{
		.uri = "/fp/flash",
		.method = HTTP_GET,
		.handler = cgi_redirect,
		.user_ctx = (void *)"/flash/",
	},
	{
		.uri = "/fp/flash/init",
		.method = HTTP_GET,
		.handler = cgi_flash_init,
	},
	{
		.uri = "/fp/flash/upload",
		.method = HTTP_POST,
		.handler = cgi_flash_upload,
	},
	{
		.uri = "/fp/flash/reboot",
		.method = HTTP_GET,
		.handler = cgi_flash_reboot,
	},
	{
		.uri = "/fp/flash/progress",
		.method = HTTP_GET,
		.handler = cgi_flash_progress,
	},
	{
		.uri = "/fp/flash/status",
		.method = HTTP_GET,
		.handler = cgi_flash_status,
	},
	{
		.uri = "/fp/storage",
		.method = HTTP_DELETE,
		.handler = cgi_storage_delete,
	},

	// UART configuration
	{
		.uri = "/uart/baud", // Legacy
		.handler = cgi_baud,
		.method = HTTP_GET,
	},
	{
		.uri = "/uart/break", // Legacy
		.handler = cgi_uart_break,
		.method = HTTP_GET,
	},
	{
		.uri = "/fp/uart/baud",
		.handler = cgi_baud,
		.method = HTTP_GET,
	},
	{
		.uri = "/fp/uart/break",
		.handler = cgi_uart_break,
		.method = HTTP_GET,
	},
	{
		.uri = "/ws/uart",
		.method = HTTP_GET,
		.handler = cgi_websocket,
		.user_ctx = (void *)&uart_websocket,
		.is_websocket = true,
	},

	// Various status pages
	{
		.uri = "/status",
		.method = HTTP_GET,
		.handler = cgi_system_status,
	},
	{
		.uri = "/ws/debug",
		.method = HTTP_GET,
		.handler = cgi_websocket,
		.user_ctx = (void *)&debug_websocket,
		.is_websocket = true,
	},
	{
		.uri = "/ws/rtt",
		.method = HTTP_GET,
		.handler = cgi_websocket,
		.user_ctx = (void *)&rtt_websocket,
		.is_websocket = true,
	},
	{
		.uri = "/fp/rtt/status",
		.handler = cgi_rtt_status,
		.method = HTTP_GET,
	},

	// Wilma Manager
	{
		.uri = "/fp/sta",
		.handler = cgi_sta_scan_results_json,
		.method = HTTP_GET,
	},
	{
		.uri = "/fp/sta/status",
		.method = HTTP_GET,
		.handler = cgi_sta_status_json,
	},
	{
		.uri = "/fp/ap",
		.handler = cgi_ap_config_json,
		.method = HTTP_GET,
	},
	{
		.uri = "/fp/ap",
		.handler = cgi_ap_configure,
		.method = HTTP_POST,
	},
	{
		.uri = "/fp/sta/scan",
		.handler = cgi_sta_start_scan_json,
		.method = HTTP_GET,
	},
	{
		.uri = "/fp/sta/connect",
		.method = HTTP_POST,
		.handler = cgi_sta_connect_json,
	},
	{
		.uri = "/fp/sta/connect",
		.method = HTTP_DELETE,
		.handler = cgi_sta_connect_json,
	},
	{
		.uri = "/ap.json", // legacy
		.handler = cgi_sta_scan_results_json,
		.method = HTTP_GET,
	},
	{
		.uri = "/ap.json", // legacy
		.handler = cgi_ap_configure,
		.method = HTTP_POST,
	},
	{
		.uri = "/apscan.json", // legacy
		.handler = cgi_sta_start_scan_json,
		.method = HTTP_GET,
	},
	{
		.uri = "/connect.json", // legacy
		.method = HTTP_GET,
		.handler = cgi_sta_connect_json,
	},
	{
		.uri = "/connect.json", // legacy
		.method = HTTP_POST,
		.handler = cgi_sta_connect_json,
	},
	{
		.uri = "/connect.json", // legacy
		.method = HTTP_DELETE,
		.handler = cgi_sta_connect_json,
	},
	{
		.uri = "/status.json", // legacy
		.method = HTTP_GET,
		.handler = cgi_sta_status_json,
	},

	// Catch-all cgi function for the filesystem
	{
		.uri = "*",
		.handler = cgi_frog_fs_hook,
		.method = HTTP_GET,
	},
};

static const int basic_handlers_count = sizeof(basic_handlers) / sizeof(*basic_handlers);

httpd_handle_t webserver_start(void)
{
	int i;
	static frogfs_config_t frogfs_config = {
		.addr = frogfs_bin,
	};
	frog_fs = frogfs_init(&frogfs_config);
	assert(frog_fs != NULL);
	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.max_uri_handlers = basic_handlers_count + 5;
	config.server_port = 80;
	config.uri_match_fn = httpd_uri_match_wildcard;

	/* This check should be a part of http_server */
	config.max_open_sockets = (CONFIG_LWIP_MAX_SOCKETS - 4);

	if (httpd_start(&http_daemon, &config) != ESP_OK) {
		ESP_LOGE(TAG, "Unable to start HTTP server");
		return NULL;
	}

	for (i = 0; i < basic_handlers_count; i++) {
		httpd_register_uri_handler(http_daemon, &basic_handlers[i]);
	}

	ESP_LOGI(TAG, "Started HTTP server on port: '%d'", config.server_port);
	ESP_LOGI(TAG, "Max URI handlers: '%d'", config.max_uri_handlers);
	ESP_LOGI(TAG, "Max Open Sessions: '%d'", config.max_open_sockets);
	ESP_LOGI(TAG, "Max Header Length: '%d'", HTTPD_MAX_REQ_HDR_LEN);
	ESP_LOGI(TAG, "Max URI Length: '%d'", HTTPD_MAX_URI_LEN);
	ESP_LOGI(TAG, "Max Stack Size: '%d'", config.stack_size);

	return http_daemon;
}
