// UART0 Library
// Jason Losh

//-----------------------------------------------------------------------------
// Hardware Target
//-----------------------------------------------------------------------------

// Target Platform: EK-TM4C123GXL
// Target uC:       TM4C123GH6PM
// System Clock:    -

// Hardware configuration:
// UART Interface:
//   U0TX (PA1) and U0RX (PA0) are connected to the 2nd controller
//   The USB on the 2nd controller enumerates to an ICDI interface and a virtual COM port

//-----------------------------------------------------------------------------
// Device includes, defines, and assembler directives
//-----------------------------------------------------------------------------

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "tm4c123gh6pm.h"
#include "uart0.h"
#include "gpio.h"
#include "wait.h"
#include "nvic.h"

// Pins
#define UART_TX PORTA,1
#define UART_RX PORTA,0

//-----------------------------------------------------------------------------
// Global variables
//-----------------------------------------------------------------------------

char out[MAX_UART_OUT];

// Receive ring buffer, filled by uart0Isr once enableUart0RxInterrupt() is
// called. The indexes run freely and wrap at 256.
static char rxBuffer[UART0_RX_BUFFER_SIZE];
static volatile uint8_t rxHead; // written by the ISR
static volatile uint8_t rxTail; // written by the reader
static volatile bool rxInterrupt;
static uart0_rx_callback_t rxCallback;

//-----------------------------------------------------------------------------
// Subroutines
//-----------------------------------------------------------------------------

// Initialize UART0
void initUart0(void)
{
    // Enable clocks
    SYSCTL_RCGCUART_R |= SYSCTL_RCGCUART_R0;
    _delay_cycles(3);
    enablePort(PORTA);

    // Configure UART0 pins
    selectPinPushPullOutput(UART_TX);
    selectPinDigitalInput(UART_RX);
    setPinAuxFunction(UART_TX, GPIO_PCTL_PA1_U0TX);
    setPinAuxFunction(UART_RX, GPIO_PCTL_PA0_U0RX);

    // Configure UART0 with default baud rate
    UART0_CTL_R = 0;                                    // turn-off UART0 to allow safe programming
    UART0_CC_R = UART_CC_CS_SYSCLK;                     // use system clock (usually 40 MHz)
}

// Set baud rate as function of instruction cycle frequency
void setUart0BaudRate(uint32_t baudRate, uint32_t fcyc)
{
    uint32_t divisorTimes128 = (fcyc * 8) / baudRate;   // calculate divisor (r) in units of 1/128,
                                                        // where r = fcyc / 16 * baudRate
    divisorTimes128 += 1;                               // add 1/128 to allow rounding
    UART0_CTL_R = 0;                                    // turn-off UART0 to allow safe programming
    UART0_IBRD_R = divisorTimes128 >> 7;                // set integer value to floor(r)
    UART0_FBRD_R = ((divisorTimes128) >> 1) & 63;       // set fractional value to round(fract(r)*64)
    UART0_LCRH_R = UART_LCRH_WLEN_8 | UART_LCRH_FEN;    // configure for 8N1 w/ 16-level FIFO
    UART0_CTL_R = UART_CTL_TXE | UART_CTL_RXE | UART_CTL_UARTEN;
                                                        // turn-on UART0
}

// Blocking function that writes a serial character when the UART buffer is not full
void putcUart0(char c)
{
    while (UART0_FR_R & UART_FR_TXFF);               // wait if uart0 tx fifo full
    UART0_DR_R = c;                                  // write character to fifo
}

// Blocking function that writes a string when the UART buffer is not full
void putsUart0(char* str)
{
    uint8_t i = 0;
    while (str[i] != '\0')
        putcUart0(str[i++]);
}

// Blocking function that returns with serial data once the buffer is not empty
char getcUart0(void)
{
    char c;
    if (!rxInterrupt)
    {
        while (UART0_FR_R & UART_FR_RXFE);           // wait if uart0 rx fifo empty
        return UART0_DR_R & 0xFF;                    // get character from fifo
    }
    while (rxHead == rxTail);                        // wait if the ring buffer is empty
    c = rxBuffer[rxTail % UART0_RX_BUFFER_SIZE];
    rxTail++;
    return c;
}

// Returns the status of the receive buffer
bool kbhitUart0(void)
{
    if (!rxInterrupt)
        return !(UART0_FR_R & UART_FR_RXFE);
    return rxHead != rxTail;
}

// Moves received characters into a ring buffer from the interrupt, so input
// isn't lost while the reader is busy (the FIFO holds only 16 characters,
// about 1.4 ms at 115200 baud). callback runs in the interrupt after new data
// arrives and may be NULL. Privileged: call before the RTOS starts.
void enableUart0RxInterrupt(uart0_rx_callback_t callback)
{
    rxCallback = callback;
    rxInterrupt = true;
    UART0_IM_R |= UART_IM_RXIM | UART_IM_RTIM;       // FIFO half full or receive timeout
    enableNvicInterrupt(INT_UART0);
}

void uart0Isr(void)
{
    while (!(UART0_FR_R & UART_FR_RXFE))
    {
        char c = UART0_DR_R & 0xFF;
        if ((uint8_t)(rxHead - rxTail) < UART0_RX_BUFFER_SIZE)
        {
            rxBuffer[rxHead % UART0_RX_BUFFER_SIZE] = c;
            rxHead++;
        }
    }
    UART0_ICR_R = UART_ICR_RXIC | UART_ICR_RTIC;
    if (rxCallback != NULL)
        rxCallback();
}
