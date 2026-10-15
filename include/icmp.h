/******************************************************************************
 * File:        icmp.h
 *
 * Author:      Giancarlo Perez
 *
 * Created:     12/12/24
 *
 * Description: -
 ******************************************************************************/

#ifndef ICMP_H_
#define ICMP_H_

//=============================================================================
// INCLUDES
//=============================================================================

#include "ip.h"
#include <stdint.h>
#include <stdbool.h>

//=============================================================================
// DEFINES AND MACROS
//=============================================================================

//=============================================================================
// TYPEDEFS AND GLOBALS
//=============================================================================

typedef struct _icmpHeader {// 8 bytes{
  uint8_t type;
  uint8_t code;
  uint16_t check;
  uint16_t id;
  uint16_t seq_no;
  uint8_t data[0];
} icmpHeader;

typedef struct _icmpEchoRequest {
    uint8_t remoteMac[6];
    uint8_t remoteIp[4];
    uint8_t dataSize;
    uint8_t data[0];
} icmpEchoRequest;

typedef struct _icmpEchoResponse {
    uint8_t remoteIp[4];
    uint8_t bytes;
    uint32_t ms;
    uint8_t ttl;
} icmpEchoResponse;

//=============================================================================
// FUNCTION PROTOTYPES
//=============================================================================

void ping(uint8_t ipAdd[]);
bool isPingRequest(etherHeader *ether);
uint8_t isPingResponse(etherHeader* ether, icmpEchoResponse* reply);
void sendPingRequest(etherHeader *ether, uint8_t ipAdd[]);
void sendPingResponse(etherHeader *ether);

#endif
