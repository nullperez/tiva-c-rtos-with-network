/******************************************************************************
 * File:        syscalls.c
 *
 * Author:      Giancarlo Perez
 *
 * Created:     10/14/26
 *
 * Description: newlib heap limit. nosys.specs' _sbrk grows without bound,
 *              which would run into the RTOS task heap above the app SRAM.
 ******************************************************************************/

#include <errno.h>
#include <stddef.h>

extern char end;          // first free byte after .bss (linker script)
extern char _app_ram_end; // end of the shared app SRAM (linker script)

void* _sbrk(ptrdiff_t increment) {
    static char* heapEnd = &end;
    char* previous = heapEnd;
    if (heapEnd + increment > &_app_ram_end) {
        errno = ENOMEM;
        return (void*)-1;
    }
    heapEnd += increment;
    return previous;
}
