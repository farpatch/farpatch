#include <esp_attr.h>
#include <esp_log.h>
#include <esp_http_server.h>
#include <lwip/sockets.h>
#include <stdbool.h>
#include <stdint.h>

#include "CBUF.h"
#include "general.h"
#include "http.h"
#include "rtt.h"
#include "rtt_if.h"

static struct sockaddr_in udp_peer_addr;
static int tcp_serv_sock = 0;
static int udp_serv_sock = 0;
static int tcp_client_sock = 0;

static bool rtt_initialized;

#define TAG "rtt"

static struct {
	volatile uint8_t m_get_idx;
	volatile uint8_t m_put_idx;
	uint8_t m_entry[256];
} rtt_msg_queue;

/* host: initialisation */
int rtt_if_init(void)
{
	if (rtt_initialized) {
		return 0;
	}
	CBUF_Init(rtt_msg_queue);
	rtt_initialized = true;
	return 0;
}

/* host: teardown */
int rtt_if_exit(void)
{
	if (!rtt_initialized) {
		return 0;
	}
	rtt_initialized = false;
	return 0;
}

/* target to host: write len bytes from the buffer starting at buf. return number bytes written */
uint32_t rtt_write(const char *buf, uint32_t len)
{
	int ret;
	http_term_broadcast_rtt((uint8_t *)buf, len);

	if (tcp_client_sock) {
		ret = send(tcp_client_sock, buf, len, 0);
		if (ret < 0) {
			ESP_LOGE(__func__, "tcp send() failed (%s)", strerror(errno));
			close(tcp_client_sock);
			tcp_client_sock = 0;
		}
	}

	if (udp_peer_addr.sin_addr.s_addr) {
		ret = sendto(udp_serv_sock, buf, len, MSG_DONTWAIT, (struct sockaddr *)&udp_peer_addr, sizeof(udp_peer_addr));
		if (ret < 0) {
			ESP_LOGE(__func__, "udp send() failed (%s)", strerror(errno));
			udp_peer_addr.sin_addr.s_addr = 0;
		}
	}

	return len;
}

/* host to target: read one character, non-blocking. return character, -1 if no character */
int32_t rtt_getchar(void)
{
	if (CBUF_IsEmpty(rtt_msg_queue)) {
		return -1;
	}
	return CBUF_Pop(rtt_msg_queue);
}

/* host to target: true if no characters available for reading */
bool rtt_nodata(void)
{
	return CBUF_IsEmpty(rtt_msg_queue);
}

void IRAM_ATTR rtt_append_data(const uint8_t *data, int len)
{
	while ((len-- > 0) && !CBUF_IsFull(rtt_msg_queue)) {
		CBUF_Push(rtt_msg_queue, *data++);
	}
}

esp_err_t cgi_rtt_status(httpd_req_t *req)
{
	char buff[256];
	char value_string[64];

	// `enable` may be 0 or nonzero
	httpd_req_get_url_query_str(req, buff, sizeof(buff));
	ESP_LOGI("rtt", "query string: %s", buff);
	if (ESP_OK == httpd_query_key_value(buff, "enable", value_string, sizeof(value_string))) {
		int enable = atoi(value_string);
		ESP_LOGI("rtt", "enable value: %d (%s)", enable, value_string);
		rtt_enabled = !!enable;
	}

	// `channel` must be either "auto" or a series of digits
	if (ESP_OK == httpd_query_key_value(buff, "channel", value_string, sizeof(value_string))) {
		// Disable all channels.
		for (uint32_t i = 0; i < MAX_RTT_CHAN; i++)
			rtt_channel_enabled[i] = false;
		if (!strcasecmp(value_string, "auto")) {
			rtt_auto_channel = true;
		} else {
			char *chan_ptr = value_string;
			rtt_auto_channel = false;
			int i = 2;
			while (chan_ptr[0] != '\0') {
				int chan = strtoul(chan_ptr, &chan_ptr, 0);
				if ((chan >= 0) && (chan < MAX_RTT_CHAN))
					rtt_channel_enabled[chan] = true;

				// Advance to the next digit in the string, e.g.
				// channel=1,2,3,4
				while ((chan_ptr[0] != '\0') && (chan_ptr[0] < '0' || chan_ptr[0] > '9')) {
					chan_ptr++;
				}
				i += 1;
			}
		}
	}

	int channel_count = 0;
	for (uint32_t i = 0; i < MAX_RTT_CHAN; i++) {
		if (rtt_channel_enabled[i]) {
			channel_count += 1;
		}
	}
	int len = 0;
	value_string[0] = '\0';
	for (uint32_t i = 0; (i < MAX_RTT_CHAN) && channel_count; i++) {
		if (rtt_channel_enabled[i]) {
			len += snprintf(value_string + len, sizeof(value_string) - len, PRId32, i);
			// Append a ',' if this isn't the last character
			if (channel_count > 1) {
				channel_count -= 1;
				len += snprintf(value_string + len, sizeof(value_string) - len, ",");
			}
		}
	}

	const char *format = "{"
						 "\"ident\":\"%s\","      // 1
						 "\"enabled\":%s,"        // 2
						 "\"found\":%s,"          // 3
						 "\"cbaddr\":%d,"         // 4
						 "\"min_poll_ms\":%d,"    // 5
						 "\"max_poll_ms\":%d,"    // 6
						 "\"max_poll_errs\":%d,"  // 7
						 "\"auto_channel\":%s,"   // 8
						 "\"max_flag_skip\":%s,"  // 9
						 "\"max_flag_block\":%s," // 10
						 "\"channels\":[%s]"      // 11
						 "}";
	len = snprintf(buff, sizeof(buff), format,
		rtt_ident,                           // 1
		rtt_enabled ? "true" : "false",      // 2
		rtt_found ? "true" : "false",        // 3
		rtt_cbaddr,                          // 4
		rtt_min_poll_ms,                     // 5
		rtt_max_poll_ms,                     // 6
		rtt_max_poll_errs,                   // 7
		rtt_auto_channel ? "true" : "false", // 8
		rtt_flag_skip ? "true" : "false",    // 9
		rtt_flag_block ? "true" : "false",   // 10
		value_string                         // 11
	);
	httpd_resp_set_type(req, "text/json");
	httpd_resp_send(req, buff, len);

	return ESP_OK;
}

static void net_rtt_task(void *params)
{
	int ret;
	uint8_t buf[1024];
	struct sockaddr_in saddr;

	tcp_serv_sock = socket(AF_INET, SOCK_STREAM, 0);
	udp_serv_sock = socket(AF_INET, SOCK_DGRAM, 0);
	tcp_client_sock = 0;

	if ((CONFIG_RTT_TCP_PORT < 0) && (CONFIG_RTT_UDP_PORT < 0)) {
		ESP_LOGI(__func__, "RTT network support is disabled in the configuration");
		return;
	}

	if (CONFIG_RTT_TCP_PORT >= 0) {
		saddr.sin_addr.s_addr = 0;
		saddr.sin_port = ntohs(CONFIG_RTT_TCP_PORT);
		saddr.sin_family = AF_INET;
		bind(tcp_serv_sock, (struct sockaddr *)&saddr, sizeof(saddr));
		listen(tcp_serv_sock, 1);
	}

	if (CONFIG_RTT_UDP_PORT >= 0) {
		saddr.sin_addr.s_addr = 0;
		saddr.sin_port = ntohs(CONFIG_RTT_UDP_PORT);
		saddr.sin_family = AF_INET;
		bind(udp_serv_sock, (struct sockaddr *)&saddr, sizeof(saddr));
	}

	while (1) {
		fd_set fds;
		struct timeval tv;
		tv.tv_sec = 1;
		tv.tv_usec = 0;

		FD_ZERO(&fds);

		if (CONFIG_RTT_TCP_PORT >= 0) {
			FD_SET(tcp_serv_sock, &fds);
		}

		if (CONFIG_RTT_UDP_PORT >= 0) {
			FD_SET(udp_serv_sock, &fds);
		}
		if (tcp_client_sock) {
			FD_SET(tcp_client_sock, &fds);
		}

		int maxfd = MAX(tcp_serv_sock, MAX(udp_serv_sock, tcp_client_sock));

		if ((ret = select(maxfd + 1, &fds, NULL, NULL, &tv) > 0)) {
			if (FD_ISSET(tcp_serv_sock, &fds)) {
				tcp_client_sock = accept(tcp_serv_sock, 0, 0);
				if (tcp_client_sock < 0) {
					ESP_LOGE(__func__, "accept() failed");
					tcp_client_sock = 0;
				} else {
					ESP_LOGI(__func__, "accepted tcp connection");

					int opt = 1; /* SO_KEEPALIVE */
					setsockopt(tcp_client_sock, SOL_SOCKET, SO_KEEPALIVE, (void *)&opt, sizeof(opt));
					opt = 3; /* s TCP_KEEPIDLE */
					setsockopt(tcp_client_sock, IPPROTO_TCP, TCP_KEEPIDLE, (void *)&opt, sizeof(opt));
					opt = 1; /* s TCP_KEEPINTVL */
					setsockopt(tcp_client_sock, IPPROTO_TCP, TCP_KEEPINTVL, (void *)&opt, sizeof(opt));
					opt = 3; /* TCP_KEEPCNT */
					setsockopt(tcp_client_sock, IPPROTO_TCP, TCP_KEEPCNT, (void *)&opt, sizeof(opt));
					opt = 1;
					setsockopt(tcp_client_sock, IPPROTO_TCP, TCP_NODELAY, (void *)&opt, sizeof(opt));
				}
			}

			if (FD_ISSET(udp_serv_sock, &fds)) {
				socklen_t slen = sizeof(udp_peer_addr);
				ret = recvfrom(udp_serv_sock, buf, sizeof(buf), 0, (struct sockaddr *)&udp_peer_addr, &slen);
				if (ret > 0) {
					rtt_append_data(buf, ret);
				} else {
					ESP_LOGE(__func__, "udp recvfrom() failed");
				}
			}

			if (tcp_client_sock && FD_ISSET(tcp_client_sock, &fds)) {
				ret = recv(tcp_client_sock, buf, sizeof(buf), MSG_DONTWAIT);
				if (ret > 0) {
					rtt_append_data(buf, ret);
				} else {
					ESP_LOGE(__func__, "tcp client recv() failed (%s)", strerror(errno));
					close(tcp_client_sock);
					tcp_client_sock = 0;
				}
			}
		}
	}
}

void rtt_init(void)
{
	ESP_LOGI(__func__, "configuring RTT for target");

	// Start RTT task
	rtt_enabled = true;
	xTaskCreate(net_rtt_task, "rtt_rx", 5 * 1024, NULL, 1, NULL);
}
