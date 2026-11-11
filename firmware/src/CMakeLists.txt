idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "." "../include"
    REQUIRES
        nvs_flash
        esp_netif
        esp_event
        esp_wifi
        esp_http_server
        fatfs
        sdmmc
        driver
        esp_timer
)
