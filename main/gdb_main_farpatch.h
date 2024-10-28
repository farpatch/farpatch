#ifndef GDB_MAIN_FARPATCH_H_
#define GDB_MAIN_FARPATCH_H_

#include <freertos/FreeRTOS.h>
#include <gdb_main.h>

#define EXCEPTION_NETWORK 0x40
#define EXCEPTION_MUTEX   0x41

#define GDB_TLS_INDEX 1
#define EXCEPTION_TLS_INDEX 2

struct gdb_wifi_instance {
	uint32_t magic;
	int sock;
	uint8_t tx_buf[1028];
	uint8_t rx_buf[1028];
	char pbuf[GDB_PACKET_BUFFER_SIZE + 4];
	uint16_t tx_bufsize;
	uint16_t rx_bufsize;
	uint16_t rx_bufpos;
	bool no_ack_mode;
	bool is_shutting_down;
	TaskHandle_t pid;
};

#endif /* GDB_MAIN_FARPATCH_H_ */
