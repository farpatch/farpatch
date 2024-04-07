#include "esp_log.h"
#include "mdns.h"
#include "wifi.h"

static const char *TAG = "farpatch-mdns";

void wilma_unique_words(const char **name1, const char **name2);

void initialise_mdns(const char *hostname)
{
	ESP_ERROR_CHECK(mdns_init());
	if (hostname == NULL) {
		char generated_hostname[32];
		const char *name1;
		const char *name2;
        wilma_unique_words(&name1, &name2);
        snprintf(generated_hostname, sizeof(generated_hostname) - 1, "farpatch-%s-%s", name1, name2);
		ESP_ERROR_CHECK(mdns_hostname_set(generated_hostname));
		ESP_LOGI(TAG, "mdns hostname set to: [%s]", generated_hostname);
	} else {
		//set mDNS hostname (required if you want to advertise services)
		ESP_ERROR_CHECK(mdns_hostname_set(hostname));
		ESP_LOGI(TAG, "mdns hostname set to: [%s]", hostname);
	}
	//set default mDNS instance name
	ESP_ERROR_CHECK(mdns_instance_name_set("Farpatch Debugger"));

	//structure with TXT records
	mdns_txt_item_t serviceTxtData[3] = {{"board", "esp32s3"}, {"u", "user"}, {"p", "password"}};

	//initialize service
	ESP_ERROR_CHECK(mdns_service_add("Farpatch-WebServer", "_http", "_tcp", 80, serviceTxtData, 3));
	ESP_ERROR_CHECK(mdns_service_subtype_add_for_host("Farpatch-WebServer", "_http", "_tcp", NULL, "_server"));
#if CONFIG_MDNS_MULTIPLE_INSTANCE
	ESP_ERROR_CHECK(mdns_service_add("ESP32-WebServer1", "_http", "_tcp", 80, NULL, 0));
#endif

	// #if CONFIG_MDNS_PUBLISH_DELEGATE_HOST
	//     char *delegated_hostname;
	//     if (-1 == asprintf(&delegated_hostname, "%s-delegated", hostname)) {
	//         abort();
	//     }

	//     mdns_ip_addr_t addr4, addr6;
	//     esp_netif_str_to_ip4("10.0.0.1", &addr4.addr.u_addr.ip4);
	//     addr4.addr.type = ESP_IPADDR_TYPE_V4;
	//     esp_netif_str_to_ip6("fd11:22::1", &addr6.addr.u_addr.ip6);
	//     addr6.addr.type = ESP_IPADDR_TYPE_V6;
	//     addr4.next = &addr6;
	//     addr6.next = NULL;
	//     ESP_ERROR_CHECK( mdns_delegate_hostname_add(delegated_hostname, &addr4) );
	//     ESP_ERROR_CHECK( mdns_service_add_for_host("test0", "_http", "_tcp", delegated_hostname, 1234, serviceTxtData, 3) );
	//     free(delegated_hostname);
	// #endif // CONFIG_MDNS_PUBLISH_DELEGATE_HOST

	//add another TXT item
	ESP_ERROR_CHECK(mdns_service_txt_item_set("_http", "_tcp", "path", "/foobar"));
	// //change TXT item value
	// ESP_ERROR_CHECK( mdns_service_txt_item_set_with_explicit_value_len("_http", "_tcp", "u", "admin", strlen("admin")) );
}
