
#include <sys/param.h>

#include <esp_http_server.h>
#include <esp_ota_ops.h>
#include <esp_log.h>
#include <esp_flash_partitions.h>
#include <esp_image_format.h>
#include <esp_err.h>

static const char *TAG = "ota-http";

esp_err_t cgi_flash_init(httpd_req_t *req)
{
	const char msg[] = "blackmagic.bin";
	httpd_resp_send(req, msg, sizeof(msg) - 1);
	return ESP_OK;
}

esp_err_t cgi_flash_upload(httpd_req_t *req)
{
	const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
	esp_ota_handle_t update_handle;
	esp_err_t err;

	err = esp_ota_begin(update_part, OTA_SIZE_UNKNOWN, &update_handle);
	if (err != ESP_OK) {
		const char msg[] = "{\"error\": \"Failed to begin the update\"}";
		httpd_resp_send(req, msg, sizeof(msg) - 1);
		ESP_LOGE(TAG, "esp_ota_begin failed, error=%d", err);
		return ESP_OK;
	}
	ESP_LOGI(TAG, "esp_ota_begin succeeded");
	httpd_resp_set_hdr(req, "Connection", "close");

	int remaining = req->content_len;

	while (remaining > 0) {
		char buff[256];
		int received;
		if ((received = httpd_req_recv(req, buff, MIN(remaining, sizeof(buff)))) <= 0) {
			if (received == HTTPD_SOCK_ERR_TIMEOUT) {
				/* Retry if timeout occurred */
				continue;
			}

			/* In case of unrecoverable error,
             * close and delete the unfinished file*/
			esp_ota_abort(update_handle);

			ESP_LOGE(TAG, "File reception failed!");
			const char msg[] = "{\"error\": \"Failed to receive file\"}";
			httpd_resp_send(req, msg, sizeof(msg) - 1);
			return ESP_OK;
		}

		err = esp_ota_write(update_handle, buff, received);
		if (err != ESP_OK) {
			const char msg[] = "{\"error\": \"Failed to write OTA\"}";
			httpd_resp_send(req, msg, sizeof(msg) - 1);
			ESP_LOGE(TAG, "Error: esp_ota_write failed! err=0x%x", err);
			esp_ota_abort(update_handle);
			return ESP_OK;
		}

		/* Keep track of remaining size of
         * the file left to be uploaded */
		remaining -= received;
	}

	err = esp_ota_end(update_handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "esp_ota_end failed! err=0x%x", err);
		const char msg[] = "{\"error\": \"Failed to complete OTA\"}";
		httpd_resp_send(req, msg, sizeof(msg) - 1);
		return ESP_OK;
	}

	esp_ota_set_boot_partition(update_part);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "esp_ota_set_boot_partition failed! err=0x%x", err);
		const char msg[] = "{\"error\": \"Failed to set new boot partition\"}";
		httpd_resp_send(req, msg, sizeof(msg) - 1);
		return ESP_OK;
	}

	ESP_LOGI(TAG, "esp ota succeeded");

	httpd_resp_set_type(req, HTTPD_TYPE_JSON);
	const char response[] = "{\"success\": true}";
	httpd_resp_send(req, response, sizeof(response) - 1);

	return ESP_OK;
}

esp_err_t cgi_flash_reboot(httpd_req_t *req)
{
	httpd_resp_sendstr(req, "Initiating reboot...");
	ESP_LOGI(TAG, "preparing to reboot in 500ms...");
	vTaskDelay(pdMS_TO_TICKS(500));
	ESP_LOGI(TAG, "Initiating a reboot...");
	esp_restart();
	return ESP_OK;
}
