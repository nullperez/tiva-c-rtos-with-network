# tiva-c-rtos-with-network
This is a project combining two separate systems I wrote during college: a preemptive RTOS and a bare-metal TCP/IP network stack for the TI TM4C123GXL LaunchPad (TM4C123GH6PM, Cortex-M4F, 40MHz)

The RTOS features priority scheduling with preemption, and task stacks are isolated using the Memory Protection Unit (MPU)

The custom TCP/IP stack implements ARP, IPv4, ICMP, UDP, TCP, DHCP, HTTP, and MQTT and communicates through an ENC28J60 ethernet controller over SPI.

The network stack operates as an independent task using an event-driven design, which remains blocked while idle and only wakes up when it receives a packet, a request from another task, or a timer event wakes it up.
This minimizes unnecessary CPU utilization, which can be displayed via the `ps` command through the UART shell.

## Features

**Kernel**
- Preemptive priority scheduler (16 priority levels, round-robin at each level)
- Tasks run unprivileged and use SVC calls to reach the kernel for elevated access
- Implements mutexes and semaphores for interprocess safety
- Memory Protection Unit keeps the kernel memory privileged-only, while each task also has its stack isolated by the MPU
- MPU violation kills the task which caused it and prints a dump
- `ps`, `ipcs`, and `meminfo` commands available through the shell

**Network**
- ENC28J60 driver with interrupt-driven packet reception
- ARP with cache, IPv4, ICMP echo (ping), UDP
- TCP: RFC9293 state machine
- DHCP client with lease renewal and IP conflict detection
- HTTP/1.1 server with a route table (example web page and JSON API)
- MQTT 3.1.1 client; PUB, SUB, and keepalive
 
**Tooling**
- CMake + GNU Arm toolchain, flashed with OpenOCD
- Serial shell (using UART0 peripheral on the MCU) for logging, as well as RTOS and network commands

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
<img width="599" height="1090" alt="Untitled Diagram drawio" src="https://github.com/user-attachments/assets/1a2d68c6-111d-4c0e-bde0-2b7899e8c54d" />

The network stack is *not* reentrant, so only the network stack itself calls it. Other tasks hand it work

### Memory Map

| Address | Size | Contents | Access |
|---|---|---|---|
| `0x20000000` | 4 kB | Kernel data and main/handler stack | privileged only |
| `0x20001000` | 16 kB | `.data`/`.bss` of drivers, network stack, apps | all tasks |
| `0x20005000` | 12 kB | Task stacks and `mallocFromHeap()` | owning task only |

Globals are shared between tasks; stacks are private. Pointers passed to
another task must point at globals, never at stack variables.


## Shell

The shell runs on the microcontroller through the UART0 peripheral at 115200 8N1, so it can be viewed on your PC

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

## Credits and license

The kernel framework and several drivers are based on course code by Jason Losh
(UT Arlington). The linker script is adapted from Esteban Duran's
[tiva-dev](https://github.com/astroesteban/tiva-dev).

Licensed under the GNU GPL v3.0, see [LICENSE](LICENSE).
