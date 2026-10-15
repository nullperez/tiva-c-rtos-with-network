/******************************************************************************
 * File:        tcp.h
 *
 * Author:      Giancarlo Perez
 *
 * Created:     12/7/24
 *
 * Description: TCP (RFC 9293) with RFC 6298 retransmission timing,
 *              RFC 5961 RST/SYN hardening and RFC 6528 initial sequence numbers
 ******************************************************************************/

#ifndef TCP_H_
#define TCP_H_

//=============================================================================
// INCLUDES
//=============================================================================

#include "ip.h"
#include "socket.h"
#include <stdint.h>
#include <stdbool.h>

//=============================================================================
// DEFINES AND MACROS
//=============================================================================

/* TCP States */
#define TCP_CLOSED 0
#define TCP_LISTEN 1
#define TCP_SYN_RECEIVED 2
#define TCP_SYN_SENT 3
#define TCP_ESTABLISHED 4
#define TCP_FIN_WAIT_1 5
#define TCP_FIN_WAIT_2 6
#define TCP_CLOSING 7
#define TCP_CLOSE_WAIT 8
#define TCP_LAST_ACK 9
#define TCP_TIME_WAIT 10

/* TCP Offset/Flags */
#define FIN 0b00000001
#define SYN 0b00000010
#define RST 0b00000100
#define PSH 0b00001000
#define ACK 0b00010000
#define URG 0b00100000
#define ECE 0b01000000
#define CWR 0b10000000
#define NS  0x100
#define OFS_SHIFT 12

/* TCP Options */
#define TCP_OPTION_END 0
#define TCP_OPTION_NO_OP 1
#define TCP_OPTION_MAX_SEGMENT_SIZE 2
#define TCP_OPTION_WINDOW_SCALE 3
#define TCP_OPTION_SACK_PERMITTED 4

/* Constants */
#define MAX_SEGMENT_SIZE 1460   // MSS advertised to the peer (fits MAX_PACKET_SIZE)
#define TCP_DEFAULT_MSS 536     // peer MSS assumed when it sends no MSS option (RFC 9293 3.7.1)

/* Config */
#define WINDOW_SIZE 1460        // receive window, data is handed to the application as it arrives
#define TCP_MAX_SYN_ATTEMPTS 3  // SYN transmissions before giving up on an active open
#define TCP_MAX_RETRANSMITS 6   // retransmissions of the same data before the connection is aborted
#define TCP_INITIAL_RTO_MS 1000 // RFC 6298 2.1
#define TCP_MIN_RTO_MS 1000     // RFC 6298 2.4
#define TCP_MAX_RTO_MS 60000    // RFC 6298 2.5
#define TCP_MSL_SECONDS 120     // maximum segment lifetime, TIME-WAIT lasts 2*MSL (RFC 9293 3.4.2)
#define TCP_FIN_WAIT_2_TIMEOUT 60 // seconds to wait for the peer's FIN after ours was acknowledged

//=============================================================================
// TYPEDEFS AND GLOBALS
//=============================================================================

#pragma pack(push)
#pragma pack(1)
typedef struct _tcpHeader {// 20 or more bytes
  uint16_t sourcePort;
  uint16_t destPort;
  uint32_t sequenceNumber;
  uint32_t acknowledgementNumber;
  uint16_t offsetFields;
  uint16_t windowSize;
  uint16_t checksum;
  uint16_t urgentPointer;
  uint8_t  data[0];
} tcpHeader;
#pragma pack(pop)

//=============================================================================
// FUNCTION PROTOTYPES
//=============================================================================

void setTcpState(socket* s, uint8_t state);
uint8_t getTcpState(socket* s);
const char* getTcpStateName(uint8_t state);
tcpHeader* getTcpHeader(etherHeader* ether);
uint8_t* getTcpData(etherHeader* ether);
uint16_t getTcpDataLength(etherHeader* ether);
bool isTcp(etherHeader *ether);
void processTcpSegment(etherHeader* ether);
void sendTcpPendingMessages(void);

// Called through the socket API (socket.h)
void openTcpConnection(socket* s);
bool listenTcp(socket* s);
uint16_t sendTcpData(socket* s, const uint8_t* data, uint16_t length);
uint16_t getTcpTxFree(socket* s);
void closeTcpConnection(socket* s);
void abortTcpConnection(socket* s);

#endif
