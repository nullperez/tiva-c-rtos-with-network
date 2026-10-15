/******************************************************************************
 * File:        mqtt.c
 *
 * Author:      Giancarlo Perez
 *
 * Created:     12/2/24
 *
 * Description: MQTT v3.1.1 packet encoding
 ******************************************************************************/


//=============================================================================
// INCLUDES
//=============================================================================

#include <stdio.h>
#include <string.h>
#include "mqtt.h"
#include "tcp.h"
#include "timer.h"
#include "uart0.h"
#include "strlib.h"


//=============================================================================
// DEFINES AND MACROS
//=============================================================================

#define lobyte(x) ((x) & 0xFF)
#define hibyte(x) (((x) >> 8) & 0xFF)

// Fixed header is 1 byte + up to 4 remaining length bytes, the body is built
// after it and the header is written right in front of the body when sending
#define MQTT_MAX_FIXED_HEADER 5
#define MQTT_MAX_BODY_SIZE (MAX_MQTT_PACKET_SIZE - MQTT_MAX_FIXED_HEADER)

//=============================================================================
// GLOBALS
//=============================================================================

uint8_t mqttBuffer[MAX_MQTT_PACKET_SIZE];
static uint16_t nextPacketId = 1;

//=============================================================================
// STATIC FUNCTIONS
//=============================================================================

static uint8_t encodeLength(uint8_t* out, uint32_t i) {
    uint8_t byte;
    uint8_t lenlen = 0;
    do {
        byte = i % 128;
        i /= 128;
        if (i > 0) {
            byte |= 0x80;
        }
        out[lenlen++] = byte;
    } while (i > 0);
    return lenlen;
}

static uint8_t* getMqttBody() {
    return mqttBuffer + MQTT_MAX_FIXED_HEADER;
}

// Packet identifiers must be non-zero (MQTT 3.1.1 2.3.1)
static uint16_t getPacketId() {
    uint16_t id = nextPacketId++;
    if (nextPacketId == 0) {
        nextPacketId = 1;
    }
    return id;
}

// Appends a UTF-8 string with its 2 byte length prefix, returns false if it does not fit
static bool putMqttString(uint8_t* body, uint16_t* i, const char* str) {
    uint32_t len = str_length(str);
    if (*i + 2 + len > MQTT_MAX_BODY_SIZE) {
        return false;
    }
    body[(*i)++] = hibyte(len);
    body[(*i)++] = lobyte(len);
    memcpy(body + *i, str, len);
    *i += len;
    return true;
}

// Prepends the fixed header to the body and queues the whole packet
// A partial packet would corrupt the stream, so nothing is sent if it does not fit
static bool sendMqttPacket(mqttClient* client, uint8_t typeAndFlags, uint16_t bodyLength) {
    uint8_t lengthBytes[4];
    uint8_t lenlen = encodeLength(lengthBytes, bodyLength);
    uint8_t* start = getMqttBody() - 1 - lenlen;
    uint16_t total = 1 + lenlen + bodyLength;
    if (client->socket == NULL || socketGetTxFree(client->socket) < total) {
        return false;
    }
    start[0] = typeAndFlags;
    memcpy(start + 1, lengthBytes, lenlen);
    socketSendTcp(client->socket, start, total);
    restartTimer(client->keepAliveTimer); // keep alive counts from the last packet sent
    return true;
}

//=============================================================================
// PUBLIC FUNCTIONS
//=============================================================================

int8_t decodeLength(const uint8_t* data, uint16_t available, uint32_t* value) {
    uint32_t multiplier = 1;
    uint32_t result = 0;
    uint8_t lenlen = 0;
    while (lenlen < available) {
        result += (data[lenlen] & 0x7F) * multiplier;
        if ((data[lenlen] & 0x80) == 0) {
            *value = result;
            return lenlen + 1;
        }
        lenlen++;
        if (lenlen == 4) {
            return -1; // at most 4 length bytes
        }
        multiplier *= 128;
    }
    return 0;
}

bool sendMqttConnect(mqttClient* client) {
    uint8_t* body = getMqttBody();
    mqttOptions* opt = &client->options; //using a ptr to save space
    uint16_t i = 0;
    // Protocol Name:
    if (!putMqttString(body, &i, "MQTT")) {
        return false;
    }
    // Version:
    body[i++] = opt->version;
    // Connect Flags:
    body[i++] = (opt->willRetain << 5) | (opt->willQos << 3) | (opt->willFlag << 2) | (opt->cleanSession << 1);
    // Keep Alive:
    body[i++] = hibyte(opt->keepAlive);
    body[i++] = lobyte(opt->keepAlive);
    // Client ID:
    if (!putMqttString(body, &i, client->clientId)) {
        return false;
    }
    // Will Topic and Message:
    if (opt->willFlag) {
        if (!putMqttString(body, &i, opt->willTopic) || !putMqttString(body, &i, opt->willMsg)) {
            return false;
        }
    }
    return sendMqttPacket(client, MQTT_CONNECT << 4, i);
}

bool sendMqttPublish(mqttClient* client, char strTopic[], char strData[]) {
    uint8_t* body = getMqttBody();
    mqttOptions* opt = &client->options;
    uint16_t i = 0;
    uint32_t msgLen = str_length(strData);
    // Topic:
    if (!putMqttString(body, &i, strTopic)) {
        return false;
    }
    // Packet Identifier (QoS 1 and 2 only):
    if (opt->qos > 0) {
        uint16_t packetId = getPacketId();
        body[i++] = hibyte(packetId);
        body[i++] = lobyte(packetId);
    }
    // Payload, no length prefix:
    if (i + msgLen > MQTT_MAX_BODY_SIZE) {
        return false;
    }
    memcpy(body + i, strData, msgLen);
    i += msgLen;
    return sendMqttPacket(client, (MQTT_PUBLISH << 4) | (opt->qos << 1), i);
}

bool sendMqttPubAck(mqttClient* client, uint16_t packetId) {
    uint8_t* body = getMqttBody();
    body[0] = hibyte(packetId);
    body[1] = lobyte(packetId);
    return sendMqttPacket(client, MQTT_PUBACK << 4, 2);
}

bool sendMqttSubscribe(mqttClient* client, char strTopic[]) {
    uint8_t* body = getMqttBody();
    mqttOptions* opt = &client->options;
    uint16_t i = 0;
    uint16_t packetId = getPacketId();
    body[i++] = hibyte(packetId);
    body[i++] = lobyte(packetId);
    if (!putMqttString(body, &i, strTopic) || i + 1 > MQTT_MAX_BODY_SIZE) {
        return false;
    }
    body[i++] = opt->qos; // requested QoS
    return sendMqttPacket(client, (MQTT_SUBSCRIBE << 4) | 0b0010, i);
}

bool sendMqttUnsubscribe(mqttClient* client, char strTopic[]) {
    uint8_t* body = getMqttBody();
    uint16_t i = 0;
    uint16_t packetId = getPacketId();
    body[i++] = hibyte(packetId);
    body[i++] = lobyte(packetId);
    if (!putMqttString(body, &i, strTopic)) {
        return false;
    }
    return sendMqttPacket(client, (MQTT_UNSUBSCRIBE << 4) | 0b0010, i);
}

bool sendMqttPingReq(mqttClient* client) {
    return sendMqttPacket(client, MQTT_PINGREQ << 4, 0);
}

bool sendMqttDisconnect(mqttClient* client) {
    return sendMqttPacket(client, MQTT_DISCONNECT << 4, 0);
}
