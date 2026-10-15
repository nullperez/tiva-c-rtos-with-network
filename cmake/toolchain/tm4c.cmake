# Cross-compilation toolchain for the TM4C123GH6PM (Cortex-M4F) using the
# GNU Arm Embedded toolchain. Adapted from github.com/astroesteban/tiva-dev.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# Bare-metal executables can't run on the host, so test compiles build a
# static library instead.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_C_COMPILER   arm-none-eabi-gcc)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_OBJCOPY      arm-none-eabi-objcopy)
set(CMAKE_SIZE         arm-none-eabi-size)

# -ffunction-sections/-fdata-sections with --gc-sections drop unused code.
# nano.specs links newlib-nano (smaller printf); nosys.specs provides stub
# system calls so snprintf and friends link without an OS.
set(CMAKE_C_FLAGS_INIT
    "-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard -ffunction-sections -fdata-sections")
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-T${CMAKE_CURRENT_LIST_DIR}/tm4c123gh6pm.ld -e ResetISR -Wl,--gc-sections --specs=nano.specs --specs=nosys.specs")

set(CMAKE_EXECUTABLE_SUFFIX_C .elf)
