/******************************************************************************
 * File:        tasks.h
 *
 * Author:      Giancarlo Perez
 *
 * Created:     10/14/26
 *
 * Description: Example application tasks running alongside the network task
 ******************************************************************************/

#ifndef TASKS_H_
#define TASKS_H_

#include "mqtt.h"

void initHw(void);
void handlePublish(const mqttData* data); // MQTT client callback, runs in netTask

void idle(void);
void blinky(void);
void mqttApp(void);

#endif
