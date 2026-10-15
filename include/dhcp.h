/******************************************************************************
 * File:        dhcp.h
 *
 * Author:      Giancarlo Perez
 *
 * Created:     12/7/24
 *
 * Description: -
 ******************************************************************************/

#ifndef DHCP_H_
#define DHCP_H_

//=============================================================================
// INCLUDES
//=============================================================================

#include <stdint.h>
#include <stdbool.h>
#include "udp.h"

//=============================================================================
// DEFINES AND MACROS
//=============================================================================

/* DHCP Message Types */
#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPDECLINE  4
#define DHCPACK      5
#define DHCPNAK      6
#define DHCPRELEASE  7
#define DHCPINFORM   8

/* DHCP States */
#define DHCP_DISABLED   0
#define DHCP_INIT       1
#define DHCP_SELECTING  2
#define DHCP_REQUESTING 3
#define DHCP_TESTING_IP 4
#define DHCP_BOUND      5
#define DHCP_RENEWING   6
#define DHCP_REBINDING  7
#define DHCP_INITREBOOT 8 // not used since ip not stored over reboot
#define DHCP_REBOOTING  9 // not used since ip not stored over reboot

/* DHCP Options */
#define DHCP_OPTION_PAD 0
#define DHCP_OPTION_SUBNET_MASK 1
#define DHCP_OPTION_DEFAULT_GATEWAY 3
#define DHCP_OPTION_TIME_SERVER 4
#define DHCP_OPTION_NAME_SERVER 5
#define DHCP_OPTION_DNS_SERVER 6
#define DHCP_OPTION_HOST_NAME 12
#define DHCP_OPTION_REQUESTED_IP 50
#define DHCP_OPTION_LEASE_TIME 51
#define DHCP_OPTION_DHCP_TYPE 53
#define DHCP_OPTION_SERVER_ID 54
#define DHCP_OPTION_PARAMETER_LIST 55
#define DHCP_OPTION_RENEW_TIME 58
#define DHCP_OPTION_REBIND_TIME 59
#define DHCP_OPTION_CLIENT_ID 61
#define DHCP_OPTION_END_MARK 255
#define DHCP_OPTION_PARAMETER_LIST_SUBNET_MASK 0x01
#define DHCP_OPTION_PARAMETER_LIST_ROUTER 0x03
#define DHCP_OPTION_PARAMETER_LIST_DNS 0x06
#define DHCP_OPTION_PARAMETER_LIST_DOMAIN_NAME 0xF
#define DHCP_OPTION_CLIENT_ID_ETHERNET 0x01

/* Constants */
//For Sending Messages
#define DHCP_SOURCE_PORT_C 68
#define DHCP_DEST_PORT_C 67
//For Responses
#define DHCP_SOURCE_PORT_S 67
#define DHCP_DEST_PORT_S 68
#define MAGIC_COOKIE 0x63538263

/* Config */
#define DHCP_MAX_OPTION_LENGTH 255

//=============================================================================
// TYPEDEFS AND GLOBALS
//=============================================================================

#pragma pack(push)
#pragma pack(1)
typedef struct _dhcpFrame // 240 or more bytes
{
  uint8_t op;
  uint8_t htype;
  uint8_t hlen;
  uint8_t hops;
  uint32_t  xid;
  uint16_t secs;
  uint16_t flags;
  uint8_t ciaddr[4];
  uint8_t yiaddr[4];
  uint8_t siaddr[4];
  uint8_t giaddr[4];
  uint8_t chaddr[16];
  uint8_t data[192];
  uint32_t magicCookie;
  uint8_t options[0];
} dhcpFrame;
#pragma pack(pop)

typedef struct _dhcpOption {
    uint8_t type;
    uint8_t length;
    uint8_t data[];
} dhcpOption;

//=============================================================================
// FUNCTION PROTOTYPES
//=============================================================================

bool isDhcpResponse(etherHeader *ether);
void addDhcpOption(uint8_t* options_ptr, uint8_t option_type, uint8_t len, uint8_t data[], uint8_t* options_length);
dhcpFrame* getDhcpFrame(etherHeader* ether);
uint8_t* getDhcpOption(etherHeader *ether, uint8_t option);
void sendDhcpMessage(etherHeader *ether, uint8_t type);
void sendDhcpPendingMessages(etherHeader *ether);
void processDhcpResponse(etherHeader *ether);
void processDhcpArpResponse(etherHeader *ether);
void enableDhcp(void);
void disableDhcp(void);
bool isDhcpEnabled(void);
void renewDhcp(void);   // user requested renewal of the current lease
void releaseDhcp(void); // releases the lease and starts over
uint32_t getDhcpLeaseSeconds();

#endif

