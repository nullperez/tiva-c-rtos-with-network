# tiva-c-rtos-net

Preemptive RTOS (from `../tiva-c-rtos`) with the bare-metal TCP/IP stack
(from `../tiva-c-network`: ARP, IP, ICMP, UDP, TCP, DHCP, HTTP, MQTT) running
as a task, on the TI EK-TM4C123GXL LaunchPad (TM4C123GH6PM, Cortex-M4F,
40 MHz) with a Microchip ENC28J60 on SPI0.

## Layout

- `core/` – GCC startup file and vector table (`ResetISR` is the entry point)
- `kernel/` – scheduler, SVC/PendSV/SysTick handlers (`kernel.c`), MPU and task
  heap (`mm.c`), fault handlers (`faults.c`)
- `drivers/` – clock, GPIO, NVIC, UART0, SPI0, EEPROM, eth0 (ENC28J60)
- `middleware/` – network protocols
- `libs/` – timer, wait, strlib, MQTT client, `_sbrk`
- `src/` – `main.c`, network task (`net_task.c`), shells, example tasks, HTTP
  server example, network stack glue
- `include/` – all headers, including `tm4c123gh6pm.h`

## Architecture

- Tasks run unprivileged on the PSP. Kernel services are SVCs (`kernel.h`).
  SVCall, PendSV, SysTick and the ISRs that call `postFromIsr()` all stay at
  NVIC priority 0 so they never nest. Don't raise any of them without adding
  critical sections to the kernel.
- SRAM map, enforced by the MPU (`mm.h` and the linker script must agree):
  - `0x20000000` 4 kB KRAM: kernel data (`KERNEL_DATA`) + MSP, privileged only
  - `0x20001000` 16 kB ARAM: `.data`/`.bss` of everything else, all tasks RW
  - `0x20005000` 12 kB heap: task stacks / `mallocFromHeap()`, owner only
  Globals are shared between tasks, stacks are private. A pointer handed to
  another task must point into ARAM, never at a stack variable.
- The network stack isn't reentrant: only `netTask` calls it. Other tasks use
  `netCall()` (async) / `netCallSync()` (waits), which copy up to
  `NET_CALL_DATA_SIZE` bytes and run a function in `netTask`. Code already in
  `netTask` (protocol/MQTT/HTTP callbacks, netCall functions) must call the
  stack directly; `netCall` from `netTask` can deadlock.
- `netTask` blocks in `waitTimeout(NET_EVENT, NET_POLL_MS)`. `NET_EVENT` is
  posted by the ENC28J60 INT pin (PC6, `etherIsr`) and by `netCall`.
- UART0 input is interrupt-driven: `uart0Isr` drains the 16-byte FIFO (full in
  ~1.4 ms at 115200) into a ring buffer and posts `UART_RX` to wake the shell.
  Don't go back to polling, pasted lines lose characters.
- The shell task handles RTOS commands and passes other lines to
  `netShellCommand()` in `netTask`. A line naming a stopped task restarts it.
- `meminfo` shows each task's peak stack use (stacks are painted at creation).
- Debug logging: `logPrintf(LOG_xxx, ...)` (`log.h`), off by default, switched
  with the shell's `debug` command. It formats on the caller's stack, so only
  call it from tasks or `main()`, never from an ISR or kernel handler.
- ENC28J60 erratum (rev B7 #12): in half-duplex, an aborted transmission can
  stall the TX logic with `TXRTS` stuck set. `putEtherPacket` resets the TX
  logic (`TXRST`) before every send and times out after 50 ms. Without this the
  board intermittently hung in its first DHCP transmit.
- Mutex/semaphore IDs and counts are in `kernel.h`. The idle task must be
  created first (task 0, can't be killed).

## Toolchain

CMake + GNU Arm (`arm-none-eabi-gcc`), no TivaWare; drivers write registers
through `tm4c123gh6pm.h`.

- Toolchain file: `cmake/toolchain/tm4c.cmake` (hard-float `fpv4-sp-d16`,
  newlib-nano, `--gc-sections`)
- Linker script: `cmake/toolchain/tm4c123gh6pm.ld`
- Make: CCS's `C:/ti/ccs2101/ccs/utils/bin/gmake.exe` (Unix Makefiles generator)
- OpenOCD: xPack at `C:/xpack/openocd/bin`. Don't use ST's OpenOCD
  (STM32CubeIDE) on the PATH: it lacks `board/ti_ek-tm4c123gxl.cfg`.

## Build, flash, debug

```sh
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_MAKE_PROGRAM=C:/ti/ccs2101/ccs/utils/bin/gmake.exe
cmake --build build                 # build/tiva_c_rtos_net.elf (+ .bin, .map, size report)
cmake --build build --target flash  # program via OpenOCD over the on-board ICDI
```

Debugging uses the Cortex-Debug VS Code extension (`.vscode/launch.json`) with
OpenOCD and `.vscode/tm4c123gxl.svd` for peripheral register views.

## Testing on this machine

- The board gets 192.168.1.180 from DHCP; this PC is 192.168.1.64.
- The serial console is usually open in PuTTY (Implicit CR in every LF,
  local echo and local line editing forced on). Scripts can only open COM4
  after PuTTY is closed. The shell doesn't echo and expects CR line endings.
- MQTT broker: the Mosquitto service only listens on localhost. For the board,
  run a second instance from a config with `listener 1883 192.168.1.64` and
  `allow_anonymous true` (`C:\Program Files\mosquitto\mosquitto.exe -c FILE -v`).
  In DHCP mode the board doesn't load the saved broker address at boot, so
  after every reset run `set mqtt 192.168.1.64` then `mqtt connect`.
  `mqtt subscribe led` + publishing on/off to `led` drives the green LED;
  `/api/status` reports LED states.
- Hang without serial access: halt with OpenOCD (`-c init -c halt -c "reg pc"
  -c "reg psp" -c resume -c shutdown`), sample a few times and map PCs with
  `arm-none-eabi-addr2line -f -e build/tiva_c_rtos_net.elf`. The PSP shows
  which task's stack (see `meminfo` addresses) is running.

## GCC notes

- New sources must be added to `add_executable` in `CMakeLists.txt`.
- Unprivileged code can't touch the PPB (`NVIC_*`, SysTick, SCB registers):
  configure interrupts in `main()` before `startRtos()`, or add an SVC.
- `waitMicrosecond` is a cycle-counted busy loop tuned for 40 MHz; in a task
  prefer `sleep()` for anything longer than a few microseconds.
- newlib-nano's printf/scanf don't support the `hh` length modifier. Use `%u`
  and scan into an `unsigned int`.
- `SRST error` from OpenOCD is harmless: the ICDI probe has no reset line.
- UART0 goes to the ICDI virtual COM port (COM4 on this machine) at 115200 8N1.
