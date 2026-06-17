cmake_minimum_required(VERSION 3.13...3.27)

# initialize pico-sdk from subdirectory
# note: this must happen before project()
include(pico-sdk/pico_sdk_init.cmake)

project(board VERSION 0.0.1 LANGUAGES C CXX ASM)

pico_sdk_init()

add_executable(board
    main.c
)

pico_generate_pio_header(board ${CMAKE_CURRENT_LIST_DIR}/i2s_rx.pio)
pico_generate_pio_header(board ${CMAKE_CURRENT_LIST_DIR}/i2s_tx.pio)

pico_enable_stdio_usb(board 0)
pico_enable_stdio_uart(board 0)

target_link_libraries(board PRIVATE
  pico_stdlib
  hardware_pio
  hardware_pwm
)

pico_add_extra_outputs(board)
