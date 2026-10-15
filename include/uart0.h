/******************************************************************************
 * File:        uart0.h
 *
 * Author:      Giancarlo Perez
 *
 * Created:     12/7/24
 *
 * Description: -
 ******************************************************************************/

#ifndef UART0_H_
#define UART0_H_

//=============================================================================
// INCLUDES
//=============================================================================

#include <stdint.h>
#include <stdbool.h>

//=============================================================================
// DEFINES AND MACROS
//=============================================================================

#define MAX_UART_OUT 100
#define UART0_RX_BUFFER_SIZE 128 // power of 2, at most 128

//=============================================================================
// TYPEDEFS AND GLOBALS
//=============================================================================

typedef void (*uart0_rx_callback_t)(void);

extern char out[MAX_UART_OUT]; // shared scratch buffer for formatted UART output, defined in uart0.c

//=============================================================================
// FUNCTION PROTOTYPES
//=============================================================================

void initUart0();
void setUart0BaudRate(uint32_t baudRate, uint32_t fcyc);
void putcUart0(char c);
void putsUart0(char* str);
char getcUart0();
bool kbhitUart0();
void enableUart0RxInterrupt(uart0_rx_callback_t callback);
void uart0Isr(void);

#endif
