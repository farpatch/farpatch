#include <stdint.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/uart.h"
#include "freertos/semphr.h"
#include "hal/uart_hal.h"
#include "nvs_flash.h"
#include "soc/uart_reg.h"
#include "soc/uart_periph.h"

#include "sdkconfig.h"

#include "lwip/sockets.h"

#include "general.h"

#include "CBUF.h"
#include "http.h"
#include "tinyprintf.h"
#include "uart.h"

static struct sockaddr_in udp_peer_addr;
static int tcp_serv_sock;
static int udp_serv_sock;
static int tcp_client_sock = 0;

// UART statistics counters
uint32_t uart_overrun_cnt;
uint32_t uart_irq_count;
uint32_t uart_rx_data_relay;
uint32_t uart_frame_error_cnt;
uint32_t uart_queue_full_cnt;
uint32_t uart_rx_count;
uint32_t uart_tx_count;

static QueueHandle_t uart_event_queue;

struct {
	volatile uint8_t m_get_idx;
	volatile uint8_t m_put_idx;
	uint8_t m_entry[256];
	SemaphoreHandle_t sem;
} dbg_msg_queue;

static void dbg_log_task(void *parameters)
{
	(void)parameters;
	while (1) {
		xSemaphoreTake(dbg_msg_queue.sem, portMAX_DELAY);

		while (!CBUF_IsEmpty(dbg_msg_queue)) {
			char c = CBUF_Pop(dbg_msg_queue);
			http_debug_putc(c, c == '\n' ? 1 : 0);
		}
	}
}

void debug_putc(char c, int flush)
{
	CBUF_Push(dbg_msg_queue, c);
	if (flush) {
		xSemaphoreGive(dbg_msg_queue.sem);
	}
}

void debug_serial_send_stdout(const uint8_t *const data, const size_t len)
{
	size_t i;
	for (i = 0; i < len; i++) {
		debug_putc(data[i], 0);
	}
}

static vprintf_like_t vprintf_orig = NULL;
static void putc_remote(void *ignored, char c)
{
	(void)ignored;
	if (c == '\n') {
		debug_putc('\r', 0);
	}

	debug_putc(c, c == '\n' ? 1 : 0);
}

static int vprintf_remote(const char *fmt, va_list va)
{
	if (vprintf_orig) {
		vprintf_orig(fmt, va);
	}
	tfp_format(NULL, putc_remote, fmt, va);
	return 1;
}

void uart_dbg_install(void)
{
	CBUF_Init(dbg_msg_queue);
	dbg_msg_queue.sem = xSemaphoreCreateBinary();
	vprintf_orig = esp_log_set_vprintf(vprintf_remote);

	xTaskCreate(&dbg_log_task, "dbg_log_main", 2048, NULL, 4, NULL);
}

static void uart_net_task(void *params)
{
	tcp_serv_sock = socket(AF_INET, SOCK_STREAM, 0);
	udp_serv_sock = socket(AF_INET, SOCK_DGRAM, 0);
	tcp_client_sock = 0;

	int ret;
	uint8_t buf[1024];

	struct sockaddr_in saddr;
	saddr.sin_addr.s_addr = 0;
	saddr.sin_port = ntohs(CONFIG_UART_TCP_PORT);
	saddr.sin_family = AF_INET;

	bind(tcp_serv_sock, (struct sockaddr *)&saddr, sizeof(saddr));

	saddr.sin_addr.s_addr = 0;
	saddr.sin_port = ntohs(CONFIG_UART_UDP_PORT);
	saddr.sin_family = AF_INET;
	bind(udp_serv_sock, (struct sockaddr *)&saddr, sizeof(saddr));
	listen(tcp_serv_sock, 1);

	while (1) {
		fd_set fds;
		struct timeval tv;
		tv.tv_sec = 1;
		tv.tv_usec = 0;

		FD_ZERO(&fds);
		FD_SET(tcp_serv_sock, &fds);
		FD_SET(udp_serv_sock, &fds);
		if (tcp_client_sock)
			FD_SET(tcp_client_sock, &fds);

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
					uart_write_bytes(TARGET_UART_IDX, (const char *)buf, ret);
					uart_tx_count += ret;
				} else {
					ESP_LOGE(__func__, "udp recvfrom() failed");
				}
			}

			if (tcp_client_sock && FD_ISSET(tcp_client_sock, &fds)) {
				ret = recv(tcp_client_sock, buf, sizeof(buf), MSG_DONTWAIT);
				if (ret > 0) {
					uart_write_bytes(TARGET_UART_IDX, (const char *)buf, ret);
					uart_tx_count += ret;
				} else {
					ESP_LOGE(__func__, "tcp client recv() failed (%s)", strerror(errno));
					close(tcp_client_sock);
					tcp_client_sock = 0;
				}
			}
		}
	}
}

static void uart_config(void)
{
	extern nvs_handle h_nvs_conf;
	uint32_t baud = 115200;
	nvs_get_u32(h_nvs_conf, "uartbaud", &baud);

	uart_config_t uart_config = {
		.baud_rate = baud,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.rx_flow_ctrl_thresh = 120,
		.source_clk = UART_SCLK_DEFAULT,
	};
	ESP_ERROR_CHECK(uart_driver_install(TARGET_UART_IDX, 4096, 256, 16, &uart_event_queue, ESP_INTR_FLAG_IRAM));
	ESP_ERROR_CHECK(uart_param_config(TARGET_UART_IDX, &uart_config));
	// Without this delay, the device locks up. It's unclear why this is.
	vTaskDelay(pdMS_TO_TICKS(100));
	ESP_ERROR_CHECK(uart_set_pin(
		TARGET_UART_IDX, CONFIG_UART_TX_GPIO, CONFIG_UART_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

	const uart_intr_config_t uart_intr = {
		.intr_enable_mask = UART_RXFIFO_FULL_INT_ENA_M | UART_RXFIFO_TOUT_INT_ENA_M | UART_FRM_ERR_INT_ENA_M |
	                        UART_RXFIFO_OVF_INT_ENA_M,
		.rxfifo_full_thresh = 80,
		.rx_timeout_thresh = 2,
		.txfifo_empty_intr_thresh = 10,
	};

	ESP_ERROR_CHECK(uart_intr_config(TARGET_UART_IDX, &uart_intr));
	uart_set_baudrate(TARGET_UART_IDX, baud);
}

static void IRAM_ATTR uart_hw_task(void *parameters)
{
	(void)parameters;
	uint8_t buf[1024];
	int ret;
	int count = 0;

	uart_config();

	while (1) {
		uart_event_t evt;

		if (xQueueReceive(uart_event_queue, (void *)&evt, portMAX_DELAY)) {
			if (evt.type == UART_FIFO_OVF) {
				uart_overrun_cnt++;
			} else if (evt.type == UART_FRAME_ERR) {
				uart_frame_error_cnt++;
			} else if (evt.type == UART_BUFFER_FULL) {
				uart_queue_full_cnt++;
			}

			count = uart_read_bytes(TARGET_UART_IDX, &buf, sizeof(buf), 0);
			if (count <= 0) {
				// ESP_LOGE(__func__, "uart gave us 0 bytes");
				continue;
			}

			uart_rx_count += count;
			http_term_broadcast_data(buf, count);

			if (tcp_client_sock) {
				ret = send(tcp_client_sock, buf, count, 0);
				if (ret < 0) {
					ESP_LOGE(__func__, "tcp send() failed (%s)", strerror(errno));
					close(tcp_client_sock);
					tcp_client_sock = 0;
				}
			}

			if (udp_peer_addr.sin_addr.s_addr) {
				ret = sendto(
					udp_serv_sock, buf, count, MSG_DONTWAIT, (struct sockaddr *)&udp_peer_addr, sizeof(udp_peer_addr));
				if (ret < 0) {
					ESP_LOGE(__func__, "udp send() failed (%s)", strerror(errno));
					udp_peer_addr.sin_addr.s_addr = 0;
				}
			}
		}
	}
}

void uart_send_break(void)
{
	uart_wait_tx_done(TARGET_UART_IDX, 10);

	uint32_t baud;
	uart_get_baudrate(TARGET_UART_IDX, &baud);    // save current baudrate
	uart_set_baudrate(TARGET_UART_IDX, baud / 2); // set half the baudrate
	const uint8_t b = 0x00;
	uart_write_bytes(TARGET_UART_IDX, &b, 1);
	uart_wait_tx_done(TARGET_UART_IDX, 10);
	uart_set_baudrate(TARGET_UART_IDX, baud); // restore baudrate
}

void uart_init(void)
{
#if !defined(CONFIG_TARGET_UART_NONE)
	ESP_LOGI(__func__, "configuring UART%d for target", TARGET_UART_IDX);

	// Start UART tasks
	xTaskCreate(uart_hw_task, "uart_hw_task", 3072, NULL, 1, NULL);
	xTaskCreate(uart_net_task, "uart_net_task", 3072, NULL, 1, NULL);
#endif
}
