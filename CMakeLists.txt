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

# FreeRTOS is kept outside version control; callers may provide another install.
set(FREERTOS_KERNEL_PATH
    "${CMAKE_CURRENT_LIST_DIR}/lib/FreeRTOS-Kernel"
    CACHE PATH "Path to the FreeRTOS Kernel")
set(FREERTOS_CONFIG_FILE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/config")
include("${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2040/FreeRTOS_Kernel_import.cmake")

# Add executable. Default name is the project name, version 0.1
add_executable(LED_MEMORY_GAME 
    src/main.c
    src/display/ledmatrix.c
    src/display/imagedata_bin.S
    src/gameengine/core.c
    src/gameengine/memory.c
    src/network/mqtt_handler.c
    src/network/network_task.c
    src/network/wifi_manager.c
    src/sensors/imudriver.c
    src/logic/game_logic.c
)

pico_set_program_name(LED_MEMORY_GAME "LED_MEMORY_GAME")
pico_set_program_version(LED_MEMORY_GAME "0.1")

# Generate PIO header
pico_generate_pio_header(LED_MEMORY_GAME ${CMAKE_CURRENT_LIST_DIR}/src/display/ws2812.pio)

# Add the standard include files to the build
target_include_directories(LED_MEMORY_GAME PRIVATE
                        config
                        src/display
                        src/gameengine
                        src/network
                        src/sensors
                        src/logic
                        ${CMAKE_CURRENT_LIST_DIR}
                )

# Add any user requested libraries
target_link_libraries(LED_MEMORY_GAME 
    pico_stdlib
    hardware_i2c
    hardware_pio
    pico_unique_id
    pico_cyw43_arch_lwip_sys_freertos
    pico_lwip_mqtt
    FreeRTOS-Kernel-Heap4
)
target_compile_definitions(LED_MEMORY_GAME PRIVATE ASYNC_CONTEXT_DEFAULT_FREERTOS_TASK_STACK_SIZE=1024)

pico_add_extra_outputs(LED_MEMORY_GAME)

# Modify the below lines to enable/disable output over UART/USB
# (As discussed, USB is enabled so your prints don't clash with the IMU on I2C pins)
pico_enable_stdio_usb(LED_MEMORY_GAME 1)
pico_enable_stdio_uart(LED_MEMORY_GAME 0)

# == THIS IS TO TEST THE DISPLAY  ======================================================
add_executable(display_test
src/display/display_test.c
src/display/ledmatrix.c
src/display/imagedata_bin.S
)
target_include_directories(display_test PRIVATE src/display)
target_link_libraries(display_test pico_stdlib hardware_pio hardware_dma)
pico_generate_pio_header(display_test ${CMAKE_CURRENT_LIST_DIR}/src/display/ws2812.pio)
pico_add_extra_outputs(display_test)
pico_enable_stdio_usb(display_test 1)
pico_enable_stdio_uart(display_test 0)

# ====================================================================================