
idf_component_register(
    REQUIRES soc nvs_flash ulp driver esp_http_server app_update esp_event esp_wifi blackmagic frogfs
    SRC_DIRS "."
             "wilma"
    INCLUDE_DIRS "."
                 "include"
                 "../components/blackmagic"
)
