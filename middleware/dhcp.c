/******************************************************************************
 * File:        dhcp.c
 *
 * Author:      Giancarlo Perez
 *
 * Created:     12/7/24
 *
 * Description: DHCP client (RFC 2131)
 ******************************************************************************/

//=============================================================================
// INCLUDES
//=============================================================================

#include "log.h"
#include "timer.h"
#include "eeprom.h"
#include "uart0.h"
#include "arp.h"
#include "dhcp.h"
#include <stdio.h>
#include <string.h>

//=============================================================================
// DEFINES AND MACROS
//=============================================================================

#define DHCP_INFINITE_LEASE 0xFFFFFFFF
#define DHCP_RETRY_SECONDS 15   // DISCOVER/REQUEST retransmission interval
#define DHCP_DECLINE_WAIT 10    // RFC 2131 3.1.5: wait at least 10 s after a DECLINE
#define DHCP_CONFLICT_WINDOW 10 // seconds to wait for an answer to the ARP probe

//=============================================================================
// GLOBALS
//=============================================================================

uint32_t xid = 0;
uint32_t leaseSeconds = 0;
uint32_t leaseT1 = 0;
uint32_t leaseT2 = 0;

bool discoverNeeded = false;
bool requestNeeded = false;
bool releaseNeeded = false;
bool declineNeeded = false;
bool ipConflictDetectionMode = false;

//possibly make a struct to save all offered addresses
uint8_t dhcpOfferedIpAdd[4];
uint8_t dhcpServerIpAdd[4];
uint8_t dhcpServerMacAdd[6];

uint8_t dhcpState = DHCP_DISABLED;
bool    dhcpEnabled = true;

// Timers (handles must start invalid, slot 0 is a real timer)
uint8_t t1PeriodicTimer = INVALID_TIMER;
uint8_t t1HitTimer = INVALID_TIMER;
uint8_t t2PeriodicTimer = INVALID_TIMER;
uint8_t t2HitTimer = INVALID_TIMER;
uint8_t leaseEndTimer = INVALID_TIMER;
uint8_t arpTestTimer = INVALID_TIMER;
uint8_t selectingTimer = INVALID_TIMER;
uint8_t newAddTimer = INVALID_TIMER;
uint8_t leaseTickTimer = INVALID_TIMER;

//=============================================================================
// STATIC FUNCTIONS
//=============================================================================

/* Forward Declarations */
void setDhcpState(uint8_t state);
uint8_t getDhcpState(void);
void callbackDhcpT1PeriodicTimer(void* c);
void callbackDhcpT1HitTimer(void* c);
void callbackDhcpT2HitTimer(void* c);
void callbackDhcpLeaseEndTimer(void* c);
void dhcpLeaseTick(void* c);

static void stopLeaseTimers() {
    stopTimerHandle(&t1HitTimer);
    stopTimerHandle(&t1PeriodicTimer);
    stopTimerHandle(&t2HitTimer);
    stopTimerHandle(&t2PeriodicTimer);
    stopTimerHandle(&leaseEndTimer);
    stopTimerHandle(&leaseTickTimer);
}

static void stopAllDhcpTimers() {
    stopLeaseTimers();
    stopTimerHandle(&arpTestTimer);
    stopTimerHandle(&selectingTimer);
    stopTimerHandle(&newAddTimer);
}

static void clearIpConfiguration() {
    setIpAddress(EMPTY_IP_ADDRESS);
    setIpSubnetMask(EMPTY_IP_ADDRESS);
    setIpGatewayAddress(EMPTY_IP_ADDRESS);
    setIpDnsAddress(EMPTY_IP_ADDRESS);
    setIpTimeServerAddress(EMPTY_IP_ADDRESS);
}

static bool isDhcpLeased() {
    uint8_t state = getDhcpState();
    return state == DHCP_BOUND || state == DHCP_RENEWING || state == DHCP_REBINDING;
}

// Drops any lease and starts over with a DISCOVER (no RELEASE is sent)
static void restartDhcp() {
    stopAllDhcpTimers();
    clearIpConfiguration();
    requestNeeded = false;
    declineNeeded = false;
    setDhcpState(DHCP_INIT);
    discoverNeeded = true;
}

// Copies an option holding an IP address, returns false if missing
static bool getDhcpOptionIp(etherHeader* ether, uint8_t option, uint8_t ip[4]) {
    uint8_t* opt = getDhcpOption(ether, option);
    if (opt == NULL || opt[1] < IP_ADD_LENGTH) {
        return false;
    }
    copyIpAddress(ip, opt + 2);
    return true;
}

// Reads a 32-bit option, returns false if missing
static bool getDhcpOptionU32(etherHeader* ether, uint8_t option, uint32_t* value) {
    uint8_t* opt = getDhcpOption(ether, option);
    if (opt == NULL || opt[1] < 4) {
        return false;
    }
    *value = ((uint32_t)opt[2] << 24) | ((uint32_t)opt[3] << 16) | ((uint32_t)opt[4] << 8) | opt[5];
    return true;
}

static uint8_t getDhcpMessageType(etherHeader* ether) {
    uint8_t* opt = getDhcpOption(ether, DHCP_OPTION_DHCP_TYPE);
    if (opt == NULL || opt[1] < 1) {
        return 0;
    }
    return opt[2];
}

static void enterRenewing() {
    setDhcpState(DHCP_RENEWING);
    requestNeeded = true;
    stopTimerHandle(&t1PeriodicTimer);
    t1PeriodicTimer = startPeriodicTimer(callbackDhcpT1PeriodicTimer, DHCP_RETRY_SECONDS, NULL);
}

// Applies the address in dhcpOfferedIpAdd and starts the lease timers
static void bindDhcp() {
    putsUart0("DHCP: IP Address acquired\n");
    setDhcpState(DHCP_BOUND);
    setIpAddress(dhcpOfferedIpAdd);
    stopLeaseTimers();
    if (leaseSeconds != DHCP_INFINITE_LEASE) {
        t1HitTimer = startOneshotTimer(callbackDhcpT1HitTimer, leaseT1, NULL);
        t2HitTimer = startOneshotTimer(callbackDhcpT2HitTimer, leaseT2, NULL);
        leaseEndTimer = startOneshotTimer(callbackDhcpLeaseEndTimer, leaseSeconds, NULL);
        leaseTickTimer = startPeriodicTimer(dhcpLeaseTick, 1, NULL);
    }
}

//=============================================================================
// PUBLIC FUNCTIONS
//=============================================================================

static const char* dhcpStateName(uint8_t state) {
    static const char* const names[] = {
        "DISABLED", "INIT", "SELECTING", "REQUESTING", "TESTING_IP",
        "BOUND", "RENEWING", "REBINDING", "INITREBOOT", "REBOOTING"
    };
    return state < sizeof(names) / sizeof(names[0]) ? names[state] : "?";
}

static const char* dhcpMessageName(uint8_t type) {
    static const char* const names[] = {
        "?", "DISCOVER", "OFFER", "REQUEST", "DECLINE", "ACK", "NAK", "RELEASE", "INFORM"
    };
    return type < sizeof(names) / sizeof(names[0]) ? names[type] : "?";
}

void setDhcpState(uint8_t state) {
    if (state != dhcpState) {
        logPrintf(LOG_DHCP, "state %s -> %s", dhcpStateName(dhcpState), dhcpStateName(state));
    }
    dhcpState = state;
}

uint8_t getDhcpState() {
    return dhcpState;
}

// New address functions
// Manually requested at start-up

void requestDhcpNewAddress() {
    if (getDhcpState() == DHCP_INIT) {
        discoverNeeded = true;
    }
}

void callbackDhcpGetNewAddressTimer(void* c) {
    newAddTimer = INVALID_TIMER;
    requestDhcpNewAddress();
}

// No ACK for our DISCOVER/REQUEST, start over
void callbackDhcpSelectingTimeout(void* c) {
    selectingTimer = INVALID_TIMER;
    if (getDhcpState() == DHCP_SELECTING || getDhcpState() == DHCP_REQUESTING) {
        putsUart0("DHCP: No response, retrying...\n");
        restartDhcp();
    }
}

// Renew functions

void callbackDhcpT1PeriodicTimer(void* c) {
    requestNeeded = true;
}

void callbackDhcpT1HitTimer(void* c) {
    t1HitTimer = INVALID_TIMER;
    putsUart0("DHCP: T1 Hit reached\n");
    enterRenewing();
}

// Rebind functions

void rebindDhcp() {
    putsUart0("DHCP: Rebinding IP, finding any DHCP server...\n");
    requestNeeded = true;
}

void callbackDhcpT2PeriodicTimer(void* c) {
    rebindDhcp();
    //stay in REBINDING
}

void callbackDhcpT2HitTimer(void* c) {
    t2HitTimer = INVALID_TIMER;
    stopTimerHandle(&t1PeriodicTimer);
    setDhcpState(DHCP_REBINDING);
    rebindDhcp();
    t2PeriodicTimer = startPeriodicTimer(callbackDhcpT2PeriodicTimer, DHCP_RETRY_SECONDS, NULL);
}

// End of lease timer, the address must not be used anymore (no RELEASE needed)
void callbackDhcpLeaseEndTimer(void* c) {
    leaseEndTimer = INVALID_TIMER;
    putsUart0("DHCP: Lease expired\n");
    restartDhcp();
}

void dhcpLeaseTick(void* c) {
    if (leaseSeconds > 0) {
        leaseSeconds--;
    }
}

// Release functions, the RELEASE is sent from sendDhcpPendingMessages
void releaseDhcp() {
    if (isDhcpLeased()) {
        releaseNeeded = true;
    }
    else if (getDhcpState() != DHCP_DISABLED) {
        restartDhcp();
    }
}

// User requested renewal
void renewDhcp() {
    if (isDhcpLeased()) {
        stopTimerHandle(&t1HitTimer);
        enterRenewing();
    }
    else {
        requestDhcpNewAddress();
    }
}

// IP conflict detection
void callbackDhcpIpConflictWindow(void* c) {
    arpTestTimer = INVALID_TIMER;
    bindDhcp();
}

void requestDhcpIpConflictTest(etherHeader* ether) {
    uint8_t ip[4] = {0, 0, 0, 0};
    sendArpRequest(ether, ip, dhcpOfferedIpAdd); // RFC 5227 probe, sender IP is 0.0.0.0
    putsUart0("DHCP: Sending ARP...\n");
    setDhcpState(DHCP_TESTING_IP);
    arpTestTimer = startOneshotTimer(callbackDhcpIpConflictWindow, DHCP_CONFLICT_WINDOW, NULL);
}

bool isDhcpIpConflictDetectionMode() {
    return ipConflictDetectionMode;
}

// Lease functions

uint32_t getDhcpLeaseSeconds() {
    return leaseSeconds;
}

// Determines whether packet is a DHCP reply to our current transaction
// Must be a UDP packet
bool isDhcpResponse(etherHeader* ether) {
    udpHeader* udp;
    dhcpFrame* dhcp;
    uint8_t localHwAddress[6];
    uint8_t i;
    if (!isUdp(ether)) {
        return false;
    }
    udp = getUdpHeader(ether);
    if (udp->destPort != htons(DHCP_DEST_PORT_S) || udp->sourcePort != htons(DHCP_SOURCE_PORT_S)) {
        return false;
    }
    if (ntohs(udp->length) < sizeof(udpHeader) + sizeof(dhcpFrame)) {
        return false;
    }
    dhcp = getDhcpFrame(ether);
    if (dhcp->op != 2 || dhcp->xid != xid || dhcp->magicCookie != MAGIC_COOKIE) {
        return false;
    }
    getEtherMacAddress(localHwAddress);
    for (i = 0; i < HW_ADD_LENGTH; i++) {
        if (dhcp->chaddr[i] != localHwAddress[i]) {
            return false; // not for this client
        }
    }
    return true;
}

dhcpFrame* getDhcpFrame(etherHeader* ether) {
    ipHeader* ip = (ipHeader*)ether->data;
    uint8_t ipHeaderLength = ip->size * 4;
    udpHeader* udp = (udpHeader*)((uint8_t*)ip + ipHeaderLength);
    dhcpFrame* dhcp = (dhcpFrame*)((uint8_t*)udp->data);
    return dhcp;
}

void addDhcpOption(uint8_t* options_ptr, uint8_t option_type, uint8_t len, uint8_t data[], uint8_t* options_length) {
    static uint8_t* last;
    if (options_ptr == NULL) {
        options_ptr = last;
    }
    if (options_ptr == NULL) {
        return;
    }
    uint32_t i = 0;
    uint32_t j;
    options_ptr[i++] = option_type;
    if (option_type != DHCP_OPTION_END_MARK) {
        options_ptr[i++] = len;
        for (j = 0; j < len; j++) {
            options_ptr[i++] = data[j];
        }
        if (options_length) {
            *options_length += (2 + len);
        }
        last = options_ptr + 2 + len;
    }
    else {
        if (options_length) {
            *options_length += 1;
        }
        last = options_ptr + 1;
    }
}

// Returns a pointer to the option (type byte) or NULL, bounded by the UDP length
uint8_t* getDhcpOption(etherHeader *ether, uint8_t option) {
    dhcpFrame* dhcp = getDhcpFrame(ether);
    udpHeader* udp = getUdpHeader(ether);
    int32_t optionsLength = (int32_t)ntohs(udp->length) - sizeof(udpHeader) - sizeof(dhcpFrame);
    int32_t i = 0;
    while (i < optionsLength) {
        uint8_t type = dhcp->options[i];
        if (type == DHCP_OPTION_END_MARK) {
            break;
        }
        if (type == DHCP_OPTION_PAD) {
            i++;
            continue;
        }
        if (i + 1 >= optionsLength || i + 2 + dhcp->options[i + 1] > optionsLength) {
            break; // truncated option
        }
        if (type == option) {
            return dhcp->options + i;
        }
        i += 2 + dhcp->options[i + 1];
    }
    return NULL;
}

// Send DHCP message (fields and options follow RFC 2131 table 5)
void sendDhcpMessage(etherHeader* ether, uint8_t type) {
    uint8_t dhcpBuffer[sizeof(dhcpFrame) + 64];
    dhcpFrame* dhcp = (dhcpFrame*)dhcpBuffer;
    socket s;
    uint8_t i;
    logPrintf(LOG_DHCP, "sending %s", dhcpMessageName(type));
    uint8_t localHwAddress[6];
    uint8_t optionData[16];
    uint8_t options_length = 0;
    uint8_t state = getDhcpState();
    bool renewing = (type == DHCPREQUEST) && (state == DHCP_RENEWING);
    bool rebinding = (type == DHCPREQUEST) && (state == DHCP_REBINDING);
    bool useCiaddr = renewing || rebinding || (type == DHCPRELEASE);
    bool unicast = renewing || (type == DHCPRELEASE);
    getEtherMacAddress(localHwAddress);

    memset(&s, 0, sizeof(s));
    s.localPort = DHCP_SOURCE_PORT_C;
    s.remotePort = DHCP_DEST_PORT_C;
    if (unicast) {
        copyIpAddress(s.remoteIpAddress, dhcpServerIpAdd);
        copyMacAddress(s.remoteHwAddress, dhcpServerMacAdd);
    }
    else {
        copyIpAddress(s.remoteIpAddress, BROADCAST_IP_ADDRESS);
        copyMacAddress(s.remoteHwAddress, BROADCAST_MAC_ADDRESS);
    }
    memset(dhcpBuffer, 0, sizeof(dhcpFrame));
    dhcp->op = 0x01;
    dhcp->htype = 0x01;
    dhcp->hlen = 0x06;
    dhcp->hops = 0x00;
    dhcp->xid = xid;
    dhcp->secs = htons(0x0000);
    // ask for broadcast replies only while we cannot receive unicast
    dhcp->flags = useCiaddr ? 0 : htons(0x8000);
    if (useCiaddr) {
        copyIpAddress(dhcp->ciaddr, dhcpOfferedIpAdd);
    }
    copyMacAddress(dhcp->chaddr, localHwAddress);
    dhcp->magicCookie = MAGIC_COOKIE;

    //DHCP Message Type - 53
    optionData[0] = type;
    addDhcpOption(dhcp->options, DHCP_OPTION_DHCP_TYPE, 1, optionData, &options_length);

    //Client Identifier - 61
    i = 0;
    optionData[i++] = DHCP_OPTION_CLIENT_ID_ETHERNET;
    copyMacAddress(optionData+i, localHwAddress);
    i += HW_ADD_LENGTH;
    addDhcpOption(NULL, DHCP_OPTION_CLIENT_ID, i, optionData, &options_length);

    //Requested IP - 50 (REQUEST in SELECTING and DECLINE only)
    if ((type == DHCPREQUEST && !renewing && !rebinding) || type == DHCPDECLINE) {
        copyIpAddress(optionData, dhcpOfferedIpAdd);
        addDhcpOption(NULL, DHCP_OPTION_REQUESTED_IP, IP_ADD_LENGTH, optionData, &options_length);
    }
    //Server ID - 54 (not allowed in RENEWING/REBINDING requests)
    if ((type == DHCPREQUEST && !renewing && !rebinding) || type == DHCPDECLINE || type == DHCPRELEASE) {
        copyIpAddress(optionData, dhcpServerIpAdd);
        addDhcpOption(NULL, DHCP_OPTION_SERVER_ID, IP_ADD_LENGTH, optionData, &options_length);
    }
    //Parameter Request List - 55
    if (type == DHCPDISCOVER || type == DHCPREQUEST) {
        i = 0;
        optionData[i++] = DHCP_OPTION_PARAMETER_LIST_SUBNET_MASK;
        optionData[i++] = DHCP_OPTION_PARAMETER_LIST_ROUTER;
        optionData[i++] = DHCP_OPTION_PARAMETER_LIST_DNS;
        optionData[i++] = DHCP_OPTION_PARAMETER_LIST_DOMAIN_NAME;
        optionData[i++] = DHCP_OPTION_TIME_SERVER;
        addDhcpOption(NULL, DHCP_OPTION_PARAMETER_LIST, i, optionData, &options_length);
    }
    //End Options - 255
    addDhcpOption(NULL, DHCP_OPTION_END_MARK, 0, 0, &options_length);
    uint16_t dhcp_length = sizeof(dhcpFrame) + (options_length);
    sendUdpMessage(ether, &s, dhcpBuffer, dhcp_length);
}

void handleDhcpAck(etherHeader *ether) {
    uint8_t ip[4];
    uint32_t value;
    dhcpFrame* dhcp = getDhcpFrame(ether);
    copyIpAddress(dhcpOfferedIpAdd, dhcp->yiaddr);
    copyMacAddress(dhcpServerMacAdd, ether->sourceAddress);
    if (getDhcpOptionIp(ether, DHCP_OPTION_SERVER_ID, ip)) {
        copyIpAddress(dhcpServerIpAdd, ip);
    }
    setIpSubnetMask(getDhcpOptionIp(ether, DHCP_OPTION_SUBNET_MASK, ip) ? ip : EMPTY_IP_ADDRESS);
    setIpGatewayAddress(getDhcpOptionIp(ether, DHCP_OPTION_DEFAULT_GATEWAY, ip) ? ip : EMPTY_IP_ADDRESS);
    setIpDnsAddress(getDhcpOptionIp(ether, DHCP_OPTION_DNS_SERVER, ip) ? ip : EMPTY_IP_ADDRESS);
    setIpTimeServerAddress(getDhcpOptionIp(ether, DHCP_OPTION_TIME_SERVER, ip) ? ip : EMPTY_IP_ADDRESS);
    // the lease time is mandatory in an ACK to a REQUEST, treat a missing one as infinite
    leaseSeconds = getDhcpOptionU32(ether, DHCP_OPTION_LEASE_TIME, &value) ? value : DHCP_INFINITE_LEASE;
    // RFC 2131 4.4.5 defaults: T1 = 0.5 * lease, T2 = 0.875 * lease
    leaseT1 = getDhcpOptionU32(ether, DHCP_OPTION_RENEW_TIME, &value) ? value : leaseSeconds / 2;
    leaseT2 = getDhcpOptionU32(ether, DHCP_OPTION_REBIND_TIME, &value) ? value : leaseSeconds - leaseSeconds / 8;
    uint8_t state = getDhcpState();
    switch (state) {
    case DHCP_RENEWING:
    case DHCP_REBINDING:
        bindDhcp();
        break;
    case DHCP_REQUESTING:
        stopTimerHandle(&selectingTimer);
        if (isDhcpIpConflictDetectionMode()) {
            requestDhcpIpConflictTest(ether);
        }
        else {
            bindDhcp();
        }
        break;
    }
}

// Message requests

bool isDhcpDiscoverNeeded() {
    return discoverNeeded;
}

bool isDhcpRequestNeeded()
{
    return requestNeeded;
}

bool isDhcpDeclineNeeded() {
    return declineNeeded;
}

bool isDhcpReleaseNeeded() {
    return releaseNeeded;
}

void sendDhcpPendingMessages(etherHeader* ether) {
    uint8_t state = getDhcpState();
    if (state == DHCP_DISABLED) {
        return;
    }
    if (isDhcpReleaseNeeded()) {
        releaseNeeded = false;
        if (isDhcpLeased()) {
            sendDhcpMessage(ether, DHCPRELEASE);
            putsUart0("DHCP: IP Address Released\n");
        }
        restartDhcp();
        return;
    }
    switch(state) {
    case DHCP_INIT:
        if (isDhcpDiscoverNeeded()) {
            discoverNeeded = false;
            xid = random32(); // new transaction
            sendDhcpMessage(ether, DHCPDISCOVER);
            putsUart0("DHCP: Sending DISCOVER...\n");
            setDhcpState(DHCP_SELECTING);
            stopTimerHandle(&selectingTimer);
            selectingTimer = startOneshotTimer(callbackDhcpSelectingTimeout, DHCP_RETRY_SECONDS, NULL);
        }
        break;
    case DHCP_SELECTING:
        if (isDhcpRequestNeeded()) {
            requestNeeded = false;
            sendDhcpMessage(ether, DHCPREQUEST);
            setDhcpState(DHCP_REQUESTING);
        }
        break;
    case DHCP_REQUESTING:
        break;
    case DHCP_TESTING_IP:
        if (isDhcpDeclineNeeded()) {
            declineNeeded = false;
            sendDhcpMessage(ether, DHCPDECLINE);
            putsUart0("DHCP: Address in use, sending DECLINE\n");
            clearIpConfiguration();
            setDhcpState(DHCP_INIT);
            stopTimerHandle(&newAddTimer);
            newAddTimer = startOneshotTimer(callbackDhcpGetNewAddressTimer, DHCP_DECLINE_WAIT, NULL);
        }
        break;
    case DHCP_BOUND:
        break;
    case DHCP_RENEWING:
        if (isDhcpRequestNeeded()) {
            requestNeeded = false;
            sendDhcpMessage(ether, DHCPREQUEST);
        }
        break;
    case DHCP_REBINDING:
        if (isDhcpRequestNeeded()) {
            requestNeeded = false;
            putsUart0("DHCP: Rebinding IP, broadcasting REQUEST\n");
            sendDhcpMessage(ether, DHCPREQUEST);
        }
        break;
    }
}

// Packet must have passed isDhcpResponse
void processDhcpResponse(etherHeader *ether) {
    uint8_t state = getDhcpState();
    uint8_t type = getDhcpMessageType(ether);
    uint8_t serverId[4];
    logPrintf(LOG_DHCP, "received %s in state %s", dhcpMessageName(type), dhcpStateName(state));
    switch (state) {
    case DHCP_SELECTING:
        // take the first offer that names its server
        if (type == DHCPOFFER && getDhcpOptionIp(ether, DHCP_OPTION_SERVER_ID, serverId)) {
            dhcpFrame* dhcp = getDhcpFrame(ether);
            copyIpAddress(dhcpOfferedIpAdd, dhcp->yiaddr);
            copyIpAddress(dhcpServerIpAdd, serverId);
            requestNeeded = true;
        }
        break;
    case DHCP_REQUESTING:
    case DHCP_RENEWING:
    case DHCP_REBINDING:
        if (type == DHCPACK) {
            handleDhcpAck(ether);
        }
        else if (type == DHCPNAK) {
            putsUart0("DHCP: Received NAK, restarting\n");
            restartDhcp();
        }
        break;
    default:
        break;
    }
}

void processDhcpArpResponse(etherHeader* ether) {
    arpPacket* arp = getArpPacket(ether);
    if (getDhcpState() == DHCP_TESTING_IP) {
        if (isIpEqual(arp->sourceIp, dhcpOfferedIpAdd)) {
            //conflicting IP
            stopTimerHandle(&arpTestTimer);
            declineNeeded = true;
        }
    }
}

void enableDhcp() {
    dhcpEnabled = true;
    restartDhcp();
}

void disableDhcp() {
    setDhcpState(DHCP_DISABLED);
    stopAllDhcpTimers();
    clearIpConfiguration();
    setIpMqttBrokerAddress(EMPTY_IP_ADDRESS);
    discoverNeeded = false;
    requestNeeded = false;
    releaseNeeded = false;
    declineNeeded = false;
    dhcpEnabled = false;
}

bool isDhcpEnabled()
{
    return dhcpEnabled;
}
