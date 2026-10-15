/******************************************************************************
 * File:        log.c
 *
 * Author:      Giancarlo Perez
 *
 * Created:     10/14/26
 *
 * Description: Debug logging by category
 ******************************************************************************/

#include <stdarg.h>
#include <stdio.h>
#include "clock.h"
#include "strlib.h"
#include "uart0.h"
#include "log.h"

#define LOG_LINE_LENGTH 120

// Shared app SRAM, so the shell task can change it and every task can read it
volatile uint32_t logMask = 0;

static const char* const names[] = {
    "eth", "arp", "ip", "tcp", "dhcp", "mqtt", "http", "net", "app"
};

void logPrintf(uint32_t category, const char* format, ...) {
    char line[LOG_LINE_LENGTH];
    const char* name = "";
    uint8_t bit;
    int n;
    va_list args;
    if (!logEnabled(category)) {
        return;
    }
    for (bit = 0; bit < sizeof(names) / sizeof(names[0]); bit++) {
        if (category & (1u << bit)) {
            name = names[bit];
            break;
        }
    }
    n = snprintf(line, sizeof(line), "[%8lu] %s: ", (unsigned long)millis(), name);
    va_start(args, format);
    vsnprintf(line + n, sizeof(line) - n - 1, format, args);
    va_end(args);
    // always end with exactly one newline, even if the message was cut short
    n = str_length(line);
    if (n == 0 || line[n - 1] != '\n') {
        line[n++] = '\n';
        line[n] = '\0';
    }
    putsUart0(line);
}

uint32_t logCategoryFromName(const char* name) {
    uint8_t bit;
    for (bit = 0; bit < sizeof(names) / sizeof(names[0]); bit++) {
        if (str_equal(name, names[bit])) {
            return 1u << bit;
        }
    }
    return 0;
}

const char* logCategoryName(uint8_t bit) {
    return bit < sizeof(names) / sizeof(names[0]) ? names[bit] : "";
}
