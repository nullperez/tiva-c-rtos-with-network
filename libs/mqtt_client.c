/******************************************************************************
 * File:        mqtt_client.c
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
#include "mqtt_client.h"
#include "mqtt.h"
#include "strlib.h"
#include "uart0.h"
#include <stdio.h>
#include <string.h>

//=============================================================================
// DEFINES AND MACROS
//=============================================================================

#ifndef NULL
 #define NULL 0
#endif

/* Configurations */
#define MAX_CONNECT_RETRIES 2
#define MQTT_VERSION MQTT_VERSION_v3_1_1
#define USE_WILL 0
#define QOS 0
#define MQTT_CONNECT_TIMEOUT 10

//=============================================================================
// GLOBALS
//=============================================================================

//we are assuming client is a singleton and only one instance can run on this device. functions shall not take parameters as they will modify this singleton

/* Globals */
static mqttClient client[1];

//=============================================================================
// STATIC FUNCTIONS
//=============================================================================

/* Forward Declarations */
static void socketErrorCallback(void* context);
static void socketEventCallback(socket* s, uint8_t event);
static void socketRecvCallback(socket* s, const uint8_t* data, uint16_t length);
static void mqttRetryConnection();
static void mqttErrorCallback(void* context);
static void mqttConnectTimeout(void* context);
static void mqttKeepAliveCallback(void* context);

static void stopMqttTimers() {
    stopTimerHandle(&client->timeoutTimer);
    stopTimerHandle(&client->keepAliveTimer);
}

//Application level error handler, application can handle Layer 4 errors here
//Layer 4 calls this function, the socket is freed by the stack afterwards
static void socketErrorCallback(void* context) {
    socketError* err = (socketError*)context;
    putsUart0("MQTT Client: ");
    putsUart0(err->errorMsg);
    putsUart0("\n\n");
}

//Layer 4 connection events
static void socketEventCallback(socket* s, uint8_t event) {
    if (s != client->socket) {
        return;
    }
    switch (event) {
    case SOCKET_EVENT_CONNECTED:
        putsUart0("MQTT Client: TCP Connection established\n");
        setMqttState(MQTT_CLIENT_STATE_TCP_CONNECTED);
        break;
    case SOCKET_EVENT_PEER_CLOSED:
        //broker closed the connection, close our side as well
        setMqttState(MQTT_CLIENT_STATE_DISCONNECTING);
        socketCloseTcp(s);
        break;
    case SOCKET_EVENT_CLOSED:
        //the socket is freed after this event
        client->socket = NULL;
        client->rxSize = 0;
        client->rxDiscard = 0;
        stopMqttTimers();
        if (getMqttState() != MQTT_CLIENT_STATE_DISCONNECTED) {
            putsUart0("MQTT Client: Disconnected from MQTT Broker\n");
        }
        setMqttState(MQTT_CLIENT_STATE_DISCONNECTED);
        break;
    }
}

static void mqttRetryConnection() {
    uint8_t mqstate = getMqttState();
    if (mqstate == MQTT_CLIENT_STATE_MQTT_CONNECTING) {
        client->connectAttempts++;
        if (client->connectAttempts > MAX_CONNECT_RETRIES) {
            putsUart0("MQTT Client: Failed to connect to MQTT Broker\n\n");
            client->connectAttempts = 0;
            disconnectMqtt();
        }
        else {
            snprintf(out, MAX_UART_OUT, "MQTT Client: Retrying connection to MQTT Broker... (%d/%d)\n", client->connectAttempts, MAX_CONNECT_RETRIES);
            putsUart0(out);
            //a second CONNECT on the same connection is a protocol violation, so start a new connection
            client->reconnect = true;
            socketAbortTcp(client->socket);
        }
    }
    else {
        //timer should be stopped when this function is called and should not restart
    }
}

//Application error handler, application will handle Layer 7 errors here
//Layer 7 calls this function
static void mqttErrorCallback(void* context) {
    mqttError* err = (mqttError*)context;
    putsUart0(err->errorMsg);
    switch(err->errorCode) {
    //MQTT layer errors
    case MQTT_ERROR_CONNECT_TIMEOUT:
        mqttRetryConnection();
        break;
    }
}

static void mqttConnectTimeout(void* context) {
    mqttError err;
    client->timeoutTimer = INVALID_TIMER;
    err.errorCode = MQTT_ERROR_CONNECT_TIMEOUT;
    str_copy(err.errorMsg, "Timed out waiting for MQTT Connect from broker.\n");
    mqttErrorCallback(&err);
}

static void mqttKeepAliveCallback(void* context) {
    logPrintf(LOG_MQTT, "sending PINGREQ");
    sendMqttPingReq(client);
}

// Handles one complete MQTT packet
static void processMqttPacket(const uint8_t* packet, uint16_t length) {
    uint8_t type = packet[0] >> 4;
    uint8_t flags = packet[0] & 0x0F;
    uint32_t bodyLength;
    int8_t lenlen = decodeLength(packet + 1, length - 1, &bodyLength);
    const uint8_t* body = packet + 1 + lenlen;
    uint8_t mqttState = getMqttState();

    if (mqttState == MQTT_CLIENT_STATE_MQTT_CONNECTING) {
        if (type == MQTT_CONNACK && bodyLength >= 2) {
            stopTimerHandle(&client->timeoutTimer);
            if (body[1] != 0) {
                snprintf(out, MAX_UART_OUT, "MQTT Client: Connection refused by broker (code %d)\n", body[1]);
                putsUart0(out);
                disconnectMqtt();
                return;
            }
            client->connectAttempts = 0;
            if (client->options.keepAlive > 0) {
                //ping a bit before the keep alive expires, the timer restarts on every packet sent
                uint32_t period = (client->options.keepAlive * 9) / 10;
                client->keepAliveTimer = startPeriodicTimer(mqttKeepAliveCallback, period > 0 ? period : 1, NULL);
            }
            setMqttState(MQTT_CLIENT_STATE_MQTT_CONNECTED);
            putsUart0("MQTT Client: Connected to MQTT Broker\n");
        }
    }
    else if (mqttState == MQTT_CLIENT_STATE_MQTT_CONNECTED) {
        switch (type) {
        case MQTT_PUBLISH: {
            mqttData data;
            uint8_t qos = (flags >> 1) & 0x03;
            uint16_t topicLen;
            uint16_t pos;
            uint16_t packetId = 0;
            if (bodyLength < 2) {
                return;
            }
            topicLen = ((uint16_t)body[0] << 8) | body[1];
            pos = 2 + topicLen;
            if (qos > 0) {
                if ((uint32_t)pos + 2 > bodyLength) {
                    return;
                }
                packetId = ((uint16_t)body[pos] << 8) | body[pos + 1];
                pos += 2;
            }
            if (pos > bodyLength) {
                return; // malformed
            }
            if (topicLen >= MAX_TOPIC_LENGTH) {
                putsUart0("MQTT Client: Received PUBLISH with a topic that is too long\n");
            }
            else {
                memcpy(data.topic, body + 2, topicLen);
                data.topic[topicLen] = '\0';
                data.data = body + pos;
                data.dataLen = bodyLength - pos;
                putsUart0("MQTT Client: Received PUBLISH\n");
                snprintf(out, MAX_UART_OUT, " Received:\n  Topic: %s\n  Data: %.*s\n", data.topic, (int)data.dataLen, (const char*)data.data);
                putsUart0(out);
                if (client->pubCallback) {
                    client->pubCallback(&data); //Pass data to application
                }
            }
            if (qos == QOS_1) {
                sendMqttPubAck(client, packetId);
            }
            //QoS 2 is not supported, subscriptions request QOS
            break;
        }
        case MQTT_PUBACK:
            logPrintf(LOG_MQTT, "received PUBACK");
            break;
        case MQTT_SUBACK:
            putsUart0("MQTT Client: Received SUBACK\n");
            break;
        case MQTT_UNSUBACK:
            putsUart0("MQTT Client: Received UNSUBACK\n");
            break;
        case MQTT_PINGRESP:
            logPrintf(LOG_MQTT, "received PINGRESP");
            break;
        }
    }
    else {
        //we only care about MQTT packets if we're connecting or connected
    }
}

//Layer 4 data, may hold several packets or only part of one
static void socketRecvCallback(socket* s, const uint8_t* data, uint16_t length) {
    while (length > 0 && s == client->socket) {
        uint16_t n;
        if (client->rxDiscard > 0) {
            n = (client->rxDiscard < length) ? client->rxDiscard : length;
            client->rxDiscard -= n;
            data += n;
            length -= n;
            continue;
        }
        n = MAX_MQTT_PACKET_SIZE - client->rxSize;
        if (n > length) {
            n = length;
        }
        memcpy(client->rxBuffer + client->rxSize, data, n);
        client->rxSize += n;
        data += n;
        length -= n;
        // process every complete packet in the buffer
        while (client->rxSize >= 2) {
            uint32_t remaining;
            uint32_t total;
            int8_t lenlen = decodeLength(client->rxBuffer + 1, client->rxSize - 1, &remaining);
            if (lenlen == 0) {
                break; // need more length bytes
            }
            if (lenlen < 0) {
                putsUart0("MQTT Client: Malformed packet from broker\n");
                socketAbortTcp(s);
                return;
            }
            total = 1 + lenlen + remaining;
            if (total > MAX_MQTT_PACKET_SIZE) {
                putsUart0("MQTT Client: Packet too large, dropped\n");
                client->rxDiscard = total - client->rxSize;
                client->rxSize = 0;
                break;
            }
            if (total > client->rxSize) {
                break; // need the rest of the packet
            }
            processMqttPacket(client->rxBuffer, total);
            if (s != client->socket) {
                return; // connection was closed while handling the packet
            }
            memmove(client->rxBuffer, client->rxBuffer + total, client->rxSize - total);
            client->rxSize -= total;
        }
    }
}

//=============================================================================
// PUBLIC FUNCTIONS
//=============================================================================

//main client loop
void runMqttClient() {
    uint8_t mqttState = getMqttState();
    switch (mqttState) {
    case MQTT_CLIENT_STATE_DISCONNECTED:
        if (client->reconnect) {
            client->reconnect = false;
            connectMqtt();
        }
        break;
    case MQTT_CLIENT_STATE_TCP_CONNECTING:
        //waiting for SOCKET_EVENT_CONNECTED
        break;
    case MQTT_CLIENT_STATE_TCP_CONNECTED:
        if (sendMqttConnect(client)) {
            setMqttState(MQTT_CLIENT_STATE_MQTT_CONNECTING);
            client->timeoutTimer = startOneshotTimer(mqttConnectTimeout, MQTT_CONNECT_TIMEOUT, NULL);
        }
        break;
    case MQTT_CLIENT_STATE_MQTT_CONNECTING:
        //handle CONNACK in processMqttPacket
        break;
    case MQTT_CLIENT_STATE_MQTT_CONNECTED:
        break;
    case MQTT_CLIENT_STATE_DISCONNECTING:
        //waiting for SOCKET_EVENT_CLOSED
        break;
    }
}

inline void setMqttState(uint8_t state) {
    client->state = state;
}

inline uint8_t getMqttState() {
    return client->state;
}

inline void setHandlePublishCallback(mqtt_callback_t cb) {
    client->pubCallback = cb;
}

//use strlib eventually
void setMqttTopics(char** topics, uint32_t count) {
    uint32_t i, j;
    if (count <= MAX_TOPICS) {
        for (i = 0; i < count; i++) {
            for (j = 0; topics[i][j] != '\0' && j < MAX_TOPIC_LENGTH - 1; j++) {
                client->mqttTopics[i][j] = topics[i][j];
            }
            client->mqttTopics[i][j] = '\0';
        }
    }
}

void getMqttTopics(char** input, uint32_t* count) {
    uint32_t i;
    for (i = 0; i < MAX_TOPICS; i++) {
        input[i] = client->mqttTopics[i];
        if (client->mqttTopics[i][0] == '\0') {
            *count = i;
            break;
        }
        *count = i + 1;
    }
}

void initMqttClient() {
    setMqttState(MQTT_CLIENT_STATE_DISCONNECTED);
    mqttOptions* opt = &client->options;
    str_copy(client->clientId, "TM4C123GXL");
    opt->version = MQTT_VERSION;
    opt->qos = QOS; //in the future have this configured by command, along with all other options
    opt->keepAlive = DEFAULT_KEEPALIVE;
    opt->cleanSession = DEFAULT_CLEANSESSION;
    opt->willFlag = 0; //set will flag off by default, wil be able to be changed by commands in the future
    opt->willQos = QOS_0;
    opt->willRetain = 0;
    client->pubCallback = NULL;
    client->socket = NULL;
    client->timeoutTimer = INVALID_TIMER;
    client->keepAliveTimer = INVALID_TIMER;
    client->connectAttempts = 0;
    client->reconnect = false;
    client->rxSize = 0;
    client->rxDiscard = 0;
}

//connects to MQTT server stored in device on port 1883
//starts with TCP connection
//once TCP established, send MQTT connect and move to MQTT_CONNECTING

void connectMqtt() {
    uint8_t mqttState = getMqttState();
    uint8_t mqserv[4];
    switch (mqttState) {
    case MQTT_CLIENT_STATE_DISCONNECTED:
        getIpMqttBrokerAddress(mqserv);
        if (!isIpValid(mqserv)) {
            putsUart0("MQTT Client: No MQTT broker address set\n");
            return;
        }
        client->socket = newSocket(SOCKET_STREAM);
        if (client->socket == NULL) {
            putsUart0("MQTT Client: No free sockets\n");
            return;
        }
        client->socket->errorCallback = socketErrorCallback;
        client->socket->eventCallback = socketEventCallback;
        client->socket->recvCallback = socketRecvCallback;
        client->rxSize = 0;
        client->rxDiscard = 0;
        snprintf(out, MAX_UART_OUT, "MQTT Client: Connecting to MQTT server %d.%d.%d.%d:%d\n", mqserv[0], mqserv[1], mqserv[2], mqserv[3], MQTT_PORT);
        putsUart0(out);
        setMqttState(MQTT_CLIENT_STATE_TCP_CONNECTING);
        socketConnectTcp(client->socket, mqserv, MQTT_PORT); // may fail right away and close the socket
        break;
    default:
        break;
    }
}

void disconnectMqtt() {
    uint8_t mqttState = getMqttState();
    client->reconnect = false;
    switch (mqttState) {
    case MQTT_CLIENT_STATE_DISCONNECTED:
        //do nothing, client already disconnected
        return;
    case MQTT_CLIENT_STATE_MQTT_CONNECTED:
        //got CONNACK, tell the broker before closing the connection
        sendMqttDisconnect(client);
        break;
    default:
        break;
    }
    stopMqttTimers();
    setMqttState(MQTT_CLIENT_STATE_DISCONNECTING);
    if (client->socket != NULL) {
        socketCloseTcp(client->socket); // SOCKET_EVENT_CLOSED moves to DISCONNECTED
    }
    else {
        setMqttState(MQTT_CLIENT_STATE_DISCONNECTED);
    }
}

void publishMqtt(char strTopic[], char strData[]) {
    if (getMqttState() == MQTT_CLIENT_STATE_MQTT_CONNECTED) {
        if (!sendMqttPublish(client, strTopic, strData)) {
            putsUart0("MQTT Client: PUBLISH too large or send buffer full\n");
        }
    }
}

void subscribeMqtt(char strTopic[]) {
    if (getMqttState() == MQTT_CLIENT_STATE_MQTT_CONNECTED) {
        if (!sendMqttSubscribe(client, strTopic)) {
            putsUart0("MQTT Client: SUBSCRIBE too large or send buffer full\n");
        }
    }
}

void unsubscribeMqtt(char strTopic[]) {
    if (getMqttState() == MQTT_CLIENT_STATE_MQTT_CONNECTED) {
        if (!sendMqttUnsubscribe(client, strTopic)) {
            putsUart0("MQTT Client: UNSUBSCRIBE too large or send buffer full\n");
        }
    }
}
