// IP Library
// Jason Losh

//-----------------------------------------------------------------------------
// Hardware Target
//-----------------------------------------------------------------------------

// Target Platform: -
// Target uC:       -
// System Clock:    -

// Hardware configuration:
// -

//-----------------------------------------------------------------------------
// Device includes, defines, and assembler directives
//-----------------------------------------------------------------------------

#ifndef IP_H_
#define IP_H_

#include "eth0.h"
#include <stdint.h>
#include <stdbool.h>

#pragma pack(push)
#pragma pack(1)
typedef struct _ipHeader // 20 or more bytes
{
    uint8_t size:4;
    uint8_t rev:4;
    uint8_t typeOfService;
    uint16_t length;
    uint16_t id;
    uint16_t flagsAndOffset;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t headerChecksum;
    uint8_t sourceIp[4];
    uint8_t destIp[4];
    uint8_t data[0]; // optional bytes or udp/tcp/icmp header
} ipHeader;
#pragma pack(pop)

// Protocols
#define PROTOCOL_ICMP 1
#define PROTOCOL_TCP  6
#define PROTOCOL_UDP  17

#define MAX_PACKET_SIZE 1518

extern const uint8_t EMPTY_IP_ADDRESS[IP_ADD_LENGTH];
extern const uint8_t BROADCAST_IP_ADDRESS[IP_ADD_LENGTH];

//-----------------------------------------------------------------------------
// Subroutines
//-----------------------------------------------------------------------------

bool isIp(etherHeader *ether);
bool isIpLengthValid(etherHeader *ether, uint16_t frameSize);
bool isIpBroadcast(etherHeader *ether);
ipHeader* getIpHeader(etherHeader* ether);
bool isIpUnicast(etherHeader *ether);
bool isIpValid(uint8_t ip[4]);
bool isIpEqual(uint8_t ip1[4], uint8_t ip2[4]);
bool isIpInSubnet(uint8_t ip1[4], uint8_t ip2[4], uint8_t netmask[4]);
uint32_t convertIpAddressToU32(const uint8_t ip[4]);
void copyIpAddress(uint8_t dest[4], const uint8_t source[4]);

void sumIpWords(void* data, uint16_t sizeInBytes, uint32_t* sum);
void calcIpChecksum(ipHeader* ip);
uint16_t getIpChecksum(uint32_t sum);

void setIpAddress(const uint8_t ip[4]);
void getIpAddress(uint8_t ip[4]);
void setIpSubnetMask(const uint8_t mask[4]);
void getIpSubnetMask(uint8_t mask[4]);
void setIpGatewayAddress(const uint8_t ip[4]);
void getIpGatewayAddress(uint8_t ip[4]);
void setIpDnsAddress(const uint8_t ip[4]);
void getIpDnsAddress(uint8_t ip[4]);
void setIpTimeServerAddress(const uint8_t ip[4]);
void getIpTimeServerAddress(uint8_t ip[4]);
void setIpMqttBrokerAddress(const uint8_t ip[4]);
void getIpMqttBrokerAddress(uint8_t ip[4]);

#endif

