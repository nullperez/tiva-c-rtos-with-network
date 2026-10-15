// Timer Service Library
// Jason Losh

//-----------------------------------------------------------------------------
// Hardware Target
//-----------------------------------------------------------------------------

// Target Platform: EK-TM4C123GXL
// Target uC:       TM4C123GH6PM
// System Clock:    40 MHz

// Hardware configuration:
// Timer 4

//-----------------------------------------------------------------------------
// Device includes, defines, and assembler directives
//-----------------------------------------------------------------------------

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "tm4c123gh6pm.h"
#include "timer.h"
#include "wait.h"

//-----------------------------------------------------------------------------
// Global variables
//-----------------------------------------------------------------------------



typedef struct {
    _tim_callback_t callback;
    void* context;
    uint32_t exp_time;
    volatile uint32_t rem_time;
    bool reload;
    volatile bool active;
    volatile bool pending; // set by tickIsr when the timer expires
} timer_t;

timer_t timers[NUM_TIMERS];

//-----------------------------------------------------------------------------
// Subroutines
//-----------------------------------------------------------------------------

void initTimer() {
    uint8_t i;
    // Enable clocks
    SYSCTL_RCGCTIMER_R |= SYSCTL_RCGCTIMER_R4;
    _delay_cycles(3);
    // Configure Timer 4 for 1 sec tick
    TIMER4_CTL_R &= ~TIMER_CTL_TAEN;                 // turn-off timer before reconfiguring
    TIMER4_CFG_R = TIMER_CFG_32_BIT_TIMER;           // configure as 32-bit timer (A+B)
    TIMER4_TAMR_R = TIMER_TAMR_TAMR_PERIOD;          // configure for periodic mode (count down)
    TIMER4_TAILR_R = 40000000;                       // set load value (1 Hz rate)
    for (i = 0; i < NUM_TIMERS; i++) {
        timers[i].callback = NULL;
        timers[i].context = NULL;
        timers[i].exp_time = 0;
        timers[i].rem_time = 0;
        timers[i].reload = false;
        timers[i].active = false;
        timers[i].pending = false;
    }
    TIMER4_CTL_R |= TIMER_CTL_TAEN;                  // turn-on timer
    TIMER4_IMR_R |= TIMER_IMR_TATOIM;                // turn-on interrupt
    NVIC_EN2_R |= 1 << (INT_TIMER4A-80);             // turn-on interrupt 86 (TIMER4A)
}

static uint8_t startTimer(_tim_callback_t callback, uint32_t seconds, void* context, bool reload) {
    uint8_t i;
    if (callback == NULL || seconds == 0) {
        return INVALID_TIMER;
    }
    for (i = 0; i < NUM_TIMERS; i++) {
        if (!timers[i].active) {
            timers[i].rem_time = 0; // keep the ISR away from this slot while it is filled in
            timers[i].pending = false;
            timers[i].exp_time = seconds;
            timers[i].callback = callback;
            timers[i].context = context;
            timers[i].reload = reload;
            timers[i].active = true;
            timers[i].rem_time = seconds;
            return i;
        }
    }
    return INVALID_TIMER;
}

uint8_t startOneshotTimer(_tim_callback_t callback, uint32_t seconds, void* context) {
    return startTimer(callback, seconds, context, false);
}

uint8_t startPeriodicTimer(_tim_callback_t callback, uint32_t seconds, void* context) {
    return startTimer(callback, seconds, context, true);
}

//stops timer by ID, returns success status
bool stopTimer(uint8_t id) {
    if (id < NUM_TIMERS) {
        if (timers[id].active) {
            timers[id].active = false;
            timers[id].rem_time = 0;
            timers[id].pending = false;
            return true;
        }
    }
    return false;
}

void stopTimerHandle(uint8_t* id) {
    if (id) {
        stopTimer(*id);
        *id = INVALID_TIMER;
    }
}

bool restartTimer(uint8_t id) {
    if (id < NUM_TIMERS) {
        if (timers[id].active) {
            timers[id].pending = false;
            timers[id].rem_time = timers[id].exp_time;
            return true;
        }
    }
    return false;
}

// Runs the callbacks of expired timers, call from the main loop
void processTimers() {
    uint8_t i;
    for (i = 0; i < NUM_TIMERS; i++) {
        if (timers[i].pending) {
            timers[i].pending = false;
            if (timers[i].active) {
                _tim_callback_t callback = timers[i].callback;
                void* context = timers[i].context;
                if (!timers[i].reload) {
                    timers[i].active = false; // free the slot before the callback so it can be reused
                }
                callback(context);
            }
        }
    }
}

// 1 second ticks
void tickIsr() {
    uint8_t i;
    for (i = 0; i < NUM_TIMERS; i++) {
        if (timers[i].active && timers[i].rem_time != 0) {
            timers[i].rem_time--;
            if (timers[i].rem_time == 0) {
                if (timers[i].reload) {
                    timers[i].rem_time = timers[i].exp_time;
                }
                timers[i].pending = true;
            }
        }
    }
    TIMER4_ICR_R = TIMER_ICR_TATOCINT;
}

// Pseudo-random number (xorshift32 stirred with the free-running timer value)
// Not cryptographically secure
uint32_t random32() {
    static uint32_t state = 0x9E3779B9;
    state ^= TIMER4_TAV_R;
    if (state == 0) {
        state = 0x9E3779B9;
    }
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}
