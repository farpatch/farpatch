#ifndef __BLACKMAGIC_WIFI_H__
#define __BLACKMAGIC_WIFI_H__

#include <esp_http_server.h>

esp_err_t cgi_sta_scan_results_json(httpd_req_t *req);
esp_err_t cgi_ap_config_json(httpd_req_t *req);
esp_err_t cgi_sta_start_scan_json(httpd_req_t *req);
esp_err_t cgi_sta_connect_json(httpd_req_t *req);
esp_err_t cgi_sta_status_json(httpd_req_t *req);
esp_err_t cgi_ap_configure(httpd_req_t *req);

#endif /* __BLACKMAGIC_WIFI_H__ */
