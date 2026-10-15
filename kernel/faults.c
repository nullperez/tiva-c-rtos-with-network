/******************************************************************************
 * File:        faults.c
 *
 * Author:      Giancarlo Perez
 *
 * Created:     12/13/24
 *
 * Description: Fault handlers. An MPU fault in a task kills that task and the
 *              system keeps running; the other faults halt.
 ******************************************************************************/

//=============================================================================
// INCLUDES
//=============================================================================

#include <stdint.h>
#include "tm4c123gh6pm.h"
#include "faults.h"
#include "kernel.h"
#include "strlib.h"
#include "uart0.h"

//=============================================================================
// STATIC FUNCTIONS
//=============================================================================

static uint32_t* getPsp(void) {
    uint32_t* psp;
    __asm volatile("mrs %0, psp" : "=r"(psp));
    return psp;
}

static uint32_t* getMsp(void) {
    uint32_t* msp;
    __asm volatile("mrs %0, msp" : "=r"(msp));
    return msp;
}

// Prints label followed by value as 0x%08X
static void putHex(char* label, uint32_t value) {
    char hex[9];
    uint8_t i;
    to_string(value, hex, 16);
    putsUart0(label);
    putsUart0("0x");
    for (i = str_length(hex); i < 8; i++) {
        putcUart0('0');
    }
    putsUart0(hex);
    putcUart0('\n');
}

static void dumpProcessStack(uint32_t* psp) {
    putsUart0("---------Process Stack Dump---------\n");
    putHex("xPSR:\t", psp[7]);
    putHex("PC:\t", psp[6]);
    putHex("LR:\t", psp[5]);
    putHex("R0:\t", psp[0]);
    putHex("R1:\t", psp[1]);
    putHex("R2:\t", psp[2]);
    putHex("R3:\t", psp[3]);
    putHex("R12:\t", psp[4]);
    putsUart0("------------------------------------\n\n");
}

//=============================================================================
// PUBLIC FUNCTIONS
//=============================================================================

void enableFaultHandlers(void) {
    // separate usage, bus and memory management (MPU) faults instead of hard faults
    NVIC_SYS_HND_CTRL_R |= NVIC_SYS_HND_CTRL_USAGE | NVIC_SYS_HND_CTRL_BUS | NVIC_SYS_HND_CTRL_MEM;
    NVIC_CFG_CTRL_R |= NVIC_CFG_CTRL_DIV0;
}

void busFaultIsr(void) {
    putHex("\nBus fault in process ", getCurrentPid());
    putHex("Fault status: ", NVIC_FAULT_STAT_R);
    putHex("Address: ", NVIC_FAULT_ADDR_R);
    dumpProcessStack(getPsp());
    while (true);
}

void usageFaultIsr(void) {
    putHex("\nUsage fault in process ", getCurrentPid());
    putHex("Fault status: ", NVIC_FAULT_STAT_R);
    dumpProcessStack(getPsp());
    while (true);
}

void hardFaultIsr(void) {
    putHex("\nHard fault in process ", getCurrentPid());
    putsUart0("------------------------------------\n");
    putHex("PSP:\t", (uint32_t)getPsp());
    putHex("MSP:\t", (uint32_t)getMsp());
    putHex("HFAULT:\t", NVIC_HFAULT_STAT_R);
    putHex("MFAULT:\t", NVIC_FAULT_STAT_R);
    dumpProcessStack(getPsp());
    while (true);
}

void mpuFaultIsr(void) {
    uint32_t pid = getCurrentPid();
    uint32_t* psp = getPsp();
    putHex("\nMPU fault in process ", pid);
    putsUart0("------------------------------------\n");
    putHex("PSP:\t", (uint32_t)psp);
    putHex("MSP:\t", (uint32_t)getMsp());
    putHex("MFAULT:\t", NVIC_FAULT_STAT_R);
    if (NVIC_FAULT_STAT_R & NVIC_FAULT_STAT_MMARV) {
        putHex("Data Address: ", NVIC_MM_ADDR_R);
    }
    dumpProcessStack(psp);
    // clear the memory management fault status bits (write 1 to clear)
    NVIC_FAULT_STAT_R = NVIC_FAULT_STAT_R & 0xFF;
    // a fault in a handler (another exception active) or in the idle task
    // can't be recovered from
    if (!(NVIC_INT_CTRL_R & NVIC_INT_CTRL_RET_BASE) || !kill_proc(pid)) {
        putsUart0("Halted\n");
        while (true);
    }
    putHex("Killed process ", pid);
}
