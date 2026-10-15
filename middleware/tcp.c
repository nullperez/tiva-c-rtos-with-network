/******************************************************************************
 * File:        tcp.c
 *
 * Author:      Giancarlo Perez
 *
 * Created:     12/7/24
 *
 * Description: TCP (RFC 9293)
 *
 *  Implemented:
 *   - active and passive open, simultaneous open, graceful close and abort
 *   - segment acceptability test, trimming and in-order delivery (out-of-order
 *     segments are dropped and re-ACKed, the peer retransmits them)
 *   - send buffer with go-back-N retransmission, RFC 6298 RTO with Karn's
 *     algorithm and exponential backoff
 *   - send window / window update rules, zero window probing
 *   - MSS option (sent and honored)
 *   - RFC 5961 challenge ACKs for RSTs and SYNs, RFC 6528 ISNs
 *   - TIME-WAIT of 2*MSL; RFC 1122 4.2.2.13 reopen from TIME-WAIT
 *
 *  Not implemented: urgent data, Nagle, delayed ACKs, congestion control
 *  (RFC 5681), window scaling, SACK, timestamps, keep-alives.
 ******************************************************************************/

//=============================================================================
// INCLUDES
//=============================================================================

#include "log.h"
#include "uart0.h"
#include "clock.h"
#include "timer.h"
#include "arp.h"
#include "ip.h"
#include "tcp.h"
#include "socket.h"
#include <stdio.h>
#include <string.h>

//=============================================================================
// DEFINES AND MACROS
//=============================================================================

// Sequence number comparisons modulo 2^32
#define SEQ_LT(a,b)  ((int32_t)((uint32_t)(a) - (uint32_t)(b)) < 0)
#define SEQ_LEQ(a,b) ((int32_t)((uint32_t)(a) - (uint32_t)(b)) <= 0)
#define SEQ_GT(a,b)  ((int32_t)((uint32_t)(a) - (uint32_t)(b)) > 0)
#define SEQ_GEQ(a,b) ((int32_t)((uint32_t)(a) - (uint32_t)(b)) >= 0)

#define MIN(a,b) ((a) < (b) ? (a) : (b))

#define TCP_HEADER_SIZE 20
#define IP_HEADER_SIZE 20
#define TCP_CLOCK_GRANULARITY_MS 1000

//=============================================================================
// TYPEDEFS AND GLOBALS
//=============================================================================

// Fields of a received segment in host byte order
typedef struct _tcpSegment {
    uint32_t seq;
    uint32_t ack;
    uint16_t flags;
    uint16_t wnd;
    uint16_t mss;     // MSS option value or 0
    uint16_t dataLen;
    const uint8_t* data;
} tcpSegment;

// Outgoing frames are built here so the received frame is never overwritten
// while it is being processed (word array for alignment)
static uint32_t tcpTxFrameWords[(MAX_PACKET_SIZE + 3) / 4];
static uint32_t issSecret = 0;
static uint16_t ipId = 0;

//=============================================================================
// STATIC FUNCTIONS
//=============================================================================

static void tcpOutput(socket* s, bool probe);
static void tcpStartRetransmitTimer(socket* s);
static void tcpTimerCallback(void* context);

// All state changes go through here so they can be logged
static void tcpChangeState(socket* s, uint8_t state) {
    if (s->state != state && logEnabled(LOG_TCP)) {
        logPrintf(LOG_TCP, "%u.%u.%u.%u:%u <-> :%u  %s -> %s",
                  s->remoteIpAddress[0], s->remoteIpAddress[1], s->remoteIpAddress[2], s->remoteIpAddress[3],
                  s->remotePort, s->localPort, getTcpStateName(s->state), getTcpStateName(state));
    }
    s->state = state;
}

static uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352D;
    x ^= x >> 15;
    x *= 0x846CA68B;
    x ^= x >> 16;
    return x;
}

// RFC 6528: ISN = M + F(localip, localport, remoteip, remoteport, secretkey)
// M is a 4 microsecond clock, F is a keyed hash (not cryptographic here)
static uint32_t generateIss(socket* s) {
    uint32_t h;
    if (issSecret == 0) {
        issSecret = random32() ^ (millis() * 2654435761u);
    }
    h = mix32(issSecret ^ convertIpAddressToU32(s->localIpAddress));
    h = mix32(h ^ convertIpAddressToU32(s->remoteIpAddress));
    h = mix32(h ^ (((uint32_t)s->localPort << 16) | s->remotePort));
    return millis() * 250 + h;
}

static void tcpNotify(socket* s, uint8_t event) {
    if (s->eventCallback) {
        s->eventCallback(s, event);
    }
}

static bool isTcpSynchronized(uint8_t state) {
    return state != TCP_CLOSED && state != TCP_LISTEN && state != TCP_SYN_SENT;
}

static uint16_t calcTcpChecksum(ipHeader* ip, tcpHeader* tcp, uint16_t tcpLength) {
    uint32_t sum = 0;
    uint16_t tmp16;
    uint16_t tcpLengthHton = htons(tcpLength);
    // pseudo-header
    sumIpWords(ip->sourceIp, 8, &sum);
    tmp16 = ip->protocol;
    sum += (tmp16 & 0xff) << 8;
    sumIpWords(&tcpLengthHton, 2, &sum);
    // header and data
    sumIpWords(tcp, tcpLength, &sum);
    return getIpChecksum(sum);
}

// Builds and transmits one segment
static void tcpTransmit(const uint8_t destHw[6], const uint8_t destIp[4], uint16_t localPort, uint16_t remotePort,
                        uint32_t seq, uint32_t ack, uint16_t flags, const uint8_t* data, uint16_t dataLen) {
    etherHeader* ether = (etherHeader*)tcpTxFrameWords;
    ipHeader* ip = (ipHeader*)ether->data;
    tcpHeader* tcp = (tcpHeader*)((uint8_t*)ip + IP_HEADER_SIZE);
    uint8_t optionsLength = 0;
    uint16_t tcpLength;
    uint8_t localHwAddress[HW_ADD_LENGTH];
    // Ether frame
    getEtherMacAddress(localHwAddress);
    copyMacAddress(ether->destAddress, destHw);
    copyMacAddress(ether->sourceAddress, localHwAddress);
    ether->frameType = htons(TYPE_IP);
    // IP header
    ip->rev = 0x4;
    ip->size = IP_HEADER_SIZE / 4;
    ip->typeOfService = 0;
    ip->id = htons(ipId++);
    ip->flagsAndOffset = 0;
    ip->ttl = 128;
    ip->protocol = PROTOCOL_TCP;
    ip->headerChecksum = 0;
    copyIpAddress(ip->destIp, destIp);
    getIpAddress(ip->sourceIp);
    // TCP header
    tcp->sourcePort = htons(localPort);
    tcp->destPort = htons(remotePort);
    tcp->sequenceNumber = htonl(seq);
    tcp->acknowledgementNumber = (flags & ACK) ? htonl(ack) : 0;
    tcp->windowSize = htons(WINDOW_SIZE);
    tcp->urgentPointer = 0;
    // MSS option on SYN segments only (RFC 9293 3.7.1)
    if (flags & SYN) {
        tcp->data[optionsLength++] = TCP_OPTION_MAX_SEGMENT_SIZE;
        tcp->data[optionsLength++] = 4;
        tcp->data[optionsLength++] = (uint8_t)(MAX_SEGMENT_SIZE >> 8);
        tcp->data[optionsLength++] = (uint8_t)(MAX_SEGMENT_SIZE & 0xFF);
    }
    if (dataLen > 0) {
        memcpy(tcp->data + optionsLength, data, dataLen);
    }
    // Length & Checksum Calculation
    tcpLength = TCP_HEADER_SIZE + optionsLength + dataLen;
    tcp->offsetFields = htons(((uint16_t)((TCP_HEADER_SIZE + optionsLength) / 4) << OFS_SHIFT) | (flags & 0x1FF));
    ip->length = htons(IP_HEADER_SIZE + tcpLength);
    calcIpChecksum(ip);
    tcp->checksum = 0;
    tcp->checksum = calcTcpChecksum(ip, tcp, tcpLength);
    putEtherPacket(ether, sizeof(etherHeader) + IP_HEADER_SIZE + tcpLength);
}

static void tcpSendSegment(socket* s, uint32_t seq, uint16_t flags, const uint8_t* data, uint16_t dataLen) {
    tcpTransmit(s->remoteHwAddress, s->remoteIpAddress, s->localPort, s->remotePort,
                seq, s->rcvNxt, flags, data, dataLen);
    if (flags & ACK) {
        s->flags &= ~ACK; // pending acknowledgment has been sent
    }
}

// RST for a segment that does not belong to a connection (RFC 9293 3.10.7.1)
static void tcpSendResetForSegment(etherHeader* ether, tcpSegment* seg) {
    ipHeader* ip = getIpHeader(ether);
    tcpHeader* tcp = getTcpHeader(ether);
    if (seg->flags & RST) {
        return; // never answer a reset with a reset
    }
    logPrintf(LOG_TCP, "no connection for segment to port %u from %u.%u.%u.%u:%u, sending RST",
              ntohs(tcp->destPort), ip->sourceIp[0], ip->sourceIp[1], ip->sourceIp[2], ip->sourceIp[3],
              ntohs(tcp->sourcePort));
    if (seg->flags & ACK) {
        tcpTransmit(ether->sourceAddress, ip->sourceIp, ntohs(tcp->destPort), ntohs(tcp->sourcePort),
                    seg->ack, 0, RST, NULL, 0);
    }
    else {
        uint32_t segLen = seg->dataLen + ((seg->flags & SYN) ? 1 : 0) + ((seg->flags & FIN) ? 1 : 0);
        tcpTransmit(ether->sourceAddress, ip->sourceIp, ntohs(tcp->destPort), ntohs(tcp->sourcePort),
                    0, seg->seq + segLen, RST | ACK, NULL, 0);
    }
}

// RFC 5961 challenge ACK
static void tcpSendChallengeAck(socket* s) {
    tcpSendSegment(s, s->sndNxt, ACK, NULL, 0);
}

static uint16_t parseTcpMssOption(tcpHeader* tcp, uint8_t headerLength) {
    uint8_t* opt = tcp->data;
    uint8_t len = headerLength - TCP_HEADER_SIZE;
    uint8_t i = 0;
    while (i < len) {
        uint8_t kind = opt[i];
        uint8_t optLen;
        if (kind == TCP_OPTION_END) {
            break;
        }
        if (kind == TCP_OPTION_NO_OP) {
            i++;
            continue;
        }
        if (i + 1 >= len) {
            break;
        }
        optLen = opt[i + 1];
        if (optLen < 2 || i + optLen > len) {
            break;
        }
        if (kind == TCP_OPTION_MAX_SEGMENT_SIZE && optLen == 4) {
            return ((uint16_t)opt[i + 2] << 8) | opt[i + 3];
        }
        i += optLen;
    }
    return 0;
}

static void tcpInitTcb(socket* s) {
    s->rto = TCP_INITIAL_RTO_MS;
    s->srtt = 0;
    s->rttvar = 0;
    s->hasRttSample = false;
    s->rttActive = false;
    s->retransmits = 0;
    s->sndMss = TCP_DEFAULT_MSS;
    s->tx_size = 0;
    s->flags = 0;
    s->finPending = false;
    s->finSent = false;
    getIpAddress(s->localIpAddress);
}

// Picks the ISN and initializes the send sequence variables
static void tcpInitSendSequence(socket* s) {
    s->iss = generateIss(s);
    s->sndUna = s->iss;
    s->sndNxt = s->iss + 1; // the SYN uses one sequence number
    s->txSeq = s->iss + 1;  // first data byte follows the SYN
}

static void tcpSetMss(socket* s, uint16_t mss) {
    s->sndMss = (mss == 0) ? TCP_DEFAULT_MSS : MIN(mss, MAX_SEGMENT_SIZE);
}

// RFC 6298 2.2 and 2.3
static void tcpUpdateRto(socket* s, uint32_t r) {
    uint32_t delta;
    if (!s->hasRttSample) {
        s->srtt = r;
        s->rttvar = r / 2;
        s->hasRttSample = true;
    }
    else {
        delta = (s->srtt > r) ? s->srtt - r : r - s->srtt;
        s->rttvar = (3 * s->rttvar + delta) / 4;
        s->srtt = (7 * s->srtt + r) / 8;
    }
    s->rto = s->srtt + ((4 * s->rttvar > TCP_CLOCK_GRANULARITY_MS) ? 4 * s->rttvar : TCP_CLOCK_GRANULARITY_MS);
    if (s->rto < TCP_MIN_RTO_MS) {
        s->rto = TCP_MIN_RTO_MS;
    }
    if (s->rto > TCP_MAX_RTO_MS) {
        s->rto = TCP_MAX_RTO_MS;
    }
}

// Ends the connection and frees the socket, SOCKET_EVENT_CLOSED is the last callback
static void tcpTerminate(socket* s, uint8_t errorCode) {
    stopTimerHandle(&s->assocTimer);
    if (errorCode != SOCKET_ERROR_NO_ERROR) {
        logPrintf(LOG_TCP, ":%u connection ended with error %u", s->localPort, errorCode);
    }
    tcpChangeState(s, TCP_CLOSED);
    if (errorCode != SOCKET_ERROR_NO_ERROR) {
        throwSocketError(s, errorCode);
    }
    tcpNotify(s, SOCKET_EVENT_CLOSED);
    deleteSocket(s);
}

// From the application's view the connection is over in TIME-WAIT, the
// socket stays with the stack for 2*MSL so delayed segments are handled
static void tcpEnterTimeWait(socket* s) {
    tcpChangeState(s, TCP_TIME_WAIT);
    s->timeWaitStart = millis();
    stopTimerHandle(&s->assocTimer);
    tcpNotify(s, SOCKET_EVENT_CLOSED);
    s->recvCallback = NULL;
    s->eventCallback = NULL;
    s->errorCallback = NULL;
    s->assocTimer = startOneshotTimer(tcpTimerCallback, 2 * TCP_MSL_SECONDS, s);
    // without a timer the socket would never leave TIME-WAIT
    if (s->assocTimer == INVALID_TIMER) {
        deleteSocket(s);
    }
}

// sndNxt - txSeq is the number of buffered bytes already sent
static bool tcpHasUnsentData(socket* s) {
    return SEQ_GEQ(s->sndNxt, s->txSeq) && (uint32_t)(s->sndNxt - s->txSeq) < s->tx_size;
}

static bool tcpHasOutstanding(socket* s) {
    return (s->sndUna != s->sndNxt) || tcpHasUnsentData(s) || (s->finPending && !s->finSent);
}

// Retransmission timer, also used for SYN retries, zero window probes,
// FIN-WAIT-2 timeout and TIME-WAIT
static void tcpTimerCallback(void* context) {
    socket* s = (socket*)context;
    s->assocTimer = INVALID_TIMER;
    if (!s->valid) {
        return;
    }
    switch (s->state) {
    case TCP_CLOSED:
    case TCP_LISTEN:
        return;
    case TCP_TIME_WAIT:
        tcpTerminate(s, SOCKET_ERROR_NO_ERROR);
        return;
    case TCP_FIN_WAIT_2:
        // our FIN was acknowledged but the peer never closed
        tcpTerminate(s, SOCKET_ERROR_NO_ERROR);
        return;
    default:
        break;
    }
    if (!tcpHasOutstanding(s)) {
        return;
    }
    s->retransmits++;
    logPrintf(LOG_TCP, ":%u retransmit %u (rto %lu ms, %s)", s->localPort, s->retransmits,
              (unsigned long)s->rto, getTcpStateName(s->state));
    if (s->state == TCP_SYN_SENT || s->state == TCP_SYN_RECEIVED) {
        if (s->retransmits >= TCP_MAX_SYN_ATTEMPTS) {
            if (s->state == TCP_SYN_SENT) {
                tcpTerminate(s, SOCKET_ERROR_TCP_SYN_ACK_TIMEOUT);
            }
            else {
                // half-open connection, a passive one was never seen by the application
                tcpTerminate(s, (s->listener != NULL) ? SOCKET_ERROR_NO_ERROR : SOCKET_ERROR_TIMEOUT);
            }
            return;
        }
        if (s->state == TCP_SYN_SENT) {
            snprintf(out, MAX_UART_OUT, "TCP: Retrying connection to server... (%d/%d)\n", s->retransmits + 1, TCP_MAX_SYN_ATTEMPTS);
            putsUart0(out);
        }
    }
    else if (s->retransmits > TCP_MAX_RETRANSMITS) {
        tcpSendSegment(s, s->sndNxt, RST, NULL, 0);
        tcpTerminate(s, SOCKET_ERROR_TIMEOUT);
        return;
    }
    // RFC 6298 5.5-5.7: back off, Karn's algorithm
    s->rto = MIN(s->rto * 2, TCP_MAX_RTO_MS);
    s->rttActive = false;
    switch (s->state) {
    case TCP_SYN_SENT:
        tcpSendSegment(s, s->iss, SYN, NULL, 0);
        break;
    case TCP_SYN_RECEIVED:
        tcpSendSegment(s, s->iss, SYN | ACK, NULL, 0);
        break;
    default:
        // go back N: resend everything from the oldest unacknowledged byte
        s->sndNxt = s->sndUna;
        s->finSent = false;
        tcpOutput(s, true);
        break;
    }
    tcpStartRetransmitTimer(s);
}

// Timer granularity is 1 s and the first tick comes after 0..1 s, so one
// second is added to never fire before the RTO
static void tcpStartRetransmitTimer(socket* s) {
    stopTimerHandle(&s->assocTimer);
    s->assocTimer = startOneshotTimer(tcpTimerCallback, s->rto / 1000 + 1, s);
}

// Sends queued data, the FIN and pending ACKs
// probe allows one byte into a zero window (RFC 9293 3.8.6.1)
static void tcpOutput(socket* s, bool probe) {
    bool sent = false;
    bool canSendData = (s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT
                        || s->state == TCP_FIN_WAIT_1 || s->state == TCP_CLOSING || s->state == TCP_LAST_ACK);
    if (canSendData) {
        uint16_t mss = MIN(s->sndMss, MAX_SEGMENT_SIZE);
        while (true) {
            uint32_t offset = s->sndNxt - s->txSeq; // bytes of tx_buffer already sent
            uint32_t inFlight = s->sndNxt - s->sndUna;
            uint32_t window = s->sndWnd;
            uint32_t usable;
            uint16_t unsent;
            uint16_t len;
            if (offset >= s->tx_size) {
                break;
            }
            unsent = s->tx_size - offset;
            if (window == 0 && probe && inFlight == 0) {
                window = 1;
            }
            usable = (window > inFlight) ? window - inFlight : 0;
            len = MIN(MIN(unsent, usable), mss);
            if (len == 0) {
                break;
            }
            tcpSendSegment(s, s->sndNxt, ACK | ((len == unsent) ? PSH : 0), s->tx_buffer + offset, len);
            if (!s->rttActive) {
                s->rttActive = true;
                s->rttSeq = s->sndNxt + len;
                s->rttStart = millis();
            }
            s->sndNxt += len;
            sent = true;
            probe = false;
        }
        // FIN once all data has been sent
        if (s->finPending && !s->finSent && (s->sndNxt - s->txSeq) == s->tx_size) {
            tcpSendSegment(s, s->sndNxt, FIN | ACK, NULL, 0);
            s->sndNxt++;
            s->finSent = true;
            sent = true;
            if (s->state == TCP_ESTABLISHED) {
                tcpChangeState(s, TCP_FIN_WAIT_1);
            }
            else if (s->state == TCP_CLOSE_WAIT) {
                tcpChangeState(s, TCP_LAST_ACK);
            }
        }
    }
    // ACK that was not piggybacked on data
    if (s->flags & ACK) {
        if (isTcpSynchronized(s->state)) {
            tcpSendSegment(s, s->sndNxt, ACK, NULL, 0);
        }
        s->flags &= ~ACK;
    }
    // RFC 6298 5.1, also arms zero window probing when data is waiting
    if ((sent || (canSendData && tcpHasOutstanding(s))) && s->assocTimer == INVALID_TIMER) {
        tcpStartRetransmitTimer(s);
    }
}

// Processes an acknowledgment of new data (SND.UNA < SEG.ACK =< SND.NXT)
static void tcpAckReceived(socket* s, uint32_t ack) {
    s->sndUna = ack;
    // drop acknowledged bytes from the send buffer
    if (SEQ_GT(ack, s->txSeq)) {
        uint32_t n = ack - s->txSeq;
        if (n > s->tx_size) {
            n = s->tx_size; // the rest acknowledges our FIN
        }
        memmove(s->tx_buffer, s->tx_buffer + n, s->tx_size - n);
        s->tx_size -= n;
        s->txSeq += n;
    }
    if (s->rttActive && SEQ_GEQ(ack, s->rttSeq)) {
        tcpUpdateRto(s, millis() - s->rttStart);
        s->rttActive = false;
    }
    s->retransmits = 0;
    // RFC 6298 5.2 and 5.3
    stopTimerHandle(&s->assocTimer);
    if (s->sndUna != s->sndNxt) {
        tcpStartRetransmitTimer(s);
    }
}

static void tcpArpResCallback(arpRespContext resp) {
    socket* s = (socket*)resp.ctxt;
    if (!s->valid || !s->arpPending) {
        return;
    }
    s->arpPending = false;
    if (resp.success) {
        copyMacAddress(s->remoteHwAddress, resp.responseMacAddress);
        tcpInitSendSequence(s);
        tcpChangeState(s, TCP_SYN_SENT);
        tcpSendSegment(s, s->iss, SYN, NULL, 0);
        s->rttActive = true;
        s->rttSeq = s->iss + 1;
        s->rttStart = millis();
        tcpStartRetransmitTimer(s);
    }
    else {
        //if ARP function responded with error (timed out)
        tcpTerminate(s, SOCKET_ERROR_ARP_TIMEOUT);
    }
}

static socket* findTcpSocket(uint8_t remoteIp[4], uint16_t remotePort, uint16_t localPort) {
    uint8_t i;
    socket* sockets = getSockets();
    for (i = 0; i < MAX_SOCKETS; i++) {
        socket* s = &sockets[i];
        if (s->valid && s->type == SOCKET_STREAM && s->state != TCP_CLOSED && s->state != TCP_LISTEN
                && s->localPort == localPort && s->remotePort == remotePort
                && isIpEqual(s->remoteIpAddress, remoteIp)) {
            return s;
        }
    }
    return NULL;
}

static socket* findTcpListener(uint16_t localPort) {
    uint8_t i;
    socket* sockets = getSockets();
    for (i = 0; i < MAX_SOCKETS; i++) {
        socket* s = &sockets[i];
        if (s->valid && s->type == SOCKET_STREAM && s->state == TCP_LISTEN && s->localPort == localPort) {
            return s;
        }
    }
    return NULL;
}

// LISTEN state (RFC 9293 3.10.7.2)
static void tcpListenInput(socket* l, etherHeader* ether, tcpSegment* seg) {
    ipHeader* ip = getIpHeader(ether);
    socket* c;
    if (seg->flags & RST) {
        return;
    }
    if (seg->flags & ACK) {
        tcpSendResetForSegment(ether, seg);
        return;
    }
    if (!(seg->flags & SYN)) {
        return;
    }
    c = newSocket(SOCKET_STREAM);
    if (c == NULL) {
        return; // no free socket, the client will retransmit its SYN
    }
    c->localPort = l->localPort;
    copyIpAddress(c->remoteIpAddress, ip->sourceIp);
    c->remotePort = ntohs(getTcpHeader(ether)->sourcePort);
    copyMacAddress(c->remoteHwAddress, ether->sourceAddress); // router MAC if the client is off-subnet
    c->listener = l;
    tcpInitTcb(c);
    tcpSetMss(c, seg->mss);
    c->irs = seg->seq;
    c->rcvNxt = seg->seq + 1;
    c->sndWnd = seg->wnd;
    c->sndWl1 = seg->seq;
    tcpInitSendSequence(c);
    c->sndWl2 = c->iss;
    tcpChangeState(c, TCP_SYN_RECEIVED);
    // data or FIN carried on the SYN is not queued, it is not acknowledged and will be retransmitted
    tcpSendSegment(c, c->iss, SYN | ACK, NULL, 0);
    c->rttActive = true;
    c->rttSeq = c->iss + 1;
    c->rttStart = millis();
    tcpStartRetransmitTimer(c);
}

// SYN-SENT state (RFC 9293 3.10.7.3)
static void tcpSynSentInput(socket* s, etherHeader* ether, tcpSegment* seg) {
    bool ackAcceptable = false;
    if (seg->flags & ACK) {
        if (SEQ_LEQ(seg->ack, s->iss) || SEQ_GT(seg->ack, s->sndNxt)) {
            tcpSendResetForSegment(ether, seg);
            return;
        }
        ackAcceptable = true;
    }
    if (seg->flags & RST) {
        if (ackAcceptable) {
            tcpTerminate(s, SOCKET_ERROR_CONNECTION_RESET);
        }
        return;
    }
    if (!(seg->flags & SYN)) {
        return;
    }
    s->irs = seg->seq;
    s->rcvNxt = seg->seq + 1;
    tcpSetMss(s, seg->mss);
    if (ackAcceptable) {
        s->sndUna = seg->ack;
    }
    if (SEQ_GT(s->sndUna, s->iss)) {
        // our SYN has been acknowledged
        s->sndWnd = seg->wnd;
        s->sndWl1 = seg->seq;
        s->sndWl2 = seg->ack;
        if (s->rttActive) {
            tcpUpdateRto(s, millis() - s->rttStart);
            s->rttActive = false;
        }
        s->retransmits = 0;
        stopTimerHandle(&s->assocTimer);
        tcpChangeState(s, TCP_ESTABLISHED);
        s->flags |= ACK;
        tcpNotify(s, SOCKET_EVENT_CONNECTED);
        if (s->valid && s->state == TCP_ESTABLISHED) {
            tcpOutput(s, false); // queued data carries the ACK, otherwise a bare ACK
        }
    }
    else {
        // simultaneous open
        tcpChangeState(s, TCP_SYN_RECEIVED);
        tcpSendSegment(s, s->iss, SYN | ACK, NULL, 0);
        tcpStartRetransmitTimer(s);
    }
}

// SYN-RECEIVED and all synchronized states (RFC 9293 3.10.7.4)
static void tcpSynchronizedInput(socket* s, tcpSegment* seg) {
    const uint32_t rcvWnd = WINDOW_SIZE;
    bool fin = (seg->flags & FIN) != 0;
    uint32_t segLen = seg->dataLen + ((seg->flags & SYN) ? 1 : 0) + (fin ? 1 : 0);
    uint32_t seq = seg->seq;
    const uint8_t* data = seg->data;
    uint16_t dataLen = seg->dataLen;
    bool acceptable;
    bool finAcked;

    // first, check sequence number
    if (segLen == 0) {
        acceptable = SEQ_GEQ(seq, s->rcvNxt) && SEQ_LT(seq, s->rcvNxt + rcvWnd);
    }
    else {
        acceptable = (SEQ_GEQ(seq, s->rcvNxt) && SEQ_LT(seq, s->rcvNxt + rcvWnd))
                  || (SEQ_GEQ(seq + segLen - 1, s->rcvNxt) && SEQ_LT(seq + segLen - 1, s->rcvNxt + rcvWnd));
    }
    if (!acceptable) {
        if (!(seg->flags & RST)) {
            // retransmitted FIN in TIME-WAIT restarts the 2*MSL timeout
            if (s->state == TCP_TIME_WAIT && fin) {
                stopTimerHandle(&s->assocTimer);
                s->assocTimer = startOneshotTimer(tcpTimerCallback, 2 * TCP_MSL_SECONDS, s);
            }
            tcpSendSegment(s, s->sndNxt, ACK, NULL, 0);
        }
        return;
    }

    // second, check the RST bit (RFC 5961 3.2)
    if (seg->flags & RST) {
        if (seq == s->rcvNxt) {
            if ((s->state == TCP_SYN_RECEIVED && s->listener != NULL)
                    || s->state == TCP_CLOSING || s->state == TCP_LAST_ACK || s->state == TCP_TIME_WAIT) {
                tcpTerminate(s, SOCKET_ERROR_NO_ERROR);
            }
            else {
                tcpTerminate(s, SOCKET_ERROR_CONNECTION_RESET);
            }
        }
        else {
            tcpSendChallengeAck(s);
        }
        return;
    }

    // fourth, check the SYN bit (RFC 5961 4.2)
    if (seg->flags & SYN) {
        tcpSendChallengeAck(s);
        return;
    }

    // fifth, check the ACK field
    if (!(seg->flags & ACK)) {
        return;
    }
    if (s->state == TCP_SYN_RECEIVED) {
        if (SEQ_GT(seg->ack, s->sndUna) && SEQ_LEQ(seg->ack, s->sndNxt)) {
            tcpChangeState(s, TCP_ESTABLISHED);
            s->sndWnd = seg->wnd;
            s->sndWl1 = seq;
            s->sndWl2 = seg->ack;
            if (s->listener != NULL) {
                socket* l = s->listener;
                if (!(l->valid && l->state == TCP_LISTEN && l->localPort == s->localPort)) {
                    // listener is gone, nobody owns this connection
                    tcpSendSegment(s, seg->ack, RST, NULL, 0);
                    tcpTerminate(s, SOCKET_ERROR_NO_ERROR);
                    return;
                }
                s->errorCallback = l->errorCallback;
                s->recvCallback = l->recvCallback;
                s->eventCallback = l->eventCallback;
                s->userData = l->userData;
            }
        }
        else {
            tcpTransmit(s->remoteHwAddress, s->remoteIpAddress, s->localPort, s->remotePort, seg->ack, 0, RST, NULL, 0);
            return;
        }
    }
    if (SEQ_GT(seg->ack, s->sndNxt)) {
        // acknowledges something not yet sent
        tcpSendSegment(s, s->sndNxt, ACK, NULL, 0);
        return;
    }
    if (SEQ_GT(seg->ack, s->sndUna)) {
        bool wasSynReceived = SEQ_LEQ(s->sndUna, s->iss);
        tcpAckReceived(s, seg->ack);
        if (wasSynReceived) {
            tcpNotify(s, (s->listener != NULL) ? SOCKET_EVENT_ACCEPTED : SOCKET_EVENT_CONNECTED);
            if (!s->valid || s->state == TCP_CLOSED) {
                return;
            }
        }
    }
    // window update
    if (SEQ_LEQ(s->sndUna, seg->ack) && (SEQ_LT(s->sndWl1, seq) || (s->sndWl1 == seq && SEQ_LEQ(s->sndWl2, seg->ack)))) {
        s->sndWnd = seg->wnd;
        s->sndWl1 = seq;
        s->sndWl2 = seg->ack;
    }
    if (s->sndWnd == 0) {
        s->retransmits = 0; // peer is alive, keep probing the zero window
    }
    finAcked = s->finSent && s->sndUna == s->sndNxt;
    switch (s->state) {
    case TCP_FIN_WAIT_1:
        if (finAcked) {
            tcpChangeState(s, TCP_FIN_WAIT_2);
            stopTimerHandle(&s->assocTimer);
            s->assocTimer = startOneshotTimer(tcpTimerCallback, TCP_FIN_WAIT_2_TIMEOUT, s);
        }
        break;
    case TCP_CLOSING:
        if (finAcked) {
            tcpEnterTimeWait(s);
        }
        return;
    case TCP_LAST_ACK:
        if (finAcked) {
            tcpTerminate(s, SOCKET_ERROR_NO_ERROR);
        }
        return;
    default:
        break;
    }

    // trim data that was already received
    if (SEQ_LT(seq, s->rcvNxt)) {
        uint32_t skip = s->rcvNxt - seq;
        if (skip > dataLen) {
            dataLen = 0;
            fin = false; // retransmitted FIN that was already counted
        }
        else {
            data += skip;
            dataLen -= skip;
        }
        seq = s->rcvNxt;
        s->flags |= ACK;
    }
    // out of order segments are not queued, a duplicate ACK asks for the missing data
    if (seq != s->rcvNxt) {
        dataLen = 0;
        fin = false;
        s->flags |= ACK;
    }
    // trim data beyond the receive window
    if (dataLen > rcvWnd) {
        dataLen = rcvWnd;
        fin = false;
    }

    // seventh, process the segment text
    if (dataLen > 0) {
        if (s->state == TCP_ESTABLISHED || s->state == TCP_FIN_WAIT_1 || s->state == TCP_FIN_WAIT_2) {
            s->rcvNxt += dataLen;
            s->flags |= ACK;
            if (s->state == TCP_FIN_WAIT_2) {
                // the peer is still sending, restart the FIN-WAIT-2 timeout
                stopTimerHandle(&s->assocTimer);
                s->assocTimer = startOneshotTimer(tcpTimerCallback, TCP_FIN_WAIT_2_TIMEOUT, s);
            }
            if (s->recvCallback) {
                s->recvCallback(s, data, dataLen);
            }
            if (!s->valid || s->state == TCP_CLOSED) {
                return; // application aborted the connection
            }
        }
        else {
            fin = false; // data after the peer's FIN is not possible, ignore the segment
        }
    }

    // eighth, check the FIN bit
    if (fin) {
        switch (s->state) {
        case TCP_SYN_RECEIVED:
        case TCP_ESTABLISHED:
            s->rcvNxt++;
            s->flags |= ACK;
            tcpChangeState(s, TCP_CLOSE_WAIT);
            tcpOutput(s, false); // acknowledge the FIN before the application reacts
            tcpNotify(s, SOCKET_EVENT_PEER_CLOSED);
            if (!s->valid || s->state == TCP_CLOSED) {
                return;
            }
            break;
        case TCP_FIN_WAIT_1:
            s->rcvNxt++;
            s->flags |= ACK;
            if (finAcked) {
                tcpOutput(s, false);
                tcpEnterTimeWait(s);
                return;
            }
            tcpChangeState(s, TCP_CLOSING);
            break;
        case TCP_FIN_WAIT_2:
            s->rcvNxt++;
            s->flags |= ACK;
            tcpOutput(s, false);
            tcpEnterTimeWait(s);
            return;
        default:
            break; // CLOSE-WAIT, CLOSING, LAST-ACK, TIME-WAIT: FIN already received
        }
    }
    tcpOutput(s, false);
}

//=============================================================================
// PUBLIC FUNCTIONS
//=============================================================================

// Set TCP state
void setTcpState(socket* s, uint8_t state) {
    if (s) {
        tcpChangeState(s, state);
    }
}

// Get TCP state
uint8_t getTcpState(socket* s) {
    if (s && s->valid) {
        return s->state;
    }
    return TCP_CLOSED;
}

const char* getTcpStateName(uint8_t state) {
    switch (state) {
    case TCP_CLOSED:       return "CLOSED";
    case TCP_LISTEN:       return "LISTEN";
    case TCP_SYN_RECEIVED: return "SYN_RECEIVED";
    case TCP_SYN_SENT:     return "SYN_SENT";
    case TCP_ESTABLISHED:  return "ESTABLISHED";
    case TCP_FIN_WAIT_1:   return "FIN_WAIT_1";
    case TCP_FIN_WAIT_2:   return "FIN_WAIT_2";
    case TCP_CLOSING:      return "CLOSING";
    case TCP_CLOSE_WAIT:   return "CLOSE_WAIT";
    case TCP_LAST_ACK:     return "LAST_ACK";
    case TCP_TIME_WAIT:    return "TIME_WAIT";
    default:               return "UNKNOWN";
    }
}

tcpHeader* getTcpHeader(etherHeader* ether) {
    ipHeader* ip = (ipHeader*)ether->data;
    tcpHeader* tcp = (tcpHeader*)((uint8_t*)ip + ip->size * 4);
    return tcp;
}

uint8_t* getTcpData(etherHeader* ether) {
    tcpHeader* tcp = getTcpHeader(ether);
    return (uint8_t*)tcp + (ntohs(tcp->offsetFields) >> OFS_SHIFT) * 4;
}

uint16_t getTcpDataLength(etherHeader* ether) {
    ipHeader* ip = getIpHeader(ether);
    tcpHeader* tcp = getTcpHeader(ether);
    uint16_t payload_length = ntohs(ip->length) - (ip->size * 4) - ((ntohs(tcp->offsetFields) >> OFS_SHIFT) * 4);
    return payload_length;
}

// Determines whether packet is a TCP segment with a valid header and checksum
// Must be an IP packet
bool isTcp(etherHeader* ether) {
    ipHeader* ip = (ipHeader*)ether->data;
    tcpHeader* tcp = (tcpHeader*)((uint8_t*)ip + ip->size * 4);
    uint16_t tcpLength;
    uint16_t headerLength;
    if (ip->protocol != PROTOCOL_TCP) {
        return false;
    }
    tcpLength = ntohs(ip->length) - ip->size * 4;
    if (tcpLength < TCP_HEADER_SIZE) {
        return false;
    }
    headerLength = (ntohs(tcp->offsetFields) >> OFS_SHIFT) * 4;
    if (headerLength < TCP_HEADER_SIZE || headerLength > tcpLength) {
        return false;
    }
    // the sum over a segment that includes a correct checksum is 0
    return calcTcpChecksum(ip, tcp, tcpLength) == 0;
}

// Segment arrival, the frame must have passed isIp, isIpUnicast and isTcp
void processTcpSegment(etherHeader* ether) {
    ipHeader* ip = getIpHeader(ether);
    tcpHeader* tcp = getTcpHeader(ether);
    uint8_t headerLength = (ntohs(tcp->offsetFields) >> OFS_SHIFT) * 4;
    uint16_t localPort = ntohs(tcp->destPort);
    uint16_t remotePort = ntohs(tcp->sourcePort);
    tcpSegment seg;
    socket* s;

    seg.seq = ntohl(tcp->sequenceNumber);
    seg.ack = ntohl(tcp->acknowledgementNumber);
    seg.flags = ntohs(tcp->offsetFields) & 0x1FF;
    seg.wnd = ntohs(tcp->windowSize);
    seg.data = (uint8_t*)tcp + headerLength;
    seg.dataLen = getTcpDataLength(ether);
    seg.mss = (seg.flags & SYN) ? parseTcpMssOption(tcp, headerLength) : 0;

    s = findTcpSocket(ip->sourceIp, remotePort, localPort);
    // RFC 1122 4.2.2.13: a new SYN may reopen a connection in TIME-WAIT
    if (s && s->state == TCP_TIME_WAIT && (seg.flags & SYN) && !(seg.flags & (ACK | RST))
            && SEQ_GT(seg.seq, s->rcvNxt) && findTcpListener(localPort) != NULL) {
        tcpTerminate(s, SOCKET_ERROR_NO_ERROR);
        s = NULL;
    }
    if (s == NULL) {
        socket* l = findTcpListener(localPort);
        if (l) {
            tcpListenInput(l, ether, &seg);
        }
        else {
            tcpSendResetForSegment(ether, &seg);
        }
        return;
    }
    if (s->state == TCP_SYN_SENT) {
        tcpSynSentInput(s, ether, &seg);
    }
    else {
        tcpSynchronizedInput(s, &seg);
    }
}

// Looping function, sends data that became sendable and pending ACKs
void sendTcpPendingMessages(void) {
    uint32_t i;
    socket* sockets = getSockets();
    for (i = 0; i < MAX_SOCKETS; i++) {
        socket* s = &sockets[i];
        if (s->valid && s->type == SOCKET_STREAM && isTcpSynchronized(s->state)) {
            if ((s->flags & ACK) || tcpHasUnsentData(s) || (s->finPending && !s->finSent)) {
                tcpOutput(s, false);
            }
        }
    }
}

// Active open: resolve the MAC address then send the SYN
void openTcpConnection(socket* s) {
    tcpInitTcb(s);
    s->arpPending = true;
    resolveMacAddress(s->remoteIpAddress, tcpArpResCallback, s);
}

// Passive open
bool listenTcp(socket* s) {
    tcpInitTcb(s);
    s->listener = NULL;
    tcpChangeState(s, TCP_LISTEN);
    return true;
}

// Queues data, it is sent as the peer's window allows
uint16_t sendTcpData(socket* s, const uint8_t* data, uint16_t length) {
    uint16_t space;
    bool canQueue = s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT
                 || s->state == TCP_SYN_SENT || s->state == TCP_SYN_RECEIVED
                 || (s->state == TCP_CLOSED && s->arpPending);
    if (!canQueue || s->finPending || length == 0) {
        return 0;
    }
    space = SOCKET_TX_BUFFER_SIZE - s->tx_size;
    if (length > space) {
        length = space;
    }
    memcpy(s->tx_buffer + s->tx_size, data, length);
    s->tx_size += length;
    tcpOutput(s, false);
    return length;
}

uint16_t getTcpTxFree(socket* s) {
    if (s->finPending) {
        return 0;
    }
    return SOCKET_TX_BUFFER_SIZE - s->tx_size;
}

// CLOSE call (RFC 9293 3.10.4)
void closeTcpConnection(socket* s) {
    switch (s->state) {
    case TCP_CLOSED:   // ARP still pending
    case TCP_LISTEN:
    case TCP_SYN_SENT:
        tcpTerminate(s, SOCKET_ERROR_NO_ERROR);
        break;
    case TCP_SYN_RECEIVED: // FIN goes out once the connection is established
    case TCP_ESTABLISHED:
    case TCP_CLOSE_WAIT:
        s->finPending = true;
        tcpOutput(s, false);
        break;
    default:
        break; // already closing
    }
}

// ABORT call (RFC 9293 3.10.5)
void abortTcpConnection(socket* s) {
    switch (s->state) {
    case TCP_SYN_RECEIVED:
    case TCP_ESTABLISHED:
    case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2:
    case TCP_CLOSE_WAIT:
        tcpSendSegment(s, s->sndNxt, RST, NULL, 0);
        break;
    default:
        break;
    }
    tcpTerminate(s, SOCKET_ERROR_NO_ERROR);
}
