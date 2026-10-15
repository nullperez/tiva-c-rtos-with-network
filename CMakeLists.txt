cmake_minimum_required(VERSION 3.20)

set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain/tm4c.cmake)

project(tiva_c_rtos_net LANGUAGES C)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(tiva_c_rtos_net
    core/tm4c123gh6pm_startup_gcc.c
    drivers/clock.c
    drivers/eeprom.c
    drivers/eth0.c
    drivers/gpio.c
    drivers/nvic.c
    drivers/spi0.c
    drivers/uart0.c
    kernel/faults.c
    kernel/kernel.c
    kernel/mm.c
    libs/mqtt_client.c
    libs/log.c
    libs/strlib.c
    libs/syscalls.c
    libs/timer.c
    libs/wait.c
    middleware/arp.c
    middleware/dhcp.c
    middleware/http.c
    middleware/icmp.c
    middleware/ip.c
    middleware/mqtt.c
    middleware/socket.c
    middleware/tcp.c
    middleware/udp.c
    src/http_server_example.c
    src/main.c
    src/net_shell.c
    src/net_task.c
    src/network_stack.c
    src/sensors.c
    src/shell.c
    src/tasks.c
)

target_include_directories(tiva_c_rtos_net PRIVATE include)
target_compile_features(tiva_c_rtos_net PRIVATE c_std_11)
target_compile_options(tiva_c_rtos_net PRIVATE -Wall)
target_link_options(tiva_c_rtos_net PRIVATE -Wl,-Map=$<TARGET_FILE_DIR:tiva_c_rtos_net>/tiva_c_rtos_net.map)

# Produce a raw binary next to the ELF and print flash/RAM usage after each build.
add_custom_command(TARGET tiva_c_rtos_net POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:tiva_c_rtos_net> tiva_c_rtos_net.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:tiva_c_rtos_net>
    VERBATIM)

# Flash the board with `cmake --build build --target flash`.
# The xPack OpenOCD install is checked before the PATH because ST's OpenOCD
# (bundled with STM32CubeIDE) doesn't ship the TI board configs.
find_program(OPENOCD_EXECUTABLE openocd HINTS C:/xpack/openocd/bin)
add_custom_target(flash
    COMMAND ${OPENOCD_EXECUTABLE} -f board/ti_ek-tm4c123gxl.cfg
            -c "program $<TARGET_FILE:tiva_c_rtos_net> verify reset exit"
    DEPENDS tiva_c_rtos_net
    USES_TERMINAL
    VERBATIM)
