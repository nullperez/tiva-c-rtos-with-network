#include "log.h"
#include "network_stack.h"
#include "eeprom.h"
#include "gpio.h"
#include "uart0.h"
#include "eth0.h"
#include "arp.h"
#include "icmp.h"
#include "dhcp.h"
#include "socket.h"
#include "udp.h"
#include "tcp.h"
#include "timer.h"
#include <stdio.h>
#include <string.h>

// receive buffer, word array so the headers are aligned
static uint32_t bufferWords[(MAX_PACKET_SIZE + 3) / 4];

bool isNetworkReady() {
    uint8_t ip[4], gw[4], sn[4];
    getIpAddress(ip);
    getIpGatewayAddress(gw);
    getIpSubnetMask(sn);
    return isIpValid(ip) && isIpValid(gw) && isIpValid(sn);
}

void netstat() {
    putsUart0("\nActive Connection\n------------------------------------------------------------\n");
    int i;
    socket* sockets = getSockets();
    for (i = 0; i < MAX_SOCKETS; i++) {
        socket* t = &sockets[i];
        if (t->valid) {
            uint8_t ip[4];
            const char* s = "";
            const char* type = "RAW";
            getIpAddress(ip);
            if (t->type == SOCKET_STREAM) {
                type = "TCP";
                s = getTcpStateName(t->state);
            }
            else if (t->type == SOCKET_DGRAM) {
                type = "UDP";
            }
            snprintf(out, MAX_UART_OUT, "%s   %d.%d.%d.%d:%d -> %d.%d.%d.%d:%d   %s\n", type, ip[0], ip[1], ip[2], ip[3], t->localPort, t->remoteIpAddress[0], t->remoteIpAddress[1], t->remoteIpAddress[2], t->remoteIpAddress[3], t->remotePort, s);
            putsUart0(out);
        }
    }
    putsUart0("------------------------------------------------------------\n");
}

void readConfiguration() {
    uint32_t temp;
    uint8_t* ip;

    if (readEeprom(EEPROM_DHCP) == EEPROM_ERASED) {
        enableDhcp();
    }
    else {
        disableDhcp();
        temp = readEeprom(EEPROM_IP);
        if (temp != EEPROM_ERASED)
        {
            ip = (uint8_t*)&temp;
            setIpAddress(ip);
        }
        temp = readEeprom(EEPROM_SUBNET_MASK);
        if (temp != EEPROM_ERASED)
        {
            ip = (uint8_t*)&temp;
            setIpSubnetMask(ip);
        }
        temp = readEeprom(EEPROM_GATEWAY);
        if (temp != EEPROM_ERASED)
        {
            ip = (uint8_t*)&temp;
            setIpGatewayAddress(ip);
        }
        temp = readEeprom(EEPROM_DNS);
        if (temp != EEPROM_ERASED)
        {
            ip = (uint8_t*)&temp;
            setIpDnsAddress(ip);
        }
        temp = readEeprom(EEPROM_TIME);
        if (temp != EEPROM_ERASED)
        {
            ip = (uint8_t*)&temp;
            setIpTimeServerAddress(ip);
        }
        temp = readEeprom(EEPROM_MQTT);
        if (temp != EEPROM_ERASED)
        {
            ip = (uint8_t*)&temp;
            setIpMqttBrokerAddress(ip);
        }
    }
}

// DHCP replies are broadcast while unconfigured and unicast when renewing
void processDhcpData(etherHeader* data) {
    if (isIp(data)) {
        if (isIpBroadcast(data) || isIpUnicast(data)) {
            if (isDhcpResponse(data)) {
                processDhcpResponse(data);
            }
        }
    }
}

void processTcpData(etherHeader* data) {
    if (isIp(data)) {
        if (isIpUnicast(data)) {
            if (isTcp(data)) {
                // finds the connection or listener, answers with RST if there is none
                processTcpSegment(data);
            }
        }
    }
}

// Compares a UDP payload to a string without relying on a terminator
static bool isUdpPayloadEqual(const uint8_t* payload, uint16_t length, const char* str) {
    uint16_t strLength = strlen(str);
    // accept an optional trailing NUL or newline
    while (length > 0 && (payload[length - 1] == '\0' || payload[length - 1] == '\n' || payload[length - 1] == '\r')) {
        length--;
    }
    return length == strLength && memcmp(payload, str, length) == 0;
}

void processUdpData(etherHeader* data) {
    socket s;
    if (isIp(data)) {
        if (isIpUnicast(data)) {
            if (isUdp(data)) {
                udpHeader* udp = getUdpHeader(data);
                if (udp->destPort == htons(68)) {
                    return; // DHCP client port, handled by processDhcpData
                }
                //Layer 5-7 logic
                uint8_t* udpData = getUdpData(data);
                uint16_t udpLength = getUdpDataLength(data);
                if (logEnabled(LOG_IP)) {
                    ipHeader* ip = getIpHeader(data);
                    logPrintf(LOG_IP, "udp %u bytes from %u.%u.%u.%u:%u to port %u", udpLength,
                              ip->sourceIp[0], ip->sourceIp[1], ip->sourceIp[2], ip->sourceIp[3], ntohs(udp->sourcePort), ntohs(udp->destPort));
                }
                if (isUdpPayloadEqual(udpData, udpLength, "on"))
                    setPinValue(GREEN_LED, 1);
                if (isUdpPayloadEqual(udpData, udpLength, "off"))
                    setPinValue(GREEN_LED, 0);
                memset(&s, 0, sizeof(s));
                getSocketInfoFromUdpPacket(data, &s);
                sendUdpMessage(data, &s, (uint8_t*)"Received", 9);
            }
        }
    }
}

void processIcmpData(etherHeader* data) {
    if (isIp(data)) {
        if (isIpUnicast(data)) {
            icmpEchoResponse r;
            if (isPingRequest(data)) {
                ipHeader* ip = getIpHeader(data);
                logPrintf(LOG_IP, "echo request from %u.%u.%u.%u, replying", ip->sourceIp[0], ip->sourceIp[1], ip->sourceIp[2], ip->sourceIp[3]);
                sendPingResponse(data);
            }
            else if (isPingResponse(data, &r)) {
                snprintf(out, MAX_UART_OUT, "Reply from %d.%d.%d.%d: bytes=%d time=%dms TTL=%d\n", r.remoteIp[0], r.remoteIp[1], r.remoteIp[2], r.remoteIp[3], r.bytes, (int)r.ms, r.ttl);
                putsUart0(out);
            }
        }
    }
}

void processArpData(etherHeader* data) {
    if (isArpRequest(data)) {
        sendArpResponse(data);
    }
    else if (isArpResponse(data)) {
        processArpResponse(data);
        processDhcpArpResponse(data);
    }
}

// Runs the protocol timers and handles at most one received packet.
// Returns true if a packet was processed, so the caller can drain the
// receive buffer before blocking.
bool runNetworkStack() {
    etherHeader* data = (etherHeader*)bufferWords;
    uint16_t size;
    // timer callbacks run here, outside of the interrupt
    processTimers();
    if (isDhcpEnabled()) {
        sendDhcpPendingMessages(data); //for DHCP state machine
    }
    sendTcpPendingMessages(); //for TCP state machine
    if (!isEtherDataAvailable()) {
        return false;
    }
    // red LED toggles on each receive buffer overflow
    if (isEtherOverflow()) {
        togglePinValue(RED_LED);
    }
    size = getEtherPacket(data, MAX_PACKET_SIZE);

    if (data->frameType == htons(TYPE_ARP)) {
        processArpData(data);
    }
    else if (isIp(data) && isIpLengthValid(data, size)) {
        // each handler can transmit a reply in place, so only one may run
        ipHeader* ip = getIpHeader(data);
        switch (ip->protocol) {
        case PROTOCOL_TCP:
            processTcpData(data);
            break;
        case PROTOCOL_ICMP:
            processIcmpData(data);
            break;
        case PROTOCOL_UDP:
            if (isDhcpEnabled()) {
                processDhcpData(data);
            }
            processUdpData(data);
            break;
        }
    }
    return true;
}
