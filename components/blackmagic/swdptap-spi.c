/**
 * @file swd-spi-tap.c
 * @author Sergey Gavrilov (who.just.the.doctor@gmail.com)
 * @version 1.0
 * @date 2021-11-25
 * 
 * Does not work due to bug with switching 3-wire mode to RX.
 * 
 * https://github.com/espressif/esp-idf/issues/7800
 * 
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <adiv5.h>

#if SWDPTAP_MODE_SPI == 1

#define SPI_HOST SPI2_HOST

#include <esp_log.h>
#include <driver/spi_master.h>
#include <string.h>
#include "soc/spi_periph.h"

#define SWDTAP_DEBUG 1
swd_proc_s swd_proc;
uint32_t swd_delay_cnt = 0;

typedef enum {
	SpiSwdDirFloat,
	SpiSwdDirDrive,
} SpiSwdDirection;

static bool spi_bus_initialized = false;
static spi_device_handle_t swd_spi_device;

inline static void swd_spi_transmit(spi_transaction_t *swd_spi_transaction)
{
	ESP_ERROR_CHECK(spi_device_polling_transmit(swd_spi_device, swd_spi_transaction));
}

inline static uint32_t swd_spi_rx(int ticks)
{
	uint32_t data = 0;
	spi_transaction_t swd_spi_transaction;
	memset(&swd_spi_transaction, 0, sizeof(swd_spi_transaction));
	swd_spi_transaction.rxlength = ticks;
	swd_spi_transaction.tx_buffer = NULL;
	swd_spi_transaction.rx_buffer = &data;

	swd_spi_transmit(&swd_spi_transaction);

#if SWDTAP_DEBUG == 1
	ESP_LOGW("spi_rx", "< [%02u] 0x%08lx", ticks, data);
#endif

	return data;
}

inline static void swd_spi_tx(uint32_t data, int ticks)
{
	spi_transaction_t swd_spi_transaction;
	memset(&swd_spi_transaction, 0, sizeof(swd_spi_transaction));
	swd_spi_transaction.length = ticks;
	swd_spi_transaction.tx_buffer = &data;
	swd_spi_transaction.rx_buffer = NULL;

	swd_spi_transmit(&swd_spi_transaction);

#if SWDTAP_DEBUG == 1
	ESP_LOGI("spi_tx", "> [%02u] 0x%08lx", ticks, data);
#endif
}

static void swdspitap_turnaround(SpiSwdDirection direction)
{
	static SpiSwdDirection old_direction = SpiSwdDirFloat;

	if (direction == old_direction)
		return;
	old_direction = direction;

	if (direction == SpiSwdDirFloat)
		if (CONFIG_TMS_SWDIO_DIR_GPIO >= 0)
			gpio_set_level(CONFIG_TMS_SWDIO_DIR_GPIO, 1);

	swd_spi_tx(1, 1);

	if (direction == SpiSwdDirDrive)
		if (CONFIG_TMS_SWDIO_DIR_GPIO >= 0)
			gpio_set_level(CONFIG_TMS_SWDIO_DIR_GPIO, 0);
}

static uint32_t swdspitap_seq_in(size_t ticks)
{
	swdspitap_turnaround(SpiSwdDirFloat);
	return swd_spi_rx(ticks);
}

static bool swdspitap_seq_in_parity(uint32_t *ret, size_t ticks)
{
	swdspitap_turnaround(SpiSwdDirFloat);
	*ret = swd_spi_rx(ticks);
	int parity = __builtin_popcount(*ret);
	uint32_t data = swd_spi_rx(1);
	parity += (data & 1);
	swdspitap_turnaround(SpiSwdDirDrive);
	return (parity & 1);
}

static void swdspitap_seq_out(uint32_t MS, size_t ticks)
{
	swdspitap_turnaround(SpiSwdDirDrive);
	swd_spi_tx(MS, ticks);
}

static void swdspitap_seq_out_parity(uint32_t MS, size_t ticks)
{
	int parity = __builtin_popcount(MS);
	swdspitap_turnaround(SpiSwdDirDrive);
	swd_spi_tx(MS, ticks);
	swd_spi_tx(parity & 1, 1);
}

static void swdptap_gpio_output_sel(uint32_t gpio_num, int func, uint32_t signal_idx)
{
	gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[gpio_num], func);
	esp_rom_gpio_connect_out_signal(gpio_num, signal_idx, 0, 0);
}

static void swdptap_gpio_input_sel(uint32_t gpio_num, int func, uint32_t signal_idx)
{
	gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[gpio_num], func);
	esp_rom_gpio_connect_in_signal(gpio_num, signal_idx, 0);
}

void swdptap_init(void)
{
	if (!spi_bus_initialized) {
		// config bus
		spi_bus_config_t swd_spi_pins = {
			.mosi_io_num = SWDIO_PIN, // SWD I/O
			.miso_io_num = SWDIO_PIN,
			.sclk_io_num = SWCLK_PIN, // SWD CLK
			.quadwp_io_num = -1,
			.quadhd_io_num = -1,
		};
		ESP_ERROR_CHECK(spi_bus_initialize(SPI_HOST, &swd_spi_pins, SPI_DMA_DISABLED));

		// add device to bus with config
		spi_device_interface_config_t swd_spi_config = {
			.mode = 0,
			.clock_speed_hz = 10 * 1000 * 1000,
			.spics_io_num = -1,
			.flags = SPI_DEVICE_3WIRE | SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_BIT_LSBFIRST,
			.queue_size = 24,
			.pre_cb = NULL,
			.post_cb = NULL,
			.input_delay_ns = 62.5,
		};
		ESP_ERROR_CHECK(spi_bus_add_device(SPI_HOST, &swd_spi_config, &swd_spi_device));

		swdptap_gpio_input_sel(SWDIO_PIN, PIN_FUNC_GPIO, spi_periph_signal[SPI_HOST].spid_in);
		swdptap_gpio_output_sel(SWDIO_PIN, PIN_FUNC_GPIO, spi_periph_signal[SPI_HOST].spid_out);
		swdptap_gpio_output_sel(SWCLK_PIN, PIN_FUNC_GPIO, spi_periph_signal[SPI_HOST].spiclk_out);

		spi_bus_initialized = true;
	}

	// set functions
	swd_proc.seq_in = swdspitap_seq_in;
	swd_proc.seq_in_parity = swdspitap_seq_in_parity;
	swd_proc.seq_out = swdspitap_seq_out;
	swd_proc.seq_out_parity = swdspitap_seq_out_parity;
}

#endif /* SWDPTAP_MODE_SPI */
