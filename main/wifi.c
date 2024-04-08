#include <stdio.h>

#include "esp_log.h"
#include "esp_mac.h"
#include <esp_netif.h>
#include "esp_system.h"
#include <esp_wifi.h>

#include "wilma/wilma.h"
#include "wifi.h"

extern const char *word_list[];

const static char http_content_type_json[] = "application/json";
const static char http_cache_control_hdr[] = "Cache-Control";
const static char http_cache_control_no_cache[] = "no-store, no-cache, must-revalidate, max-age=0";
// const static char http_cache_control_cache[] = "public, max-age=31536000";
const static char http_pragma_hdr[] = "Pragma";
const static char http_pragma_no_cache[] = "no-cache";

esp_err_t cgi_sta_start_scan_json(httpd_req_t *req)
{
	httpd_resp_set_type(req, http_content_type_json);
	httpd_resp_set_hdr(req, http_cache_control_hdr, http_cache_control_no_cache);
	httpd_resp_set_hdr(req, http_pragma_hdr, http_pragma_no_cache);
	httpd_resp_sendstr(req, "{}\n");

	/* delay a short amount of time to let the HTTP connection flush */
	vTaskDelay(1000 / portTICK_PERIOD_MS);
	/* request a wifi scan */
	wilma_start_scan();
	return ESP_OK;
}

static void cgi_ap_json_cb(
	const uint8_t ssid[32], uint8_t channel, int8_t rssi, uint8_t auth_mode, uint32_t index, uint32_t max, void *arg)
{
	char line_buffer[110];
	httpd_req_t *req = (httpd_req_t *)arg;
	int is_last = index == max - 1;

	// Make sure the SSID is null-terminated, otherwise really weird things happen
	uint8_t null_terminated_ssid[33];
	memcpy(null_terminated_ssid, ssid, 32);
	null_terminated_ssid[32] = '\0';

	httpd_resp_sendstr_chunk(req, "{\"ssid\":");
	wilma_json_print_string(null_terminated_ssid, (unsigned char *)line_buffer);
	httpd_resp_sendstr_chunk(req, line_buffer);

	snprintf(line_buffer, sizeof(line_buffer), ",\"channel\":%d,\"rssi\":%d,\"auth_mode\":%d}%c", channel, rssi,
		auth_mode, is_last ? '\0' : ',');
	httpd_resp_sendstr_chunk(req, line_buffer);
}

esp_err_t cgi_ap_config_json(httpd_req_t *req)
{
	char ssid[33];
	char escaped_ssid[100] = {};
	bool has_password = wilma_ap_ssid(ssid);
	bool enabled = wilma_ap_enabled();
	wilma_json_print_string((unsigned char *)ssid, (unsigned char *)escaped_ssid);

	httpd_resp_set_type(req, http_content_type_json);
	httpd_resp_set_hdr(req, http_cache_control_hdr, http_cache_control_no_cache);
	httpd_resp_set_hdr(req, http_pragma_hdr, http_pragma_no_cache);

	httpd_resp_sendstr_chunk(req, "{\"ssid\":");
	httpd_resp_sendstr_chunk(req, escaped_ssid);
	httpd_resp_sendstr_chunk(req, ",\"enabled\":");
	if (enabled) {
		httpd_resp_sendstr_chunk(req, "true");
	} else {
		httpd_resp_sendstr_chunk(req, "false");
	}
	httpd_resp_sendstr_chunk(req, ",\"password\":");
	if (has_password) {
		httpd_resp_sendstr_chunk(req, "true");
	} else {
		httpd_resp_sendstr_chunk(req, "false");
	}
	httpd_resp_sendstr_chunk(req, "}");

	httpd_resp_sendstr_chunk(req, NULL);

	return ESP_OK;
}

esp_err_t cgi_sta_scan_results_json(httpd_req_t *req)
{
	httpd_resp_set_type(req, http_content_type_json);
	httpd_resp_set_hdr(req, http_cache_control_hdr, http_cache_control_no_cache);
	httpd_resp_set_hdr(req, http_pragma_hdr, http_pragma_no_cache);

	httpd_resp_sendstr_chunk(req, "[");
	wilma_foreach_visible_ap(cgi_ap_json_cb, req);
	httpd_resp_sendstr_chunk(req, "]");

	httpd_resp_sendstr_chunk(req, NULL);

	return ESP_OK;
}

static esp_err_t cgi_connect_json_add(httpd_req_t *req)
{
	char ssid[33] = {};
	char password[65] = {};

	ESP_LOGI(__func__, "http_server_netconn_serve: POST /connect.json");
	if ((ESP_OK != httpd_req_get_hdr_value_str(req, "X-Custom-ssid", ssid, sizeof(ssid))) ||
		(ESP_OK != httpd_req_get_hdr_value_str(req, "X-Custom-pwd", password, sizeof(password)))) {
		const char *error = "{\"error\": \"missing ssid or password\"}";
		ESP_LOGE(__func__, "http_server_netconn_serve: missing SSID or password");

		httpd_resp_set_type(req, http_content_type_json);
		httpd_resp_set_hdr(req, http_cache_control_hdr, http_cache_control_no_cache);
		httpd_resp_set_hdr(req, http_pragma_hdr, http_pragma_no_cache);
		return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error);
	}

	wilma_add_ssid(ssid, password);

	httpd_resp_set_type(req, http_content_type_json);
	httpd_resp_set_hdr(req, http_cache_control_hdr, http_cache_control_no_cache);
	httpd_resp_set_hdr(req, http_pragma_hdr, http_pragma_no_cache);
	httpd_resp_send(req, "{}", 2);

	return ESP_OK;
}

static esp_err_t cgi_connect_json_remove(httpd_req_t *req)
{
	char ssid[33] = {};
	if (ESP_OK != httpd_req_get_hdr_value_str(req, "X-Custom-ssid", ssid, sizeof(ssid))) {
		const char *error = "{\"error\": \"missing ssid\"}";
		ESP_LOGE(__func__, "http_server_netconn_serve: missing SSID");

		httpd_resp_set_type(req, http_content_type_json);
		httpd_resp_set_hdr(req, http_cache_control_hdr, http_cache_control_no_cache);
		httpd_resp_set_hdr(req, http_pragma_hdr, http_pragma_no_cache);
		return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error);
	}
	wilma_remove_ssid(ssid);
	httpd_resp_set_type(req, http_content_type_json);
	httpd_resp_set_hdr(req, http_cache_control_hdr, http_cache_control_no_cache);
	httpd_resp_set_hdr(req, http_pragma_hdr, http_pragma_no_cache);
	httpd_resp_send(req, "{}", 2);

	return ESP_OK;
}

esp_err_t cgi_ap_configure(httpd_req_t *req)
{
	char ssid[33] = {};
	char password[65] = {};
	bool enabled = true;

	ESP_LOGI(__func__, "http_server_netconn_serve: POST /ap.json");
	if (ESP_OK != httpd_req_get_hdr_value_str(req, "X-Custom-ssid", ssid, sizeof(ssid))) {
		if (!httpd_req_get_hdr_value_len(req, "X-Custom-disable")) {
			const char *error = "{\"error\": \"missing x-custom-ssid or x-custom-disable header\"}";
			ESP_LOGE(__func__, "http_server_netconn_serve: missing X-Custom-ssid or X-Custom-disable");

			httpd_resp_set_type(req, http_content_type_json);
			httpd_resp_set_hdr(req, http_cache_control_hdr, http_cache_control_no_cache);
			httpd_resp_set_hdr(req, http_pragma_hdr, http_pragma_no_cache);
			return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error);
		}
		ESP_LOGI(__func__, "disabling AP mode");
		enabled = false;
	}

	// Ignore errors -- if the password is present, then we'll use the password, otherwise
	// it's an open AP.
	if (ESP_OK != httpd_req_get_hdr_value_str(req, "X-Custom-pwd", password, sizeof(password))) {
		memset(password, 0, sizeof(password));
	}

	wilma_set_ap_ssid_password(ssid, password);
	wilma_set_ap_enabled(enabled);

	httpd_resp_set_type(req, http_content_type_json);
	httpd_resp_set_hdr(req, http_cache_control_hdr, http_cache_control_no_cache);
	httpd_resp_set_hdr(req, http_pragma_hdr, http_pragma_no_cache);
	httpd_resp_send(req, "{}", 2);

	return ESP_OK;
}

esp_err_t cgi_sta_connect_json(httpd_req_t *req)
{
	ESP_LOGI(__func__, "http_server_netconn_serve: %s /connect.json", http_method_str(req->method));
	switch (req->method) {
	case HTTP_POST:
		return cgi_connect_json_add(req);
	case HTTP_DELETE:
		return cgi_connect_json_remove(req);
	default:
		return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "request type not allowed");
	}
}

static void cgi_status_json_ssid_cb(
	const uint8_t ssid[32], wilma_sta_state_t state, uint32_t index, uint32_t max, void *arg)
{
	char line_buffer[110];

	// Make sure the SSID is null-terminated, otherwise really weird things happen
	uint8_t null_terminated_ssid[33];
	memcpy(null_terminated_ssid, ssid, 32);
	null_terminated_ssid[32] = '\0';

	httpd_req_t *req = (httpd_req_t *)arg;
	int is_last = index == max - 1;

	httpd_resp_sendstr_chunk(req, "{\"ssid\":");
	wilma_json_print_string(null_terminated_ssid, (unsigned char *)line_buffer);
	httpd_resp_sendstr_chunk(req, line_buffer);

	snprintf(line_buffer, sizeof(line_buffer), ",\"state\":%d}%c", state, is_last ? '\0' : ',');
	httpd_resp_sendstr_chunk(req, line_buffer);
}

esp_err_t cgi_sta_status_json(httpd_req_t *req)
{
	httpd_resp_set_type(req, http_content_type_json);
	httpd_resp_set_hdr(req, http_cache_control_hdr, http_cache_control_no_cache);
	httpd_resp_set_hdr(req, http_pragma_hdr, http_pragma_no_cache);

	{
		esp_netif_ip_info_t ip_info = wilma_get_ip_info();
		char ip[IP4ADDR_STRLEN_MAX]; /* note: IP4ADDR_STRLEN_MAX is defined in lwip */
		char gw[IP4ADDR_STRLEN_MAX];
		char netmask[IP4ADDR_STRLEN_MAX];
		char ip_info_json[85];
		const char *ip_info_json_format = "{\"ip\":\"%s\",\"netmask\":\"%s\",\"gw\":\"%s\",\"ssids\":[";

		esp_ip4addr_ntoa(&ip_info.ip, ip, IP4ADDR_STRLEN_MAX);
		esp_ip4addr_ntoa(&ip_info.gw, gw, IP4ADDR_STRLEN_MAX);
		esp_ip4addr_ntoa(&ip_info.netmask, netmask, IP4ADDR_STRLEN_MAX);

		snprintf(ip_info_json, sizeof(ip_info_json), ip_info_json_format, ip, netmask, gw);
		httpd_resp_sendstr_chunk(req, ip_info_json);
	}
	wilma_foreach_configured_ssid(cgi_status_json_ssid_cb, req);
	httpd_resp_sendstr_chunk(req, "]}");
	httpd_resp_sendstr_chunk(req, NULL);

	return ESP_OK;
}
