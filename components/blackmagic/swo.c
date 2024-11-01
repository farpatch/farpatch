/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2012  Black Sphere Technologies Ltd.
 * Written by Gareth McMullin <gareth@blacksphere.co.nz>
 *
 * Copyright (C) 2014 Fredrik Ahlberg <fredrik@z80.se>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* This file implements capture of the swo output.
 *
 * ARM DDI 0403D - ARMv7M Architecture Reference Manual
 * ARM DDI 0337I - Cortex-M3 Technical Reference Manual
 * ARM DDI 0314H - CoreSight Components Technical Reference Manual
 */

#include "general.h"

#include <esp_clk_tree.h>
#include <esp_log.h>
#include <esp_task_wdt.h>
#include <gdb_packet.h>
#include <inttypes.h>
#include <lwip/err.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <lwip/sys.h>
#include "platform.h"
#include "sdkconfig.h"
#include "swo.h"
#include "swo-manchester.h"
#include "swo-uart.h"

static const char TAG[] = "swo";

/* Current SWO decoding mode being used */
swo_coding_e swo_current_mode;

/* Whether ITM decoding is engaged */
bool swo_itm_decoding = false;

static uint8_t itm_decoded_buffer[64];
static uint16_t itm_decoded_buffer_index = 0;
static uint32_t itm_decode_mask = 0;  /* bitmask of channels to print */
static uint8_t itm_packet_length = 0; /* decoder state */
static bool itm_decode_packet = false;

// A list of clients to send SWO traffic to.
static int swo_clients[16] = {};
static int swo_client_count = 0;

static uint16_t swo_itm_decode(const uint8_t *data, uint16_t len)
{
	/* Step through each byte in the SWO data buffer */
	for (uint16_t idx = 0; idx < len; ++idx) {
		/* If we're waiting for a new ITM packet, start decoding the new byte as a header */
		if (itm_packet_length == 0) {
			/* Check that the required to be 0 bit of the SWIT packet is, and that the size bits aren't 0 */
			if ((data[idx] & 0x04U) == 0U && (data[idx] & 0x03U) != 0U) {
				/* Now extract the stimulus port address (stream number) and payload size */
				uint8_t stream = data[idx] >> 3U;
				/* Map 1 -> 1, 2 -> 2, and 3 -> 4 */
				itm_packet_length = 1U << ((data[idx] & 3U) - 1U);
				/* Determine if the packet should be displayed */
				itm_decode_packet = (itm_decode_mask & (1U << stream)) != 0U;
			} else {
				/* If the bit is not 0, this is an invalid SWIT packet, so reset state */
				itm_decode_packet = false;
				itm_decoded_buffer_index = 0;
			}
		} else {
			/* If we should actually decode this packet, then forward the data to the decoded data buffer */
			if (itm_decode_packet) {
				itm_decoded_buffer[itm_decoded_buffer_index++] = data[idx];
				/* If the buffer has filled up and needs flushing, try to flush the data to the serial endpoint */
				if (itm_decoded_buffer_index == sizeof(itm_decoded_buffer)) {
					/* However, if the link is not yet up, drop the packet data silently */
					// if (usb_get_config() && gdb_serial_get_dtr())
					// 	debug_serial_send_stdout(itm_decoded_buffer, itm_decoded_buffer_index);
					itm_decoded_buffer_index = 0U;
				}
			}
			/* Mark the byte consumed regardless */
			--itm_packet_length;
		}
	}
	// If there is data in the buffer, send it to any listening TCP connections
	return len;
}

void swo_post(void *data, size_t len)
{
	for (int i = 0; i < (sizeof(swo_clients) / sizeof(*swo_clients)); i += 1) {
		if (swo_clients[i] <= 0) {
			continue;
		}
		if (send(swo_clients[i], data, len, 0) <= 0) {
			close(swo_clients[i]);
			swo_clients[i] = 0;
		}
	}
}

void swo_listen_task(void *ignored)
{
	int swo_server;

	(void)ignored;
	if (CONFIG_SWO_TCP_PORT == -1) {
		return;
	}
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(CONFIG_SWO_TCP_PORT);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	assert((swo_server = socket(PF_INET, SOCK_STREAM, 0)) != -1);
	int opt = 1;
	assert(setsockopt(swo_server, SOL_SOCKET, SO_REUSEADDR, (void *)&opt, sizeof(opt)) != -1);
	assert(setsockopt(swo_server, IPPROTO_TCP, TCP_NODELAY, (void *)&opt, sizeof(opt)) != -1);

	assert(bind(swo_server, (struct sockaddr *)&addr, sizeof(addr)) != -1);
	assert(listen(swo_server, 1) != -1);

	ESP_LOGI(TAG, "swo server listening on port %d", CONFIG_SWO_TCP_PORT);

	while (1) {
		int s = accept(swo_server, NULL, NULL);
		if (s <= 0) {
			continue;
		}

		// Look for a free slot in the connection array.
		bool found = false;
		for (int i = 0; i < (sizeof(swo_clients) / sizeof(*swo_clients)); i += 1) {
			if (swo_clients[i] == 0) {
				swo_clients[i] = s;
				swo_client_count += 1;
				found = true;
				break;
			}
		}
		if (!found) {
			ESP_LOGE("swo", "unable to accept connection %d because connection table is full", s);
			close(s);
		}
	}
}

void swo_baud(unsigned int baud)
{
#if SWO_ENCODING == 2 || SWO_ENCODING == 3
	if (swo_current_mode == swo_nrz_uart) {
		swo_uart_set_baudrate(baud);
	}
#endif
}

void swo_deinit(const bool deallocate)
{
#if SWO_ENCODING == 1 || SWO_ENCODING == 3
	if (swo_current_mode == swo_manchester)
		swo_manchester_deinit();
#endif
#if SWO_ENCODING == 2 || SWO_ENCODING == 3
	if (swo_current_mode == swo_nrz_uart) {
		swo_uart_deinit();
	}
#endif
	swo_current_mode = swo_none;
}

void swo_init(const swo_coding_e swo_mode, const uint32_t baudrate, const uint32_t itm_stream_bitmask)
{
#if SWO_ENCODING == 1
	(void)baudrate;
#endif

	/* Configure the ITM decoder and state */
	itm_decode_mask = itm_stream_bitmask;
	swo_itm_decoding = itm_stream_bitmask != 0;

	/* Now determine which mode to enable and initialise it */
#if SWO_ENCODING == 1 || SWO_ENCODING == 3
	if (swo_mode == swo_manchester)
		swo_manchester_init();
#endif
#if SWO_ENCODING == 2 || SWO_ENCODING == 3
	if (swo_mode == swo_nrz_uart) {
		/* Ensure the baud rate is something sensible */
		swo_uart_init(baudrate ? baudrate : SWO_DEFAULT_BAUD);
		uint32_t detected_baudrate = swo_uart_get_baudrate();
		gdb_outf("Baudrate: %" PRIu32 " ", detected_baudrate);
		ESP_LOGI(TAG, "starting SWO with baudrate %" PRId32, detected_baudrate);
	}
#endif
	/* Make a note of which mode we initialised into */
	swo_current_mode = swo_mode;
}
