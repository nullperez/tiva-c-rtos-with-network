/*
 * File:        tm4c123gh6pm.ld
 * Description: Linker script for the TM4C123GH6PM running the RTOS.
 *              Based on the tiva-c-network script by Esteban Duran
 *              (@astroesteban).
 *
 * SRAM is split into three areas. The addresses must match include/mm.h,
 * which programs the MPU to enforce them.
 *
 *   0x20000000 - 0x20000FFF  KRAM   4 kB  kernel data + main/handler stack (MSP)
 *                                         privileged only (MPU background map)
 *   0x20001000 - 0x20004FFF  ARAM  16 kB  .data/.bss of everything else (drivers,
 *                                         network stack, apps), RW for every task
 *   0x20005000 - 0x20007FFF  HEAP  12 kB  task stacks and mallocFromHeap(),
 *                                         each allocation is only visible to its
 *                                         owner (MPU subregions)
 */

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 256K
    KRAM  (rw)  : ORIGIN = 0x20000000, LENGTH = 4K
    ARAM  (rw)  : ORIGIN = 0x20001000, LENGTH = 16K
    HEAP  (rw)  : ORIGIN = 0x20005000, LENGTH = 12K
}

/* Minimum MSP space left below the top of KRAM for main(), ISRs and the kernel */
MIN_MSP_SIZE = 2K;

SECTIONS
{
    /* Main stack at the top of KRAM, used by main() and then by all handlers */
    PROVIDE( _stack_ptr = ORIGIN(KRAM) + LENGTH(KRAM) );

    .text :
    {
        _text = .;
        KEEP(*(.vector_table))                  /* vector table from the startup file */
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    /* stack unwinding tables that newlib may pull in */
    .ARM.exidx :
    {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    } > FLASH

    /* Kernel data (KERNEL_DATA in kernel.h). Must come before .bss, which
       would otherwise claim the .bss.kernel input sections. */
    .kernel_bss (NOLOAD) :
    {
        . = ALIGN(4);
        _kbss = .;
        *(.bss.kernel*)
        . = ALIGN(4);
        _ekbss = .;
    } > KRAM

    ASSERT(_ekbss + MIN_MSP_SIZE <= _stack_ptr, "kernel data leaves too little room for the main stack")

    /* initialized data, copied from flash by the startup file */
    .data :
    {
        . = ALIGN(4);
        _data = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > ARAM AT >FLASH

    _sidata = LOADADDR(.data);

    /* zero-initialized data, cleared by the startup file */
    .bss :
    {
        . = ALIGN(4);
        _bss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > ARAM

    /* newlib's heap (_sbrk in libs/syscalls.c) uses what's left of ARAM */
    PROVIDE( end = _ebss );
    _app_ram_end = ORIGIN(ARAM) + LENGTH(ARAM);
}
