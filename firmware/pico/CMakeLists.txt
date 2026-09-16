cmake_minimum_required(VERSION 3.13)

# Board is a Raspberry Pi Pico 2 (RP2350A).  Set PICO_SDK_PATH in the
# environment, or pass -DPICO_SDK_PATH=... on the command line.
set(PICO_BOARD pico2 CACHE STRING "Board type")
set(PICO_PLATFORM rp2350 CACHE STRING "Platform")

include(pico_sdk_import.cmake)

# The frame codec is shared verbatim with the ESP32-C3 firmware, so it
# lives one level up rather than being copied into both trees.
set(COMMON_DIR ${CMAKE_CURRENT_LIST_DIR}/../common)

project(preamp_ctrl C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(preamp_ctrl
    src/main.c
    src/audio.c
    src/pga2320.c
    src/bd37033.c
    src/relays.c
    src/encoder.c
    src/irrx.c
    src/meter.c
    src/fft.c
    ${COMMON_DIR}/proto.c
    src/linkpi.c
    src/linkesp.c
    src/settings.c
    src/power.c
    src/menu.c
)

target_include_directories(preamp_ctrl PRIVATE include src ${COMMON_DIR})

target_link_libraries(preamp_ctrl
    pico_stdlib
    pico_multicore
    pico_flash
    hardware_spi
    hardware_i2c
    hardware_adc
    hardware_dma
    hardware_uart
    hardware_gpio
    hardware_irq
    hardware_timer
    hardware_clocks
    hardware_flash
    hardware_sync
)

# USB serial is used for the debug console; the two hardware UARTs are
# both taken (UART0 -> Pi over the isolator, UART1 -> ESP32-C3).
pico_enable_stdio_usb(preamp_ctrl 1)
pico_enable_stdio_uart(preamp_ctrl 0)

pico_add_extra_outputs(preamp_ctrl)
