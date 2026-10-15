/******************************************************************************
 * File:        shell.c
 *
 * Author:      Giancarlo Perez
 *
 * Created:     12/7/24
 *
 * Description: UART0 shell task. Process and IPC commands are handled here;
 *              any other line goes to netTask (net_shell.c), and a line that
 *              names a stopped task restarts it.
 ******************************************************************************/

//=============================================================================
// INCLUDES
//=============================================================================

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel.h"
#include "mm.h"
#include "uart0.h"
#include "strlib.h"
#include "eth0.h"
#include "net_task.h"
#include "net_shell.h"
#include "log.h"
#include "shell.h"

//=============================================================================
// DEFINES AND MACROS
//=============================================================================

//=============================================================================
// STATIC FUNCTIONS
//=============================================================================

static const char* stateName(uint8_t state) {
    switch (state) {
    case STATE_STOPPED: return "STOPPED";
    case STATE_READY: return "READY";
    case STATE_DELAYED: return "DELAYED";
    case STATE_BLOCKED_MUTEX: return "BLOCKED_MUTEX";
    case STATE_BLOCKED_SEMAPHORE: return "BLOCKED_SEMAPHORE";
    default: return "INVALID";
    }
}

static void ps(void) {
    psInfo info[MAX_TASKS];
    char str[100];
    uint8_t i;
    memset(info, 0, sizeof(info));
    getProcessInfo(info);
    putsUart0("PID         Name            CPU     Prio  State              Wait\n");
    for (i = 0; i < MAX_TASKS; i++) {
        if (info[i].state != STATE_INVALID) {
            // runtime is in ms per PS_REFRESH_TIME seconds, so this is tenths of a percent
            unsigned long cpu = info[i].runtime / PS_REFRESH_TIME;
            snprintf(str, sizeof(str), "0x%08lX  %-15.15s %3lu.%lu%%  %-5lu %-18s ",
                     (unsigned long)info[i].pid, info[i].name, cpu / 10, cpu % 10,
                     (unsigned long)info[i].prio, stateName(info[i].state));
            putsUart0(str);
            if (info[i].mutex_or_sem != 0xFF) {
                snprintf(str, sizeof(str), "%s %u", info[i].state == STATE_BLOCKED_MUTEX ? "mutex" : "sem",
                         info[i].mutex_or_sem);
                putsUart0(str);
            }
            putcUart0('\n');
        }
    }
}

static void printQueue(const uint8_t queue[], uint8_t size, char names[MAX_TASKS][16]) {
    uint8_t j;
    for (j = 0; j < size; j++) {
        putsUart0(names[queue[j]]);
        if (j < size - 1) {
            putsUart0(" -> ");
        }
    }
    putcUart0('\n');
}

static void ipcs(void) {
    ipcsInfo info;
    char str[60];
    uint8_t i;
    memset(&info, 0, sizeof(info));
    getIpcsInfo(&info);
    putsUart0("Mutex  Locked  Owner           Queue\n");
    for (i = 0; i < MAX_MUTEXES; i++) {
        snprintf(str, sizeof(str), "%-6u %-7s %-15s ", i, info.mutexes[i].lock ? "yes" : "no",
                 info.mutexes[i].lock ? info.nameArr[info.mutexes[i].lockedBy] : "-");
        putsUart0(str);
        printQueue(info.mutexes[i].processQueue, info.mutexes[i].queueSize, info.nameArr);
    }
    putsUart0("\nSemaphore  Count  Queue\n");
    for (i = 0; i < MAX_SEMAPHORES; i++) {
        snprintf(str, sizeof(str), "%-10u %-6u ", i, info.semaphores[i].count);
        putsUart0(str);
        printQueue(info.semaphores[i].processQueue, info.semaphores[i].queueSize, info.nameArr);
    }
}

static void meminfo(void) {
    memInfo info[MAX_ALLOCS];
    char str[80];
    uint8_t i;
    memset(info, 0, sizeof(info));
    getMemInfo(info);
    putsUart0("Owner           Address     Size   Peak stack\n");
    for (i = 0; i < MAX_ALLOCS; i++) {
        if (info[i].valid) {
            snprintf(str, sizeof(str), "%-15.15s 0x%08lX  %-6lu %lu.%lu%%\n", info[i].ownerName,
                     (unsigned long)info[i].baseAdd, (unsigned long)info[i].size,
                     (unsigned long)info[i].usage / 10, (unsigned long)info[i].usage % 10);
            putsUart0(str);
        }
    }
}

static void stopTask(uint32_t pid, const char* name) {
    if (pid != 0 && stopThread((_fn)pid)) {
        putsUart0("Stopped ");
    }
    else {
        putsUart0("Could not stop ");
    }
    putsUart0((char*)name);
    putcUart0('\n');
}

// Case-insensitive match for on/off style arguments
static bool argIs(const char* arg, const char* value) {
    while (*arg != '\0' && to_lower(*arg) == *value) {
        arg++;
        value++;
    }
    return *arg == '\0' && *value == '\0';
}

// debug                    list the log categories
// debug on|off             all categories
// debug CATEGORY [on|off]  one category, on if no state is given
static void debug(const char* category, const char* state) {
    uint32_t mask;
    uint8_t bit;
    if (category == NULL) {
        putsUart0("Log categories:");
        for (bit = 0; logCategoryName(bit)[0] != '\0'; bit++) {
            putcUart0(' ');
            putsUart0((char*)logCategoryName(bit));
            putsUart0(logEnabled(1u << bit) ? "=on" : "=off");
        }
        putsUart0("\nUsage: debug on|off | debug CATEGORY [on|off]\n");
        return;
    }
    if (argIs(category, "on") || argIs(category, "off")) {
        logMask = argIs(category, "on") ? LOG_ALL : 0;
        return;
    }
    mask = logCategoryFromName(category);
    if (mask == 0) {
        putsUart0("Unknown category, type debug for the list\n");
    }
    else if (state == NULL || argIs(state, "on")) {
        logMask |= mask;
    }
    else {
        logMask &= ~mask;
    }
}

static void resetEtherCall(void* data) {
    resetEther();
}

static void help(void) {
    putsUart0("RTOS commands:\n");
    putsUart0("  ps | ipcs | meminfo\n");
    putsUart0("  kill PID | pkill NAME | NAME (restart a stopped task)\n");
    putsUart0("  pidof NAME\n");
    putsUart0("  preempt [on|off]\n");
    putsUart0("  sched [prio|rr]\n");
    putsUart0("  debug [on|off] | debug CATEGORY [on|off]\n");
    putsUart0("  reboot\n");
}

// Runs one command line, returns false if it should go to netTask instead
static bool runCommand(char* line) {
    char* save;
    char* token = strtok_r(line, " ", &save);
    char* arg = strtok_r(NULL, " ", &save);
    uint32_t pid;
    if (token == NULL) {
        return true;
    }
    if (str_equal(token, "ps")) {
        ps();
    }
    else if (str_equal(token, "ipcs")) {
        ipcs();
    }
    else if (str_equal(token, "meminfo")) {
        meminfo();
    }
    else if (str_equal(token, "kill") && arg != NULL) {
        stopTask(strtoul(arg, NULL, 16), arg);
    }
    else if (str_equal(token, "pkill") && arg != NULL) {
        stopTask(pidof(arg), arg);
    }
    else if (str_equal(token, "pidof") && arg != NULL) {
        char str[20];
        pid = pidof(arg);
        snprintf(str, sizeof(str), pid ? "0x%08lX\n" : "Invalid task\n", (unsigned long)pid);
        putsUart0(str);
    }
    else if (str_equal(token, "preempt")) {
        if (arg != NULL) {
            setPreemption(argIs(arg, "on"));
        }
    }
    else if (str_equal(token, "sched")) {
        if (arg != NULL) {
            setScheduler(!argIs(arg, "rr"));
        }
    }
    else if (str_equal(token, "debug")) {
        debug(arg, strtok_r(NULL, " ", &save));
    }
    else if (str_equal(token, "reboot")) {
        netCallSync(resetEtherCall, NULL, 0);
        reboot();
    }
    else if (str_equal(token, "help")) {
        help();
        return false; // netTask adds the network commands
    }
    else if ((pid = pidof(token)) != 0) {
        putsUart0(restartThread((_fn)pid) ? "Restarted " : "Could not restart ");
        putsUart0(token);
        putcUart0('\n');
    }
    else {
        return false;
    }
    return true;
}

//=============================================================================
// PUBLIC FUNCTIONS
//=============================================================================

// UART0 receive interrupt callback, wakes the shell
void shellRxCallback(void) {
    postFromIsr(UART_RX);
}

void shell(void) {
    char line[MAX_CHARS + 1];
    char cmd[MAX_CHARS + 1];
    uint8_t count = 0;
    char c;
    putsUart0("\nType help for a list of commands\n> ");
    while (true) {
        while (kbhitUart0()) {
            c = getcUart0();
            if (c == 13 || count == MAX_CHARS) {
                line[count] = '\0';
                count = 0;
                str_copy(cmd, line); // runCommand tokenizes its copy
                if (!runCommand(cmd)) {
                    netCallSync(netShellCommand, line, str_length(line) + 1);
                }
                putsUart0("> ");
            }
            else if ((c == 8 || c == 127) && count > 0) {
                count--;
            }
            else if (c >= ' ' && c < 127) {
                line[count++] = c;
            }
        }
        wait(UART_RX);
    }
}
