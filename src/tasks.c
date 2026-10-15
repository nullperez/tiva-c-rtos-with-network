/******************************************************************************
 * File:        tasks.c
 *
 * Author:      Giancarlo Perez
 *
 * Created:     10/14/26
 *
 * Description: Example application tasks running alongside the network task
 *
 * mqttApp shows both directions of the netTask interface:
 *  - publishing: every MQTT_APP_PERIOD_MS it queues a publish with netCall()
 *    and carries on without waiting for the network
 *  - receiving: the MQTT client callback (in netTask) leaves the message in
 *    an inbox and posts MQTT_RECEIVED, which wakes mqttApp. Subscribe with
 *    "mqtt subscribe led" in the shell, then publish "on" or "off" to "led".
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
#include "gpio.h"
#include "strlib.h"
#include "mqtt_client.h"
#include "network_stack.h"
#include "net_task.h"
#include "sensors.h"
#include "tasks.h"

//=============================================================================
// DEFINES AND MACROS
//=============================================================================

#define MQTT_APP_PERIOD_MS 10000
#define MQTT_APP_TOPIC "sensor"
#define MQTT_APP_DATA_LENGTH 32

//=============================================================================
// TYPEDEFS AND GLOBALS
//=============================================================================

typedef struct _mqttMessage {
    char topic[MAX_TOPIC_LENGTH];
    char data[MQTT_APP_DATA_LENGTH];
} mqttMessage;

// Written by handlePublish (netTask) while inboxFull is false, read by mqttApp
// while it is true. Globals live in the shared app SRAM, so both tasks can
// reach them; task stacks are private to their task.
static mqttMessage inbox;
static volatile bool inboxFull;

//=============================================================================
// STATIC FUNCTIONS
//=============================================================================

// Runs in netTask with a copy of mqttApp's message
static void publishCall(void* data) {
    mqttMessage* m = data;
    publishMqtt(m->topic, m->data);
}

//=============================================================================
// PUBLIC FUNCTIONS
//=============================================================================

void initHw(void) {
    enablePort(PORTF);
    selectPinPushPullOutput(RED_LED);
    selectPinPushPullOutput(GREEN_LED);
    selectPinPushPullOutput(BLUE_LED);
}

void handlePublish(const mqttData* data) {
    uint16_t length = data->dataLen;
    if (inboxFull) {
        return; // mqttApp hasn't taken the last one yet
    }
    if (length > MQTT_APP_DATA_LENGTH - 1) {
        length = MQTT_APP_DATA_LENGTH - 1;
    }
    str_copy(inbox.topic, data->topic);
    memcpy(inbox.data, data->data, length);
    inbox.data[length] = '\0';
    inboxFull = true;
    post(MQTT_RECEIVED);
}

// One task must be ready at all times or the scheduler will fail.
// WFI stops the CPU clock until the next interrupt (SysTick at the latest);
// the yield lets a task that interrupt made ready run in cooperative mode.
void idle(void) {
    while (true) {
        __asm volatile("wfi");
        yield();
    }
}

// Heartbeat, keeps blinking whatever the network is doing
void blinky(void) {
    while (true) {
        togglePinValue(BLUE_LED);
        sleep(500);
    }
}

void mqttApp(void) {
    mqttMessage m;
    while (true) {
        if (waitTimeout(MQTT_RECEIVED, MQTT_APP_PERIOD_MS)) {
            logPrintf(LOG_APP, "mqtt message: %s = %s", inbox.topic, inbox.data);
            if (str_equal(inbox.topic, "led")) {
                setPinValue(GREEN_LED, str_equal(inbox.data, "on"));
            }
            inboxFull = false;
        }
        else {
            // publishMqtt drops it if the client isn't connected
            str_copy(m.topic, MQTT_APP_TOPIC);
            to_string(readSensor1(), m.data, 10);
            logPrintf(LOG_APP, "publishing %s = %s", m.topic, m.data);
            netCall(publishCall, &m, sizeof(m));
        }
    }
}
