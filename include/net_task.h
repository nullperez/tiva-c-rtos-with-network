/******************************************************************************
 * File:        net_task.h
 *
 * Author:      Giancarlo Perez
 *
 * Created:     10/14/26
 *
 * Description: Runs the network stack as an RTOS task
 *
 * The stack isn't reentrant, so only netTask calls into it. Other tasks hand
 * it work with netCall()/netCallSync(): the function runs later in netTask's
 * context with a copy of the data. Code already running in netTask (protocol
 * callbacks, HTTP handlers, netCall functions) calls the stack directly and
 * must not use netCall, since netTask can't wait on itself.
 *
 * netTask sleeps until something needs it: the ENC28J60 interrupt (packet
 * received), a netCall request, or NET_POLL_MS for the protocol timers.
 ******************************************************************************/

#ifndef NET_TASK_H_
#define NET_TASK_H_

//=============================================================================
// INCLUDES
//=============================================================================

#include <stdint.h>
#include <stdbool.h>

//=============================================================================
// DEFINES AND MACROS
//=============================================================================

#define NET_CALL_DATA_SIZE 96       // max bytes copied with a request
#define NET_CALL_QUEUE_SIZE 4       // power of 2
#define NET_POLL_MS 10              // timer resolution for TCP, DHCP, MQTT and HTTP
#define NET_MAX_PACKETS_PER_WAKE 8  // packets handled before lower priority tasks get a turn

//=============================================================================
// TYPEDEFS AND GLOBALS
//=============================================================================

// data points to the request's copy of the caller's data, word aligned
typedef void (*net_call_t)(void* data);

//=============================================================================
// FUNCTION PROTOTYPES
//=============================================================================

// Brings up the interface and protocols. Call from main() before startRtos().
void initNetwork(void);
void netTask(void);

// Queue fn to run in netTask with a copy of size bytes of data (data may be
// NULL if size is 0). netCall returns once the request is queued, blocking
// only while the queue is full; netCallSync returns after fn has run.
// Both return false if size is larger than NET_CALL_DATA_SIZE.
bool netCall(net_call_t fn, const void* data, uint16_t size);
bool netCallSync(net_call_t fn, const void* data, uint16_t size);

// GPIO port C handler (ENC28J60 INT on PC6)
void etherIsr(void);

#endif
