#include <freertos/FreeRTOS.h>

#include <assert.h>
#include <inttypes.h>
#include <lwip/err.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <lwip/sys.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "cortexm.h"
#include "exception.h"
#include "gdb_if.h"
#include "gdb_main_farpatch.h"
#include "gdb_main.h"
#include "gdb_packet.h"
#include "general.h"
#include "morse.h"
#include "platform.h"
#include "rtt.h"
#include "target.h"
#include "target_internal.h"

static SemaphoreHandle_t bmp_core_mutex = NULL;
static volatile int num_clients;
static volatile bool gdb_is_free = true;

uint32_t poll_rtt_ms(void);

void bmp_core_lock(void)
{
	while (!xSemaphoreTake(bmp_core_mutex, pdMS_TO_TICKS(300))) {
		ESP_LOGE("gdb", "Failed to take BMP core mutex after 300 ms! Trying again...");
	}
}

void bmp_core_unlock(void)
{
	if (xSemaphoreGive(bmp_core_mutex) != pdTRUE) {
		ESP_LOGE("gdb", "Failed to give BMP core mutex!");
	}
}

IRAM_ATTR char *gdb_packet_buffer(void)
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
	num_clients -= 1;

	if (instance->sock != -1) {
		close(instance->sock);
	}
	gdb_is_free = true;

	free(instance);
	vTaskDelete(NULL);
}

static void cortexm_vector_disable(target_s *target, uint32_t vectors) {
	// This function only makes sense for cortex targets
	if (!target_is_cortexm(target)) {
		return;
	}
	uint32_t existing = cortexm_demcr_read(target);
	uint32_t updated = existing & ~vectors;
	if (updated != existing) {
		cortexm_demcr_write(target, updated);
	}
}

static void IRAM_ATTR gdb_wifi_task(void *arg)
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

	num_clients += 1;

	char *pbuf = instance->rx_buf;

	while (!gdb_is_free) {
		platform_delay(10);
	}
	gdb_is_free = false;

	if (gdb_target_running && cur_target) {
		target_halt_request(cur_target);
		gdb_target_running = false;
	}

	target_s *prev_target = cur_target;

	while (true) {
		TRY(EXCEPTION_ALL)
		{
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
			// If the target changes, re-update the vectors.
			if (cur_target != prev_target) {
				uint32_t vectors_to_disable = 0
#if !defined(CONFIG_CATCH_CORE_RESET)
	| CORTEXM_DEMCR_VC_CORERESET
#endif
#if !defined(CONFIG_CATCH_CORE_HARDFAULT)
	| CORTEXM_DEMCR_VC_HARDERR
#endif
				;
				if (vectors_to_disable) {
					cortexm_vector_disable(cur_target, vectors_to_disable);
				}
				prev_target = cur_target;
			}
		}
		CATCH()
		{
		case EXCEPTION_NETWORK:
			ESP_LOGE("gdb", "network exception -- exiting: %s", exception_frame.msg);
			TRY(EXCEPTION_ALL)
			{
				target_list_free();
			}
			CATCH()
			{
			default:
				ESP_LOGE("gdb", "exception freeing target list");
				break;
			}
			gdb_wifi_destroy(instance);
			return;
		default:
			gdb_putpacketz("EFF");
			target_list_free();
			morse("TARGET LOST.", 1);
			gdb_wifi_destroy(instance);
			return;
		}
	}
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

	xTaskCreate(gdb_wifi_task, name, 5000, (void *)instance, tskIDLE_PRIORITY + 2, &instance->pid);
}

void gdb_net_task(void *arg)
{
	struct sockaddr_in addr;
	int gdb_if_serv;
	int opt;

	bmp_core_mutex = xSemaphoreCreateMutex();

	addr.sin_family = AF_INET;
	addr.sin_port = htons(CONFIG_GDB_TCP_PORT);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	assert((gdb_if_serv = socket(PF_INET, SOCK_STREAM, 0)) != -1);
	opt = 1;
	assert(setsockopt(gdb_if_serv, SOL_SOCKET, SO_REUSEADDR, (void *)&opt, sizeof(opt)) != -1);
	assert(setsockopt(gdb_if_serv, IPPROTO_TCP, TCP_NODELAY, (void *)&opt, sizeof(opt)) != -1);

	assert(bind(gdb_if_serv, (struct sockaddr *)&addr, sizeof(addr)) != -1);
	assert(listen(gdb_if_serv, 1) != -1);

	ESP_LOGI("gdb", "Listening on TCP:%d", CONFIG_GDB_TCP_PORT);

	while (1) {
		int s = accept(gdb_if_serv, NULL, NULL);
		if (s > 0) {
			new_bmp_wifi_instance(s);
		}
	}
}

#ifdef CONFIG_RTT_ON_BOOT
const char *target_halt_reason_str[] = {
	"RUNNING", /* Target not halted */
	"ERROR",   /* Failed to read target status */
	"REQUEST",
	"STEPPING",
	"BREAKPOINT",
	"WATCHPOINT",
	"FAULT",
};

void rtt_monitor_task(void *params)
{
	(void)params;
	extern target_controller_s gdb_controller;

	struct bmp_wifi_instance *instance = malloc(sizeof(struct bmp_wifi_instance));
	if (!instance) {
		return;
	}
	memset(instance, 0, sizeof(*instance));
	instance->sock = -1;
	instance->is_shutting_down = true; // Set this to `true` to prevent IO from occurring

	void *tls[2] = {};
	tls[0] = instance;
	vTaskSetThreadLocalStoragePointer(NULL, GDB_TLS_INDEX, tls); // used for exception handling

	while (true) {
		gdb_is_free = true;
		while (num_clients > 0) {
			platform_delay(700);
			continue;
		}
		gdb_is_free = false;
		TRY(EXCEPTION_ALL)
		{
			// Scan for the target
			if (!cur_target) {
				// TODO: Extend this to JTAG scan as well
				if (!adiv5_swd_scan(0)) {
					platform_delay(200);
					continue;
				}
				cur_target = target_attach_n(1, &gdb_controller);
				if (!cur_target) {
					platform_delay(200);
					continue;
				}
				// If we successfully attached, set the target running
				target_halt_resume(cur_target, false);
				gdb_target_running = true;
			}

			ESP_LOGI("rtt", "monitor attached to target");
			while (cur_target && (num_clients == 0)) {
				/* poll target */
				target_addr_t watch;
				target_halt_reason_e reason = target_halt_poll(cur_target, &watch);
				if (reason) {
					ESP_LOGI("rtt", "target halted: %s", target_halt_reason_str[reason]);
					gdb_target_running = false;
					if (cur_target) {
						target_halt_resume(cur_target, false);
						gdb_target_running = true;
					}
					break;
				}
				poll_rtt(cur_target);
				platform_delay(rtt_min_poll_ms);
			}
			// No target and no clients, delay for a bit
			if (num_clients == 0) {
				platform_delay(200);
			}
		}
		CATCH()
		{
		case EXCEPTION_NETWORK:
			ESP_LOGE("rtt", "network exception in RTT thread: %s", exception_frame.msg);
			target_list_free();
			target_detach(cur_target);
			cur_target = NULL;
			break;
		default:
			ESP_LOGE("rtt", "generic exception in RTT thread: %s", exception_frame.msg);
			target_list_free();
			target_detach(cur_target);
			cur_target = NULL;
			break;
		}
	}
}
#endif /* CONFIG_RTT_ON_BOOT */
