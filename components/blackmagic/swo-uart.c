#include "general.h"
#include "platform.h"
#include "swo.h"
#include "swo-uart.h"
#include <esp_clk_tree.h>
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <freertos/queue.h>
#include <gdb_packet.h>
#include "sdkconfig.h"

#include <driver/uart.h>
#include <hal/uart_ll.h>

#define SWO_UART_UPDATE_BAUD 0x1000
#define SWO_UART_TERMINATE   0x1001

#define SWO_AUTOBAUD_SAMPLES       100
#define SWO_AUTOBAUD_MAXIMUM_TRIES 100000

// If we get this many framing errors in a row,
// re-run autobaud.
#define RE_AUTOBAUD_THRESHOLD 20

static const char TAG[] = "swo-uart";

static TaskHandle_t rx_pid;
static volatile bool should_exit_calibration;
static QueueHandle_t swo_uart_event_queue;

#if defined(ESP32S3)
static esp_err_t uart_reset_rx_fifo(uart_port_t uart_num)
{
	// we read the data out and make `fifo_len == 0 && rd_addr == wr_addr`.
	while (
		&SWO_UART->status.rxfifo_cnt != 0 || (&SWO_UART->mem_rx_status.wr_addr != &SWO_UART->mem_rx_status.rd_addr)) {
		READ_PERI_REG(UART_FIFO_REG(uart_num));
	}
	return ESP_OK;
}
#else
static esp_err_t uart_reset_rx_fifo(uart_port_t uart_num)
{
	uart_ll_rxfifo_rst(&SWO_UART);
	return ESP_OK;
}
#endif

static uint32_t uart_get_clk_frequency(uart_dev_t *uart)
{
	soc_module_clk_t source_clk;
	uint32_t sclk_freq;
	uart_ll_get_sclk(uart, &source_clk);
	ESP_ERROR_CHECK(esp_clk_tree_src_get_freq_hz(source_clk, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &sclk_freq));
	return sclk_freq;
}

uint32_t swo_uart_get_baudrate(void)
{
	return uart_ll_get_baudrate(&SWO_UART, uart_get_clk_frequency(&SWO_UART));
}

static int32_t uart_baud_detect(uart_port_t uart_num, int sample_bits, int max_tries)
{
	int tries = 0;

	uint32_t sclk_freq = uart_get_clk_frequency(&SWO_UART);
	uint32_t intena_reg = uart_ll_get_intr_ena_status(&SWO_UART);

	// Reset the whole UART -- this is required  to clear the
	// various autobaud counters.
	int __DECLARE_RCC_ATOMIC_ENV
		__attribute__((unused)); // To avoid build errors/warnings about __DECLARE_RCC_ATOMIC_ENV
	uart_ll_enable_bus_clock(uart_num, true);
	uart_ll_reset_register(uart_num);

	// Disable the interruput.
	uart_ll_disable_intr_mask(&SWO_UART, ~0);
	uart_ll_clr_intsts_mask(&SWO_UART, ~0);

	// Clear the previous result.
	uart_ll_set_autobaud_en(&SWO_UART, false);
	vTaskDelay(pdMS_TO_TICKS(10));
	uart_ll_set_autobaud_en(&SWO_UART, true);

	while (uart_ll_get_rxd_edge_cnt(&SWO_UART) < sample_bits) {
		if (tries++ >= max_tries) {
			// Disable the baudrate detection
			uart_ll_set_autobaud_en(&SWO_UART, false);

			// Reset the fifo
			ESP_LOGD(TAG, "Autobaud detection failed");
			uart_reset_rx_fifo(uart_num);
			uart_ll_ena_intr_mask(&SWO_UART, intena_reg);
			return 0;
		}
		vTaskDelay(pdMS_TO_TICKS(10));
	}
	int high_pulse_cnt = uart_ll_get_high_pulse_cnt(&SWO_UART);
	int low_pulse_cnt = uart_ll_get_low_pulse_cnt(&SWO_UART);

	int32_t detected_baudrate = sclk_freq / ((low_pulse_cnt + high_pulse_cnt + 2) / 2);
	// ESP_LOGI(TAG, "Edge count: %d", uart_ll_get_rxd_edge_cnt(&SWO_UART));
	// ESP_LOGI(TAG, "Positive pulse count: %d", uart_ll_get_pos_pulse_cnt(&SWO_UART));
	// ESP_LOGI(TAG, "Negative pulse count: %d", uart_ll_get_neg_pulse_cnt(&SWO_UART));
	// ESP_LOGI(TAG, "High pulse count: %d", high_pulse_cnt);
	// ESP_LOGI(TAG, "Low pulse count: %d", low_pulse_cnt);
	// ESP_LOGI(TAG, "Sclk frequency: %" PRId32, sclk_freq);
	// ESP_LOGI(TAG, "Detected frequency: %" PRId32, detected_baudrate);

	// Disable the baudrate detection
	uart_ll_set_autobaud_en(&SWO_UART, false);

	// Reset the fifo
	uart_reset_rx_fifo(uart_num);
	uart_ll_ena_intr_mask(&SWO_UART, intena_reg);

	return detected_baudrate;
}

static uint32_t uart_reconfigure(uint32_t baud_rate)
{
	esp_err_t ret;

	ret = uart_set_pin(SWO_UART_IDX, UART_PIN_NO_CHANGE, CONFIG_TDO_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "unable to configure SWO UART pin: %s", esp_err_to_name(ret));
		goto out;
	}

	uart_config_t uart_config = {
		.baud_rate = baud_rate,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
	};
	ret = uart_param_config(SWO_UART_IDX, &uart_config);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "unable to configure SWO UART driver: %s", esp_err_to_name(ret));
		goto out;
	}

	const uart_intr_config_t uart_intr = {
		.intr_enable_mask = UART_RXFIFO_FULL_INT_ENA_M | UART_RXFIFO_TOUT_INT_ENA_M | UART_FRM_ERR_INT_ENA_M |
	                        UART_RXFIFO_OVF_INT_ENA_M,
		.rxfifo_full_thresh = 80,
		.rx_timeout_thresh = 2,
		.txfifo_empty_intr_thresh = 10,
	};

	ret = uart_intr_config(SWO_UART_IDX, &uart_intr);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "unable to configure UART interrupt: %s", esp_err_to_name(ret));
		goto out;
	}

	if (baud_rate == 0) {
		ESP_LOGI(TAG, "baud rate not specified, initiating autobaud detection");
		baud_rate = uart_baud_detect(SWO_UART_IDX, SWO_AUTOBAUD_SAMPLES, SWO_AUTOBAUD_MAXIMUM_TRIES);
		ESP_LOGI(TAG, "baud rate detected as %" PRId32, baud_rate);
		// Recursion -- as long as this is not zero, this will only recurse once.
		if (baud_rate != 0) {
			return uart_reconfigure(baud_rate);
		}
	}

	return baud_rate;

out:
	uart_driver_delete(SWO_UART_IDX);
	rx_pid = NULL;
	vTaskDelete(NULL);
	return 0;
}

/**
 * @brief UART Receive Task
 *
 */
static void swo_uart_rx_task(void *arg)
{
	esp_err_t ret;
	uint8_t uart_data[256];

	ret = uart_driver_install(SWO_UART_IDX, 4096, 256, 16, &swo_uart_event_queue, ESP_INTR_FLAG_IRAM);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "unable to install SWO UART driver: %s", esp_err_to_name(ret));
		goto out;
	}

	uint32_t baud_rate = (uint32_t)arg;
	uint32_t uart_errors = 0;
	bool autobaud = baud_rate == 0;
	baud_rate = uart_reconfigure(baud_rate);

	ESP_LOGI(TAG, "UART driver started with baud rate of %" PRId32, swo_uart_get_baudrate());

	while (1) {
		uart_event_t evt;

		if (xQueueReceive(swo_uart_event_queue, (void *)&evt, 100)) {
			if (evt.type == UART_BUFFER_FULL) {
				ESP_LOGI(TAG, "UART FIFO is full");
			}
			// For framing errors, count up quickly and let it slowly relax.
			if (evt.type == UART_FRAME_ERR) {
				uart_errors += 3;
				if (autobaud && (uart_errors >= (RE_AUTOBAUD_THRESHOLD * 3))) {
					ESP_LOGI(TAG, "re-running autobaud due to an excessive number of framing errors");
					baud_rate = uart_reconfigure(0);
					uart_errors = 0;
				}
				continue;
			}
			if (uart_errors) {
				uart_errors -= 1;
			}

			if (evt.type == SWO_UART_TERMINATE) {
				break;
			}

			if (baud_rate == 0 || evt.type == SWO_UART_UPDATE_BAUD) {
				if ((evt.size != 0) && (evt.type == SWO_UART_UPDATE_BAUD)) {
					autobaud = false;
					baud_rate = evt.size;
					ESP_LOGI(TAG, "setting baud rate to %" PRIu32, baud_rate);
					uart_set_baudrate(SWO_UART_IDX, baud_rate);
				} else {
					autobaud = true;
					baud_rate = uart_reconfigure(0);
				}
			}

			size_t bytes_read = uart_read_bytes(SWO_UART_IDX, uart_data, sizeof(uart_data), 0);

			if (bytes_read > 0) {
				swo_post(uart_data, bytes_read);
				// char logstr[bytes_read * 3 + 1];
				// memset(logstr, 0, sizeof(logstr));
				// int j;
				// for (j = 0; j < bytes_read; j++) {
				// 	sprintf(logstr + (j * 3), " %02x", uart_data[j]);
				// }
				// ESP_LOGI(TAG, "uart has rx %d bytes: %s", bytes_read, logstr);
			}
		} else if (baud_rate == 0) {
			autobaud = true;
			baud_rate = uart_reconfigure(0);
		}
	}

out:
	uart_driver_delete(SWO_UART_IDX);
	rx_pid = NULL;
	vTaskDelete(NULL);
}

void swo_uart_set_baudrate(unsigned int baud)
{
	uart_event_t msg;
	msg.type = SWO_UART_UPDATE_BAUD;
	msg.size = baud;
	xQueueSend(swo_uart_event_queue, &msg, portMAX_DELAY);
}

void swo_uart_deinit(void)
{
	if (rx_pid) {
		uart_event_t msg;
		msg.type = SWO_UART_TERMINATE;
		xQueueSend(swo_uart_event_queue, &msg, portMAX_DELAY);

		while (rx_pid != NULL) {
			vTaskDelay(pdMS_TO_TICKS(10));
		}
	}
}

void swo_uart_init(const uint32_t baudrate)
{
	if (!rx_pid) {
		ESP_LOGI(TAG, "initializing with baudrate of %" PRId32, baudrate);
		xTaskCreate(swo_uart_rx_task, "swo_rx_task", 2048, (void *)baudrate, 10, &rx_pid);
	} else {
		ESP_LOGI(TAG, "already initialized, updating baudrate to %" PRId32, baudrate);
		swo_uart_set_baudrate(baudrate);
	}
}
