/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2011  Black Sphere Technologies Ltd.
 * Written by Gareth McMullin <gareth@blacksphere.co.nz>
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

/* This file implements a transparent channel over which the GDB Remote
 * Serial Debugging protocol is implemented.  This implementation for Linux
 * uses a TCP server on port 2022.
 */
#include <stdio.h>

#include <lwip/err.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <lwip/sys.h>

#include "esp_log.h"

#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>

#include "gdb_if.h"
#include "gdb_main_farpatch.h"
#include "gdb_packet.h"

#include "exception.h"
#include "general.h"
#include "hex_utils.h"
#include "target.h"

#include <string.h>
#include <assert.h>

#define GDB_TLS_INDEX     1
#define EXCEPTION_NETWORK 0x40

static IRAM_ATTR unsigned char gdb_wifi_if_getchar(struct gdb_wifi_instance *instance)
{
	if (instance->is_shutting_down) {
		return 0;
	}

	if (instance->rx_bufpos < instance->rx_bufsize) {
		return instance->rx_buf[instance->rx_bufpos++];
	}

	instance->rx_bufpos = 0;
	instance->rx_bufsize = recv(instance->sock, instance->rx_buf, sizeof(instance->rx_buf), 0);
	if (instance->rx_bufsize <= 0) {
		instance->is_shutting_down = true;
		close(instance->sock);
		raise_exception(EXCEPTION_NETWORK, "error on getchar");
		// should not be reached
		return 0;
	}
	return instance->rx_buf[instance->rx_bufpos++];
}

static IRAM_ATTR unsigned char gdb_wifi_if_getchar_to(struct gdb_wifi_instance *instance, int timeout)
{
	if (instance->is_shutting_down) {
		return 0xff;
	}
	fd_set fds;
	struct timeval tv;

	tv.tv_sec = timeout / 1000;
	tv.tv_usec = (timeout % 1000) * 1000;

	FD_ZERO(&fds);
	FD_SET(instance->sock, &fds);

	int ret = select(instance->sock + 1, &fds, NULL, NULL, (timeout >= 0) ? &tv : NULL);
	if (ret > 0) {
		char c = gdb_wifi_if_getchar(instance);
		return c;
	}

	if (ret < 0) {
		instance->is_shutting_down = true;
		close(instance->sock);
		raise_exception(EXCEPTION_NETWORK, "error on getchar_to");
	}
	return 0xFF;
}

static IRAM_ATTR void gdb_wifi_if_putchar(struct gdb_wifi_instance *instance, unsigned char c, int flush)
{
	// ESP_LOGI("putchar", "instance: %p putting: %c (%d?) %d/%d", instance, c, flush, instance->tx_bufsize, sizeof(instance->tx_buf));
	if (instance->is_shutting_down) {
		return;
	}

	instance->tx_buf[instance->tx_bufsize++] = c;
	if (flush || (instance->tx_bufsize >= sizeof(instance->tx_buf))) {
		if (instance->sock > 0) {
			int ret = send(instance->sock, instance->tx_buf, instance->tx_bufsize, 0);
			if (ret <= 0) {
				instance->is_shutting_down = true;
				close(instance->sock);
				raise_exception(EXCEPTION_NETWORK, "error on putchar");
				// should not be reached
				return;
			}
		}
		instance->tx_bufsize = 0;
	}
}

IRAM_ATTR bool verify_magic(struct gdb_wifi_instance *bmp)
{
	assert(bmp->magic == 0x55239912);
	return bmp->magic == 0x55239912;
}

IRAM_ATTR unsigned char gdb_if_getchar_to(int timeout)
{
	void *ptr = pvTaskGetThreadLocalStoragePointer(NULL, GDB_TLS_INDEX);
	assert(ptr);
	verify_magic(ptr);
	return gdb_wifi_if_getchar_to(ptr, timeout);
}

IRAM_ATTR unsigned char gdb_if_getchar(void)
{
	void *ptr = pvTaskGetThreadLocalStoragePointer(NULL, GDB_TLS_INDEX);
	assert(ptr);
	verify_magic(ptr);
	return gdb_wifi_if_getchar(ptr);
}

IRAM_ATTR void gdb_if_putchar(unsigned char c, int flush)
{
	void *ptr = pvTaskGetThreadLocalStoragePointer(NULL, GDB_TLS_INDEX);
	assert(ptr);
	verify_magic(ptr);
	gdb_wifi_if_putchar(ptr, c, flush);
}

IRAM_ATTR void gdb_target_printf(struct target_controller *tc, const char *fmt, va_list ap)
{
	(void)tc;
	gdb_voutf(fmt, ap);
}
