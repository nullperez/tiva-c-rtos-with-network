// Wait functions
// Jason Losh

//-----------------------------------------------------------------------------
// Hardware Target
//-----------------------------------------------------------------------------

// Target uC:       TM4C123GH6PM
// System Clock:    40 MHz

//-----------------------------------------------------------------------------
// Device includes, defines, and assembler directives
//-----------------------------------------------------------------------------

#include <stdint.h>
#include "tm4c123gh6pm.h"
#include "wait.h"

// Approximate busy waiting (in units of microseconds), given a 40 MHz system clock
// The loop is a single asm block so GCC keeps the branches and labels together.
// Numeric labels are local: "2b" means the previous "2:", "3f" the next "3:".
void waitMicrosecond(uint32_t us) {
    __asm volatile(
        "1:  MOV  R1, #6    \n"     // 1
        "2:  SUB  R1, #1    \n"     // 6
        "    CBZ  R1, 3f    \n"     // 5+1*3
        "    NOP            \n"     // 5
        "    NOP            \n"     // 5
        "    B    2b        \n"     // 5*2 (speculative, so P=1)
        "3:  SUB  %0, #1    \n"     // 1
        "    CBZ  %0, 4f    \n"     // 1
        "    NOP            \n"     // 1
        "    B    1b        \n"     // 1*2 (speculative, so P=1)
        "4:                 \n"     // ---
                                    // 40 clocks/us + error
        : "+l" (us)                 // CBZ needs a low register (R0-R7)
        :
        : "r1");
}

