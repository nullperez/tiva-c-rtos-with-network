/******************************************************************************
 * File:        main.c
 *
 * Author:      Giancarlo Perez
 *
 * Created:     10/14/26
 *
 * Description: RTOS with the network stack running as a task
 ******************************************************************************/

// Target Platform: EK-TM4C123GXL w/ ENC28J60
// Target uC:       TM4C123GH6PM
// System Clock:    40 MHz

// Hardware configuration:
// ENC28J60 Ethernet controller on SPI0
//   MOSI (SSI0Tx) on PA5
//   MISO (SSI0Rx) on PA4
//   SCLK (SSI0Clk) on PA2
//   ~CS (SW controlled) on PA3
//   WOL on PB3
//   RESET on PB2
//   INT on PC6
// UART0 (PA0/PA1) on the ICDI virtual COM port, 115200 8N1
// LEDs: red PF1 (RX overflow), blue PF2 (blinky), green PF3 (UDP/MQTT "led")

//=============================================================================
// INCLUDES
//=============================================================================

#include <stdint.h>
#include <stdbool.h>
#include "tm4c123gh6pm.h"
#include "clock.h"
#include "uart0.h"
#include "kernel.h"
#include "mm.h"
#include "faults.h"
#include "mqtt_client.h"
#include "net_task.h"
#include "shell.h"
#include "tasks.h"

//=============================================================================
// MAIN FUNCTION
//=============================================================================

int main(void) {
    bool ok;

    // Hardware and network bring-up runs privileged, before the scheduler
    initSystemClockTo40Mhz();
    initUart0();
    setUart0BaudRate(115200, 40e6);
    initHw();
    enableFaultHandlers();
    initMpu();

    initRtos();
    initMutex(NET_QUEUE_MUTEX);
    initMutex(NET_SYNC_MUTEX);
    initSemaphore(NET_EVENT, 0);
    initSemaphore(NET_SLOTS, NET_CALL_QUEUE_SIZE);
    initSemaphore(NET_CALL_DONE, 0);
    initSemaphore(MQTT_RECEIVED, 0);
    initSemaphore(UART_RX, 0);

    initNetwork();
    setHandlePublishCallback(handlePublish);
    enableUart0RxInterrupt(shellRxCallback);

    // Idle must be the first task (0 = highest priority, 15 = lowest)
    ok = createThread(idle, "Idle", 15, 512);
    ok &= createThread(netTask, "Network", 2, 4096);
    ok &= createThread(shell, "Shell", 8, 2048);
    ok &= createThread(mqttApp, "MqttApp", 10, 1024);
    ok &= createThread(blinky, "Blinky", 12, 512);

    if (ok) {
        startRtos(); // never returns
    }
    putsUart0("Could not create the tasks\n");
    while (true);
}
