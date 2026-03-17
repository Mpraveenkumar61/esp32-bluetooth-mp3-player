idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        bt
        fatfs
        sdmmc
        driver
        vfs
        esp_wifi
        esp_http_client
        esp_event
        tcp_transport
        esp_netif
        libmad
        esp_timer
)