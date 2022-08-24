#include "general.h"
#include <esp_wifi.h>
#include <esp_mac.h>

char *serial_no_read(char *s)
{
	uint64_t chipid;
	esp_read_mac((uint8_t *)&chipid, ESP_MAC_WIFI_SOFTAP);
	memset(s, 0, DFU_SERIAL_LENGTH);
	snprintf(s, DFU_SERIAL_LENGTH - 1, "FP-%06" PRIX32, (uint32_t)chipid);
    return s;
}
