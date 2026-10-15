/******************************************************************************
 * File:        timer.h
 *
 * Author:      Giancarlo Perez
 *
 * Created:     12/7/24
 *
 * Description: -
 ******************************************************************************/

#ifndef TIMER_H_
#define TIMER_H_

//=============================================================================
// INCLUDES
//=============================================================================

#include <stdint.h>
#include <stdbool.h>

//=============================================================================
// DEFINES AND MACROS
//=============================================================================

#define NUM_TIMERS 25
#define INVALID_TIMER 255

//=============================================================================
// TYPEDEFS AND GLOBALS
//=============================================================================

typedef void (*_tim_callback_t)(void*);

//=============================================================================
// FUNCTION PROTOTYPES
//=============================================================================

// Callbacks are NOT run from the interrupt. tickIsr() only marks timers as
// expired; processTimers() must be called from the main loop to run them.
// A one-shot timer's slot is freed right before its callback runs, so a
// callback should invalidate its own handle (set it to INVALID_TIMER).
void initTimer();
uint8_t startOneshotTimer(_tim_callback_t callback, uint32_t seconds, void* context);
uint8_t startPeriodicTimer(_tim_callback_t callback, uint32_t seconds, void* context);
bool stopTimer(uint8_t id);
void stopTimerHandle(uint8_t* id); // stops the timer and sets *id = INVALID_TIMER
bool restartTimer(uint8_t id);
void processTimers();
void tickIsr();
uint32_t random32();

#endif
