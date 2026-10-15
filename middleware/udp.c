/******************************************************************************
 * File:        udp.c
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

#include "ip.h"
#include "udp.h"
#include <stdio.h>

//=============================================================================
// DEFINES AND MACROS
//=============================================================================

//=============================================================================
// GLOBALS
//=============================================================================


//=============================================================================
// PUBLIC FUNCTIONS
//=============================================================================

// Determines whether packet is UDP datagram
// Must be an IP packet
bool isUdp(etherHeader *ether) {
    ipHeader *ip = (ipHeader*)ether->data;
    uint8_t ipHeaderLength = ip->size * 4;
    udpHeader *udp = (udpHeader*)((uint8_t*)ip + ipHeaderLength);
    bool ok;
    uint16_t tmp16;
    uint32_t sum = 0;
    ok = (ip->protocol == PROTOCOL_UDP);
    // UDP length must fit inside the IP datagram
    ok = ok && (ntohs(udp->length) >= sizeof(udpHeader));
    ok = ok && (ntohs(udp->length) <= ntohs(ip->length) - ipHeaderLength);
    // a checksum of 0 means the sender did not compute one (RFC 768)
    if (ok && udp->check != 0) {
        // 32-bit sum over pseudo-header
        sumIpWords(ip->sourceIp, 8, &sum);
        tmp16 = ip->protocol;
        sum += (tmp16 & 0xff) << 8;
        sumIpWords(&udp->length, 2, &sum);
        // add udp header and data
        sumIpWords(udp, ntohs(udp->length), &sum);
        ok = (getIpChecksum(sum) == 0);
    }
    return ok;
}

udpHeader* getUdpHeader(etherHeader* ether) {
    ipHeader* ip = (ipHeader*)ether->data;
    uint8_t ipHeaderLength = ip->size * 4;
    udpHeader* udp = (udpHeader*)((uint8_t*)ip + ipHeaderLength);
    return udp;
}

// Gets pointer to UDP payload of frame
uint8_t* getUdpData(etherHeader *ether) {
    ipHeader *ip = (ipHeader*)ether->data;
    uint8_t ipHeaderLength = ip->size * 4;
    udpHeader *udp = (udpHeader*)((uint8_t*)ip + ipHeaderLength);
    return udp->data;
}

// Gets length of UDP payload of frame
uint16_t getUdpDataLength(etherHeader *ether) {
    udpHeader *udp = getUdpHeader(ether);
    return ntohs(udp->length) - sizeof(udpHeader);
}

// Send UDP message
void sendUdpMessage(etherHeader* ether, socket* s, uint8_t data[], uint16_t dataSize) {
    uint16_t i;
    uint32_t sum;
    uint16_t tmp16;
    uint16_t udpLength;
    uint8_t *copyData;
    uint8_t localHwAddress[6];
    uint8_t localIpAddress[4];

    // Ether frame
    getEtherMacAddress(localHwAddress);
    getIpAddress(localIpAddress);
    for (i = 0; i < HW_ADD_LENGTH; i++) {
        ether->destAddress[i] = s->remoteHwAddress[i]; //potentially use ARP table, socket kind of already does this.
        ether->sourceAddress[i] = localHwAddress[i];
    }
    ether->frameType = htons(TYPE_IP);

    // IP header
    ipHeader* ip = (ipHeader*)ether->data;
    ip->rev = 0x4;
    ip->size = 0x5;
    ip->typeOfService = 0;
    ip->id = 0;
    ip->flagsAndOffset = 0;
    ip->ttl = 128;
    ip->protocol = PROTOCOL_UDP;
    ip->headerChecksum = 0;
    for (i = 0; i < IP_ADD_LENGTH; i++)
    {
        ip->destIp[i] = s->remoteIpAddress[i];
        ip->sourceIp[i] = localIpAddress[i];
    }
    uint8_t ipHeaderLength = ip->size * 4;

    // UDP header
    udpHeader* udp = (udpHeader*)((uint8_t*)ip + (ip->size * 4));
    udp->sourcePort = htons(s->localPort);
    udp->destPort = htons(s->remotePort);
    // adjust lengths
    udpLength = sizeof(udpHeader) + dataSize;
    ip->length = htons(ipHeaderLength + udpLength);
    // 32-bit sum over ip header
    calcIpChecksum(ip);
    // set udp length
    udp->length = htons(udpLength);
    // copy data
    copyData = udp->data;
    for (i = 0; i < dataSize; i++)
        copyData[i] = data[i];
    // 32-bit sum over pseudo-header
    sum = 0;
    sumIpWords(ip->sourceIp, 8, &sum);
    tmp16 = ip->protocol;
    sum += (tmp16 & 0xff) << 8;
    sumIpWords(&udp->length, 2, &sum);
    // add udp header
    udp->check = 0;
    sumIpWords(udp, udpLength, &sum);
    udp->check = getIpChecksum(sum);
    // a computed checksum of 0 is transmitted as all ones (RFC 768)
    if (udp->check == 0)
        udp->check = 0xFFFF;
    // send packet with size = ether + udp hdr + ip header + udp_size
    putEtherPacket(ether, sizeof(etherHeader) + ipHeaderLength + udpLength);
}
