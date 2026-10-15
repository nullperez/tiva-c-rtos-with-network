#ifndef NETWORK_H_
#define NETWORK_H_

#include <stdbool.h>
#include <stdint.h>

// EEPROM Map
#define EEPROM_DHCP        1
#define EEPROM_IP          2
#define EEPROM_SUBNET_MASK 3
#define EEPROM_GATEWAY     4
#define EEPROM_DNS         5
#define EEPROM_TIME        6
#define EEPROM_MQTT        7
#define EEPROM_ERASED      0xFFFFFFFF

// Pins
#define RED_LED PORTF,1
#define BLUE_LED PORTF,2
#define GREEN_LED PORTF,3
#define PUSH_BUTTON PORTF,4

bool isNetworkReady(void);
void netstat(void);
void readConfiguration(void);
bool runNetworkStack(void);

#endif
