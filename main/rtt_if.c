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
#include "sdkconfig.h"

static struct sockaddr_in udp_peer_addr;
static int tcp_serv_sock[CONFIG_RTT_MAX_CHANNELS] = {};
static int udp_serv_sock = 0;

static int tcp_client_sock[CONFIG_RTT_MAX_CONNECTIONS] = {};
static int tcp_client_channel[CONFIG_RTT_MAX_CONNECTIONS] = {};

static bool rtt_initialized;
const static char http_content_type_json[] = "application/json";

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
uint32_t rtt_write(const uint32_t channel, const char *buf, uint32_t len)
{
	int ret;
	int index;

	// Only support channel 0 for http for now
	if (channel == 0) {
		http_term_broadcast_rtt((uint8_t *)buf, len);
	}

	for (index = 0; index < CONFIG_RTT_MAX_CONNECTIONS; index += 1) {
		if ((tcp_client_sock[index] != 0) && (tcp_client_channel[index] == channel)) {
			ret = send(tcp_client_sock[index], buf, len, 0);
			if (ret < 0) {
				ESP_LOGE(__func__, "tcp send() failed (%s)", strerror(errno));
				close(tcp_client_sock[index]);
				tcp_client_sock[index] = 0;
			}
		}
	}

	if ((channel == 0) && (udp_peer_addr.sin_addr.s_addr != 0)) {
		ret = sendto(udp_serv_sock, buf, len, MSG_DONTWAIT, (struct sockaddr *)&udp_peer_addr, sizeof(udp_peer_addr));
		if (ret < 0) {
			ESP_LOGE(__func__, "udp send() failed (%s)", strerror(errno));
			udp_peer_addr.sin_addr.s_addr = 0;
		}
	}

	return len;
}

/* host to target: read one character, non-blocking. return character, -1 if no character */
int32_t rtt_getchar(const uint32_t channel)
{
	(void)channel;
	if (CBUF_IsEmpty(rtt_msg_queue)) {
		return -1;
	}
	return CBUF_Pop(rtt_msg_queue);
}

/* host to target: true if no characters available for reading */
bool rtt_nodata(const uint32_t channel)
{
	(void)channel;
	return CBUF_IsEmpty(rtt_msg_queue);
}

void IRAM_ATTR rtt_append_data(const uint32_t channel, const uint8_t *data, int len)
{
	// Only support down buffer for channel 0 for now
	if (channel != 0U) {
		return;
	}
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
	httpd_resp_set_type(req, http_content_type_json);
	httpd_resp_send(req, buff, len);

	return ESP_OK;
}

static bool accept_tcp(int sock, int channel)
{
	int new_sock_index = -1;
	int index;

	for (index = 0; index < CONFIG_RTT_MAX_CONNECTIONS; index += 1) {
		if (tcp_client_sock[index] == 0) {
			new_sock_index = index;
			break;
		}
	}
	if (new_sock_index == -1) {
		ESP_LOGE(__func__, "no free tcp client sockets");
		close(accept(sock, 0, 0));
		return false;
	}
	tcp_client_sock[new_sock_index] = accept(sock, 0, 0);
	if (tcp_client_sock[new_sock_index] < 0) {
		ESP_LOGE(__func__, "accept() failed (%s)", strerror(errno));
		tcp_client_sock[new_sock_index] = 0;
		return false;
	}
	ESP_LOGI(__func__, "accepted tcp connection for channel %d", channel);
	tcp_client_channel[new_sock_index] = channel;

	int opt = 1; /* SO_KEEPALIVE */
	setsockopt(tcp_client_sock[new_sock_index], SOL_SOCKET, SO_KEEPALIVE, (void *)&opt, sizeof(opt));
	opt = 3; /* s TCP_KEEPIDLE */
	setsockopt(tcp_client_sock[new_sock_index], IPPROTO_TCP, TCP_KEEPIDLE, (void *)&opt, sizeof(opt));
	opt = 1; /* s TCP_KEEPINTVL */
	setsockopt(tcp_client_sock[new_sock_index], IPPROTO_TCP, TCP_KEEPINTVL, (void *)&opt, sizeof(opt));
	opt = 3; /* TCP_KEEPCNT */
	setsockopt(tcp_client_sock[new_sock_index], IPPROTO_TCP, TCP_KEEPCNT, (void *)&opt, sizeof(opt));
	opt = 1;
	setsockopt(tcp_client_sock[new_sock_index], IPPROTO_TCP, TCP_NODELAY, (void *)&opt, sizeof(opt));
	return true;
}

static void net_rtt_task(void *params)
{
	int ret;
	int index;
	uint8_t buf[1024];
	struct sockaddr_in saddr;

	for (index = 0; index < CONFIG_RTT_MAX_CHANNELS; index += 1) {
		tcp_serv_sock[index] = socket(AF_INET, SOCK_STREAM, 0);
	}
	udp_serv_sock = socket(AF_INET, SOCK_DGRAM, 0);

	if ((CONFIG_RTT_TCP_PORT < 0) && (CONFIG_RTT_UDP_PORT < 0)) {
		ESP_LOGI(__func__, "RTT network support is disabled in the configuration");
		return;
	}

	if (CONFIG_RTT_TCP_PORT >= 0) {
		saddr.sin_addr.s_addr = 0;
		saddr.sin_family = AF_INET;

		for (index = 0; index < CONFIG_RTT_MAX_CHANNELS; index += 1) {
			saddr.sin_port = ntohs(CONFIG_RTT_TCP_PORT + index);
			bind(tcp_serv_sock[index], (struct sockaddr *)&saddr, sizeof(saddr));
			listen(tcp_serv_sock[index], 1);
		}

		memset(tcp_client_sock, 0, sizeof(tcp_client_sock));
		memset(tcp_client_channel, 0, sizeof(tcp_client_channel));
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

		int maxfd = 0;

		if (CONFIG_RTT_TCP_PORT >= 0) {
			for (index = 0; index < CONFIG_RTT_MAX_CHANNELS; index += 1) {
				FD_SET(tcp_serv_sock[index], &fds);
				maxfd = MAX(maxfd, tcp_serv_sock[index]);
			}
		}

		if (CONFIG_RTT_UDP_PORT >= 0) {
			FD_SET(udp_serv_sock, &fds);
			maxfd = MAX(maxfd, udp_serv_sock);
		}

		for (index = 0; index < CONFIG_RTT_MAX_CONNECTIONS; index += 1) {
			if (tcp_client_sock[index]) {
				FD_SET(tcp_client_sock[index], &fds);
				maxfd = MAX(maxfd, tcp_client_sock[index]);
			}
		}

		if ((ret = select(maxfd + 1, &fds, NULL, NULL, &tv) > 0)) {
			for (index = 0; index < CONFIG_RTT_MAX_CHANNELS; index += 1) {
				if (FD_ISSET(tcp_serv_sock[index], &fds)) {
					if (!accept_tcp(tcp_serv_sock[index], index)) {
						continue;
					}
				}
			}

			if (FD_ISSET(udp_serv_sock, &fds)) {
				socklen_t slen = sizeof(udp_peer_addr);
				ret = recvfrom(udp_serv_sock, buf, sizeof(buf), 0, (struct sockaddr *)&udp_peer_addr, &slen);
				if (ret > 0) {
					rtt_append_data(0, buf, ret);
				} else {
					ESP_LOGE(__func__, "udp recvfrom() failed (%s)", strerror(errno));
				}
			}

			for (index = 0; index < CONFIG_RTT_MAX_CONNECTIONS; index += 1) {
				if (tcp_client_sock[index] && FD_ISSET(tcp_client_sock[index], &fds)) {
					ret = recv(tcp_client_sock[index], buf, sizeof(buf), MSG_DONTWAIT);
					if (ret > 0) {
						rtt_append_data(tcp_client_channel[index], buf, ret);
					} else {
						ESP_LOGE(__func__, "tcp client recv() failed (%s)", strerror(errno));
						close(tcp_client_sock[index]);
						tcp_client_sock[index] = 0;
						tcp_client_channel[index] = 0;
					}
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
