cmake_minimum_required(VERSION 3.13...3.27)

# initialize pico-sdk from subdirectory
# note: this must happen before project()
include(pico-sdk/pico_sdk_init.cmake)

project(board)

# initialize the Raspberry Pi Pico SDK
pico_sdk_init()

add_executable(board
    main.c
)

pico_enable_stdio_usb(board 1)
pico_enable_stdio_uart(board 0)

# Add pico_stdlib library which aggregates commonly used features
target_link_libraries(board pico_stdlib)

# create map/bin/hex/uf2 file in addition to ELF.
pico_add_extra_outputs(board)
