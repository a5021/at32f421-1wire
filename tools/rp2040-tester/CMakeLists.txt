cmake_minimum_required(VERSION 3.13)

# Pico SDK location: set PICO_SDK_PATH in the environment or on the command
# line (-DPICO_SDK_PATH=...).
if(NOT PICO_SDK_PATH)
    if(DEFINED ENV{PICO_SDK_PATH})
        set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
    else()
        message(FATAL_ERROR "Set PICO_SDK_PATH (environment or -DPICO_SDK_PATH)")
    endif()
endif()

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(rp2040_i2c_tester C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(i2c_tester
    src/main.c
    src/i2c_util.c
    src/crc8.c
    src/crc16.c
    src/tests.c
    src/lineedit.c
    src/scl_probe.c
    src/suites_bus.c
    src/suites_map.c
    src/suites_ow.c
    src/suites_stress.c
    src/suites_fault.c
)

target_include_directories(i2c_tester PRIVATE src)

if(NOT PICO_BOARD)
    set(PICO_BOARD pico)
endif()

target_compile_definitions(i2c_tester PRIVATE
    TESTER_VERSION="1.0"
    TESTER_SDK_VERSION="${PICO_SDK_VERSION_STRING}"
    TESTER_BOARD="${PICO_BOARD}"
)

target_link_libraries(i2c_tester
    pico_stdlib
    hardware_i2c
    hardware_pio
    hardware_dma
    hardware_adc
)

pico_enable_stdio_usb(i2c_tester 1)
pico_enable_stdio_uart(i2c_tester 0)
pico_add_extra_outputs(i2c_tester)
