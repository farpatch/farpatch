/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2012 Black Sphere Technologies Ltd.
 * Written by Gareth McMullin <gareth@blacksphere.co.nz>
 * Copyright (C) 2024 1BitSquared <info@1bitsquared.com>
 * Modified by Rachel Mant <git@dragonmux.network>
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

#ifndef PLATFORMS_COMMON_SWO_H
#define PLATFORMS_COMMON_SWO_H

/* Default to a baudrate of 0, which means "autobaud" */
#define SWO_DEFAULT_BAUD 0

typedef enum swo_coding {
	swo_none,
	swo_manchester,
	swo_nrz_uart,
} swo_coding_e;

extern swo_coding_e swo_current_mode;

/* Initialisation and deinitialisation functions (ties into command.c) */
void swo_init(swo_coding_e swo_mode, uint32_t baudrate, uint32_t itm_stream_bitmask);
void swo_deinit(bool deallocate);

/* Send SWO data to anyone who's listening */
void swo_post(const uint8_t *data, size_t len);

#endif /* PLATFORMS_COMMON_SWO_H */
