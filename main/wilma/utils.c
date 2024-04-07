#include <esp_mac.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/// Externally generated dictionary of words used for generating
/// the unique, per-device name.
extern const char *word_list[];

static SemaphoreHandle_t WILMA_JSON_MUTEX = NULL;

void wilma_unique_words(const char **name1, const char **name2)
{
    uint8_t chipid[8];
    esp_read_mac(chipid, ESP_MAC_WIFI_SOFTAP);
    uint32_t chip_hi_idx = chipid[5] | ((chipid[4] & 3) << 8);
    uint32_t chip_lo_idx = (chipid[4] >> 2) | ((chipid[3] & 15) << 6);
    *name1 = word_list[chip_hi_idx];
    *name2 = word_list[chip_lo_idx];
}

int wilma_update_wifi_ssid(void *ssid)
{
	const char *chip_hi;
	const char *chip_lo;
	wilma_unique_words(&chip_hi, &chip_lo);
    memset(ssid, 0, 32);
	return snprintf(ssid, 32, "%s (%s %s)", CONFIG_DEFAULT_AP_SSID_PREFIX, chip_hi, chip_lo);
}

bool wilma_lock_json_buffer(TickType_t xTicksToWait)
{
    if (WILMA_JSON_MUTEX)
    {
        if (xSemaphoreTake(WILMA_JSON_MUTEX, xTicksToWait) == pdTRUE)
        {
            return true;
        }
        else
        {
            return false;
        }
    }
    else
    {
        return false;
    }
}

void wilma_unlock_json_buffer(void)
{
    xSemaphoreGive(WILMA_JSON_MUTEX);
}

void wilma_utils_init(void)
{
    WILMA_JSON_MUTEX = xSemaphoreCreateMutex();
}

void wilma_utils_cleanup(void)
{
    vSemaphoreDelete(WILMA_JSON_MUTEX);
}

char *wilma_reason_to_str(uint8_t reason)
{
    switch (reason)
    {
    case 1:
        return "WIFI_REASON_UNSPECIFIED";
    case 2:
        return "WIFI_REASON_AUTH_EXPIRE";
    case 3:
        return "WIFI_REASON_AUTH_LEAVE";
    case 4:
        return "WIFI_REASON_ASSOC_EXPIRE";
    case 5:
        return "WIFI_REASON_ASSOC_TOOMANY";
    case 6:
        return "WIFI_REASON_NOT_AUTHED";
    case 7:
        return "WIFI_REASON_NOT_ASSOCED";
    case 8:
        return "WIFI_REASON_ASSOC_LEAVE";
    case 9:
        return "WIFI_REASON_ASSOC_NOT_AUTHED";
    case 10:
        return "WIFI_REASON_DISASSOC_PWRCAP_BAD";
    case 11:
        return "WIFI_REASON_DISASSOC_SUPCHAN_BAD";
    case 12:
        return "WIFI_REASON_BSS_TRANSITION_DISASSOC";
    case 13:
        return "WIFI_REASON_IE_INVALID";
    case 14:
        return "WIFI_REASON_MIC_FAILURE";
    case 15:
        return "WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT";
    case 16:
        return "WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT";
    case 17:
        return "WIFI_REASON_IE_IN_4WAY_DIFFERS";
    case 18:
        return "WIFI_REASON_GROUP_CIPHER_INVALID";
    case 19:
        return "WIFI_REASON_PAIRWISE_CIPHER_INVALID";
    case 20:
        return "WIFI_REASON_AKMP_INVALID";
    case 21:
        return "WIFI_REASON_UNSUPP_RSN_IE_VERSION";
    case 22:
        return "WIFI_REASON_INVALID_RSN_IE_CAP";
    case 23:
        return "WIFI_REASON_802_1X_AUTH_FAILED";
    case 24:
        return "WIFI_REASON_CIPHER_SUITE_REJECTED";
    case 25:
        return "WIFI_REASON_TDLS_PEER_UNREACHABLE";
    case 26:
        return "WIFI_REASON_TDLS_UNSPECIFIED";
    case 27:
        return "WIFI_REASON_SSP_REQUESTED_DISASSOC";
    case 28:
        return "WIFI_REASON_NO_SSP_ROAMING_AGREEMENT";
    case 29:
        return "WIFI_REASON_BAD_CIPHER_OR_AKM";
    case 30:
        return "WIFI_REASON_NOT_AUTHORIZED_THIS_LOCATION";
    case 31:
        return "WIFI_REASON_SERVICE_CHANGE_PERCLUDES_TS";
    case 32:
        return "WIFI_REASON_UNSPECIFIED_QOS";
    case 33:
        return "WIFI_REASON_NOT_ENOUGH_BANDWIDTH";
    case 34:
        return "WIFI_REASON_MISSING_ACKS";
    case 35:
        return "WIFI_REASON_EXCEEDED_TXOP";
    case 36:
        return "WIFI_REASON_STA_LEAVING";
    case 37:
        return "WIFI_REASON_END_BA";
    case 38:
        return "WIFI_REASON_UNKNOWN_BA";
    case 39:
        return "WIFI_REASON_TIMEOUT";
    case 46:
        return "WIFI_REASON_PEER_INITIATED";
    case 47:
        return "WIFI_REASON_AP_INITIATED";
    case 48:
        return "WIFI_REASON_INVALID_FT_ACTION_FRAME_COUNT";
    case 49:
        return "WIFI_REASON_INVALID_PMKID";
    case 50:
        return "WIFI_REASON_INVALID_MDE";
    case 51:
        return "WIFI_REASON_INVALID_FTE";
    case 67:
        return "WIFI_REASON_TRANSMISSION_LINK_ESTABLISH_FAILED";
    case 68:
        return "WIFI_REASON_ALTERATIVE_CHANNEL_OCCUPIED";
    case 200:
        return "WIFI_REASON_BEACON_TIMEOUT";
    case 201:
        return "WIFI_REASON_NO_AP_FOUND";
    case 202:
        return "WIFI_REASON_AUTH_FAIL";
    case 203:
        return "WIFI_REASON_ASSOC_FAIL";
    case 204:
        return "WIFI_REASON_HANDSHAKE_TIMEOUT";
    case 205:
        return "WIFI_REASON_CONNECTION_FAIL";
    case 206:
        return "WIFI_REASON_AP_TSF_RESET";
    case 207:
        return "WIFI_REASON_ROAMING";
    case 208:
        return "WIFI_REASON_ASSOC_COMEBACK_TIME_TOO_LONG";
    case 209:
        return "WIFI_REASON_SA_QUERY_TIMEOUT";
    default:
        return "unknown";
    }
}

// Based on cJSON
bool wilma_json_print_string(const unsigned char *input, unsigned char *output_buffer)
{
    const unsigned char *input_pointer = NULL;
    unsigned char *output = NULL;
    unsigned char *output_pointer = NULL;
    size_t output_length = 0;
    /* numbers of additional characters needed for escaping */
    size_t escape_characters = 0;

    if (output_buffer == NULL)
    {
        return false;
    }

    /* empty string */
    if (input == NULL)
    {
        // output = ensure(output_buffer, sizeof("\"\""), hooks);
        if (output == NULL)
        {
            return false;
        }
        strcpy((char *)output, "\"\"");

        return true;
    }

    /* set "flag" to 1 if something needs to be escaped */
    for (input_pointer = input; *input_pointer; input_pointer++)
    {
        if (strchr("\"\\\b\f\n\r\t", *input_pointer))
        {
            /* one character escape sequence */
            escape_characters++;
        }
        else if (*input_pointer < 32)
        {
            /* UTF-16 escape sequence uXXXX */
            escape_characters += 5;
        }
    }
    output_length = (size_t)(input_pointer - input) + escape_characters;

    /* in the original cJSON it is possible to realloc here in case output buffer is too small.
     * This is overkill for an embedded system. */
    output = output_buffer;

    /* no characters have to be escaped */
    if (escape_characters == 0)
    {
        output[0] = '\"';
        memcpy(output + 1, input, output_length);
        output[output_length + 1] = '\"';
        output[output_length + 2] = '\0';

        return true;
    }

    output[0] = '\"';
    output_pointer = output + 1;
    /* copy the string */
    for (input_pointer = input; *input_pointer != '\0'; (void)input_pointer++, output_pointer++)
    {
        if ((*input_pointer > 31) && (*input_pointer != '\"') && (*input_pointer != '\\'))
        {
            /* normal character, copy */
            *output_pointer = *input_pointer;
        }
        else
        {
            /* character needs to be escaped */
            *output_pointer++ = '\\';
            switch (*input_pointer)
            {
            case '\\':
                *output_pointer = '\\';
                break;
            case '\"':
                *output_pointer = '\"';
                break;
            case '\b':
                *output_pointer = 'b';
                break;
            case '\f':
                *output_pointer = 'f';
                break;
            case '\n':
                *output_pointer = 'n';
                break;
            case '\r':
                *output_pointer = 'r';
                break;
            case '\t':
                *output_pointer = 't';
                break;
            default:
                /* escape and print as unicode codepoint */
                sprintf((char *)output_pointer, "u%04x", *input_pointer);
                output_pointer += 4;
                break;
            }
        }
    }
    output[output_length + 1] = '\"';
    output[output_length + 2] = '\0';

    return true;
}
