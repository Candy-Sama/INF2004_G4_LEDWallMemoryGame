# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico_w CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(LED_MEMORY_GAME C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(LED_MEMORY_GAME 
    src/main.c
    src/display/ledmatrix.c
    src/gameengine/core.c
    src/gameengine/memory.c
    src/network/mqtt_handler.c
    src/network/wifi_manager.c
    src/sensors/imudriver.c
)

pico_set_program_name(LED_MEMORY_GAME "LED_MEMORY_GAME")
pico_set_program_version(LED_MEMORY_GAME "0.1")

# Generate PIO header
pico_generate_pio_header(LED_MEMORY_GAME ${CMAKE_CURRENT_LIST_DIR}/src/display/ws2812.pio)

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(LED_MEMORY_GAME 0)
pico_enable_stdio_usb(LED_MEMORY_GAME 1)

# Add the standard library to the build
target_link_libraries(LED_MEMORY_GAME
        pico_stdlib)

# Add the standard include files to the build
target_include_directories(LED_MEMORY_GAME PRIVATE
                        config
                        src/display
                        src/gameengine
                        src/network
                        src/sensors
                        ${CMAKE_CURRENT_LIST_DIR}
                )

# Add any user requested libraries
target_link_libraries(LED_MEMORY_GAME 
    pico_stdlib
    hardware_i2c
    hardware_pio
    hardware_dma
    hardware_timer
    pico_cyw43_arch_lwip_threadsafe_background
    pico_lwip_mqtt
)

pico_add_extra_outputs(LED_MEMORY_GAME)

pico_enable_stdio_usb(LED_MEMORY_GAME 1)
pico_enable_stdio_uart(LED_MEMORY_GAME 0)
