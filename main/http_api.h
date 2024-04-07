#ifndef HTTP_API_H__
#define HTTP_API_H__

#include <esp_http_server.h>

esp_err_t cgi_voltages(httpd_req_t *req);
esp_err_t cgi_status(httpd_req_t *req);
esp_err_t cgi_targets(httpd_req_t *req);
esp_err_t cgi_scan_jtag(httpd_req_t *req);
esp_err_t cgi_scan_swd(httpd_req_t *req);

#endif /* HTTP_API_H__ */
