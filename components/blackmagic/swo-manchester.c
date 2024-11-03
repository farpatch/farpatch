
#include "general.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include <driver/rmt_rx.h>
#include "platform.h"
#include "swo.h"
#include "swo-manchester.h"

static rmt_channel_handle_t rx_channel = NULL;
static const char TAG[] = "swo-manchester";
static TaskHandle_t rx_pid;

#define SWO_MANCHESTER_FREQ_HZ 20 * 1000 * 1000 // 20MHz resolution, 1 tick = 50ns
#define SWO_IRQ_INDEX          0
#define SWO_MANCHESTER_WORDS   SOC_RMT_MEM_WORDS_PER_CHANNEL

struct Manchester {
	bool previous : 1;
	bool second : 1;
	bool clock : 1;
};

struct Accumulator {
	int offset;
	uint8_t acc;
};

struct SymbolCacheItem {
	int16_t zero;
	int16_t one;
};

#define SYMBOL_CACHE_SIZE 1024
struct SymbolCache {
	struct SymbolCacheItem items[SYMBOL_CACHE_SIZE];
	uint16_t head;
	uint16_t tail;
	uint32_t overflow;
};

struct SymbolCache symbol_cache;

struct RmtState {
	QueueHandle_t receive_queue;
	TaskHandle_t receive_task;
	uint16_t bit_time;
	uint8_t ring_buffer[1024];
	uint16_t buffer_length;
	rmt_symbol_word_t raw_symbols[SWO_MANCHESTER_WORDS];
	bool is_stopped;
	struct Manchester manchester;
	struct Accumulator accumulator;
	uint8_t skip_counter;
	// SWO only allows for 8 bytes at a time before starting a new transaction
	uint8_t byte_counter;
};

static struct RmtState rmt_state;

static const rmt_receive_config_t receive_config = {
	.signal_range_min_ns = 100,    // Theoretical maximum -- 10 Mbps
	.signal_range_max_ns = 100000, // Manchester must be at least 10 kHz
	.flags.en_partial_rx = true,   // We want to receive a continuous stream of data
};

static void IRAM_ATTR reset_rmt_state(struct RmtState *rmt_state)
{
	rmt_state->is_stopped = true;
	rmt_state->manchester.second = false;
	rmt_state->manchester.previous = false;
	rmt_state->manchester.clock = false;
	rmt_state->accumulator.acc = 0;
	rmt_state->accumulator.offset = 0;
	rmt_state->skip_counter = 0;
	rmt_state->byte_counter = 0;
}

static bool IRAM_ATTR is_short(uint16_t bit_time, uint16_t duration)
{
	// Bit times should be +/- 1 due to rounding
	return abs(bit_time - duration) < 2;
}

static bool IRAM_ATTR is_long(uint16_t bit_time, uint16_t duration)
{
	return is_short(bit_time, duration / 2);
}

static bool IRAM_ATTR is_end(uint16_t bit_time, uint16_t duration)
{
	return bit_time && ((duration == 0) || (!is_short(bit_time, duration) && !is_long(bit_time, duration)));
}

// Append one nit (half a bit) to the accumulator.
// One nibble is half a byte. One nit is half a bit.
static void IRAM_ATTR append_nit_to_accumulator(bool bit, void *context)
{
	struct RmtState *rmt_state = context;
	bool first = rmt_state->manchester.previous;
	bool second = bit;
	// Invalid state
	if (!first && !second) {
		// Inverted stop state
		reset_rmt_state(rmt_state);
		return;
	}
	if (first && second) {
		// Stop state
		reset_rmt_state(rmt_state);
		return;
	}
	if (rmt_state->skip_counter) {
		// Skip a bit due to start condition
		rmt_state->skip_counter -= 1;
		return;
	}
	// High-Low is 1, Low-High is 0
	if (second) {
		rmt_state->accumulator.acc |= 1 << rmt_state->accumulator.offset;
	}
	rmt_state->accumulator.offset++;
	if (rmt_state->accumulator.offset >= 8) {
		rmt_state->byte_counter += 1;
		if (rmt_state->buffer_length < sizeof(rmt_state->ring_buffer)) {
			rmt_state->ring_buffer[rmt_state->buffer_length++] = rmt_state->accumulator.acc;
		}
		// Skip the next start bit.
		if (rmt_state->byte_counter == 8) {
			rmt_state->byte_counter = 0;
			rmt_state->skip_counter = 1;
		}
		rmt_state->accumulator.offset = 0;
		rmt_state->accumulator.acc = 0;
	}
}

// Process one nit. One nit is half a bit.
static void IRAM_ATTR process_duration(struct RmtState *rmt_state, uint16_t duration, bool level)
{
	// Recalculate the duration if we're in the STOP state.
	if (rmt_state->is_stopped) {
		rmt_state->bit_time = duration;
		rmt_state->is_stopped = false;
		rmt_state->skip_counter = 1;
		rmt_state->manchester.previous = level;
		rmt_state->manchester.second = true;
		return;
	}

	// Tick the clock once per nit.
	rmt_state->manchester.clock = !rmt_state->manchester.clock;

	if (rmt_state->manchester.second) {
		append_nit_to_accumulator(level, rmt_state);
		rmt_state->manchester.second = false;
	} else {
		rmt_state->manchester.previous = level;
		rmt_state->manchester.second = true;
	}

	if (is_end(rmt_state->bit_time, duration)) {
		reset_rmt_state(rmt_state);
		return;
	}

	if (is_long(rmt_state->bit_time, duration)) {
		// Process long pairs as two bits
		rmt_state->manchester.clock = !rmt_state->manchester.clock;
		if (rmt_state->manchester.second) {
			append_nit_to_accumulator(level, rmt_state);
			rmt_state->manchester.second = false;
		} else {
			rmt_state->manchester.previous = level;
			rmt_state->manchester.second = true;
		}
	}
}

static bool IRAM_ATTR swo_rmt_rx_done_callback(
	rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *edata, void *user_data)
{
	BaseType_t high_task_wakeup = pdFALSE;
	struct RmtState *rmt_state = user_data;

	for (size_t i = 0; i < edata->num_symbols; i += 1) {
		uint16_t next = (symbol_cache.head + 1) & (SYMBOL_CACHE_SIZE - 1);
		if (next != symbol_cache.tail) {
			symbol_cache.items[symbol_cache.head].zero = edata->received_symbols[i].duration0;
			symbol_cache.items[symbol_cache.head].one = edata->received_symbols[i].duration1;
			symbol_cache.head = next;
		} else {
			symbol_cache.overflow += 1;
		}
	}

	if (edata->flags.is_last) {
		if (ESP_OK != rmt_receive(channel, rmt_state->raw_symbols, sizeof(rmt_state->raw_symbols), &receive_config)) {
			ESP_EARLY_LOGE(TAG, "%s(%d): unable to rmt_receive()", __FUNCTION__, __LINE__);
		}
	}

	vTaskNotifyGiveIndexedFromISR(rmt_state->receive_task, SWO_IRQ_INDEX, &high_task_wakeup);
	return high_task_wakeup == pdTRUE;
}

static void swo_manchester_rx_task(void *state)
{
	struct RmtState *rmt_state = state;

	rmt_state->receive_task = xTaskGetCurrentTaskHandle();

	reset_rmt_state(rmt_state);
	ESP_ERROR_CHECK(rmt_receive(rx_channel, rmt_state->raw_symbols, sizeof(rmt_state->raw_symbols), &receive_config));

	while (1) {
		ulTaskNotifyTakeIndexed(SWO_IRQ_INDEX, pdTRUE, portMAX_DELAY);

		rmt_state->buffer_length = 0;
		while (symbol_cache.tail != symbol_cache.head) {
			int16_t zero = symbol_cache.items[symbol_cache.tail].zero;
			int16_t one = symbol_cache.items[symbol_cache.tail].one;
			symbol_cache.tail = (symbol_cache.tail + 1) & (SYMBOL_CACHE_SIZE - 1);

			process_duration(rmt_state, zero, false);
			process_duration(rmt_state, one, true);
			// ESP_LOGI(TAG, " %d  0:%" PRId16 ",1:%" PRId16, symbol_cache.tail, zero, one);
		}
		if (symbol_cache.overflow) {
			ESP_LOGE(TAG, "%" PRId32 " bytes were lost due to overflow", symbol_cache.overflow);
			symbol_cache.overflow = 0;
		}
		if (rmt_state->buffer_length > 0) {
			swo_post(rmt_state->ring_buffer, rmt_state->buffer_length);
		}
	}
}

void swo_manchester_init(void)
{
	if (rx_channel != NULL) {
		ESP_LOGI(TAG, "swo manchester already initialized -- skipping reinitialization");
		return;
	}

	rmt_rx_channel_config_t rx_channel_cfg = {
		.clk_src = RMT_CLK_SRC_DEFAULT,
		.resolution_hz = SWO_MANCHESTER_FREQ_HZ,
		.mem_block_symbols = SWO_MANCHESTER_WORDS,
		.gpio_num = CONFIG_TDO_GPIO,
	};

	ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_channel_cfg, &rx_channel));

	ESP_LOGI(TAG, "register RX done callback");
	if (!rmt_state.receive_queue) {
		rmt_state.receive_queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
		assert(rmt_state.receive_queue);
	}
	rmt_rx_event_callbacks_t cbs = {
		.on_recv_done = swo_rmt_rx_done_callback,
	};
	ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(rx_channel, &cbs, &rmt_state));

	ESP_ERROR_CHECK(rmt_enable(rx_channel));

	// ready to receive
	if (!rx_pid) {
		xTaskCreate(swo_manchester_rx_task, "swo_m_rx_task", 4096, &rmt_state, 10, &rx_pid);
	}
}

void swo_manchester_deinit(void)
{
	if (!rx_channel) {
		ESP_LOGE(TAG, "manchester mode not initialized");
	}
	if (rx_pid) {
		ESP_ERROR_CHECK(rmt_disable(rx_channel));
		ESP_ERROR_CHECK(rmt_del_channel(rx_channel));
		vTaskDelete(rx_pid);
		rx_pid = NULL;
	}
	rx_channel = NULL;
}
