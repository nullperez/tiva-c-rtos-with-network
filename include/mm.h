/******************************************************************************
 * File:        mm.h
 *
 * Author:      Giancarlo Perez
 *
 * Created:     12/7/24
 *
 * Description: MPU setup and the task heap (subregion allocator)
 ******************************************************************************/

#ifndef MM_H_
#define MM_H_

//=============================================================================
// INCLUDES
//=============================================================================

#include <stdint.h>
#include <stdbool.h>

//=============================================================================
// DEFINES AND MACROS
//=============================================================================

#define _kB 1024
#define _MB (1024*1024)

// SRAM map, must match cmake/toolchain/tm4c123gh6pm.ld
//   0x20000000 - 0x20000FFF  kernel data + MSP        privileged only
//   0x20001000 - 0x20004FFF  shared app .data/.bss    every task (RW)
//   0x20005000 - 0x20007FFF  task heap                owner only
#define SRAM_ADDR          0x20000000
#define APP_SRAM_ADDR      0x20001000
#define APP_SRAM_SIZE      (16*_kB)
#define HEAP_ADDR          0x20005000
#define HEAP_TOP           0x20008000

// The heap is covered by two MPU regions. Each region has 8 subregions that
// are enabled per task, so an allocation is a run of whole subregions.
#define HEAP_A_ADDR        0x20005000 // 4 kB, 8 x 512 B subregions
#define HEAP_A_SIZE        (4*_kB)
#define HEAP_B_ADDR        0x20006000 // 8 kB, 8 x 1 kB subregions
#define HEAP_B_SIZE        (8*_kB)
#define HEAP_SUBREGIONS    16

#define FLASH_ADDR         0x00000000
#define PERIPHERAL_ADDR    0x40000000

#define MAX_ALLOCS 12

//=============================================================================
// TYPEDEFS AND GLOBALS
//=============================================================================

typedef struct {
    void* ptr;
    uint32_t size;
    uint32_t owner; // task index
    uint8_t valid;
} alloc_entry;

extern alloc_entry allocTable[MAX_ALLOCS];

//=============================================================================
// FUNCTION PROTOTYPES
//=============================================================================

void initMpu(void);
void* malloc_(uint32_t bytes, uint32_t owner);
void free_to_heap(void* ptr);
uint32_t getAllocSize(void* ptr);
uint16_t createNoSramAccessMask(void);
void addSramAccessWindow(uint16_t* srdBitMask, void* baseAdd, uint32_t size_in_bytes);
void applySramAccessMask(uint16_t srdBitMask);

#endif
