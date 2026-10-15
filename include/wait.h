// Wait functions
// Jason Losh

//-----------------------------------------------------------------------------
// Hardware Target
//-----------------------------------------------------------------------------

// Target uC:       TM4C123GH6PM
// System Clock:    40 MHz

#ifndef WAIT_H_
#define WAIT_H_

#include <stdint.h>

//-----------------------------------------------------------------------------
// Subroutines
//-----------------------------------------------------------------------------

void waitMicrosecond(uint32_t us);

// GCC replacement for TI's _delay_cycles() compiler intrinsic. Waits at least
// the given number of cycles (not cycle-exact), e.g. after enabling a
// peripheral clock.
static inline void _delay_cycles(uint32_t cycles) {
    while (cycles--) {
        __asm volatile("nop");
    }
}

#endif

