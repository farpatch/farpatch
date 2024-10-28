
idf_component_register(
    REQUIRES app_update blackmagic driver esp_event esp_http_server esp_wifi frogfs nvs_flash soc ulp
    SRC_DIRS "."
             "wilma"
    INCLUDE_DIRS "."
                 "include"
                 "../components/blackmagic"
)
