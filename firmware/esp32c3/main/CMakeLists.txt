# proto.c is shared verbatim with the Pico firmware - same framing, same
# message ids, one place to change either.
idf_component_register(
    SRCS
        "main.c"
        "state.c"
        "picolink.c"
        "radio.c"
        "../../common/proto.c"
    INCLUDE_DIRS
        "."
        "../../common"
    REQUIRES
        driver
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        nvs_flash
)
