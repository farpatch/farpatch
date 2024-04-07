#ifndef WILMA_H__
#define WILMA_H__

#include <lwip/ip_addr.h>
#include <esp_netif.h>
#include <stdbool.h>

typedef enum {
	STA_STATE_NEW = 0,
	STA_STATE_OK = 1,
	STA_STATE_AUTH_FAILURE = 2,
} wilma_sta_state_t;

void wilma_unique_words(const char **name1, const char **name2);

void wilma_start(void);
void wilma_stop(void);
void wilma_start_scan(void);
void wilma_scan_then_connect(void);
int wilma_add_ssid(const char *ssid, const char *password);
int wilma_remove_ssid(const char *ssid);

void wilma_set_ap_enabled(bool enabled);
bool wilma_ap_enabled(void);
const char *wilma_current_ssid(void);

// Return the currently-configured SSID. Returns true if a password is required.
bool wilma_ap_ssid(char[33]);
void wilma_set_ap_ssid_password(const char *ssid, const char *password);

/// @brief  Print the string to the output buffer. Make sure the output buffer is large enough to hold the escaped string.
/// @param input
/// @param output_buffer
/// @return
bool wilma_json_print_string(const unsigned char *input, unsigned char *output_buffer);

esp_netif_ip_info_t wilma_get_ip_info(void);
esp_err_t wilma_foreach_configured_ssid(
	void (*ssid_callback)(const uint8_t ssid[32], wilma_sta_state_t state, uint32_t index, uint32_t max, void *arg),
	void *arg);
esp_err_t wilma_foreach_visible_ap(void (*ap_callback)(const uint8_t ssid[32], uint8_t channel, int8_t rssi,
									   uint8_t auth_mode, uint32_t index, uint32_t max, void *arg),
	void *arg);

/// @brief  Low-level queue interaction from ISR
void wilma_forget_config_from_isr(void *has_awoken);


#endif /* WILMA_H__ */
