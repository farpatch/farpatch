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

#define SWO_UART_RECALCULATE_BAUD 0x1000
#define SWO_UART_TERMINATE        0x1001

static const char TAG[] = "swo-uart";

static TaskHandle_t rx_pid;
static volatile bool should_exit_calibration;
QueueHandle_t swo_uart_event_queue;

#if defined(ESP32S3)
static esp_err_t uart_reset_rx_fifo(uart_port_t uart_num)
{
	//Due to hardware issue, we can not use fifo_rst to reset uart fifo.
	//See description about UART_TXFIFO_RST and UART_RXFIFO_RST in <<esp32_technical_reference_manual>> v2.6 or later.

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
	int low_period = 0;
	int high_period = 0;
	int tries = 0;

	uint32_t intena_reg = uart_ll_get_intr_ena_status(&SWO_UART);

	// Disable the interruput.
	uart_ll_disable_intr_mask(&SWO_UART, ~0);
	uart_ll_clr_intsts_mask(&SWO_UART, ~0);

	// Filter
	//	&SWO_UART->auto_baud.glitch_filt = 4;

	// Clear the previous result
	uart_ll_set_autobaud_en(&SWO_UART, false);
	uart_ll_set_autobaud_en(&SWO_UART, true);
	ESP_LOGI(__func__, "waiting for %d samples", sample_bits);
	while (uart_ll_get_rxd_edge_cnt(&SWO_UART) < sample_bits) {
		if (tries++ >= max_tries) {
			// Disable the baudrate detection
			uart_ll_set_autobaud_en(&SWO_UART, false);

			// Reset the fifo
			ESP_LOGD(__func__, "resetting the fifo");
			uart_reset_rx_fifo(uart_num);
			uart_ll_ena_intr_mask(&SWO_UART, intena_reg);
			return -1;
		}
		vTaskDelay(pdMS_TO_TICKS(10));
		// esp_task_wdt_reset();
		// ets_delay_us(10);
	}
	low_period = uart_ll_get_low_pulse_cnt(&SWO_UART);
	high_period = uart_ll_get_high_pulse_cnt(&SWO_UART);

	// Disable the baudrate detection
	uart_ll_set_autobaud_en(&SWO_UART, false);

	// Reset the fifo
	ESP_LOGD(__func__, "resetting the fifo");
	uart_reset_rx_fifo(uart_num);
	uart_ll_ena_intr_mask(&SWO_UART, intena_reg);

	// Set the clock divider reg
	SWO_UART.clkdiv.clkdiv = (low_period > high_period) ? high_period : low_period;
	return swo_uart_get_baudrate();
}

/**
 * @brief UART Receive Task
 *
 */
static void swo_uart_rx_task(void *arg)
{
	uint32_t default_baud = (uint32_t)arg;
	int baud_rate = default_baud;
	esp_err_t ret;
	uint8_t buf[256];
	int bufpos = 0;

	if (baud_rate == 0) {
		baud_rate = 115200;
	}

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
	ret = uart_driver_install(SWO_UART_IDX, 4096, 256, 16, &swo_uart_event_queue, 0);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "unable to install SWO UART driver: %s", esp_err_to_name(ret));
		goto out;
	}

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

	if (default_baud == 0) {
		ESP_LOGI(TAG, "baud rate not specified, initiating autobaud detection");
		baud_rate = uart_baud_detect(SWO_UART_IDX, 20, 10);
		ESP_LOGI(TAG, "baud rate detected as %d", baud_rate);
	}

	ESP_LOGI(TAG, "UART driver started with baud rate of %d, beginning reception...", baud_rate);

	while (1) {
		uart_event_t evt;

		if (xQueueReceive(swo_uart_event_queue, (void *)&evt, 100)) {
			if (evt.type == UART_FIFO_OVF) {
				// uart_overrun_cnt++;
			}
			if (evt.type == UART_FRAME_ERR) {
				// uart_errors++;
			}
			if (evt.type == UART_BUFFER_FULL) {
				// uart_queue_full_cnt++;
			}

			if (evt.type == SWO_UART_TERMINATE) {
				break;
			}

			if (baud_rate == -1 || evt.type == SWO_UART_RECALCULATE_BAUD) {
				esp_err_t ret = uart_set_pin(
					SWO_UART_IDX, UART_PIN_NO_CHANGE, CONFIG_TDO_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
				if (ret != ESP_OK) {
					ESP_LOGE(TAG, "unable to configure SWO UART pin: %s", esp_err_to_name(ret));
					goto out;
				}

				if ((evt.size != 0) && (evt.type == SWO_UART_RECALCULATE_BAUD)) {
					baud_rate = evt.size;
					ESP_LOGI(TAG, "setting baud rate to %d", baud_rate);
					uart_set_baudrate(SWO_UART_IDX, baud_rate);
				} else {
					ESP_LOGD(TAG, "detecting baud rate");
					baud_rate = uart_baud_detect(SWO_UART_IDX, 20, 50);
					ESP_LOGI(TAG, "baud rate detected as %d", baud_rate);
				}
			}

			bufpos = uart_read_bytes(SWO_UART_IDX, &buf[bufpos], sizeof(buf) - bufpos, 0);

			if (bufpos > 0) {
				char logstr[bufpos * 3 + 1];
				memset(logstr, 0, sizeof(logstr));
				int j;
				for (j = 0; j < bufpos; j++) {
					sprintf(logstr + (j * 3), " %02x", buf[j]);
				}
				ESP_LOGI(TAG, "uart has rx %d bytes: %s", bufpos, logstr);
				// uart_rx_count += bufpos;
				// http_term_broadcast_data(buf, bufpos);

				bufpos = 0;
			} // if(bufpos)
		} else if (baud_rate == -1) {
			esp_err_t ret =
				uart_set_pin(SWO_UART_IDX, UART_PIN_NO_CHANGE, CONFIG_TDO_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
			if (ret != ESP_OK) {
				ESP_LOGE(TAG, "unable to configure SWO UART pin: %s", esp_err_to_name(ret));
				goto out;
			}

			ESP_LOGD(TAG, "detecting baud rate");
			baud_rate = uart_baud_detect(SWO_UART_IDX, 20, 50);
			ESP_LOGI(TAG, "baud rate detected as %d", baud_rate);
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
	msg.type = 0x1000;
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
		ESP_LOGI(TAG, "initializing swo");
		xTaskCreate(swo_uart_rx_task, "swo_rx_task", 2048, (void *)baudrate, 10, &rx_pid);
	} else {
		ESP_LOGI(TAG, "swo already initialized, reinitializing...");
		swo_uart_set_baudrate(baudrate);
	}
}
