#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <lwip/ip_addr.h>
#include <nvs.h>
#include <string.h>

#include "wilma.h"

// #undef ESP_LOGD
// #define ESP_LOGD(...) ESP_LOGI(__VA_ARGS__)

void wilma_utils_init(void);
void wilma_utils_cleanup(void);
bool wilma_lock_json_buffer(TickType_t xTicksToWait);
void wilma_unlock_json_buffer(void);
int wilma_update_wifi_ssid(void *ssid);
char *wilma_reason_to_str(uint8_t reason);
static void wifi_configure_softap(bool force_on);

static esp_err_t connect_to_station_index(size_t index);

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#define ESP_WIFI_SAE_MODE                 WPA3_SAE_PWE_HUNT_AND_PECK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#define MAX_SCAN_RESULTS                  64
#define WILMA_TASK_PRIORITY               5

/// The number of stations that we will try to connect to that are visible.
/// If a station is configured but is not in range, then this number does not
/// apply.
#define WILMA_MAX_CANDIDATE_STATIONS 4

/* @brief The number of times to retry connecting to an AP before moving on to the next one */
#define RETRIES_BEFORE_CONTINUING 2

/* FreeRTOS event group to signal when we are connected*/
static const char *TAG = "wilma";
static int WILMA_RETRY_NUM = 0;

/* objects used to manipulate the main queue of events */
static QueueHandle_t WILMA_QUEUE;

/* @brief software timer to wait between each connection retry.
 * There is no point hogging a hardware timer for a functionality like this which only needs to be 'accurate enough' */
static TimerHandle_t WILMA_RETRY_TIMER = NULL;
static const TickType_t WILMA_RETRY_TIMER_MS = 10000;

static esp_netif_t *ESP_NETIF_AP;
static esp_netif_t *ESP_NETIF_STA;
static esp_event_handler_instance_t WILMA_INSTANCE_ANY_ID;
static esp_event_handler_instance_t WILMA_INSTANCE_GOT_IP;
static EventGroupHandle_t WILMA_EVENT_GROUP;

/**
 * @brief Defines the complete list of all messages that the wifi_manager can process.
 *
 * Some of these message are events ("EVENT"), and some of them are action ("ORDER")
 * Each of these messages can trigger a callback function and each callback function is stored
 * in a function pointer array for convenience. Because of this behavior, it is extremely important
 * to maintain a strict sequence and the top level special element 'MESSAGE_CODE_COUNT'
 *
 * @see wifi_manager_set_callback
 */
typedef enum WilmaMessageType {
	NONE = 0,
	// reserved = 1,
	// reserved = 2,
	// reserved = 3,
	// reserved = 4,
	WM_ORDER_START_WIFI_SCAN = 5,
	WM_ORDER_START_WIFI_SCAN_THEN_CONNECT = 6,
	WM_ORDER_CONNECT_STA = 7,
	// reserved = 8,
	// reserved = 9,
	// reserved = 10,
	// reserved = 11,
	// reserved = 12,
	WM_ORDER_STOP_AP = 13,
	WM_ORDER_FORGET_CONFIG = 14,
	WM_ORDER_SHUTDOWN = 15,
	WM_MESSAGE_CODE_COUNT /* important for the callback array */

} WilmaMessageType;

/**
 * @brief Structure used to store one message in the queue.
 */
typedef struct {
	WilmaMessageType code;
	void *param;
} WilmaQueueMessage;

/* @brief task handle for the main wifi_manager task */
static TaskHandle_t WILMA_TASK = NULL;

/* @brief An explicit "DISCONNECT" event was sent, indicating we shouldn't reconnect */
const int WILMA_DISCONNECTED_BIT = BIT0;

const int WILMA_FAIL_BIT = BIT1;

/* @brief We were connected to an AP before something happened */
const int WILMA_CONNECTED_BIT = BIT2;

/* @brief We're actively trying to connect as a station */
const int WILMA_CONNECTING_BIT = BIT3;

/* @brief Restart a connection sequence after scan is done */
const int WILMA_CONNECT_AFTER_SCAN_BIT = BIT4;

/* @brief Try to connect to the same AP again, because the config was updated in the interim */
const int WILMA_RETRY_CONNECTION_BIT = BIT5;

/* @brief When set, means a scan is in progress */
const int WILMA_SCAN_BIT = BIT7;

// MPC added from https://github.com/tonyp7/esp32-wifi-manager/issues/110#issuecomment-1018650169
/* @brief Set under the "WM_ORDER_CONNECT_STA" case and clear under the "EVENT_STA_DISCONNECTED" case. */
const int WILMA_REQUEST_ORDER_CONNECT_STA_BIT = BIT9;

// NVS configuration
static nvs_handle_t WILMA_NVS_HANDLE;
static const char *NVS_NAMESPACE = "wms";
static const char *AP_ENABLED_KEY = "ap_en";
static const char *AP_SSID_KEY = "ap_ssid";
static const char *AP_PASSWORD_KEY = "ap_pass";
static const char *STA_KEY = "sta";

/// An entry in the NVS for a station. This will be an array of
/// entries within the NVS.
typedef struct {
	uint8_t version;
	uint8_t state;
	uint8_t padding[2];
	uint8_t ssid[32];
	uint8_t password[64];
} WilmaNvsStationEntry;

/// An entry in the list of APs we're trying to connect to. This will be sorted
/// by RSSI.
typedef struct {
	uint8_t ssid[32];
	uint8_t password[64];
	uint8_t channel;
	int8_t rssi;
	wifi_auth_mode_t auth_mode;
} WilmaStationConnectionEntry;

/// An entry in the list of visible APs.
typedef struct {
	uint8_t ssid[32];
	uint8_t channel;
	int8_t rssi;
	uint8_t auth_mode;
} WilmaVisibleApEntry;

/// Parameter to WM_ORDER_CONNECT_STA when we want to connect to a specific AP.
typedef struct {
	uint8_t ssid[32];
	uint8_t password[64];
} WilmaConnectStaParam;

/// A list of all the candidate APs that we recognize and can connect to, sorted
/// by RSSI. When in STA mode, we'll loop through this list and try to connect to
/// each station in turn.
static WilmaStationConnectionEntry WILMA_CONNECTION_LIST[WILMA_MAX_CANDIDATE_STATIONS];
static uint8_t WILMA_CONNECTION_LIST_COUNT = 0;
static uint32_t WILMA_CONNECTION_INDEX = 0;
static uint8_t WILMA_STATION_SSID[33];

/// @brief  A list of all the visible APs, sorted by RSSI.
static WilmaVisibleApEntry WILMA_VISIBLE_APS[MAX_SCAN_RESULTS];
static uint16_t WILMA_VISIBLE_APS_COUNT = 0;

static void log_connection(const uint8_t ssid[32], const uint8_t password[64])
{
	(void)ssid;
	(void)password;
	// int i;
	// char buffer[256];
	// for (i = 0; i < 64; i++)
	// {
	// 	sprintf(buffer + (i * 3), " %02x", password[i]);
	// }
	// ESP_LOGD(TAG, "Connecting to station SSID: %s  Password: %s", (const char *)ssid, (const char *)password);
	ESP_LOGD(TAG, "Connecting to station SSID: %s", (const char *)ssid);
}

static BaseType_t send_message(WilmaMessageType code, void *param)
{
	WilmaQueueMessage msg;
	msg.code = code;
	msg.param = param;
	return xQueueSend(WILMA_QUEUE, &msg, portMAX_DELAY);
}

void wilma_forget_config_from_isr(void *has_awoken)
{
	WilmaQueueMessage msg;
	msg.code = WM_ORDER_FORGET_CONFIG;
	msg.param = NULL;
	xQueueSendFromISR(WILMA_QUEUE, &msg, has_awoken);
}

void wilma_start_scan(void)
{
	send_message(WM_ORDER_START_WIFI_SCAN, NULL);
}

void wilma_scan_then_connect(void)
{
	send_message(WM_ORDER_START_WIFI_SCAN_THEN_CONNECT, NULL);
}

/// Start a wifi scan. This is very disruptive and disconnects both STA and AP,
/// so it should be used sparingly.
static void start_scan(void)
{
	const wifi_scan_config_t scan_config = {.ssid = 0, .bssid = 0, .channel = 0, .show_hidden = true};
	EventBits_t uxBits;

	/* if a scan is already in progress this message is simply ignored thanks to the WILMA_SCAN_BIT uxBit */
	uxBits = xEventGroupGetBits(WILMA_EVENT_GROUP);
	if (!(uxBits & WILMA_SCAN_BIT)) {
		xEventGroupSetBits(WILMA_EVENT_GROUP, WILMA_SCAN_BIT);
		ESP_ERROR_CHECK(esp_wifi_disconnect());
		ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, false));
	}
}

static int get_saved_station_count(void) {
	esp_err_t err;
	unsigned int required_size;
	err = nvs_get_blob(WILMA_NVS_HANDLE, STA_KEY, NULL, &required_size);
	if (err != ESP_OK) {
		ESP_LOGD(TAG, "Unable to get station count: %s", esp_err_to_name(err));
		return 0;
	}
	return required_size / sizeof(WilmaNvsStationEntry);
}

WilmaNvsStationEntry *get_saved_stations(size_t *count)
{
	size_t required_size = 0;
	esp_err_t err;

	*count = 0;

	err = nvs_get_blob(WILMA_NVS_HANDLE, STA_KEY, NULL, &required_size);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Unable to get station list: %s", esp_err_to_name(err));
		return NULL;
	}

	*count = required_size / sizeof(WilmaNvsStationEntry);
	if (*count == 0) {
		ESP_LOGE(TAG, "no entries found in configured list");
		return NULL;
	}

	WilmaNvsStationEntry *sta_entries = malloc(required_size);
	if (sta_entries == NULL) {
		ESP_LOGE(TAG, "malloc(%d) failed", required_size);
		return NULL;
	}

	err = nvs_get_blob(WILMA_NVS_HANDLE, STA_KEY, sta_entries, &required_size);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "nvs_get_blob(%s) failed: %s", STA_KEY, esp_err_to_name(err));
		free(sta_entries);
		return NULL;
	}

	return sta_entries;
}

static void update_saved_stations(WilmaNvsStationEntry *sta_entries, size_t count)
{
	if (count == 0) {
		ESP_ERROR_CHECK(nvs_erase_key(WILMA_NVS_HANDLE, STA_KEY));
	} else {
		ESP_ERROR_CHECK(nvs_set_blob(WILMA_NVS_HANDLE, STA_KEY, sta_entries, count * sizeof(WilmaNvsStationEntry)));
	}
	ESP_ERROR_CHECK(nvs_commit(WILMA_NVS_HANDLE));
}

static esp_err_t update_ssid_state(const uint8_t ssid[32], wilma_sta_state_t new_state)
{
	size_t station_count;
	WilmaNvsStationEntry *sta_entries = get_saved_stations(&station_count);
	if (!sta_entries) {
		ESP_LOGE(TAG, "No wifi store found");
		return ESP_FAIL;
	}

	int idx;
	for (idx = 0; idx < station_count; idx += 1) {
		if (!memcmp(sta_entries[idx].ssid, ssid, sizeof(sta_entries[idx].ssid))) {
			if (sta_entries[idx].state == new_state) {
				ESP_LOGD(TAG, "SSID state for index %d is already %d", idx, new_state);
			} else {
				ESP_LOGD(TAG, "Updating existing entry: %s -> %d", sta_entries[idx].ssid, new_state);
				sta_entries[idx].state = new_state;
				update_saved_stations(sta_entries, station_count);
			}
			free(sta_entries);
			return ESP_OK;
		}
	}

	ESP_LOGE(TAG, "SSID not found: %s", ssid);
	free(sta_entries);
	return ESP_OK;
}

int wilma_add_ssid(const char *ssid, const char *password)
{
	size_t station_count;

	WilmaNvsStationEntry *sta_entries = get_saved_stations(&station_count);

	if (!sta_entries) {
		// Create a brand-new entry
		WilmaNvsStationEntry new_entry = {
			.version = 1,
			.state = STA_STATE_NEW,
			.ssid = {0},
			.password = {0},
		};
		ESP_LOGE(TAG, "No wifi store found. Creating new entry for SSID %s", ssid);
		memcpy(new_entry.ssid, ssid, MIN(strlen(ssid), sizeof(new_entry.ssid)));
		memcpy(new_entry.password, password, MIN(strlen(password), sizeof(new_entry.ssid)));
		log_connection(new_entry.ssid, new_entry.password);
		update_saved_stations(&new_entry, 1);

		// Start connecting to the first entry in the list
		if (ESP_OK == connect_to_station_index(0)) {
			xEventGroupSetBits(WILMA_EVENT_GROUP, WILMA_CONNECTING_BIT);
			ESP_ERROR_CHECK(esp_wifi_connect());
		}
		return ESP_OK;
	}

	// See if the entry exists. If so, update it. If not, create a new one.
	for (int i = 0; i < station_count; i++) {
		if (strncmp((char *)sta_entries[i].ssid, ssid, sizeof(sta_entries[i].ssid)) == 0) {
			ESP_LOGD(TAG, "Updating password for existing entry %s", ssid);
			memset(sta_entries[i].password, 0, sizeof(sta_entries[i].password));
			memcpy(sta_entries[i].password, password, MIN(strlen(password), sizeof(sta_entries[i].password)));
			log_connection(sta_entries[i].ssid, sta_entries[i].password);
			sta_entries[i].state = STA_STATE_NEW;
			update_saved_stations(sta_entries, station_count);
			free(sta_entries);

			WilmaConnectStaParam *param = malloc(sizeof(WilmaConnectStaParam));
			memset(param, 0, sizeof(WilmaConnectStaParam));
			memcpy(param->password, password, MIN(strlen(password), sizeof(param->password)));
			memcpy(param->ssid, ssid, MIN(strlen(ssid), sizeof(param->ssid)));
			send_message(WM_ORDER_CONNECT_STA, param);

			return ESP_OK;
		}
	}

	// Create a new entry
	ESP_LOGD(TAG, "Adding configuration for new SSID %s", ssid);
	sta_entries = realloc(sta_entries, sizeof(*sta_entries) * (station_count + 1));
	if (sta_entries == NULL) {
		ESP_LOGE(TAG, "realloc failed");
		return ESP_FAIL;
	}
	memset(&sta_entries[station_count], 0, sizeof(WilmaNvsStationEntry));
	memcpy(sta_entries[station_count].ssid, ssid, MIN(strlen(ssid), sizeof(sta_entries[station_count].ssid)));
	memcpy(sta_entries[station_count].password, password,
		MIN(strlen(password), sizeof(sta_entries[station_count].password)));
	sta_entries[station_count].state = STA_STATE_NEW;
	update_saved_stations(sta_entries, station_count + 1);
	log_connection(sta_entries[station_count].ssid, sta_entries[station_count].password);
	free(sta_entries);

	WilmaConnectStaParam *param = malloc(sizeof(WilmaConnectStaParam));
	memset(param, 0, sizeof(WilmaConnectStaParam));
	memcpy(param->password, password, MIN(strlen(password), sizeof(param->password)));
	memcpy(param->ssid, ssid, MIN(strlen(ssid), sizeof(param->ssid)));
	send_message(WM_ORDER_CONNECT_STA, param);

	return ESP_OK;
}

static esp_err_t clear_sta_config(void)
{
	esp_err_t err;

	wifi_config_t conf;
	if (xEventGroupGetBits(WILMA_EVENT_GROUP) & WILMA_CONNECTED_BIT) {
		xEventGroupSetBits(WILMA_EVENT_GROUP, WILMA_DISCONNECTED_BIT);
	}
	xEventGroupClearBits(WILMA_EVENT_GROUP, WILMA_CONNECTED_BIT | WILMA_CONNECTING_BIT);
	err = esp_wifi_disconnect();
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "esp_wifi_disconnect() failed: %s", esp_err_to_name(err));
	}

	// Clear out the config so we don't try to reconnect to the same AP.
	memset(&conf, 0, sizeof(conf));
	err = esp_wifi_set_config(WIFI_IF_STA, &conf);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "clear config failed: %s", esp_err_to_name(err));
	}
	return err;
}

int wilma_remove_ssid(const char *ssid)
{
	size_t station_count;
	esp_err_t err;

	WilmaNvsStationEntry *sta_entries = get_saved_stations(&station_count);
	if (!sta_entries) {
		ESP_LOGE(TAG, "No wifi store found");
		return ESP_OK;
	}

	// See if the specified SSID exists. If so, take the entry from the end of the list
	// and move it to the location of the removed entry.
	if (station_count == 0) {
		ESP_LOGE(TAG, "the sta_entries value was not NULL, but the count is 0");
		free(sta_entries);
		return ESP_OK;
	}

	for (int i = 0; i < station_count; i++) {
		if (strncmp((char *)sta_entries[i].ssid, ssid, sizeof(sta_entries[i].ssid)) != 0) {
			continue;
		}

		if (i != (station_count - 1)) {
			ESP_LOGD(TAG, "Removing existing entry: %s", ssid);
			memcpy(&sta_entries[i], &sta_entries[station_count - 1], sizeof(WilmaNvsStationEntry));
		}
		update_saved_stations(sta_entries, station_count - 1);
		free(sta_entries);

		wifi_config_t conf;

		// If we were just told to forget the AP we're currently connected to, then disconnect
		// and clear the config.
		err = esp_wifi_get_config(WIFI_IF_STA, &conf);
		if ((err == ESP_OK) && (strncmp((char *)conf.sta.ssid, ssid, sizeof(conf.sta.ssid)) == 0)) {
			// Clear out the list of known stations, forcing us to rescan to prevent a reconnection
			WILMA_CONNECTION_LIST_COUNT = 0;
			memset(WILMA_CONNECTION_LIST, 0, sizeof(WILMA_CONNECTION_LIST));
			memset(WILMA_STATION_SSID, 0, sizeof(WILMA_STATION_SSID));
			clear_sta_config();
		}
		return ESP_OK;
	}

	ESP_LOGE(TAG, "SSID not found: %s", ssid);
	return ESP_OK;
}

/// Compare two APs based on their RSSI. If the RSSI is the same, compare
/// based on their SSID.
static int ap_compare_rssi(const void *a, const void *b)
{
	const wifi_ap_record_t *ap_a = a;
	const wifi_ap_record_t *ap_b = b;
	if (ap_a->rssi > ap_b->rssi) {
		return -1;
	} else if (ap_a->rssi < ap_b->rssi) {
		return 1;
	} else {
		return strcmp((const char *)ap_a->ssid, (const char *)ap_b->ssid);
	}
}

static void ap_filter_sort_unique(wifi_ap_record_t *ap_list, uint16_t *ap_count)
{
	int total_unique;
	wifi_ap_record_t *first_free;
	total_unique = *ap_count;

	first_free = NULL;

	for (int i = 0; i < *ap_count - 1; i++) {
		wifi_ap_record_t *ap = &ap_list[i];

		/* skip the previously removed ap_count */
		if (ap->ssid[0] == 0)
			continue;

		/* remove the identical SSID+authmodes */
		for (int j = i + 1; j < *ap_count; j++) {
			wifi_ap_record_t *ap1 = &ap_list[j];
			if ((strcmp((const char *)ap->ssid, (const char *)ap1->ssid) == 0) &&
				(ap->authmode == ap1->authmode)) { /* same SSID, different auth mode is skipped */
				/* save the rssi for the display */
				if ((ap1->rssi) > (ap->rssi))
					ap->rssi = ap1->rssi;
				/* clearing the record */
				memset(ap1, 0, sizeof(wifi_ap_record_t));
			}
		}
	}
	/* reorder the list so ap_count follow each other in the list */
	for (int i = 0; i < *ap_count; i++) {
		wifi_ap_record_t *ap = &ap_list[i];
		/* skipping all that has no name */
		if (ap->ssid[0] == 0) {
			/* mark the first free slot */
			if (first_free == NULL)
				first_free = ap;
			total_unique--;
			continue;
		}
		if (first_free != NULL) {
			memcpy(first_free, ap, sizeof(wifi_ap_record_t));
			memset(ap, 0, sizeof(wifi_ap_record_t));
			/* find the next free slot */
			for (int j = 0; j < *ap_count; j++) {
				if (ap_list[j].ssid[0] == 0) {
					first_free = &ap_list[j];
					break;
				}
			}
		}
	}
	/* update the length of the list */
	*ap_count = total_unique;

	/* Sort based on RSSI */
	qsort(ap_list, *ap_count, sizeof(wifi_ap_record_t), ap_compare_rssi);
}

esp_netif_ip_info_t wilma_get_ip_info(void)
{
	esp_netif_ip_info_t ip_info;
	ESP_ERROR_CHECK(esp_netif_get_ip_info(ESP_NETIF_STA, &ip_info));
	return ip_info;
}

const char *wilma_current_ssid(void)
{
	return (const char *)WILMA_STATION_SSID;
}

esp_err_t wilma_foreach_configured_ssid(
	void (*ssid_callback)(const uint8_t ssid[32], wilma_sta_state_t state, uint32_t index, uint32_t max, void *arg),
	void *arg)
{
	size_t station_count;

	WilmaNvsStationEntry *sta_entries = get_saved_stations(&station_count);
	if (!sta_entries) {
		ESP_LOGD(TAG, "No configured stations found");
		return ESP_OK;
	}

	if (station_count == 0) {
		ESP_LOGE(TAG, "no entries found in configured list despite sta_entries being non-NULL");
		free(sta_entries);
		return ESP_OK;
	}

	uint32_t index;
	for (index = 0; index < station_count; index++) {
		ssid_callback(sta_entries[index].ssid, sta_entries[index].state, index, station_count, arg);
	}

	free(sta_entries);
	return ESP_OK;
}

esp_err_t wilma_foreach_visible_ap(void (*ap_callback)(const uint8_t ssid[32], uint8_t channel, int8_t rssi,
									   uint8_t auth_mode, uint32_t index, uint32_t max, void *arg),
	void *arg)
{
	int idx;
	if (!wilma_lock_json_buffer(pdMS_TO_TICKS(1000))) {
		ESP_LOGE(TAG, "could not get access to mutex");
		return ESP_FAIL;
	}

	for (idx = 0; idx < WILMA_VISIBLE_APS_COUNT; idx++) {
		ap_callback(WILMA_VISIBLE_APS[idx].ssid, WILMA_VISIBLE_APS[idx].channel, WILMA_VISIBLE_APS[idx].rssi,
			WILMA_VISIBLE_APS[idx].auth_mode, idx, WILMA_VISIBLE_APS_COUNT, arg);
	}

	wilma_unlock_json_buffer();

	return ESP_OK;
}

/// @brief  Update both the list of available APs and the list of potential APs to connect to
static esp_err_t update_ssid_list(void)
{
	esp_err_t ret = ESP_FAIL;
	wifi_ap_record_t *records = NULL;
	int idx;

	/* make sure the http server isn't trying to access the list while it gets refreshed */
	if (!wilma_lock_json_buffer(pdMS_TO_TICKS(1000))) {
		ESP_LOGE(TAG, "could not get access to json mutex in wifi_scan");
		return ESP_FAIL;
	}

	if (esp_wifi_scan_get_ap_num(&WILMA_VISIBLE_APS_COUNT) != ESP_OK) {
		ESP_LOGE(TAG, "Failed to get count of scanned APs");
		goto exit;
	}

	if (!WILMA_VISIBLE_APS_COUNT) {
		ESP_LOGD(TAG, "Scan result empty");
		ret = ESP_OK;
		goto exit;
	}

	WILMA_VISIBLE_APS_COUNT = MIN(WILMA_VISIBLE_APS_COUNT, MAX_SCAN_RESULTS);
	records = (wifi_ap_record_t *)calloc(WILMA_VISIBLE_APS_COUNT, sizeof(wifi_ap_record_t));
	if (!records) {
		ESP_LOGE(TAG, "Failed to allocate memory for AP list");
		WILMA_VISIBLE_APS_COUNT = 0;
		goto exit;
	}

	if (esp_wifi_scan_get_ap_records(&WILMA_VISIBLE_APS_COUNT, records) != ESP_OK) {
		ESP_LOGE(TAG, "Failed to get scanned AP records");
		goto exit;
	}

	ap_filter_sort_unique(records, &WILMA_VISIBLE_APS_COUNT);
	size_t saved_station_count;
	WilmaNvsStationEntry *sta_entries = get_saved_stations(&saved_station_count);

	ESP_LOGD(TAG, "\tS.N. %-32s %-12s %s %s %s", "SSID", "BSSID", "RSSI", "AUTH", "CHAN");
	WILMA_CONNECTION_LIST_COUNT = 0;
	int j;
	for (idx = 0; idx < WILMA_VISIBLE_APS_COUNT; idx++) {
		memcpy(WILMA_VISIBLE_APS[idx].ssid, records[idx].ssid, sizeof(WILMA_VISIBLE_APS[idx].ssid));
		WILMA_VISIBLE_APS[idx].channel = records[idx].primary;
		WILMA_VISIBLE_APS[idx].rssi = records[idx].rssi;
		WILMA_VISIBLE_APS[idx].auth_mode = records[idx].authmode;

		if (WILMA_CONNECTION_LIST_COUNT < WILMA_MAX_CANDIDATE_STATIONS) {
			for (j = 0; j < saved_station_count; j++) {
				if (strncmp((char *)sta_entries[j].ssid, (char *)records[idx].ssid, sizeof(sta_entries[j].ssid)) == 0) {
					memcpy(WILMA_CONNECTION_LIST[WILMA_CONNECTION_LIST_COUNT].ssid, records[idx].ssid,
						sizeof(WILMA_CONNECTION_LIST[WILMA_CONNECTION_LIST_COUNT].ssid));
					memcpy(WILMA_CONNECTION_LIST[WILMA_CONNECTION_LIST_COUNT].password, sta_entries[j].password,
						sizeof(WILMA_CONNECTION_LIST[WILMA_CONNECTION_LIST_COUNT].password));
					WILMA_CONNECTION_LIST[WILMA_CONNECTION_LIST_COUNT].channel = records[idx].primary;
					WILMA_CONNECTION_LIST[WILMA_CONNECTION_LIST_COUNT].rssi = records[idx].rssi;
					WILMA_CONNECTION_LIST[WILMA_CONNECTION_LIST_COUNT].auth_mode = records[idx].authmode;
					WILMA_CONNECTION_LIST_COUNT += 1;
					if (WILMA_CONNECTION_LIST_COUNT >= WILMA_MAX_CANDIDATE_STATIONS) {
						ESP_LOGD(
							TAG, "Reached maximum number of candidate stations (%d)", WILMA_MAX_CANDIDATE_STATIONS);
						break;
					}
					break;
				}
			}
		}

		ESP_LOGD(TAG, "\t[%2d] %-32s %02x%02x%02x%02x%02x%02x %4d %4d %4d", idx, records[idx].ssid,
			records[idx].bssid[0], records[idx].bssid[1], records[idx].bssid[2], records[idx].bssid[3],
			records[idx].bssid[4], records[idx].bssid[5], records[idx].rssi, records[idx].authmode,
			records[idx].primary);
	}

	free(records);
	free(sta_entries);

	ret = ESP_OK;

exit:
	wilma_unlock_json_buffer();
	return ret;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
	if (event_base == WIFI_EVENT) {
		switch (event_id) {
		case WIFI_EVENT_STA_START:
			ESP_LOGD(TAG, "WIFI_EVENT_STA_START (doing nothing)");
			// ESP_ERROR_CHECK(esp_wifi_connect());
			break;

		case WIFI_EVENT_AP_START:
			ESP_LOGD(TAG, "WIFI_EVENT_AP_START");
			break;

		case WIFI_EVENT_AP_STOP:
			ESP_LOGD(TAG, "WIFI_EVENT_AP_STOP");
			break;

		case WIFI_EVENT_STA_CONNECTED: {
			wifi_event_sta_connected_t *event = (wifi_event_sta_connected_t *)event_data;
			ESP_LOGD(TAG, "WIFI_EVENT_STA_CONNECTED");
			memcpy(WILMA_STATION_SSID, event->ssid, sizeof(WILMA_STATION_SSID));
			if (xEventGroupGetBits(WILMA_EVENT_GROUP) & WILMA_RETRY_CONNECTION_BIT) {
				xEventGroupClearBits(WILMA_EVENT_GROUP, WILMA_RETRY_CONNECTION_BIT);
				ESP_ERROR_CHECK(esp_wifi_connect());
			}
			break;
		}

		case WIFI_EVENT_STA_DISCONNECTED: {
			ESP_LOGD(TAG, "WIFI_EVENT_STA_DISCONNECTED");
			wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
			ESP_LOGI(TAG, "Disconnected from AP %s. RSSI: %d, reason: %d (%s)", event->ssid, event->rssi, event->reason,
				wilma_reason_to_str(event->reason));
			if (xEventGroupGetBits(WILMA_EVENT_GROUP) & WILMA_RETRY_CONNECTION_BIT) {
				xEventGroupClearBits(WILMA_EVENT_GROUP, WILMA_RETRY_CONNECTION_BIT);
				ESP_ERROR_CHECK(esp_wifi_connect());
			} else if (xEventGroupGetBits(WILMA_EVENT_GROUP) & WILMA_DISCONNECTED_BIT) {
				ESP_LOGD(TAG, "WIFI_EVENT_STA_DISCONNECTED: ignoring because we were told to disconnect");
				xEventGroupClearBits(WILMA_EVENT_GROUP, WILMA_DISCONNECTED_BIT);

				if (ESP_OK != connect_to_station_index(0)) {
					// If there are no known APs, re-scan and try again
					xTimerStart(WILMA_RETRY_TIMER, (TickType_t)0);
					break;
				}
				xEventGroupSetBits(WILMA_EVENT_GROUP, WILMA_CONNECTING_BIT);
				ESP_ERROR_CHECK(esp_wifi_connect());
				break;
			} else if (xEventGroupGetBits(WILMA_EVENT_GROUP) & WILMA_SCAN_BIT) {
				// Don't retry to connect to the AP if we're in the middle of a scan
				ESP_LOGD(TAG, "WIFI_EVENT_STA_DISCONNECTED: ignoring because we're scanning");
				break;
			} else if (WILMA_RETRY_NUM < RETRIES_BEFORE_CONTINUING) {
				WILMA_RETRY_NUM++;
				ESP_LOGD(TAG, "Trying again to connect to AP (try %d/%d)", WILMA_RETRY_NUM, RETRIES_BEFORE_CONTINUING);

				esp_err_t err = esp_wifi_connect();
				switch (err) {
				case ESP_OK:
					break;
				case ESP_ERR_WIFI_NOT_STARTED:
					if (event->reason == WIFI_REASON_ASSOC_LEAVE) {
						ESP_LOGD(TAG, "wifi not started, we're probably rebooting");
						break;
					}
					/* Fall through */
				default:
					ESP_ERROR_CHECK(err);
					break;
				}
			} else {
				xEventGroupSetBits(WILMA_EVENT_GROUP, WILMA_FAIL_BIT);
				xEventGroupClearBits(WILMA_EVENT_GROUP, WILMA_CONNECTING_BIT);
				if (event->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) {
					// Update the "state" to indicate authentication failure
					update_ssid_state(event->ssid, STA_STATE_AUTH_FAILURE);
				}

				if (WILMA_CONNECTION_INDEX < WILMA_CONNECTION_LIST_COUNT - 1) {
					ESP_LOGD(TAG, "Failed  to connect. Moving on to next AP...");
					if (ESP_OK != connect_to_station_index(WILMA_CONNECTION_INDEX + 1)) {
						xEventGroupSetBits(WILMA_EVENT_GROUP, WILMA_CONNECTING_BIT);
						ESP_ERROR_CHECK(esp_wifi_connect());
						WILMA_RETRY_NUM = 0;
					}
				} else {
					ESP_LOGD(TAG, "No more APs to try -- starting AP mode");
					clear_sta_config();

					// Start the AP in case we're in a new place with no known APs
					wifi_configure_softap(true);

					// TODO: Try again after a set amount of time
					xTimerStart(WILMA_RETRY_TIMER, (TickType_t)0);
				}
			}
			break;
		}

		case WIFI_EVENT_AP_STACONNECTED:
			ESP_LOGI(TAG, "WIFI_EVENT_AP_STACONNECTED");
			break;

		case WIFI_EVENT_AP_STADISCONNECTED:
			ESP_LOGD(TAG, "WIFI_EVENT_AP_STADISCONNECTED");
			break;

		case WIFI_EVENT_SCAN_DONE:
			ESP_LOGD(TAG, "WIFI_EVENT_SCAN_DONE");
			xEventGroupClearBits(WILMA_EVENT_GROUP, WILMA_SCAN_BIT);
			update_ssid_list();

			// If we were connected before the scan, reconnect to the AP
			if (xEventGroupGetBits(WILMA_EVENT_GROUP) & (WILMA_CONNECTED_BIT | WILMA_CONNECTING_BIT)) {
				ESP_ERROR_CHECK(esp_wifi_connect());
			} else if (xEventGroupGetBits(WILMA_EVENT_GROUP) & WILMA_CONNECT_AFTER_SCAN_BIT) {
				xEventGroupClearBits(WILMA_EVENT_GROUP, WILMA_CONNECT_AFTER_SCAN_BIT);
				send_message(WM_ORDER_CONNECT_STA, NULL);
			}
			break;

		default:
			ESP_LOGI(TAG, "Unhandled wifi event: %" PRId32, event_id);
			break;
		}
	} else if (event_base == IP_EVENT) {
		switch (event_id) {
		case IP_EVENT_STA_GOT_IP: {
			ESP_LOGD(TAG, "IP_EVENT_STA_GOT_IP");
			ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
			ESP_LOGD(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
			WILMA_RETRY_NUM = 0;

			// Stop a queued SSID scan
			xTimerStop(WILMA_RETRY_TIMER, (TickType_t)0);

			xEventGroupClearBits(WILMA_EVENT_GROUP, WILMA_CONNECTING_BIT);
			xEventGroupSetBits(WILMA_EVENT_GROUP, WILMA_CONNECTED_BIT);

			// Update the "state" to indicate authentication failure
			update_ssid_state(WILMA_STATION_SSID, STA_STATE_OK);

			// Re-create the AP, which will shut it down if it doesn't need
			// to run anymorre.
			wifi_configure_softap(false);

			break;
		}

		case IP_EVENT_ASSIGNED_IP_TO_CLIENT: {
			ip_event_assigned_ip_to_client_t *event = (ip_event_assigned_ip_to_client_t *)event_data;
			ESP_LOGD(TAG, "station attached to AP with ip:" IPSTR, IP2STR(&event->ip));
			break;
		}

		default:
			ESP_LOGI(TAG, "Unhandled IP event: %" PRId32, event_id);
			break;
		}
	} else {
		ESP_LOGE(TAG, "Unsupported event base: %s", event_base);
	}
}

static bool cfg_ap_enabled(void)
{
	esp_err_t err;
	uint8_t ap_enabled;
	err = nvs_get_u8(WILMA_NVS_HANDLE, AP_ENABLED_KEY, &ap_enabled);
	if (err != ESP_OK) {
		// Default to having the AP enabled, in case there is no config
		ESP_LOGD(TAG, "Unable to get whether AP was enabled -- assuming \"yes\": %s", esp_err_to_name(err));
		return true;
	}
	return ap_enabled != 0;
}

void wilma_set_ap_enabled(bool enabled)
{
	esp_err_t err;
	err = nvs_set_u8(WILMA_NVS_HANDLE, AP_ENABLED_KEY, enabled);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "nvs_set_u8(%s) failed: %s", AP_ENABLED_KEY, esp_err_to_name(err));
	}
	ESP_LOGD(TAG, "AP mode is now %s", enabled ? "enabled" : "disabled");
	ESP_ERROR_CHECK(nvs_commit(WILMA_NVS_HANDLE));

	// Re-call init, which will enable or disable the AP as necessary.
	wifi_configure_softap(false);
}

static void cfg_ap_set_ssid(const char *ssid)
{
	esp_err_t err;
	if (strlen(ssid) == 0) {
		err = nvs_erase_key(WILMA_NVS_HANDLE, AP_SSID_KEY);
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "nvs_erase_key(%s) failed: %s", AP_SSID_KEY, esp_err_to_name(err));
		}
		err = nvs_erase_key(WILMA_NVS_HANDLE, AP_PASSWORD_KEY);
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "nvs_erase_key(%s) failed: %s", AP_PASSWORD_KEY, esp_err_to_name(err));
		}
		ESP_ERROR_CHECK(nvs_commit(WILMA_NVS_HANDLE));
		return;
	}
	err = nvs_set_str(WILMA_NVS_HANDLE, AP_SSID_KEY, ssid);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "nvs_set_str(%s) failed: %s", AP_SSID_KEY, esp_err_to_name(err));
	}
	ESP_ERROR_CHECK(nvs_commit(WILMA_NVS_HANDLE));
}

static void cfg_ap_set_password(const char *password)
{
	esp_err_t err;
	err = nvs_set_str(WILMA_NVS_HANDLE, AP_PASSWORD_KEY, password);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "nvs_set_str(%s) failed: %s", AP_PASSWORD_KEY, esp_err_to_name(err));
	}
	ESP_ERROR_CHECK(nvs_commit(WILMA_NVS_HANDLE));
}

void wilma_set_ap_ssid_password(const char *ssid, const char *password)
{
	if (strlen(password) > 64) {
		ESP_LOGE(TAG, "Password length too long -- ignoring");
		return;
	}
	if (strlen(ssid) > 32) {
		ESP_LOGE(TAG, "SSID length of %d is too long -- ignoring", strlen(ssid));
		return;
	}
	cfg_ap_set_ssid(ssid);
	cfg_ap_set_password(password);
}

static void cfg_ap_fill_ssid(wifi_config_t *ap_config)
{
	esp_err_t err;
	size_t ssid_length;

	memset(ap_config->ap.ssid, 0, sizeof(ap_config->ap.ssid));
	err = nvs_get_str(WILMA_NVS_HANDLE, AP_SSID_KEY, NULL, &ssid_length);
	if (err != ESP_OK) {
		// Fill default name
		ap_config->ap.ssid_len = wilma_update_wifi_ssid(ap_config->ap.ssid);
		// ESP_LOGE(TAG, "Unable to get configured AP SSID string, using default of %s: %s", ap_config->ap.ssid, esp_err_to_name(err));

		return;
	}

	if (ssid_length > sizeof(ap_config->ap.ssid)) {
		ap_config->ap.ssid_len = wilma_update_wifi_ssid(ap_config->ap.ssid);

		ESP_LOGE(
			TAG, "Configured AP SSID length of %d is too long -- defaulting to %s", ssid_length, ap_config->ap.ssid);
		nvs_erase_key(WILMA_NVS_HANDLE, AP_SSID_KEY);     // Ignore return value
		nvs_erase_key(WILMA_NVS_HANDLE, AP_PASSWORD_KEY); // Ignore return value
		ESP_ERROR_CHECK(nvs_commit(WILMA_NVS_HANDLE));
		return;
	}

	err = nvs_get_str(WILMA_NVS_HANDLE, AP_SSID_KEY, (void *)ap_config->ap.ssid, &ssid_length);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "nvs_get_str(%s) failed: %s", AP_SSID_KEY, esp_err_to_name(err));
		ap_config->ap.ssid_len = wilma_update_wifi_ssid(ap_config->ap.ssid);
		return;
	}
	ap_config->ap.ssid_len = ssid_length;
}

static void cfg_ap_fill_password(wifi_config_t *ap_config)
{
	esp_err_t err;
	size_t password_length;

	memset(ap_config->ap.password, 0, sizeof(ap_config->ap.password));
	err = nvs_get_str(WILMA_NVS_HANDLE, AP_PASSWORD_KEY, NULL, &password_length);
	if (err != ESP_OK) {
		ESP_LOGD(TAG, "Unable to get AP password: %s -- using default", esp_err_to_name(err));
		memcpy(ap_config->ap.password, CONFIG_DEFAULT_AP_PASSWORD, strlen(CONFIG_DEFAULT_AP_PASSWORD));
		return;
	}

	if (password_length == 0) {
		return;
	}

	if (password_length > sizeof(ap_config->ap.password)) {
		ESP_LOGE(TAG, "password_length of %d is longer than available space of %d -- using default password",
			password_length, sizeof(ap_config->ap.password));
		memcpy(ap_config->ap.password, CONFIG_DEFAULT_AP_PASSWORD, strlen(CONFIG_DEFAULT_AP_PASSWORD));
		return;
	}

	err = nvs_get_str(WILMA_NVS_HANDLE, AP_PASSWORD_KEY, (void *)ap_config->ap.password, &password_length);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "nvs_get_str(%s) failed: %s", AP_PASSWORD_KEY, esp_err_to_name(err));
		memcpy(ap_config->ap.password, CONFIG_DEFAULT_AP_PASSWORD, strlen(CONFIG_DEFAULT_AP_PASSWORD));
		return;
	}
}

bool wilma_ap_enabled(void)
{
	return cfg_ap_enabled();
}

bool wilma_ap_ssid(char ssid[33])
{
	esp_err_t err;
	wifi_config_t ap_config;
	char password[65] = {};
	cfg_ap_fill_ssid(&ap_config);

	memset(ssid, 0, 33);
	strncpy(ssid, (char *)ap_config.ap.ssid, 32);

	// Determine if there's a password
	size_t password_length;
	err = nvs_get_str(WILMA_NVS_HANDLE, AP_PASSWORD_KEY, NULL, &password_length);
	if (err != ESP_OK) {
		password_length = strlen(CONFIG_DEFAULT_AP_PASSWORD);
	} else {
		password_length = strlen(password);
	}

	return password_length != 0;
}

static esp_err_t connect_to_station_index(size_t index)
{
	if (!WILMA_CONNECTION_LIST_COUNT) {
		ESP_LOGD(TAG, "No stations in connection list, starting AP mode");
		wifi_configure_softap(true);
		return ESP_FAIL;
	}

	if (index >= WILMA_CONNECTION_LIST_COUNT) {
		ESP_LOGE(TAG, "Index %d is out of range -- looping", index);
		index = 0;
	}
	WILMA_CONNECTION_INDEX = index;
	wifi_config_t wifi_sta_config = {.sta = {
										 .scan_method = WIFI_ALL_CHANNEL_SCAN,
										 .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
										 .sae_pwe_h2e = ESP_WIFI_SAE_MODE,
										 .channel = 0, // Specify channel 0 to scan all channels and pick the strongest
									 }};
	memcpy(
		wifi_sta_config.sta.ssid, WILMA_CONNECTION_LIST[WILMA_CONNECTION_INDEX].ssid, sizeof(wifi_sta_config.sta.ssid));
	memcpy(wifi_sta_config.sta.password, WILMA_CONNECTION_LIST[WILMA_CONNECTION_INDEX].password,
		sizeof(wifi_sta_config.sta.password));
	log_connection(wifi_sta_config.sta.ssid, wifi_sta_config.sta.password);

	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));
	// ESP_LOGI(TAG, "Connecting to station SSID %s", wifi_sta_config.sta.ssid);
	return ESP_OK;
}

static void retry_timer_cb(TimerHandle_t xTimer)
{
	/* stop the timer */
	xTimerStop(xTimer, (TickType_t)0);

	if (get_saved_station_count() <= 0) {
		ESP_LOGD(TAG, "No configured stations -- not performing scan + connect");
		return;
	}

	ESP_LOGD(TAG, "Retry Timer Tick! Sending WM_ORDER_CONNECT_STA to start connection process over again");

	/* Re-scan and kick off a connect afterwards */
	wilma_scan_then_connect();
}

// Initialize soft AP. If `force_on` is `false`, the AP will only start
// if it's configured to do so. Set to `true` when there are no other
// APs around to connect to.
static void wifi_configure_softap(bool force_on)
{
	if (ESP_NETIF_AP == NULL) {
		ESP_NETIF_AP = esp_netif_create_default_wifi_ap();
	}

	// Don't start the AP if we're already connected to a known AP, we're connecting to an AP,
	// we're not configured to start an AP, or we weren't forced to.
	if (!force_on && !cfg_ap_enabled() &&
		(xEventGroupGetBits(WILMA_EVENT_GROUP) & (WILMA_CONNECTING_BIT | WILMA_CONNECTED_BIT))) {
		ESP_LOGD(TAG, "AP mode is disabled and known APs were discovered");
		if (xEventGroupGetBits(WILMA_EVENT_GROUP) & WILMA_CONNECTING_BIT) {
			ESP_LOGD(TAG, "WILMA_CONNECTING_BIT is set");
		}
		if (xEventGroupGetBits(WILMA_EVENT_GROUP) & WILMA_CONNECTED_BIT) {
			ESP_LOGD(TAG, "WILMA_CONNECTED_BIT is set");
		}
		esp_wifi_set_mode(WIFI_MODE_STA);
		return;
	}
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

	esp_netif_ip_info_t ip_info;
	ip_info.ip.addr = esp_ip4addr_aton(CONFIG_DEFAULT_AP_IP);
	ip_info.gw.addr = esp_ip4addr_aton(CONFIG_DEFAULT_AP_GATEWAY);
	ip_info.netmask.addr = esp_ip4addr_aton(CONFIG_DEFAULT_AP_NETMASK);
	esp_netif_dhcps_stop(ESP_NETIF_AP);
	esp_netif_set_ip_info(ESP_NETIF_AP, &ip_info);
	esp_netif_dhcps_start(ESP_NETIF_AP);

	wifi_config_t wifi_ap_config = {
		.ap =
			{
				.ssid = CONFIG_DEFAULT_AP_SSID_PREFIX,
				.ssid_len = strlen(CONFIG_DEFAULT_AP_SSID_PREFIX),
				.channel = CONFIG_DEFAULT_AP_CHANNEL,
				.password = CONFIG_DEFAULT_AP_PASSWORD,
				.max_connection = CONFIG_DEFAULT_AP_MAX_CONNECTIONS,
				.authmode = WIFI_AUTH_WPA2_PSK,
				.pmf_cfg =
					{
						.required = false,
					},
			},
	};

	cfg_ap_fill_ssid(&wifi_ap_config);
	cfg_ap_fill_password(&wifi_ap_config);

	if (wifi_ap_config.ap.password[0] == 0) {
		wifi_ap_config.ap.authmode = WIFI_AUTH_OPEN;
	}

	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));

	ESP_LOGD(TAG, "wifi_init_softap finished. SSID:%s password:%s channel:%d", wifi_ap_config.ap.ssid,
		wifi_ap_config.ap.password[0] ? (char *)wifi_ap_config.ap.password : "[Open]", wifi_ap_config.ap.channel);
}

/* Initialize wifi station */
static esp_netif_t *wifi_init_sta(void)
{
	esp_netif_t *esp_netif_sta = esp_netif_create_default_wifi_sta();

	ESP_LOGD(TAG, "wifi_init_sta finished.");

	return esp_netif_sta;
}

static void wilma_thread(void *data)
{
	(void)data;

	WilmaQueueMessage msg;
	BaseType_t xStatus;

	ESP_ERROR_CHECK(esp_netif_init());

	ESP_ERROR_CHECK(esp_event_loop_create_default());

	ESP_ERROR_CHECK(esp_event_handler_instance_register(
		WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &WILMA_INSTANCE_ANY_ID));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(
		IP_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &WILMA_INSTANCE_GOT_IP));

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	/* Initialize AP */
	ESP_LOGD(TAG, "initializing softap");
	wifi_configure_softap(false);

	/* Initialize STA */
	ESP_LOGD(TAG, "initializing station mode");
	ESP_NETIF_STA = wifi_init_sta();

	ESP_ERROR_CHECK(esp_wifi_start());

	/* Set sta as the default interface */
	esp_netif_set_default_netif(ESP_NETIF_AP);

	wilma_scan_then_connect();

	/* State: Scan -> Connect -> Pause -> Repeat */
	while (1) {
		xStatus = xQueueReceive(WILMA_QUEUE, &msg, portMAX_DELAY);
		if (xStatus != pdPASS) {
			ESP_LOGE(TAG, "xQueueReceive failed: %d", xStatus);
			continue;
		}

		switch (msg.code) {
		case WM_ORDER_START_WIFI_SCAN_THEN_CONNECT:
			xEventGroupSetBits(WILMA_EVENT_GROUP, WILMA_CONNECT_AFTER_SCAN_BIT);
			/* Fall through */
		case WM_ORDER_START_WIFI_SCAN:
			ESP_LOGD(TAG, "Starting wifi scan");
			start_scan();
			break;

		case WM_ORDER_FORGET_CONFIG:
			ESP_LOGD(TAG, "Forgetting wifi configuration and restoring defaults");
			nvs_erase_all(WILMA_NVS_HANDLE);
			(void)nvs_commit(WILMA_NVS_HANDLE);
			clear_sta_config();
			wifi_configure_softap(true);
			break;

		case WM_ORDER_CONNECT_STA: {
			WilmaConnectStaParam *param = (WilmaConnectStaParam *)msg.param;
			if (param == NULL) {
				ESP_LOGD(TAG, "Starting connection by consulting the list of discovered APs");
				WILMA_RETRY_NUM = 0;
				if (ESP_OK != connect_to_station_index(0)) {
					// If there are no known APs, re-scan and try again
					xTimerStart(WILMA_RETRY_TIMER, (TickType_t)0);
					break;
				}
			} else {
				WILMA_RETRY_NUM = 0;
				ESP_LOGD(TAG, "Connecting to SSID %s", param->ssid);
				wifi_config_t wifi_sta_config = {.sta = {
													 .scan_method = WIFI_FAST_SCAN,
													 .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
													 .sae_pwe_h2e = ESP_WIFI_SAE_MODE,
													 .channel = 0,
												 }};
				memcpy(wifi_sta_config.sta.ssid, param->ssid, sizeof(wifi_sta_config.sta.ssid));
				memcpy(wifi_sta_config.sta.password, param->password, sizeof(wifi_sta_config.sta.password));
				// TODO: This doesn't seem to properly adjust the password. If the user was
				// connected and they try to reconnect with an invalid password, the system
				// will continue to use the old, correct password.
				clear_sta_config();
				log_connection(wifi_sta_config.sta.ssid, wifi_sta_config.sta.password);
				ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));
				free(param);
			}
			// Initiate the connection, but only if we're not currently scanning.
			if (!(xEventGroupGetBits(WILMA_EVENT_GROUP) & (WILMA_SCAN_BIT | WILMA_CONNECTING_BIT))) {
				ESP_ERROR_CHECK(esp_wifi_connect());
			} else {
				// we're currently connecting, but the configuration has been updated. Retry the
				// connection after it fails.
				xEventGroupSetBits(WILMA_EVENT_GROUP, WILMA_RETRY_CONNECTION_BIT);
			}
			break;
		}

		case WM_ORDER_SHUTDOWN:
			ESP_LOGD(TAG, "Shutting down");
			esp_wifi_stop();
			esp_wifi_deinit();
			esp_netif_destroy(ESP_NETIF_AP);
			esp_netif_destroy(ESP_NETIF_STA);
			esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, WILMA_INSTANCE_GOT_IP);
			esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, WILMA_INSTANCE_ANY_ID);
			esp_event_loop_delete_default();
			nvs_close(WILMA_NVS_HANDLE);
			vEventGroupDelete(WILMA_EVENT_GROUP);
			vQueueDelete(WILMA_QUEUE);
			wilma_utils_cleanup();
			vTaskDelete(NULL);
			break;

		default:
			ESP_LOGI(TAG, "Unknown message code: %d", msg.code);
			continue;
		}
	}
}

void wilma_start(void)
{
	/* disable the default wifi logging */
	esp_log_level_set("wifi", ESP_LOG_ERROR);
	// esp_log_level_set(TAG, ESP_LOG_DEBUG);

	ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &WILMA_NVS_HANDLE));

	WILMA_EVENT_GROUP = xEventGroupCreate();
	wilma_utils_init();
	WILMA_QUEUE = xQueueCreate(3, sizeof(WilmaQueueMessage));

	/* start wifi manager task */
	xTaskCreate(wilma_thread, "WiLma", 4000, NULL, WILMA_TASK_PRIORITY, &WILMA_TASK);

	/* create timer for to keep track of retries */
	WILMA_RETRY_TIMER = xTimerCreate(NULL, pdMS_TO_TICKS(WILMA_RETRY_TIMER_MS), pdFALSE, (void *)0, retry_timer_cb);
}

void wilma_stop(void)
{
	send_message(WM_ORDER_SHUTDOWN, NULL);
}
