/******************************************************************************
 * File:        ip.c
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
#include <stdio.h>

//=============================================================================
// DEFINES AND MACROS
//=============================================================================

//=============================================================================
// GLOBALS
//=============================================================================

const uint8_t EMPTY_IP_ADDRESS[IP_ADD_LENGTH] = {0, 0, 0, 0};
const uint8_t BROADCAST_IP_ADDRESS[IP_ADD_LENGTH] = {255, 255, 255, 255};

uint8_t ipAddress[IP_ADD_LENGTH] = {0,0,0,0};
uint8_t ipSubnetMask[IP_ADD_LENGTH] = {0,0,0,0};
uint8_t ipGwAddress[IP_ADD_LENGTH] = {0,0,0,0};
uint8_t ipDnsAddress[IP_ADD_LENGTH] = {0,0,0,0};
uint8_t ipTimeServerAddress[IP_ADD_LENGTH] = {0,0,0,0};
uint8_t ipMqttBrokerAddress[IP_ADD_LENGTH] = {0,0,0,0};

//=============================================================================
// PUBLIC FUNCTIONS
//=============================================================================

// Determines whether packet is a valid, unfragmented IPv4 datagram
// Fragments are dropped since reassembly is not implemented
bool isIp(etherHeader *ether)
{
    ipHeader *ip = (ipHeader*)ether->data;
    uint8_t ipHeaderLength = ip->size * 4;
    uint32_t sum = 0;
    bool ok;
    ok = (ether->frameType == htons(TYPE_IP));
    ok = ok && (ip->rev == 4) && (ip->size >= 5);
    ok = ok && (ntohs(ip->length) >= ipHeaderLength);
    // drop fragments (MF set or fragment offset != 0)
    ok = ok && ((ntohs(ip->flagsAndOffset) & 0x3FFF) == 0);
    if (ok)
    {
        sumIpWords(ip, ipHeaderLength, &sum);
        ok = (getIpChecksum(sum) == 0);
    }
    return ok;
}

// Determines whether the IP total length fits in the received frame
bool isIpLengthValid(etherHeader *ether, uint16_t frameSize)
{
    ipHeader *ip = (ipHeader*)ether->data;
    if (frameSize < sizeof(etherHeader) + 20)
        return false;
    return ntohs(ip->length) <= frameSize - sizeof(etherHeader);
}

// Determines whether packet is addressed to the limited broadcast address
bool isIpBroadcast(etherHeader *ether)
{
    ipHeader* ip = (ipHeader*)ether->data;
    return isIpEqual(ip->destIp, (uint8_t*)BROADCAST_IP_ADDRESS);
}

// Determines if the IP address is valid
bool isIpValid(uint8_t ip[4]) {
    return ip[0] || ip[1] || ip[2] || ip[3];
}

inline ipHeader* getIpHeader(etherHeader* ether) {
    return (ipHeader*)ether->data;
}

// Determines whether packet is unicast to this ip
// Must be an IP packet
bool isIpUnicast(etherHeader *ether)
{
    ipHeader* ip = (ipHeader*)ether->data;
    uint8_t i = 0;
    bool ok = true;
    while (ok && (i < IP_ADD_LENGTH))
    {
        ok = (ip->destIp[i] == ipAddress[i]);
        i++;
    }
    return ok;
}

bool isIpInSubnet(uint8_t ip1[4], uint8_t ip2[4], uint8_t netmask[4]) {
    uint8_t i;
    for (i = 0; i < IP_ADD_LENGTH; i++) {
        if ((ip1[i] & netmask[i]) != (ip2[i] & netmask[i])) {
            return false;
        }
    }
    return true;
}

// Converts 255.255.255.255 to 0xFFFFFFFF
uint32_t convertIpAddressToU32(const uint8_t ip[4]) {
    uint32_t addr = 0;
    uint32_t i;
    for (i = 0; i < IP_ADD_LENGTH; i++)
        addr |= (ip[i] << i*8);
    return addr;
}

void copyIpAddress(uint8_t dest[4], const uint8_t source[4]) {
    uint32_t i;
    for (i = 0; i < IP_ADD_LENGTH; i++) {
        dest[i] = source[i];
    }
}

bool isIpEqual(uint8_t ip1[4], uint8_t ip2[4]) {
    bool ok = true;
    uint8_t i;
    for (i = 0; i < IP_ADD_LENGTH; i++) {
        if (ip1[i] != ip2[i]) {
            ok = false;
            break;
        }
    }
    return ok;
}

// Calculate sum of words
// Must use getEtherChecksum to complete 1's compliment addition
void sumIpWords(void* data, uint16_t sizeInBytes, uint32_t* sum)
{
    uint8_t* pData = (uint8_t*)data;
    uint16_t i;
    uint8_t phase = 0;
    uint16_t data_temp;
    for (i = 0; i < sizeInBytes; i++)
    {
        if (phase)
        {
            data_temp = *pData;
            *sum += data_temp << 8;
        }
        else
          *sum += *pData;
        phase = 1 - phase;
        pData++;
    }
}

// Completes 1's compliment addition by folding carries back into field
uint16_t getIpChecksum(uint32_t sum)
{
    uint16_t result;
    // this is based on rfc1071
    while ((sum >> 16) > 0)
      sum = (sum & 0xFFFF) + (sum >> 16);
    result = sum & 0xFFFF;
    return ~result;
}

void calcIpChecksum(ipHeader* ip)
{
    // 32-bit sum over ip header
    uint32_t sum = 0;
    sumIpWords(ip, 10, &sum);
    sumIpWords(ip->sourceIp, (ip->size * 4) - 12, &sum);
    ip->headerChecksum = getIpChecksum(sum);
}

// Sets IP address
void setIpAddress(const uint8_t ip[4])
{
    uint8_t i;
    for (i = 0; i < IP_ADD_LENGTH; i++)
        ipAddress[i] = ip[i];
}

// Gets IP address
void getIpAddress(uint8_t ip[4])
{
    uint8_t i;
    for (i = 0; i < IP_ADD_LENGTH; i++)
        ip[i] = ipAddress[i];
}

// Sets IP subnet mask
void setIpSubnetMask(const uint8_t mask[4])
{
    uint8_t i;
    for (i = 0; i < IP_ADD_LENGTH; i++)
        ipSubnetMask[i] = mask[i];
}

// Gets IP subnet mask
void getIpSubnetMask(uint8_t mask[4])
{
    uint8_t i;
    for (i = 0; i < IP_ADD_LENGTH; i++)
        mask[i] = ipSubnetMask[i];
}

// Sets IP gateway address
void setIpGatewayAddress(const uint8_t ip[4])
{
    uint8_t i;
    for (i = 0; i < IP_ADD_LENGTH; i++)
        ipGwAddress[i] = ip[i];
}

// Gets IP gateway address
void getIpGatewayAddress(uint8_t ip[4])
{
    uint8_t i;
    for (i = 0; i < IP_ADD_LENGTH; i++)
        ip[i] = ipGwAddress[i];
}

// Sets IP DNS address
void setIpDnsAddress(const uint8_t ip[4])
{
    uint8_t i;
    for (i = 0; i < IP_ADD_LENGTH; i++)
        ipDnsAddress[i] = ip[i];
}

// Gets IP gateway address
void getIpDnsAddress(uint8_t ip[4])
{
    uint8_t i;
    for (i = 0; i < IP_ADD_LENGTH; i++)
        ip[i] = ipDnsAddress[i];
}

// Sets IP time server address
void setIpTimeServerAddress(const uint8_t ip[4])
{
    uint8_t i;
    for (i = 0; i < IP_ADD_LENGTH; i++)
        ipTimeServerAddress[i] = ip[i];
}

// Gets IP time server address
void getIpTimeServerAddress(uint8_t ip[4])
{
    uint8_t i;
    for (i = 0; i < IP_ADD_LENGTH; i++)
        ip[i] = ipTimeServerAddress[i];
}

// Sets IP time server address
void setIpMqttBrokerAddress(const uint8_t ip[4])
{
    uint8_t i;
    for (i = 0; i < IP_ADD_LENGTH; i++)
        ipMqttBrokerAddress[i] = ip[i];
}

// Gets IP time server address
void getIpMqttBrokerAddress(uint8_t ip[4])
{
    uint8_t i;
    for (i = 0; i < IP_ADD_LENGTH; i++)
        ip[i] = ipMqttBrokerAddress[i];
}
