/******************************************************************************
 * File:        log.h
 *
 * Author:      Giancarlo Perez
 *
 * Created:     10/14/26
 *
 * Description: Debug logging by category, switched at runtime with the shell's
 *              "debug" command. All categories start disabled.
 *
 * Lines look like "[   12345] tcp: ..." with the time in ms since startup.
 * Only call from tasks or from main() (formats on the caller's stack), never
 * from an interrupt or kernel handler.
 ******************************************************************************/

#ifndef LOG_H_
#define LOG_H_

#include <stdint.h>
#include <stdbool.h>

// categories
#define LOG_ETH    0x0001 // ENC28J60 frames, TX resets, RX overflows
#define LOG_ARP    0x0002 // ARP requests/replies, cache updates
#define LOG_IP     0x0004 // ICMP and UDP traffic
#define LOG_TCP    0x0008 // TCP state changes, retransmits, resets
#define LOG_DHCP   0x0010 // DHCP messages and state
#define LOG_MQTT   0x0020 // MQTT keepalive/ack traffic
#define LOG_HTTP   0x0040 // HTTP requests
#define LOG_NET    0x0080 // netTask wakeups and netCall requests
#define LOG_APP    0x0100 // example application tasks
#define LOG_ALL    0x01FF

extern volatile uint32_t logMask;

static inline bool logEnabled(uint32_t category) {
    return (logMask & category) != 0;
}

void logPrintf(uint32_t category, const char* format, ...) __attribute__((format(printf, 2, 3)));

// Shell helpers: category name <-> bit, "" if unknown
uint32_t logCategoryFromName(const char* name);
const char* logCategoryName(uint8_t bit);

#endif
