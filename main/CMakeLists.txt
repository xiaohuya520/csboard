idf_component_register(
    SRCS "main.c"
         "cs_sys.c"
         "cs_data.c"
         "cs_net.c"
         "cs_portal.c"
         "cs_niko.c"
         "cs_avatar.c"
         "cs_avatar_data.c"
         "cs_qr_data.c"
         "cs_logos.c"
         "cs_logo_data.c"
         "cs_font_cn16.c"
    INCLUDE_DIRS "."
    REQUIRES bsp
             esp_event
             esp_hw_support
             esp_netif
             esp_timer
             esp_wifi
             nvs_flash
             esp_http_client
             esp_http_server
             json
             mbedtls
             esp-tls
             esp_partition
             lwip
             esp_hw_support
             driver
)
