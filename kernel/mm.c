/******************************************************************************
 * File:        mm.c
 *
 * Author:      Giancarlo Perez
 *
 * Created:     12/7/24
 *
 * Description: MPU setup and the task heap (subregion allocator)
 *
 * MPU regions:
 *   Background  0x00000000 - 0xFFFFFFFF  privileged only (PRIVDEFEN)
 *   0 Heap A    0x20005000 - 0x20005FFF  8 x 512 B subregions, per task
 *   1 Heap B    0x20006000 - 0x20007FFF  8 x 1 kB subregions, per task
 *   2 App SRAM  0x20001000 - 0x20004FFF  shared .data/.bss, all tasks
 *   3 Flash     0x00000000 - 0x0003FFFF  all tasks
 *   4 Periph    0x40000000 - 0x43FFFFFF  all tasks (includes bit-band alias)
 *
 * Kernel data (0x20000000 - 0x20000FFF) is only covered by the background
 * region, so unprivileged code can't touch it.
 ******************************************************************************/

//=============================================================================
// INCLUDES
//=============================================================================

#include <stdint.h>
#include <stddef.h>
#include "tm4c123gh6pm.h"
#include "kernel.h"
#include "mm.h"

//=============================================================================
// DEFINES AND MACROS
//=============================================================================

#define REGION_HEAP_A   0
#define REGION_HEAP_B   1
#define REGION_APP      2
#define REGION_FLASH    3
#define REGION_PERIPH   4

#define AP_RW_RW        (0x3 << 24)          // privileged RW, unprivileged RW
#define AP_RO_RO        (0x6 << 24)          // privileged RO, unprivileged RO
#define SIZE(log2)      (((log2) - 1) << 1)  // region size is 2^(SIZE+1)

// Memory types from the TM4C123GH6PM datasheet (table 3-6). SRAM must be
// Normal memory; Device memory faults on the unaligned accesses the network
// stack makes into packet buffers.
#define ATTR_SRAM       (NVIC_MPU_ATTR_SHAREABLE | NVIC_MPU_ATTR_CACHEABLE)
#define ATTR_FLASH      (NVIC_MPU_ATTR_CACHEABLE)
#define ATTR_PERIPH     (NVIC_MPU_ATTR_SHAREABLE | NVIC_MPU_ATTR_BUFFRABLE)

//=============================================================================
// GLOBALS
//=============================================================================

KERNEL_DATA alloc_entry allocTable[MAX_ALLOCS];
KERNEL_DATA static uint16_t inUse; // one bit per heap subregion

//=============================================================================
// STATIC FUNCTIONS
//=============================================================================

static void* subregionAddr(uint32_t sr) {
    return (void*)(sr < 8 ? HEAP_A_ADDR + sr * 512 : HEAP_B_ADDR + (sr - 8) * 1024);
}

static uint32_t subregionSize(uint32_t sr) {
    return sr < 8 ? 512 : 1024;
}

static uint32_t subregionFromAddr(void* addr) {
    uint32_t a = (uint32_t)addr;
    return a < HEAP_B_ADDR ? (a - HEAP_A_ADDR) / 512 : 8 + (a - HEAP_B_ADDR) / 1024;
}

static void setRegion(uint8_t region, uint32_t base, uint32_t attr) {
    NVIC_MPU_NUMBER_R = region;
    NVIC_MPU_BASE_R = base;
    NVIC_MPU_ATTR_R = attr;
}

//=============================================================================
// PUBLIC FUNCTIONS
//=============================================================================

void initMpu(void) {
    NVIC_MPU_CTRL_R = 0;
    setRegion(REGION_HEAP_A, HEAP_A_ADDR, NVIC_MPU_ATTR_XN | AP_RW_RW | ATTR_SRAM
              | NVIC_MPU_ATTR_SRD_M | SIZE(12) | NVIC_MPU_ATTR_ENABLE);
    setRegion(REGION_HEAP_B, HEAP_B_ADDR, NVIC_MPU_ATTR_XN | AP_RW_RW | ATTR_SRAM
              | NVIC_MPU_ATTR_SRD_M | SIZE(13) | NVIC_MPU_ATTR_ENABLE);
    // 32 kB region with 4 kB subregions, only subregions 1-4 enabled
    setRegion(REGION_APP, SRAM_ADDR, NVIC_MPU_ATTR_XN | AP_RW_RW | ATTR_SRAM
              | (0xE1 << 8) | SIZE(15) | NVIC_MPU_ATTR_ENABLE);
    setRegion(REGION_FLASH, FLASH_ADDR, AP_RO_RO | ATTR_FLASH | SIZE(18) | NVIC_MPU_ATTR_ENABLE);
    setRegion(REGION_PERIPH, PERIPHERAL_ADDR, NVIC_MPU_ATTR_XN | AP_RW_RW | ATTR_PERIPH
              | SIZE(26) | NVIC_MPU_ATTR_ENABLE);
    NVIC_MPU_CTRL_R = NVIC_MPU_CTRL_PRIVDEFEN | NVIC_MPU_CTRL_ENABLE;
    __asm volatile("dsb\n isb" ::: "memory");
}

// Allocates a run of whole heap subregions. Requests of 512 B or less take one
// 512 B subregion, larger ones are rounded up to a multiple of 1 kB. Returns
// NULL if no run of free subregions adds up to exactly that size.
void* malloc_(uint32_t bytes, uint32_t owner) {
    uint32_t size, start, sr, total, i;
    if (bytes == 0) {
        return NULL;
    }
    size = bytes <= 512 ? 512 : (bytes + 1023) & ~1023u;
    for (i = 0; i < MAX_ALLOCS && allocTable[i].valid; i++);
    if (i == MAX_ALLOCS) {
        return NULL;
    }
    for (start = 0; start < HEAP_SUBREGIONS; start++) {
        total = 0;
        for (sr = start; sr < HEAP_SUBREGIONS && total < size && !(inUse & (1u << sr)); sr++) {
            total += subregionSize(sr);
        }
        if (total == size) {
            while (sr-- > start) {
                inUse |= 1u << sr;
            }
            allocTable[i].ptr = subregionAddr(start);
            allocTable[i].size = size;
            allocTable[i].owner = owner;
            allocTable[i].valid = 1;
            return allocTable[i].ptr;
        }
    }
    return NULL;
}

void free_to_heap(void* ptr) {
    uint32_t i, sr, total;
    for (i = 0; i < MAX_ALLOCS; i++) {
        if (allocTable[i].valid && allocTable[i].ptr == ptr) {
            for (sr = subregionFromAddr(ptr), total = 0; total < allocTable[i].size; sr++) {
                inUse &= ~(1u << sr);
                total += subregionSize(sr);
            }
            allocTable[i].valid = 0;
        }
    }
}

uint32_t getAllocSize(void* ptr) {
    uint32_t i;
    for (i = 0; i < MAX_ALLOCS; i++) {
        if (allocTable[i].valid && allocTable[i].ptr == ptr) {
            return allocTable[i].size;
        }
    }
    return 0;
}

// Bit n set = subregion n is disabled (no unprivileged access)
uint16_t createNoSramAccessMask(void) {
    return 0xFFFF;
}

void addSramAccessWindow(uint16_t* srdBitMask, void* baseAdd, uint32_t size_in_bytes) {
    uint32_t sr = subregionFromAddr(baseAdd);
    uint32_t total = 0;
    while (total < size_in_bytes && sr < HEAP_SUBREGIONS) {
        *srdBitMask &= ~(1u << sr);
        total += subregionSize(sr++);
    }
}

void applySramAccessMask(uint16_t srdBitMask) {
    NVIC_MPU_NUMBER_R = REGION_HEAP_A;
    NVIC_MPU_ATTR_R = (NVIC_MPU_ATTR_R & ~NVIC_MPU_ATTR_SRD_M) | ((srdBitMask & 0xFF) << 8);
    NVIC_MPU_NUMBER_R = REGION_HEAP_B;
    NVIC_MPU_ATTR_R = (NVIC_MPU_ATTR_R & ~NVIC_MPU_ATTR_SRD_M) | ((srdBitMask >> 8) << 8);
}
