#ifndef _OTA_HTTP_H
#define _OTA_HTTP_H

#ifdef __cplusplus
extern "C" {
#endif

/* HTTP Server OTA Support
 */

esp_err_t cgi_flash_init(httpd_req_t *req);
esp_err_t cgi_flash_upload(httpd_req_t *req);
esp_err_t cgi_flash_reboot(httpd_req_t *req);
esp_err_t cgi_flash_progress(httpd_req_t *req);
esp_err_t cgi_flash_status(httpd_req_t *req);

#ifdef __cplusplus
}
#endif

#endif /* _OTA_HTTP_H */
