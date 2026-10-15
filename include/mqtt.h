/******************************************************************************
 * File:        mqtt.h
 *
 * Author:      Giancarlo Perez
 *
 * Created:     12/2/24
 *
 * Description: -
 ******************************************************************************/

#ifndef MQTT_H_
#define MQTT_H_

//=============================================================================
// INCLUDES
//=============================================================================

#include "tcp.h"
#include <stdint.h>
#include <stdbool.h>

//=============================================================================
// DEFINES AND MACROS
//=============================================================================

/* MQTT Message Types */
#define MQTT_CONNECT 1
#define MQTT_CONNACK 2
#define MQTT_PUBLISH 3
#define MQTT_PUBACK 4
#define MQTT_PUBREC 5
#define MQTT_PUBREL 6
#define MQTT_PUBCOMP 7
#define MQTT_SUBSCRIBE 8
#define MQTT_SUBACK 9
#define MQTT_UNSUBSCRIBE 10
#define MQTT_UNSUBACK 11
#define MQTT_PINGREQ 12
#define MQTT_PINGRESP 13
#define MQTT_DISCONNECT 14
#define MQTT_AUTH 15

/* MQTT QoS Types */
#define QOS_0 0
#define QOS_1 1
#define QOS_2 2
#define QOS_3 3

/* MQTT Flags */
#define MQTT_FLAG_USERNAME 0x80
#define MQTT_FLAG_PASSWORD 0x40
#define MQTT_FLAG_WILLRETAIN 0x20
#define MQTT_FLAG_QOS_0 0x00
#define MQTT_FLAG_QOS_1 0x08
#define MQTT_FLAG_QOS_2 0x10
#define MQTT_FLAG_QOS_3 0x18
#define MQTT_FLAG_WILL 0x04
#define MQTT_FLAG_CLEANSESSION 0x02

/* MQTT Client Error Types */
#define MQTT_OK 0
#define MQTT_ERROR_CONNECT_INVALID_SOCKET 1
#define MQTT_ERROR_CONNECT_INVALID_STATE 2
#define MQTT_ERROR_CONNECT_TIMEOUT 3


/* MQTT Client States */
#define MQTT_CLIENT_STATE_DISCONNECTED 0
#define MQTT_CLIENT_STATE_INIT 1
#define MQTT_CLIENT_STATE_TCP_CONNECTING 2
#define MQTT_CLIENT_STATE_TCP_CONNECTED 3
#define MQTT_CLIENT_STATE_MQTT_CONNECTING 4
#define MQTT_CLIENT_STATE_MQTT_CONNECTED 5
#define MQTT_CLIENT_STATE_DISCONNECTING 6

/* MQTT Broker States */


/* MQTT Default Send Parameters */
      //MQTT_CONNECT_
#define DEFAULT_KEEPALIVE 60
#define DEFAULT_CLEANSESSION 1
#define DEFAULT_WILLTOPIC NULL
#define DEFAULT_WILLMSG NULL
#define DEFAULT_WILLQOS QOS_0
#define DEFAULT_WILLRETAIN 0

/* MQTT Versions */
#define MQTT_VERSION_v3_1_1 4

/* Constants */
#define MAX_CLIENT_ID_LENGTH 20
#define MAX_TOPICS 8
#define MAX_TOPIC_LENGTH 30
#define MAX_MQTT_DATA_SIZE 50
#define MAX_MQTT_PACKET_SIZE 256
#define MQTT_PORT 1883

//=============================================================================
// TYPEDEFS AND GLOBALS
//=============================================================================

typedef struct _mqttData {
    char topic[MAX_TOPIC_LENGTH];
    const void* data;
    uint16_t dataLen;
} mqttData;

typedef void (*mqtt_callback_t)(const mqttData* data); //function(mqttData context)

typedef struct _mqttOptions {
    uint8_t version;
    uint8_t qos;
    uint16_t keepAlive;
    uint8_t cleanSession;
    uint8_t willFlag;
    uint8_t willQos;
    uint8_t willRetain;
    char willTopic[MAX_TOPIC_LENGTH];
    char willMsg[MAX_MQTT_DATA_SIZE];
} mqttOptions;

typedef struct _mqttClient {
    char clientId[MAX_CLIENT_ID_LENGTH];
    socket* socket;
    uint8_t state;
    char mqttTopics[MAX_TOPICS][MAX_TOPIC_LENGTH];
    uint16_t topicCount;
    mqttOptions options;
    //callbacks
    mqtt_callback_t pubCallback;

    //timers
    uint8_t timeoutTimer;
    uint8_t keepAliveTimer;

    //connection retries
    uint8_t connectAttempts;
    bool reconnect;

    //TCP is a byte stream, packets are reassembled here
    uint8_t rxBuffer[MAX_MQTT_PACKET_SIZE];
    uint16_t rxSize;
    uint32_t rxDiscard; // bytes left of a packet too large for rxBuffer
} mqttClient;

typedef struct _mqttBroker {
    uint8_t numClients;
} mqttBroker;

typedef struct _mqttError {
    uint8_t errorCode;
    socket* sk;
    char errorMsg[SOCKET_ERROR_MAX_MSG_LEN];
} mqttError;

typedef struct _mqttHeader {// 20 or more bytes
    uint8_t flags; //4bits packet type - 4 bits flags
    uint8_t data[0];
} mqttHeader;


//=============================================================================
// FUNCTION PROTOTYPES
//=============================================================================

// Returns the number of length bytes, 0 if more data is needed, -1 if malformed
int8_t decodeLength(const uint8_t* data, uint16_t available, uint32_t* value);
// Send functions return false if the packet does not fit in the TCP send buffer
bool sendMqttConnect(mqttClient* client);
bool sendMqttPublish(mqttClient* client, char strTopic[], char strData[]);
bool sendMqttPubAck(mqttClient* client, uint16_t packetId);
bool sendMqttSubscribe(mqttClient* client, char strTopic[]);
bool sendMqttUnsubscribe(mqttClient* client, char strTopic[]);
bool sendMqttPingReq(mqttClient* client);
bool sendMqttDisconnect(mqttClient* client);

#endif

