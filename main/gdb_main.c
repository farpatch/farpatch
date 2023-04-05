

#include <assert.h>
#include <inttypes.h>
#include <lwip/err.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <lwip/sys.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "exception.h"
#include "gdb_if.h"
#include "gdb_main_farpatch.h"
#include "gdb_main.h"
#include "gdb_packet.h"
#include "general.h"
#include "morse.h"
#include "rtt.h"

static int num_clients;

char *gdb_packet_buffer(void)
{
	void **ptr = (void **)pvTaskGetThreadLocalStoragePointer(NULL, GDB_TLS_INDEX);
	assert(ptr);
	struct bmp_wifi_instance *instance_ptr = *ptr;
	return instance_ptr->rx_buf;
}

struct exception **get_innermost_exception(void)
{
	void **ptr = (void **)pvTaskGetThreadLocalStoragePointer(NULL, GDB_TLS_INDEX);
	assert(ptr);
	return (struct exception **)&ptr[1];
}

static void gdb_wifi_destroy(struct bmp_wifi_instance *instance)
{
	ESP_LOGI("gdb", "destroy %d", instance->sock);
	num_clients--;

	close(instance->sock);

	TaskHandle_t pid = instance->pid;
	free(instance);
	vTaskDelete(pid);
}

#define MAYBE_SLEEP(last, current)                                                                  \
	do {                                                                                            \
		current = xTaskGetTickCount();                                                              \
		if ((current - last) > pdMS_TO_TICKS(500)) {                                                \
			/*ESP_LOGI("bmp", "delaying because last delay was %ld ticks ago", (current - last));*/ \
			last = current;                                                                         \
			vTaskDelay(2);                                                                          \
		}                                                                                           \
	} while (0)

static void gdb_wifi_task(void *arg)
{
	struct bmp_wifi_instance *instance = (struct bmp_wifi_instance *)arg;

	void *tls[2] = {};
	tls[0] = arg;
	vTaskSetThreadLocalStoragePointer(NULL, GDB_TLS_INDEX, tls); // used for exception handling

	ESP_LOGI("gdb", "Started task %d this:%p tlsp:%p", instance->sock, instance, tls);

	int opt = 1;
	setsockopt(instance->sock, IPPROTO_TCP, TCP_NODELAY, (void *)&opt, sizeof(opt));
	opt = 1; /* SO_KEEPALIVE */
	setsockopt(instance->sock, SOL_SOCKET, SO_KEEPALIVE, (void *)&opt, sizeof(opt));
	opt = 3; /* s TCP_KEEPIDLE */
	setsockopt(instance->sock, IPPROTO_TCP, TCP_KEEPIDLE, (void *)&opt, sizeof(opt));
	opt = 1; /* s TCP_KEEPINTVL */
	setsockopt(instance->sock, IPPROTO_TCP, TCP_KEEPINTVL, (void *)&opt, sizeof(opt));
	opt = 3; /* TCP_KEEPCNT */
	setsockopt(instance->sock, IPPROTO_TCP, TCP_KEEPCNT, (void *)&opt, sizeof(opt));
	opt = 1;

	num_clients++;

	char *pbuf = instance->rx_buf;

	TickType_t last_sleep = xTaskGetTickCount();
	TickType_t current_sleep = last_sleep;
	while (true) {
		volatile struct exception e;
		TRY_CATCH (e, EXCEPTION_ALL) {
			SET_IDLE_STATE(0);
			while (gdb_target_running && cur_target) {
				gdb_poll_target();

				// Check again, as `gdb_poll_target()` may
				// alter these variables.
				if (!gdb_target_running || !cur_target) {
					break;
				}
				char c = (char)gdb_if_getchar_to(0);
				if (c == '\x03' || c == '\x04') {
					target_halt_request(cur_target);
				}
				MAYBE_SLEEP(last_sleep, current_sleep);
				if (rtt_enabled)
					poll_rtt(cur_target);
			}

			SET_IDLE_STATE(1);
			size_t size = gdb_getpacket(pbuf, GDB_PACKET_BUFFER_SIZE);
			// If port closed and target detached, stay idle
			if ((pbuf[0] != 0x04) || cur_target) {
				SET_IDLE_STATE(0);
			}
			gdb_main(pbuf, sizeof(instance->rx_buf), size);
			MAYBE_SLEEP(last_sleep, current_sleep);
		}
		if (e.type == EXCEPTION_NETWORK) {
			ESP_LOGE("exception", "network exception -- exiting: %s", e.msg);
			target_list_free();
			break;
		}
		if (e.type) {
			gdb_putpacketz("EFF");
			target_list_free();
			morse("TARGET LOST.", 1);
		}
	}

	gdb_wifi_destroy(instance);
}

static void new_bmp_wifi_instance(int sock)
{
	char name[CONFIG_FREERTOS_MAX_TASK_NAME_LEN];
	snprintf(name, sizeof(name) - 1, "gdbc fd:%" PRId16, sock);

	struct bmp_wifi_instance *instance = malloc(sizeof(struct bmp_wifi_instance));
	if (!instance) {
		return;
	}

	memset(instance, 0, sizeof(*instance));
	instance->sock = sock;

	xTaskCreate(gdb_wifi_task, name, 12000, (void *)instance, tskIDLE_PRIORITY + 1, &instance->pid);
}

void gdb_net_task(void *arg)
{
	struct sockaddr_in addr;
	int gdb_if_serv;
	int opt;

	addr.sin_family = AF_INET;
	addr.sin_port = htons(CONFIG_TCP_PORT);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	assert((gdb_if_serv = socket(PF_INET, SOCK_STREAM, 0)) != -1);
	opt = 1;
	assert(setsockopt(gdb_if_serv, SOL_SOCKET, SO_REUSEADDR, (void *)&opt, sizeof(opt)) != -1);
	assert(setsockopt(gdb_if_serv, IPPROTO_TCP, TCP_NODELAY, (void *)&opt, sizeof(opt)) != -1);

	assert(bind(gdb_if_serv, (struct sockaddr *)&addr, sizeof(addr)) != -1);
	assert(listen(gdb_if_serv, 1) != -1);

	ESP_LOGI("gdb", "Listening on TCP:%d", CONFIG_TCP_PORT);

	while (1) {
		int s = accept(gdb_if_serv, NULL, NULL);
		if (s > 0) {
			new_bmp_wifi_instance(s);
		}
	}
}
