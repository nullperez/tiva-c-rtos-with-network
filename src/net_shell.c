#include "net_shell.h"
#include "tm4c123gh6pm.h"
#include "uart0.h"
#include "eeprom.h"
#include "eth0.h"
#include "arp.h"
#include "ip.h"
#include "icmp.h"
#include "dhcp.h"
#include "mqtt_client.h"
#include "strlib.h"
#include <inttypes.h>
#include <network_stack.h>
#include <stdio.h>

extern arp_entry_t arpTable[MAX_ARP_ENTRIES];

// newlib-nano doesn't support the hh length modifier, so scan into an
// unsigned int and truncate.
static uint8_t asciiToUint8(const char str[]) {
    unsigned int data = 0;
    if (str[0] == '0' && to_lower(str[1]) == 'x')
        sscanf(str, "%x", &data);
    else
        sscanf(str, "%u", &data);
    return (uint8_t)data;
}

static void ipconfig() {
    uint8_t i;
    char str[32];
    uint8_t mac[6];
    uint8_t ip[4];
    getEtherMacAddress(mac);
    putsUart0("\nIP Configuration\n------------------------------------------------------------\n");
    putsUart0("  MAC:   ");
    for (i = 0; i < HW_ADD_LENGTH; i++)
    {
        snprintf(str, sizeof(str), "%02X", mac[i]);
        putsUart0(str);
        if (i < HW_ADD_LENGTH-1)
            putcUart0(':');
    }
    putcUart0('\n');
    getIpAddress(ip);
    putsUart0("  IP:    ");
    for (i = 0; i < IP_ADD_LENGTH; i++)
    {
        snprintf(str, sizeof(str), "%u", ip[i]);
        putsUart0(str);
        if (i < IP_ADD_LENGTH-1)
            putcUart0('.');
    }
    if (isDhcpEnabled())
        putsUart0(" (dhcp)");
    else
        putsUart0(" (static)");
    putcUart0('\n');
    getIpSubnetMask(ip);
    putsUart0("  SN:    ");
    for (i = 0; i < IP_ADD_LENGTH; i++)
    {
        snprintf(str, sizeof(str), "%u", ip[i]);
        putsUart0(str);
        if (i < IP_ADD_LENGTH-1)
            putcUart0('.');
    }
    putcUart0('\n');
    getIpGatewayAddress(ip);
    putsUart0("  GW:    ");
    for (i = 0; i < IP_ADD_LENGTH; i++)
    {
        snprintf(str, sizeof(str), "%u", ip[i]);
        putsUart0(str);
        if (i < IP_ADD_LENGTH-1)
            putcUart0('.');
    }
    putcUart0('\n');
    getIpDnsAddress(ip);
    putsUart0("  DNS:   ");
    for (i = 0; i < IP_ADD_LENGTH; i++)
    {
        snprintf(str, sizeof(str), "%u", ip[i]);
        putsUart0(str);
        if (i < IP_ADD_LENGTH-1)
            putcUart0('.');
    }
    putcUart0('\n');
    getIpTimeServerAddress(ip);
    putsUart0("  NTP:   ");
    for (i = 0; i < IP_ADD_LENGTH; i++)
    {
        snprintf(str, sizeof(str), "%u", ip[i]);
        putsUart0(str);
        if (i < IP_ADD_LENGTH-1)
            putcUart0('.');
    }
    putcUart0('\n');
    getIpMqttBrokerAddress(ip);
    putsUart0("  MQTT:  ");
    for (i = 0; i < IP_ADD_LENGTH; i++)
    {
        snprintf(str, sizeof(str), "%u", ip[i]);
        putsUart0(str);
        if (i < IP_ADD_LENGTH-1)
            putcUart0('.');
    }
    putcUart0('\n');
    if (isDhcpEnabled())
    {
        putsUart0("  Lease: ");
        uint32_t s, m, h, d;
        s = getDhcpLeaseSeconds();
        d = s / (24*60*60);
        s -= d * (24*60*60);
        h = s / (60*60);
        s -= h * (60*60);
        m = s / 60;
        snprintf(str, sizeof(str), "%"PRIu32"d:%02"PRIu32"h:%02"PRIu32"m\n", d, h, m);
        putsUart0(str);
    }
    if (isEtherLinkUp())
        putsUart0("  Link is up\n");
    else
        putsUart0("  Link is down\n");
    putsUart0("------------------------------------------------------------\n\n");
}

// Runs one shell command line in netTask, see shell.c
void netShellCommand(void* data) {
    char* line = data;
    uint8_t i;
    uint8_t ip[IP_ADD_LENGTH];
    uint32_t* p32;
    char* topic,* payload,* token;
    token = str_tokenize(line, " ");
    if (token == NULL) {
        return;
    }
    if (str_equal(token, "dhcp")) {
        token = str_tokenize(NULL, " ");
        if (str_equal(token, "renew")) {
            renewDhcp();
        }
        else if (str_equal(token, "release")) {
            releaseDhcp();
        }
        else if (str_equal(token, "on")) {
            enableDhcp();
            writeEeprom(EEPROM_DHCP, EEPROM_ERASED);
        }
        else if (str_equal(token, "off")) {
            disableDhcp();
            writeEeprom(EEPROM_DHCP, 0);
        }
        else {
            putsUart0("Error in dhcp argument\r");
        }
    }
    else if (str_equal(token, "mqtt")) {
        token = str_tokenize(NULL, " ");
        if (str_equal(token, "connect")) {
            connectMqtt();
        }
        if (str_equal(token, "disconnect")) {
            disconnectMqtt();
        }
        if (str_equal(token, "publish")) {
            topic = str_tokenize(NULL, " ");
            payload = str_tokenize(NULL, " ");
            if (topic != NULL && payload != NULL)
                publishMqtt(topic, payload);
        }
        if (str_equal(token, "subscribe")) {
            topic = str_tokenize(NULL, " ");
            if (topic != NULL)
                subscribeMqtt(topic);
        }
        if (str_equal(token, "unsubscribe")) {
            topic = str_tokenize(NULL, " ");
            if (topic != NULL)
                unsubscribeMqtt(topic);
        }
    }
    else if (str_equal(token, "ipconfig")) {
        ipconfig();
    }
    /*if (str_equal(token, "sendudp")) {
        socket* udp = newSocket(SOCKET_DGRAM);
        uint8_t destIp[4] = {192, 168, 1, 163};
        uint16_t destPort = 8080;
        char* data = "HelloWorld";
        uint16_t len = str_length(data);
        socketSendTo(udp, destIp, destPort, (uint8_t*)data, len); //async
    }*/
    else if (str_equal(token, "netstat")) {
        netstat();
    }
    else if (str_equal(token, "arp")) {
        token = str_tokenize(NULL, " ");
        if (str_equal(token, "clear")) {
            clearArpTable();
        }
        else {
            displayArpTable();
        }
    }
    else if (str_equal(token, "ping")) {
        for (i = 0; i < IP_ADD_LENGTH; i++)
        {
            token = str_tokenize(NULL, " .");
            ip[i] = asciiToUint8(token);
        }
        ping(ip);
    }
    else if (str_equal(token, "set")) {
        token = str_tokenize(NULL, " ");
        if (str_equal(token, "ip")) {
            for (i = 0; i < IP_ADD_LENGTH; i++) {
                token = str_tokenize(NULL, " .");
                ip[i] = asciiToUint8(token);
            }
            setIpAddress(ip);
            p32 = (uint32_t*)ip;
            writeEeprom(EEPROM_IP, *p32);
        }
        if (str_equal(token, "sn")) {
            for (i = 0; i < IP_ADD_LENGTH; i++) {
                token = str_tokenize(NULL, " .");
                ip[i] = asciiToUint8(token);
            }
            setIpSubnetMask(ip);
            p32 = (uint32_t*)ip;
            writeEeprom(EEPROM_SUBNET_MASK, *p32);
        }
        if (str_equal(token, "gw")) {
            for (i = 0; i < IP_ADD_LENGTH; i++) {
                token = str_tokenize(NULL, " .");
                ip[i] = asciiToUint8(token);
            }
            setIpGatewayAddress(ip);
            p32 = (uint32_t*)ip;
            writeEeprom(EEPROM_GATEWAY, *p32);
        }
        if (str_equal(token, "dns")) {
            for (i = 0; i < IP_ADD_LENGTH; i++) {
                token = str_tokenize(NULL, " .");
                ip[i] = asciiToUint8(token);
            }
            setIpDnsAddress(ip);
            p32 = (uint32_t*)ip;
            writeEeprom(EEPROM_DNS, *p32);
        }
        if (str_equal(token, "time")) {
            for (i = 0; i < IP_ADD_LENGTH; i++) {
                token = str_tokenize(NULL, " .");
                ip[i] = asciiToUint8(token);
            }
            setIpTimeServerAddress(ip);
            p32 = (uint32_t*)ip;
            writeEeprom(EEPROM_TIME, *p32);
        }
        if (str_equal(token, "mqtt")) {
            for (i = 0; i < IP_ADD_LENGTH; i++) {
                token = str_tokenize(NULL, " .");
                ip[i] = asciiToUint8(token);
            }
            setIpMqttBrokerAddress(ip);
            p32 = (uint32_t*)ip;
            writeEeprom(EEPROM_MQTT, *p32);
        }
    }
    else if (str_equal(token, "help")) {
        putsUart0("Network commands:\n");
        putsUart0("  arp [clear]\n");
        putsUart0("  dhcp on|off|renew|release\n");
        putsUart0("  mqtt ACTION [USER [PASSWORD]]\n");
        putsUart0("    where ACTION = {connect|disconnect|publish TOPIC DATA\n");
        putsUart0("                   |subscribe TOPIC|unsubscribe TOPIC}\n");
        putsUart0("  ipconfig\n");
        putsUart0("  netstat\n");
        putsUart0("  ping w.x.y.z\n");
        putsUart0("  set ip|gw|dns|time|mqtt|sn w.x.y.z\n");
    }
    else {
        putsUart0("Invalid command\n");
    }
}
