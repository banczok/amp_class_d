# proto.c is shared verbatim with the Pico and the C3 - same framing,
# same message ids, one place to change any of them.
idf_component_register(
    SRCS
        "main.c"
        "display.c"
        "input.c"
        "link.c"
        "net.c"
        "art.c"
        "battery.c"
        "ui.c"
        "../../common/proto.c"
    INCLUDE_DIRS
        "."
        "../../common"
    REQUIRES
        driver
        esp_lcd
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        esp_http_client
        esp_adc
        nvs_flash
)
