#include <driver/uart.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <lwip/sockets.h>
#include "platform.h"
#include "websocket.h"


// The Websocket API doesn't provide any way to generate control packets, including
// PING packets. To work around this, define our own concept of commands rather than
// treating everything as a stream of data.
#define PKT_DATA 0
#define PKT_PING 1

struct websocket_session {
	int fd;
	uint32_t cookie;
};

static struct websocket_session debug_handles[8];
static struct websocket_session rtt_handles[8];
static struct websocket_session uart_handles[8];
extern httpd_handle_t http_daemon;

struct websocket_config {
	struct websocket_session *handles;
	uint32_t handle_count;
	void (*recv_cb)(httpd_handle_t handle, httpd_req_t *req, uint8_t *data, int len);
};

static void on_rtt_receive(httpd_handle_t server, httpd_req_t *req, uint8_t *data, int len)
{
	void rtt_append_data(const uint8_t *data, int len);
	rtt_append_data(data, len);
}

static void on_uart_receive(httpd_handle_t server, httpd_req_t *req, uint8_t *data, int len)
{
	uart_write_bytes(TARGET_UART_IDX, (const uint8_t *)data, len);
}

static void on_debug_receive(httpd_handle_t server, httpd_req_t *req, uint8_t *data, int len)
{
	ESP_LOGI(__func__, "received text from debug channel: %s", data);
}

const struct websocket_config debug_websocket = {
	.handles = debug_handles,
	.handle_count = sizeof(debug_handles) / sizeof(debug_handles[0]),
	.recv_cb = on_debug_receive,
};

const struct websocket_config uart_websocket = {
	.handles = uart_handles,
	.handle_count = sizeof(uart_handles) / sizeof(uart_handles[0]),
	.recv_cb = on_uart_receive,
};

const struct websocket_config rtt_websocket = {
	.handles = rtt_handles,
	.handle_count = sizeof(rtt_handles) / sizeof(rtt_handles[0]),
	.recv_cb = on_rtt_receive,
};

static void websocket_broadcast(
	httpd_handle_t hd, struct websocket_session *handles, int handle_max, const uint8_t *buffer, size_t count)
{
	if ((hd == NULL) || (handles == NULL) || (handle_max == 0) || (buffer == NULL) || (count == 0)) {
		return;
	}

	static const char pkt_data[] = {PKT_DATA};
	static const httpd_ws_frame_t ws_pkt_type = {
		.type = HTTPD_WS_TYPE_BINARY,
		.len = sizeof(pkt_data),
		.payload = (void *)pkt_data,
		.fragmented = true,
		.final = false,
	};
	const httpd_ws_frame_t ws_pkt_payload = {
		.type = HTTPD_WS_TYPE_CONTINUE,
		.payload = (void *)buffer,
		.len = count,
		.fragmented = true,
		.final = true,
	};
	int ret;
	int i;

	for (i = 0; i < handle_max; i++) {
		if (handles[i].fd == 0) {
			continue;
		}
		ret = httpd_ws_send_frame_async(hd, handles[i].fd, (httpd_ws_frame_t *)&ws_pkt_type);
		if (ret != ESP_OK) {
			ESP_LOGE(__func__, "sockfd %d (index %d) is invalid! connection closed?", handles[i].fd, i);
			handles[i].fd = 0;
		}
		ret = httpd_ws_send_frame_async(hd, handles[i].fd, (httpd_ws_frame_t *)&ws_pkt_payload);
		if (ret != ESP_OK) {
			ESP_LOGE(__func__, "sockfd %d (index %d) is invalid! connection closed?", handles[i].fd, i);
			handles[i].fd = 0;
		}
	}
}

void http_term_broadcast_uart(uint8_t *data, size_t len)
{
	websocket_broadcast(http_daemon, uart_handles, sizeof(uart_handles) / sizeof(uart_handles[0]), data, len);
}

void http_term_broadcast_rtt(uint8_t *data, size_t len)
{
	websocket_broadcast(http_daemon, rtt_handles, sizeof(rtt_handles) / sizeof(rtt_handles[0]), data, len);
}

void http_debug_putc(uint8_t c, int flush)
{
	static uint8_t buf[256];
	static int bufsize = 0;

	buf[bufsize++] = c;
	if (flush || (bufsize == sizeof(buf))) {
		websocket_broadcast(http_daemon, debug_handles, sizeof(debug_handles) / sizeof(debug_handles[0]), buf, bufsize);
		bufsize = 0;
	}
}

static void cgi_websocket_close(void *ctx)
{
	uint32_t *fd = ctx;
	*fd = 0;
}

// `ws_pkt` has already had its `len` field filled in by the caller, and it's
// defined to be non-zero.
static esp_err_t dispatch_websocket_data(httpd_req_t *req, httpd_ws_frame_t ws_pkt)
{
	struct websocket_config *cfg = req->user_ctx;
	esp_err_t ret = ESP_OK;
	uint8_t *buffer = NULL;
	uint8_t stack_buffer[256];

	if (ws_pkt.len < sizeof(stack_buffer)) {
		ws_pkt.payload = stack_buffer;
	} else {
		buffer = malloc(ws_pkt.len);
		ws_pkt.payload = buffer;
	}

	ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
	if (ret != ESP_OK) {
		ESP_LOGE(__func__, "httpd_ws_recv_frame frame unable to receive data: %d", ret);
		goto out;
	}

	switch (ws_pkt.payload[0]) {
	case PKT_PING:
		httpd_ws_frame_t pong_pkt;
		memset(&pong_pkt, 0, sizeof(httpd_ws_frame_t));
		pong_pkt.type = HTTPD_WS_TYPE_BINARY;
		// Just copy the ping packet back to the sender. The opcode will be the same,
		// so we don't need to strip the first byte.
		pong_pkt.payload = ws_pkt.payload;
		pong_pkt.len = ws_pkt.len;
		ret = httpd_ws_send_frame(req, &pong_pkt);
		if (ret != ESP_OK) {
			ESP_LOGE(__func__, "httpd_ws_send_frame failed to send pong: %d", ret);
		}
		break;
	case PKT_DATA:
		if (cfg->recv_cb) {
			cfg->recv_cb(http_daemon, req, ws_pkt.payload + 1, ws_pkt.len - 1);
		} else {
			ESP_LOGE(__func__, "receive function was NULL");
		}
		break;
	default:
		ESP_LOGE(__func__, "unknown packet type %d", ws_pkt.payload[0]);
		break;
	}

out:
	if (buffer) {
		free(buffer);
	}

	return ret;
}

esp_err_t cgi_websocket(httpd_req_t *req)
{
	esp_err_t ret;
	httpd_ws_frame_t ws_pkt;
	struct websocket_config *cfg = req->user_ctx;

	if (req->method == HTTP_GET) {
		int sockfd = httpd_req_to_sockfd(req);
		int opt;

		// Enable reusing this socket
		opt = 1;
		assert(setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (void *)&opt, sizeof(opt)) != -1);

		ESP_LOGI(__func__, "handshake done on %s, the new connection was opened with sockfd %d", req->uri, sockfd);
		int i;
		int free_idx = -1;

		// See if the sockfd is already in use, possibly due to an unclean close
		for (i = 0; i < cfg->handle_count; i++) {
			if (cfg->handles[i].fd == sockfd) {
				ESP_LOGE(__func__, "sockfd %d already existed in the handle list -- not adding duplicate", sockfd);
				req->sess_ctx = &cfg->handles[i];
				req->free_ctx = cgi_websocket_close;
				return ESP_OK;
			} else if (cfg->handles[i].fd == 0) {
				free_idx = i;
			}
		}

		// The socket handle is new, so add it to the list
		if (free_idx != -1) {
			int opt = 1;
			setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (void *)&opt, sizeof(opt));
			cfg->handles[free_idx].fd = sockfd;
			cfg->handles[free_idx].cookie = esp_random();
			req->sess_ctx = &cfg->handles[free_idx];
			req->free_ctx = cgi_websocket_close;
			return ESP_OK;
		}
		ESP_LOGE(__func__, "no free sockets to handle this connection");
		return ESP_OK;
	}

	memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
	ws_pkt.type = HTTPD_WS_TYPE_BINARY;
	ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
	if (ret != ESP_OK) {
		ESP_LOGE(__func__, "httpd_ws_recv_frame failed to get frame len with %d", ret);
		return ret;
	}

	// There's nothing to do if the packet was empty
	if (ws_pkt.len <= 0) {
		return ESP_OK;
	}

	return dispatch_websocket_data(req, ws_pkt);
}
