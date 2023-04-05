#ifndef GDB_MAIN_FARPATCH_H_
#define GDB_MAIN_FARPATCH_H_

#include <freertos/FreeRTOS.h>
#include <gdb_main.h>

#define EXCEPTION_NETWORK 0x40
#define EXCEPTION_MUTEX   0x41

#define GDB_TLS_INDEX 1

struct bmp_wifi_instance {
	int sock;
	int tx_bufsize;
	int unget;
	int index;
	TaskHandle_t pid;
	bool no_ack_mode;
	bool is_shutting_down;
	uint8_t tx_buf[1024];
	int rx_buf_index;
	char rx_buf[GDB_PACKET_BUFFER_SIZE + 1];
};

#endif /* GDB_MAIN_FARPATCH_H_ */
