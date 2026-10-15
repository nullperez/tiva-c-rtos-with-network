/******************************************************************************
 * File:        socket.h
 *
 * Author:      Giancarlo Perez
 *
 * Created:     12/7/24
 *
 * Description: -
 ******************************************************************************/

#ifndef SOCKET_H_
#define SOCKET_H_

//=============================================================================
// INCLUDES
//=============================================================================

#include "ip.h"
#include "timer.h"
#include <stdint.h>
#include <stdbool.h>

//=============================================================================
// DEFINES AND MACROS
//=============================================================================

/* Socket Types */
#define SOCKET_STREAM 0 //TCP
#define SOCKET_DGRAM 1 //UDP
#define SOCKET_RAW 2 //raw (ICMP, ARP)


/* Socket Errors */
#define SOCKET_ERROR_MAX_MSG_LEN 60
#define SOCKET_ERROR_NO_ERROR 0
#define SOCKET_ERROR_ARP_TIMEOUT 1
#define SOCKET_ERROR_TCP_SYN_ACK_TIMEOUT 2
#define SOCKET_ERROR_CONNECTION_RESET 3
#define SOCKET_ERROR_TIMEOUT 4

/* Socket Events (passed to eventCallback) */
#define SOCKET_EVENT_CONNECTED 1   // active open completed (socketConnectTcp)
#define SOCKET_EVENT_ACCEPTED 2    // passive open completed, s is a new socket created by a listener
#define SOCKET_EVENT_PEER_CLOSED 3 // remote side sent FIN, no more data will be received
#define SOCKET_EVENT_CLOSED 4      // connection is over, the socket must not be used after this event

#define MAX_SOCKETS 5 // HTTP listener + 2 HTTP connections + MQTT + 1 spare
#define SOCKET_TX_BUFFER_SIZE 1024

//=============================================================================
// TYPEDEFS AND GLOBALS
//=============================================================================

typedef struct _socket socket;

// Called with in-order received data, the data must be copied before returning
typedef void (*socket_recv_callback_t)(socket* s, const uint8_t* data, uint16_t length);
typedef void (*socket_event_callback_t)(socket* s, uint8_t event);

// UDP/TCP socket
struct _socket {
    uint8_t type;
    uint8_t  localIpAddress[4];
    uint16_t localPort;
    uint8_t  remoteIpAddress[4];
    uint16_t remotePort;
    uint8_t  remoteHwAddress[6];
    //TCP control block (RFC 9293 3.3.1)
    uint8_t  state;
    uint32_t iss;     // initial send sequence number
    uint32_t sndUna;  // oldest unacknowledged sequence number
    uint32_t sndNxt;  // next sequence number to send
    uint32_t sndWl1;  // segment sequence number used for last window update
    uint32_t sndWl2;  // segment acknowledgment number used for last window update
    uint16_t sndWnd;  // remote receive window
    uint16_t sndMss;  // remote maximum segment size
    uint32_t irs;     // initial receive sequence number
    uint32_t rcvNxt;  // next sequence number expected
    //TCP retransmission (RFC 6298), times in ms
    uint32_t srtt;
    uint32_t rttvar;
    uint32_t rto;
    uint32_t rttSeq;   // acknowledgment that completes the RTT measurement
    uint32_t rttStart;
    bool     rttActive;
    bool     hasRttSample;
    uint8_t  retransmits;
    uint32_t timeWaitStart;
    //send buffer, holds unacknowledged and unsent data (UDP: datagram waiting for ARP)
    //points into a static pool so temporary sockets on the stack stay small
    uint8_t* tx_buffer;
    uint16_t tx_size;
    uint32_t txSeq;    // sequence number of tx_buffer[0]
    uint16_t flags;    // control flags waiting to be sent (ACK)
    bool     finPending; // close requested, FIN is sent after the buffered data
    bool     finSent;
    bool     arpPending;
    uint8_t  assocTimer;
    socket*  listener;  // listening socket that created this socket (passive open)
    uint8_t  valid;
    //application interface
    _tim_callback_t errorCallback; // receives a socketError*
    socket_recv_callback_t recvCallback;
    socket_event_callback_t eventCallback;
    void*    userData;
};

typedef struct socketError {
    uint8_t errorCode;
    socket* sk;
    char errorMsg[SOCKET_ERROR_MAX_MSG_LEN];
} socketError;

//=============================================================================
// FUNCTION PROTOTYPES
//=============================================================================

// Socket ownership: for TCP the stack frees the socket once the connection
// is over. SOCKET_EVENT_CLOSED is always the last callback for a socket.
void initSockets();
socket* newSocket(uint8_t type);
void deleteSocket(socket *s);
socket* getSockets();
void socketSendTo(socket* s, uint8_t serverIp[4], uint16_t port, uint8_t data[], uint16_t length);
void socketConnectTcp(socket* s, uint8_t ip[4], uint16_t port);
bool socketListenTcp(socket* s, uint16_t port);
uint16_t socketSendTcp(socket* s, const uint8_t* data, uint16_t length); // returns bytes queued
uint16_t socketGetTxFree(socket* s);
void socketCloseTcp(socket* s);
void socketAbortTcp(socket* s);
uint32_t getSocketId(socket* s);
socket* getSocketFromLocalPort(uint16_t l_port);
void getSocketInfoFromArpResponse(etherHeader* ether, socket* s);
void getSocketInfoFromUdpPacket(etherHeader* ether, socket* s);
void getSocketInfoFromTcpPacket(etherHeader* ether, socket* s);
void throwSocketError(socket* s, uint8_t errorCode);


#endif
