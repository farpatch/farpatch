#include "esp_log.h"
#include "mdns.h"
#include "sdkconfig.h"
#include "wifi.h"

static const char *TAG = CONFIG_PRODUCT_NAME "-mdns";

void wilma_unique_words(const char **name1, const char **name2);

void initialise_mdns(const char *hostname)
{
	char generated_instance[64];
	ESP_ERROR_CHECK(mdns_init());
	if (hostname == NULL) {
		char generated_hostname[32];
		const char *name1;
		const char *name2;
		wilma_unique_words(&name1, &name2);
		snprintf(generated_hostname, sizeof(generated_hostname) - 1, CONFIG_PRODUCT_NAME "-%s-%s", name1, name2);
		ESP_ERROR_CHECK(mdns_hostname_set(generated_hostname));
		snprintf(generated_instance, sizeof(generated_instance) - 1, "%s (%s %s)", CONFIG_DEFAULT_AP_SSID_PREFIX, name1,
			name2);
		ESP_LOGI(TAG, "mdns hostname for \"%s\" set to: %s.local", generated_instance, generated_hostname);
	} else {
		//set mDNS hostname (required if you want to advertise services)
		ESP_ERROR_CHECK(mdns_hostname_set(hostname));
		strncpy(generated_instance, hostname, sizeof(generated_instance) - 1);
		ESP_LOGI(TAG, "mdns hostname for \"%s\" set to: %s.local", generated_instance, hostname);
	}
	//set default mDNS instance name
	ESP_ERROR_CHECK(mdns_instance_name_set(CONFIG_DEFAULT_AP_SSID_PREFIX " Debugger"));

	//structure with TXT records
	mdns_txt_item_t serviceTxtData[3] = {{"board", CONFIG_PRODUCT_NAME}, {"u", "user"}, {"p", "password"}};

	//initialize service
	ESP_ERROR_CHECK(mdns_service_add(generated_instance, "_http", "_tcp", 80, serviceTxtData, 3));
	ESP_ERROR_CHECK(mdns_service_subtype_add_for_host(generated_instance, "_http", "_tcp", NULL, "_server"));

	// Advertise GDB
	ESP_ERROR_CHECK(mdns_service_add(generated_instance, "_gdb", "_tcp", CONFIG_GDB_TCP_PORT, serviceTxtData, 1));
	ESP_ERROR_CHECK(mdns_service_subtype_add_for_host(generated_instance, "_gdb", "_tcp", NULL, "_server"));

	// Advertise the BMDA network port (which is the same as the GDB port)
	ESP_ERROR_CHECK(mdns_service_add(generated_instance, "_bmda", "_tcp", CONFIG_GDB_TCP_PORT, serviceTxtData, 1));
	ESP_ERROR_CHECK(mdns_service_subtype_add_for_host(generated_instance, "_bmda", "_tcp", NULL, "_server"));

	// If RTT is configured, advertise that port as well
	if (CONFIG_RTT_TCP_PORT > 0) {
		ESP_ERROR_CHECK(mdns_service_add(generated_instance, "_rtt", "_tcp", CONFIG_RTT_TCP_PORT, serviceTxtData, 1));
		ESP_ERROR_CHECK(mdns_service_subtype_add_for_host(generated_instance, "_rtt", "_tcp", NULL, "_server"));
	}

#if !defined(CONFIG_TARGET_UART_NONE)
	ESP_ERROR_CHECK(mdns_service_add(generated_instance, "_uart", "_tcp", CONFIG_UART_TCP_PORT, serviceTxtData, 1));
	ESP_ERROR_CHECK(mdns_service_subtype_add_for_host(generated_instance, "_uart", "_tcp", NULL, "_server"));
#endif

	if (CONFIG_SWO_TCP_PORT != -1) {
		ESP_ERROR_CHECK(mdns_service_add(generated_instance, "_swo", "_tcp", CONFIG_SWO_TCP_PORT, serviceTxtData, 1));
		ESP_ERROR_CHECK(mdns_service_subtype_add_for_host(generated_instance, "_swo", "_tcp", NULL, "_server"));
	}
}
