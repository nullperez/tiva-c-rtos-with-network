/******************************************************************************
 * File:        net_task.c
 *
 * Author:      Giancarlo Perez
 *
 * Created:     10/14/26
 *
 * Description: Runs the network stack as an RTOS task
 ******************************************************************************/

//=============================================================================
// INCLUDES
//=============================================================================

#include "log.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "tm4c123gh6pm.h"
#include "kernel.h"
#include "nvic.h"
#include "uart0.h"
#include "eeprom.h"
#include "eth0.h"
#include "socket.h"
#include "timer.h"
#include "mqtt_client.h"
#include "network_stack.h"
#include "http_server_example.h"
#include "net_task.h"

//=============================================================================
// TYPEDEFS AND GLOBALS
//=============================================================================

_Static_assert((NET_CALL_QUEUE_SIZE & (NET_CALL_QUEUE_SIZE - 1)) == 0,
               "NET_CALL_QUEUE_SIZE must be a power of 2 so the indexes can wrap");

typedef struct _netRequest {
    net_call_t fn;
    bool sync;
    uint32_t data[(NET_CALL_DATA_SIZE + 3) / 4];
} netRequest;

// Ring buffer of requests. NET_SLOTS counts the free entries, so producers
// never overrun the consumer. The indexes run freely and wrap at 256.
static netRequest queue[NET_CALL_QUEUE_SIZE];
static volatile uint8_t queueHead; // next request to run, only netTask writes it
static volatile uint8_t queueTail; // next free entry, written under NET_QUEUE_MUTEX

//=============================================================================
// STATIC FUNCTIONS
//=============================================================================

static bool submit(net_call_t fn, const void* data, uint16_t size, bool sync) {
    netRequest* r;
    if (fn == NULL || size > NET_CALL_DATA_SIZE) {
        return false;
    }
    wait(NET_SLOTS);
    lock(NET_QUEUE_MUTEX);
    r = &queue[queueTail % NET_CALL_QUEUE_SIZE];
    r->fn = fn;
    r->sync = sync;
    if (size > 0) {
        memcpy(r->data, data, size);
    }
    queueTail++;
    unlock(NET_QUEUE_MUTEX);
    logPrintf(LOG_NET, "queued %s request (%u bytes)", sync ? "sync" : "async", size);
    post(NET_EVENT);
    return true;
}

static void runNetCalls(void) {
    while (queueHead != queueTail) {
        netRequest* r = &queue[queueHead % NET_CALL_QUEUE_SIZE];
        bool sync = r->sync;
        r->fn(r->data);
        queueHead++;
        post(NET_SLOTS);
        if (sync) {
            post(NET_CALL_DONE);
        }
    }
}

//=============================================================================
// PUBLIC FUNCTIONS
//=============================================================================

void initNetwork(void) {
    initTimer();
    initSockets();

    putsUart0("\nStarting eth0\n");
    initEther(ETHER_UNICAST | ETHER_BROADCAST | ETHER_HALFDUPLEX);
    setEtherMacAddress(0x02, 0x03, 0x04, 0x05, 0x06, 0xF6);

    initEeprom();
    readConfiguration();

    initMqttClient();
    initHttpServerExample();

    enableEtherInterrupt();
    enableNvicInterrupt(INT_GPIOC);
}

void netTask(void) {
    uint8_t packets;
    while (true) {
        // one pass handles everything that is pending, so drop extra wakeups
        while (waitTimeout(NET_EVENT, 0));
        packets = 0;
        while (packets < NET_MAX_PACKETS_PER_WAKE && runNetworkStack()) {
            packets++;
        }
        if (packets > 0 || queueHead != queueTail) {
            logPrintf(LOG_NET, "wake: %u packet(s), %u request(s)", packets,
                      (uint8_t)(queueTail - queueHead));
        }
        runNetCalls();
        runMqttClient();
        runHttpServerExample();
        if (packets < NET_MAX_PACKETS_PER_WAKE) {
            waitTimeout(NET_EVENT, NET_POLL_MS);
        }
        else {
            // more packets are waiting; let lower priority tasks run first
            logPrintf(LOG_NET, "packet limit reached, yielding for 1 ms");
            sleep(1);
        }
    }
}

bool netCall(net_call_t fn, const void* data, uint16_t size) {
    return submit(fn, data, size, false);
}

bool netCallSync(net_call_t fn, const void* data, uint16_t size) {
    bool ok;
    lock(NET_SYNC_MUTEX);
    ok = submit(fn, data, size, true);
    if (ok) {
        wait(NET_CALL_DONE);
    }
    unlock(NET_SYNC_MUTEX);
    return ok;
}

void etherIsr(void) {
    clearEtherInterrupt();
    postFromIsr(NET_EVENT);
}
