/******************************************************************************
 * File:        arp.c
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

#include "log.h"
#include "uart0.h"
#include "arp.h"
#include "ip.h"
#include "timer.h"
#include <stdio.h>
#include <stdint.h>

//=============================================================================
// DEFINES AND MACROS
//=============================================================================

#define ARP_RETRY_SECONDS 1
#define MAX_ARP_ATTEMPTS 3
#define MAX_ARP_REQUESTS 5

#define ARP_FRAME_SIZE (sizeof(etherHeader) + sizeof(arpPacket))

//=============================================================================
// GLOBALS
//=============================================================================

arp_entry_t arpTable[MAX_ARP_ENTRIES];
uint8_t arpTableNext = 0; // next slot to replace when the table is full
arpRequest arpReqs[MAX_ARP_REQUESTS];

//=============================================================================
// STATIC FUNCTIONS
//=============================================================================

static void freeArpRequest(arpRequest* arpReq) {
    stopTimerHandle(&arpReq->arpTimer);
    arpReq->active = 0;
    arpReq->callback = NULL;
    arpReq->ctxt = NULL;
}

static void arpTimeoutCallback(void* c) {
    arpRequest* arpReq = (arpRequest*)c;
    if (!arpReq->active) {
        return;
    }
    arpReq->attempts++;
    logPrintf(LOG_ARP, "no reply from %u.%u.%u.%u (attempt %u)", arpReq->ipAdd[0], arpReq->ipAdd[1], arpReq->ipAdd[2], arpReq->ipAdd[3], arpReq->attempts);
    if (arpReq->attempts >= MAX_ARP_ATTEMPTS) {
        arpRespContext resp;
        _arp_callback_t callback = arpReq->callback;
        resp.success = 0;
        resp.ctxt = arpReq->ctxt;
        freeArpRequest(arpReq); // free before the callback so it can start a new request
        if (callback) {
            callback(resp); //mac address invalid
        }
    }
    else {
        //Nth arp attempt...
        uint8_t ether[ARP_FRAME_SIZE];
        uint8_t localIp[IP_ADD_LENGTH];
        getIpAddress(localIp);
        sendArpRequest((etherHeader*)ether, localIp, arpReq->ipAdd);
    }
}

// Validates the fixed fields of an Ethernet/IPv4 ARP packet
static bool isArpPacketValid(etherHeader* ether) {
    arpPacket* arp = (arpPacket*)ether->data;
    return (ether->frameType == htons(TYPE_ARP))
        && (arp->hardwareType == htons(1))
        && (arp->protocolType == htons(TYPE_IP))
        && (arp->hardwareSize == HW_ADD_LENGTH)
        && (arp->protocolSize == IP_ADD_LENGTH);
}

// Returns the table index for an IP address or -1
static int8_t findArpEntry(uint8_t ipAddress[]) {
    uint8_t i;
    for (i = 0; i < MAX_ARP_ENTRIES; i++) {
        if (arpTable[i].valid && isIpEqual(arpTable[i].ipAddress, ipAddress)) {
            return i;
        }
    }
    return -1;
}

//=============================================================================
// PUBLIC FUNCTIONS
//=============================================================================

void displayArpTable() {
    putsUart0("\nARP Cache\n------------------------------------------------------------\n");
    putsUart0(" IP Address                  MAC Address\n");
    uint8_t i;
    for (i = 0; i < MAX_ARP_ENTRIES; i++) {
        if (arpTable[i].valid) {
            uint8_t* ip = arpTable[i].ipAddress;
            uint8_t* mac = arpTable[i].macAddress;
            char ipStr[16];
            char macStr[18];
            snprintf(ipStr, 16, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
            snprintf(macStr, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            snprintf(out, MAX_UART_OUT, " %-28s%-20s\n", ipStr, macStr);
            putsUart0(out);
        }
    }
    putsUart0("------------------------------------------------------------\n\n");
}

void clearArpTable() {
    uint8_t i;
    for (i = 0; i < MAX_ARP_ENTRIES; i++) {
        arpTable[i].valid = 0;
    }
    arpTableNext = 0;
}

// Adds an entry, or updates the MAC of an existing entry (RFC 826 merge)
// When the table is full the oldest inserted entry is replaced
void addArpEntry(uint8_t ipAddress[], uint8_t macAddress[]) {
    uint8_t i;
    int8_t index = findArpEntry(ipAddress);
    if (index < 0) {
        // use a free slot if there is one
        for (i = 0; i < MAX_ARP_ENTRIES && index < 0; i++) {
            if (!arpTable[i].valid) {
                index = i;
            }
        }
        if (index < 0) {
            index = arpTableNext;
            arpTableNext = (arpTableNext + 1) % MAX_ARP_ENTRIES;
        }
    }
    arp_entry_t* entry = &arpTable[index];
    copyIpAddress(entry->ipAddress, ipAddress);
    copyMacAddress(entry->macAddress, macAddress);
    entry->valid = 1;
}

//writes to mac address, returns 0 If no entry
uint8_t lookupArpEntry(uint8_t ipAddress[], uint8_t macAddressToWrite[]) {
    int8_t index = findArpEntry(ipAddress);
    if (index < 0) {
        return 0; // No entry found for the given IP address
    }
    if (macAddressToWrite) {
        copyMacAddress(macAddressToWrite, arpTable[index].macAddress);
    }
    return 1;
}

arpPacket* getArpPacket(etherHeader* ether) {
    arpPacket* arp = (arpPacket*)ether->data;
    return arp;
}

//ctxt must persist between loops, essentially must be a global
//the callback may be called before this function returns (cache hit or no free request slots)
void resolveMacAddress(uint8_t ipAdd[4], _arp_callback_t cb, void* ctxt) {
    uint8_t localIp[4];
    uint8_t subnetMask[4];
    uint8_t gateway[4];
    uint8_t remoteMac[6];
    uint8_t target[4];
    uint8_t i;
    arpRespContext resp;
    getIpAddress(localIp);
    getIpSubnetMask(subnetMask);
    getIpGatewayAddress(gateway);
    //if IP is in same subnet, ARP for that IP, else ARP for the gateway
    copyIpAddress(target, isIpInSubnet(localIp, ipAdd, subnetMask) ? ipAdd : gateway);
    resp.ctxt = ctxt;
    if (lookupArpEntry(target, remoteMac)) {
        resp.success = 1;
        copyMacAddress(resp.responseMacAddress, remoteMac); //return MAC if exists in table
        cb(resp);
        return;
    }
    for (i = 0; i < MAX_ARP_REQUESTS; i++) {
        if (!arpReqs[i].active) {
            uint8_t ether[ARP_FRAME_SIZE];
            arpRequest* req = &arpReqs[i];
            copyIpAddress(req->ipAdd, target);
            req->callback = cb;
            req->attempts = 0;
            req->ctxt = ctxt;
            req->active = 1;
            req->arpTimer = startPeriodicTimer(arpTimeoutCallback, ARP_RETRY_SECONDS, req);
            sendArpRequest((etherHeader*)ether, localIp, target);
            return;
        }
    }
    // no free request slot
    resp.success = 0;
    cb(resp);
}

// Cancels pending requests for a context (e.g. a socket that is being deleted)
void cancelArpRequests(void* ctxt) {
    uint8_t i;
    for (i = 0; i < MAX_ARP_REQUESTS; i++) {
        if (arpReqs[i].active && arpReqs[i].ctxt == ctxt) {
            freeArpRequest(&arpReqs[i]);
        }
    }
}

void processArpResponse(etherHeader* ether) {
    uint8_t i;
    arpPacket* arp = getArpPacket(ether);
    logPrintf(LOG_ARP, "%u.%u.%u.%u is at %02X:%02X:%02X:%02X:%02X:%02X", arp->sourceIp[0], arp->sourceIp[1], arp->sourceIp[2], arp->sourceIp[3],
              arp->sourceAddress[0], arp->sourceAddress[1], arp->sourceAddress[2],
              arp->sourceAddress[3], arp->sourceAddress[4], arp->sourceAddress[5]);
    addArpEntry(arp->sourceIp, arp->sourceAddress);
    for (i = 0; i < MAX_ARP_REQUESTS; i++) {
        //if this response answers one of our requests, call the request callback
        if (arpReqs[i].active && isIpEqual(arpReqs[i].ipAdd, arp->sourceIp)) {
            arpRespContext resp;
            _arp_callback_t callback = arpReqs[i].callback;
            resp.success = 1;
            copyMacAddress(resp.responseMacAddress, arp->sourceAddress);
            resp.ctxt = arpReqs[i].ctxt;
            freeArpRequest(&arpReqs[i]);
            if (callback) {
                callback(resp); //if target ip is external, response mac address will be router
            }
        }
    }
}

// Determines whether packet is ARP response
bool isArpResponse(etherHeader *ether) {
    arpPacket* arp = getArpPacket(ether);
    return isArpPacketValid(ether) && (arp->op == htons(2));
}

// Sends an ARP response given the request data
void sendArpResponse(etherHeader *ether) {
    arpPacket *arp = (arpPacket*)ether->data;
    uint8_t i, tmp;
    uint8_t localHwAddress[HW_ADD_LENGTH];
    logPrintf(LOG_ARP, "answering who-has from %u.%u.%u.%u", arp->sourceIp[0], arp->sourceIp[1], arp->sourceIp[2], arp->sourceIp[3]);

    // the requester is talking to us, so cache its address (RFC 826)
    addArpEntry(arp->sourceIp, arp->sourceAddress);
    // set op to response
    arp->op = htons(2);
    // swap source and destination fields
    getEtherMacAddress(localHwAddress);
    for (i = 0; i < HW_ADD_LENGTH; i++) {
        arp->destAddress[i] = arp->sourceAddress[i];
        ether->destAddress[i] = ether->sourceAddress[i];
        ether->sourceAddress[i] = arp->sourceAddress[i] = localHwAddress[i];
    }
    for (i = 0; i < IP_ADD_LENGTH; i++) {
        tmp = arp->destIp[i];
        arp->destIp[i] = arp->sourceIp[i];
        arp->sourceIp[i] = tmp;
    }
    // send packet
    putEtherPacket(ether, sizeof(etherHeader) + sizeof(arpPacket));
}

// Determines whether packet is ARP request for our IP
bool isArpRequest(etherHeader* ether) {
    arpPacket *arp = (arpPacket*)ether->data;
    uint8_t localIpAddress[IP_ADD_LENGTH];
    getIpAddress(localIpAddress);
    return isArpPacketValid(ether)
        && (arp->op == htons(1))
        && isIpValid(localIpAddress)
        && isIpEqual(arp->destIp, localIpAddress);
}

// Sends an ARP request
void sendArpRequest(etherHeader *ether, uint8_t ipFrom[], uint8_t ipTo[]) {
    arpPacket *arp = (arpPacket*)ether->data;
    uint8_t i;
    uint8_t localHwAddress[HW_ADD_LENGTH];
    logPrintf(LOG_ARP, "who-has %u.%u.%u.%u", ipTo[0], ipTo[1], ipTo[2], ipTo[3]);

    // fill ethernet frame
    getEtherMacAddress(localHwAddress);
    for (i = 0; i < HW_ADD_LENGTH; i++) {
        ether->sourceAddress[i] = localHwAddress[i];
        ether->destAddress[i] = 0xFF;
    }
    ether->frameType = htons(TYPE_ARP);
    // fill arp frame
    arp->hardwareType = htons(1);
    arp->protocolType = htons(TYPE_IP);
    arp->hardwareSize = HW_ADD_LENGTH;
    arp->protocolSize = IP_ADD_LENGTH;
    arp->op = htons(1);
    for (i = 0; i < HW_ADD_LENGTH; i++) {
        arp->sourceAddress[i] = localHwAddress[i];
        arp->destAddress[i] = 0x00; // target hardware address is unknown
    }
    for (i = 0; i < IP_ADD_LENGTH; i++) {
        arp->sourceIp[i] = ipFrom[i];
        arp->destIp[i] = ipTo[i];
    }
    // send packet
    putEtherPacket(ether, sizeof(etherHeader) + sizeof(arpPacket));
}
