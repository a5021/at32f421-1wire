cmake_minimum_required(VERSION 3.20)

project(at32f421-1wire C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(LDSCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/vendor/at32f421/AT32F421x8_FLASH.ld)

add_executable(${PROJECT_NAME}
    src/ow_crc.c
    src/ow_hal.c
    src/manager.c
    src/onewire.c
    src/ds18b20.c
    src/ds2431.c
    src/clock.c
    app/i2c_hw.c
    app/i2c_slave.c
    app/main.c
    vendor/at32f421/system_at32f421.c
    vendor/at32f421/startup_at32f421.s
)

target_include_directories(${PROJECT_NAME} PRIVATE src vendor/at32f421)
target_compile_definitions(${PROJECT_NAME} PRIVATE AT32F421F8P7)
option(DIAG_BUILD "Disable automatic I2C recovery and the hardware watchdog for diagnostics" OFF)
if(DIAG_BUILD)
    target_compile_definitions(${PROJECT_NAME} PRIVATE DIAG_BUILD)
endif()
target_compile_options(${PROJECT_NAME} PRIVATE
    -Wall -Wextra -Wshadow -ffunction-sections -fdata-sections)
set_target_properties(${PROJECT_NAME} PROPERTIES
    SUFFIX ".elf"
    LINK_DEPENDS ${LDSCRIPT})
target_link_options(${PROJECT_NAME} PRIVATE
    -T${LDSCRIPT}
    -Wl,--gc-sections
    -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.map
    -Wl,--print-memory-usage)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${PROJECT_NAME}>
            ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}>
            ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${PROJECT_NAME}>)
