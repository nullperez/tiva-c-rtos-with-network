/******************************************************************************
 * File:        socket.c
 *
 * Author:      Giancarlo Perez
 *
 * Created:     12/7/24
 *
 * Description: -
 ******************************************************************************/

//=============================================================================
// INCLUDES
//=============================================================================

#include "socket.h"
#include "ip.h"
#include "arp.h"
#include "udp.h"
#include "tcp.h"
#include "timer.h"
#include <stdio.h>
#include <string.h>

//=============================================================================
// DEFINES AND MACROS
//=============================================================================

#define EPHEMERAL_PORT_MIN 49152

//=============================================================================
// GLOBALS
//=============================================================================

socket sockets[MAX_SOCKETS];
static uint8_t socketTxBuffers[MAX_SOCKETS][SOCKET_TX_BUFFER_SIZE];

//=============================================================================
// STATIC FUNCTIONS
//=============================================================================

static bool isLocalPortInUse(uint16_t port) {
    uint8_t i;
    for (i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i].valid && sockets[i].localPort == port) {
            return true;
        }
    }
    return false;
}

static uint16_t getEphemeralPort() {
    uint16_t port;
    do {
        port = (random32() & 0x3FFF) + EPHEMERAL_PORT_MIN;
    } while (isLocalPortInUse(port));
    return port;
}

// Finds the TCP socket that has been in TIME-WAIT the longest
static socket* getOldestTimeWaitSocket() {
    uint8_t i;
    socket* oldest = NULL;
    for (i = 0; i < MAX_SOCKETS; i++) {
        socket* s = &sockets[i];
        if (s->valid && s->type == SOCKET_STREAM && s->state == TCP_TIME_WAIT) {
            if (oldest == NULL || (int32_t)(s->timeWaitStart - oldest->timeWaitStart) < 0) {
                oldest = s;
            }
        }
    }
    return oldest;
}

//=============================================================================
// PUBLIC FUNCTIONS
//=============================================================================

void initSockets(void) {
    uint8_t i;
    for (i = 0; i < MAX_SOCKETS; i++) {
        memset(&sockets[i], 0, sizeof(socket));
        sockets[i].state = TCP_CLOSED;
        sockets[i].assocTimer = INVALID_TIMER;
        sockets[i].tx_buffer = socketTxBuffers[i];
    }
}

// Returns a zeroed socket with an unused ephemeral local port, or NULL
// When every socket is in use, the oldest TCP socket in TIME-WAIT is reused
socket* newSocket(uint8_t type) {
    uint8_t i;
    socket* s = NULL;
    for (i = 0; i < MAX_SOCKETS && s == NULL; i++) {
        if (!sockets[i].valid) {
            s = &sockets[i];
        }
    }
    if (s == NULL) {
        s = getOldestTimeWaitSocket();
        if (s != NULL) {
            deleteSocket(s);
        }
    }
    if (s != NULL) {
        uint8_t* txBuffer = socketTxBuffers[s - sockets];
        memset(s, 0, sizeof(socket));
        s->tx_buffer = txBuffer;
        s->type = type;
        s->state = TCP_CLOSED;
        s->assocTimer = INVALID_TIMER;
        s->localPort = getEphemeralPort();
        s->valid = 1;
    }
    return s;
}

void deleteSocket(socket* s) {
    if (s != NULL && s >= sockets && s < sockets + MAX_SOCKETS && s->valid) {
        stopTimerHandle(&s->assocTimer);
        cancelArpRequests(s);
        s->state = TCP_CLOSED;
        s->valid = 0;
    }
}

uint32_t getSocketId(socket* s) {
    uint32_t i;
    for (i = 0; i < MAX_SOCKETS; i++) {
        if (&sockets[i] == s) {
            return i;
        }
    }
    return 0xFFFFFFFF;
}

socket* getSockets() {
    return sockets;
}

static void socketSendToCallback(arpRespContext resp) {
    socket* s = (socket*)resp.ctxt;
    if (!s->valid) {
        return;
    }
    if (resp.success) {
        //if MAC was retrieved
        uint8_t buffer[MAX_PACKET_SIZE];
        copyMacAddress(s->remoteHwAddress, resp.responseMacAddress);
        sendUdpMessage((etherHeader*)buffer, s, s->tx_buffer, s->tx_size);
    }
    else {
        //could not resolve mac (No arp)
        throwSocketError(s, SOCKET_ERROR_ARP_TIMEOUT);
    }
}

void socketSendTo(socket* s, uint8_t serverIp[4], uint16_t port, uint8_t data[], uint16_t length) {
    if (s->type == SOCKET_DGRAM && length <= SOCKET_TX_BUFFER_SIZE) {
        getIpAddress(s->localIpAddress);
        copyIpAddress(s->remoteIpAddress, serverIp);
        s->remotePort = port;
        memcpy(s->tx_buffer, data, length);
        s->tx_size = length;
        resolveMacAddress(serverIp, socketSendToCallback, s);
    }
    else {
        //not a UDP socket or too long
    }
}

//return anything in the buffer
//update the buffer in process UDP or TCP response or anothjer  response??
void socketRecvFrom(socket* s) {
    if (s->type == SOCKET_DGRAM) {

    }
    else {

    }
}

// Starts an active open, completion is reported with SOCKET_EVENT_CONNECTED
void socketConnectTcp(socket* s, uint8_t serverIp[4], uint16_t port) {
    if (s->type == SOCKET_STREAM && s->state == TCP_CLOSED && !s->arpPending) {
        copyIpAddress(s->remoteIpAddress, serverIp);
        s->remotePort = port;
        openTcpConnection(s);
    }
    else {
        //not a tcp socket or already in use
    }
}

// Passive open, every accepted connection is a new socket that inherits the
// callbacks and userData of the listener and is announced with SOCKET_EVENT_ACCEPTED
bool socketListenTcp(socket* s, uint16_t port) {
    if (s->type != SOCKET_STREAM || s->state != TCP_CLOSED) {
        return false;
    }
    if (port != s->localPort && isLocalPortInUse(port)) {
        return false;
    }
    s->localPort = port;
    return listenTcp(s);
}

// Queues data for transmission, returns the number of bytes accepted
uint16_t socketSendTcp(socket* s, const uint8_t* data, uint16_t length) {
    if (s == NULL || !s->valid || s->type != SOCKET_STREAM) {
        return 0;
    }
    return sendTcpData(s, data, length);
}

uint16_t socketGetTxFree(socket* s) {
    if (s == NULL || !s->valid || s->type != SOCKET_STREAM) {
        return 0;
    }
    return getTcpTxFree(s);
}

// Graceful close, buffered data is sent before the FIN
void socketCloseTcp(socket* s) {
    if (s != NULL && s->valid && s->type == SOCKET_STREAM) {
        closeTcpConnection(s);
    }
}

// Abortive close, sends RST and frees the socket
void socketAbortTcp(socket* s) {
    if (s != NULL && s->valid && s->type == SOCKET_STREAM) {
        abortTcpConnection(s);
    }
}

// Get socket from ephemeral port, may expand this in the future to get socket from all fields
socket* getSocketFromLocalPort(uint16_t l_port) {
    uint32_t i;
    for (i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i].localPort == l_port && sockets[i].valid) {
            return &sockets[i];
        }
    }
    return NULL;
}

// Get socket information from a received ARP response message
void getSocketInfoFromArpResponse(etherHeader* ether, socket* s) {
    arpPacket *arp = (arpPacket*)ether->data;
    uint8_t i;
    for (i = 0; i < HW_ADD_LENGTH; i++)
        s->remoteHwAddress[i] = arp->sourceAddress[i];
    for (i = 0; i < IP_ADD_LENGTH; i++)
        s->remoteIpAddress[i] = arp->sourceIp[i];
}

// Get socket information from a received UDP packet
void getSocketInfoFromUdpPacket(etherHeader* ether, socket* s) {
    ipHeader *ip = (ipHeader*)ether->data;
    uint8_t ipHeaderLength = ip->size * 4;
    udpHeader *udp = (udpHeader*)((uint8_t*)ip + ipHeaderLength);
    uint8_t i;
    for (i = 0; i < HW_ADD_LENGTH; i++)
        s->remoteHwAddress[i] = ether->sourceAddress[i];
    for (i = 0; i < IP_ADD_LENGTH; i++)
        s->remoteIpAddress[i] = ip->sourceIp[i];
    s->remotePort = ntohs(udp->sourcePort);
    s->localPort = ntohs(udp->destPort);
}

// Get socket information from a received TCP packet
void getSocketInfoFromTcpPacket(etherHeader* ether, socket* s) {
    ipHeader *ip = (ipHeader*)ether->data;
    uint8_t ipHeaderLength = ip->size * 4;
    tcpHeader* tcp = (tcpHeader*)((uint8_t*)ip + ipHeaderLength);
    uint8_t i;
    for (i = 0; i < HW_ADD_LENGTH; i++)
        s->remoteHwAddress[i] = ether->sourceAddress[i];
    for (i = 0; i < IP_ADD_LENGTH; i++)
        s->remoteIpAddress[i] = ip->sourceIp[i];
    s->remotePort = ntohs(tcp->sourcePort);
    s->localPort = ntohs(tcp->destPort);
}

//this function shall be called anytime an error occurs and the application needs to be aware of it
void throwSocketError(socket* s, uint8_t errorCode) {
    socketError err;
    err.errorCode = errorCode;
    err.errorMsg[0] = '\0';
    switch (errorCode) {
    case SOCKET_ERROR_ARP_TIMEOUT:
        snprintf(err.errorMsg, SOCKET_ERROR_MAX_MSG_LEN, "Could not reach %d.%d.%d.%d (timed out)", s->remoteIpAddress[0], s->remoteIpAddress[1], s->remoteIpAddress[2], s->remoteIpAddress[3]);
        break;
    case SOCKET_ERROR_TCP_SYN_ACK_TIMEOUT:
        snprintf(err.errorMsg, SOCKET_ERROR_MAX_MSG_LEN, "No response from %d.%d.%d.%d (timed out)", s->remoteIpAddress[0], s->remoteIpAddress[1], s->remoteIpAddress[2], s->remoteIpAddress[3]);
        break;
    case SOCKET_ERROR_CONNECTION_RESET:
        snprintf(err.errorMsg, SOCKET_ERROR_MAX_MSG_LEN, "Connection was reset by remote host (%d.%d.%d.%d:%d)", s->remoteIpAddress[0], s->remoteIpAddress[1], s->remoteIpAddress[2], s->remoteIpAddress[3], s->remotePort);
        break;
    case SOCKET_ERROR_TIMEOUT:
        snprintf(err.errorMsg, SOCKET_ERROR_MAX_MSG_LEN, "Connection to %d.%d.%d.%d:%d timed out", s->remoteIpAddress[0], s->remoteIpAddress[1], s->remoteIpAddress[2], s->remoteIpAddress[3], s->remotePort);
        break;
    }
    err.sk = s;
    if (s->errorCallback) {
        s->errorCallback(&err); // for TCP the stack frees the socket after reporting the error
    }
}
