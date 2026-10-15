# tiva-c-rtos-net

A preemptive RTOS for the TI **EK-TM4C123GXL** LaunchPad (TM4C123GH6PM, Cortex-M4F,
40 MHz) with a bare-metal TCP/IP stack (ARP, IP, ICMP, UDP, TCP, DHCP, HTTP, MQTT)
running as one of its tasks, talking to a Microchip **ENC28J60** Ethernet controller
over SPI. No TivaWare, no vendor RTOS, no lwIP: the kernel, drivers and protocols
write registers directly.

The network task sleeps until there's work. A received packet (ENC28J60
interrupt), a request from another task, or a protocol timer wakes it. Other
tasks run alongside it under a priority scheduler, with their stacks isolated
by the MPU.

## Features

**Kernel**
- Preemptive priority scheduler (16 levels, round-robin within a level), with
  runtime switches to plain round-robin or cooperative mode
- Tasks run unprivileged and reach the kernel through SVC calls
- Mutexes and counting semaphores, `sleep()`, `waitTimeout()`, and
  `postFromIsr()` for interrupt handlers
- FPU context saved per task (lazy stacking)
- MPU: the kernel's data is privileged-only, and each task stack is visible
  only to its own task
- An MPU violation kills only the offending task and prints a register dump
- `ps`, `ipcs` and `meminfo` (including each task's peak stack use)
- Idle task sleeps with `WFI`

**Network**
- ENC28J60 driver with interrupt-driven receive and the transmit-hang
  errata workaround
- ARP with cache, IPv4, ICMP echo (both directions), UDP
- TCP: RFC 9293 state machine, RFC 6298 retransmission timing
- DHCP client with lease renewal and IP conflict detection
- HTTP/1.1 server with a route table (example web page and JSON API)
- MQTT 3.1.1 client: publish, subscribe, keepalive

**Tooling**
- CMake + GNU Arm toolchain, flashing with OpenOCD, debugging in VS Code
  (Cortex-Debug + SVD register views)
- Serial shell for RTOS and network commands
- Category-based debug logging, switchable at runtime

## Hardware

| ENC28J60 | LaunchPad |
|---|---|
| SI (MOSI) | PA5 (SSI0Tx) |
| SO (MISO) | PA4 (SSI0Rx) |
| SCK | PA2 (SSI0Clk) |
| CS | PA3 (GPIO) |
| INT | PC6 |
| RESET | PB2 |
| WOL | PB3 |

LEDs: red PF1 (receive overflow), blue PF2 (heartbeat), green PF3 (driven over
UDP or MQTT). The console is UART0 on the LaunchPad's debug USB port, 115200 8N1.

## Architecture

```
            ┌───────────────────── tasks (unprivileged, PSP) ─────────────────────┐
            │                                                                     │
  UART0 ──► │  Shell ──netCallSync()──┐                                           │
   ISR      │                         ▼                                           │
            │  MqttApp ──netCall()──► request queue ──► Network task ──► TCP/IP   │
ENC28J60 ──►│                                           (owns the stack)  stack   │
 INT ISR    │  Blinky, Idle (WFI)                                                 │
            └─────────────────────────────────────────────────────────────────────┘
                        SVC │ ▲ PendSV
            ┌───────────────▼─┴── kernel (privileged, MSP) ───────────────────────┐
            │  scheduler · semaphores · mutexes · MPU · fault handlers            │
            └─────────────────────────────────────────────────────────────────────┘
```

The network stack isn't reentrant, so only the network task calls it. Other
tasks hand it work:

```c
static void publishCall(void* data) {      // runs in the network task
    mqttMessage* m = data;
    publishMqtt(m->topic, m->data);
}

mqttMessage m = { "sensor", "23" };
netCall(publishCall, &m, sizeof(m));       // copies m, returns without waiting
netCallSync(publishCall, &m, sizeof(m));   // waits until it has run
```

Incoming data goes the other way: protocol callbacks run in the network task,
which hands results to application tasks through shared globals and a
semaphore (see `handlePublish` and `mqttApp` in `src/tasks.c`).

### Memory map

| Address | Size | Contents | Access |
|---|---|---|---|
| `0x20000000` | 4 kB | Kernel data and main/handler stack | privileged only |
| `0x20001000` | 16 kB | `.data`/`.bss` of drivers, network stack, apps | all tasks |
| `0x20005000` | 12 kB | Task stacks and `mallocFromHeap()` | owning task only |

Globals are shared between tasks; stacks are private. Pointers passed to
another task must point at globals, never at stack variables.

## Building and flashing

Requirements: CMake 3.20+, the GNU Arm toolchain (`arm-none-eabi-gcc`), GNU
Make, and [xPack OpenOCD](https://xpack-dev-tools.github.io/openocd-xpack/),
which includes `board/ti_ek-tm4c123gxl.cfg`.

```sh
cmake -S . -B build -G "Unix Makefiles"   # add -DCMAKE_MAKE_PROGRAM=... if make isn't on PATH
cmake --build build                       # build/tiva_c_rtos_net.elf, .bin, .map
cmake --build build --target flash        # program over the on-board ICDI
```

OpenOCD reports `SRST error` because the ICDI probe has no reset line. It
falls back to a software reset, so the message is harmless.

`.vscode/launch.json` is set up for debugging with the Cortex-Debug
extension.

## Shell

Connect at 115200 8N1. The shell doesn't echo: turn on local echo and
implicit CR-in-LF in your terminal.

| Command | |
|---|---|
| `ps` / `ipcs` / `meminfo` | Tasks and CPU use / mutexes and semaphores / allocations and peak stack use |
| `kill PID` / `pkill NAME` / `NAME` | Stop a task / stop by name / restart a stopped task |
| `sched prio\|rr`, `preempt on\|off` | Scheduler mode |
| `debug [on\|off]`, `debug CATEGORY [on\|off]` | Debug logging (`eth arp ip tcp dhcp mqtt http net app`) |
| `ipconfig`, `netstat`, `arp [clear]`, `ping w.x.y.z` | Network status |
| `dhcp on\|off\|renew\|release`, `set ip\|sn\|gw\|dns\|mqtt w.x.y.z` | Network configuration (saved to EEPROM) |
| `mqtt connect\|disconnect\|publish T D\|subscribe T\|unsubscribe T` | MQTT client |
| `reboot` | Reset the board |

## Trying it out

The board gets an address from DHCP (`ipconfig` shows it). Then:

- `ping` it, or open `http://<board-ip>/` (status page) or `/api/status` (JSON).
- **MQTT**, with a broker such as Mosquitto listening on your LAN:
  ```
  set mqtt <broker-ip>
  mqtt connect
  mqtt subscribe led
  ```
  The example task publishes to `sensor` every 10 s. Publishing `on` or `off`
  to `led` switches the green LED:
  ```sh
  mosquitto_sub -h <broker-ip> -t sensor
  mosquitto_pub -h <broker-ip> -t led -m on
  ```
  In DHCP mode the broker address isn't restored at boot; set it again after
  a reset.
- **UDP**: send `on` or `off` to any port on the board to switch the green LED.

## Project layout

```
core/        startup code and vector table
kernel/      scheduler and SVC/PendSV/SysTick handlers, MPU and task heap, fault handlers
drivers/     clock, GPIO, NVIC, UART0, SPI0, EEPROM, ENC28J60
middleware/  ARP, IP, ICMP, UDP, TCP, DHCP, HTTP, MQTT, sockets
libs/        timers, string helpers, MQTT client, logging, newlib hooks
src/         main, network task, shells, example tasks, HTTP server example
include/     headers, including tm4c123gh6pm.h
cmake/       toolchain file and linker script
```

## Credits and license

The kernel framework and several drivers are based on course code by Jason Losh
(UT Arlington). The linker script is adapted from Esteban Duran's
[tiva-dev](https://github.com/astroesteban/tiva-dev).

Licensed under the GNU GPL v3.0, see [LICENSE](LICENSE).
