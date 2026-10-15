/******************************************************************************
 * File:        http_server_example.c
 *
 * Author:      Giancarlo Perez
 *
 * Created:     10/8/26
 *
 * Description: Example web server built on http.c
 *
 *  GET  /            status page with buttons to toggle the LEDs
 *  POST /led         form body "led=red|green|blue", toggles it and redirects to /
 *  GET  /api/status  device status as JSON
 *  GET  /about       larger constant page, sent straight from flash in several segments
 *
 *  Try it:
 *    curl -i http://<ip>/api/status
 *    curl -i -d "led=green" http://<ip>/led
 *    curl -I http://<ip>/            (HEAD)
 ******************************************************************************/

//=============================================================================
// INCLUDES
//=============================================================================

#include "http_server_example.h"
#include "http.h"
#include "network_stack.h"
#include "mqtt_client.h"
#include "clock.h"
#include "gpio.h"
#include "ip.h"
#include "eth0.h"
#include "uart0.h"
#include <string.h>

//=============================================================================
// GLOBALS
//=============================================================================

static const char ABOUT_HTML[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width\"><title>About</title></head><body>"
    "<h1>About this device</h1>"
    "<p>This page is a constant string stored in flash. It is larger than the socket's TCP send "
    "buffer, so the server queues it in pieces as the browser acknowledges data.</p>"
    "<h2>Stack</h2><ul>"
    "<li>ENC28J60 Ethernet driver on SPI0</li>"
    "<li>ARP with a request queue and cache</li>"
    "<li>IPv4 (no fragmentation)</li>"
    "<li>ICMP echo</li>"
    "<li>UDP</li>"
    "<li>DHCP client (RFC 2131)</li>"
    "<li>TCP (RFC 9293) with RFC 6298 retransmission timing, RFC 5961 challenge ACKs and RFC 6528 sequence numbers</li>"
    "<li>MQTT 3.1.1 client</li>"
    "<li>HTTP/1.1 server (RFC 9110/9112), one request per connection</li>"
    "</ul>"
    "<h2>Limits</h2><ul>"
    "<li>Out of order TCP segments are dropped and re-requested instead of being queued</li>"
    "<li>No congestion control, window scaling or SACK</li>"
    "<li>Request bodies up to 256 bytes, no chunked uploads</li>"
    "</ul>"
    "<p>Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore "
    "et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut "
    "aliquip ex ea commodo consequat. Duis aute irure dolor in reprehenderit in voluptate velit esse cillum "
    "dolore eu fugiat nulla pariatur. Excepteur sint occaecat cupidatat non proident, sunt in culpa qui "
    "officia deserunt mollit anim id est laborum.</p>"
    "<p>Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore "
    "et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut "
    "aliquip ex ea commodo consequat. Duis aute irure dolor in reprehenderit in voluptate velit esse cillum "
    "dolore eu fugiat nulla pariatur. Excepteur sint occaecat cupidatat non proident, sunt in culpa qui "
    "officia deserunt mollit anim id est laborum.</p>"
    "<p><a href=\"/\">Back</a></p></body></html>\n";

//=============================================================================
// STATIC FUNCTIONS
//=============================================================================

static const char* getMqttStateName() {
    switch (getMqttState()) {
    case MQTT_CLIENT_STATE_DISCONNECTED: return "disconnected";
    case MQTT_CLIENT_STATE_MQTT_CONNECTED: return "connected";
    case MQTT_CLIENT_STATE_DISCONNECTING: return "disconnecting";
    default: return "connecting";
    }
}

static const char* onOff(bool value) {
    return value ? "on" : "off";
}

// GET /
static void handleIndex(const httpRequest* request, httpResponse* response) {
    uint8_t ip[4];
    uint8_t mac[6];
    getIpAddress(ip);
    getEtherMacAddress(mac);
    httpPrintf(response,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width\"><title>TM4C123</title></head><body>"
        "<h1>TM4C123 Network Stack</h1><p>IP: %d.%d.%d.%d<br>MAC: %02X:%02X:%02X:%02X:%02X:%02X<br>"
        "Uptime: %lu s<br>MQTT: %s<br>Your IP: %d.%d.%d.%d</p>",
        ip[0], ip[1], ip[2], ip[3], mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        (unsigned long)(millis() / 1000), getMqttStateName(),
        request->remoteIp[0], request->remoteIp[1], request->remoteIp[2], request->remoteIp[3]);
    httpPrintf(response,
        "<form method=\"post\" action=\"/led\">"
        "<button name=\"led\" value=\"red\">Red: %s</button> "
        "<button name=\"led\" value=\"green\">Green: %s</button> "
        "<button name=\"led\" value=\"blue\">Blue: %s</button></form>"
        "<p><a href=\"/api/status\">JSON</a> | <a href=\"/about\">About</a></p></body></html>\n",
        onOff(getPinValue(RED_LED)), onOff(getPinValue(GREEN_LED)), onOff(getPinValue(BLUE_LED)));
}

// POST /led
static void handleLed(const httpRequest* request, httpResponse* response) {
    char led[8];
    if (!httpGetParam(request->body, "led", led, sizeof(led))) {
        httpSetBody(response, HTTP_BAD_REQUEST, "text/plain; charset=utf-8", "missing led parameter\n");
        return;
    }
    if (strcmp(led, "red") == 0) {
        togglePinValue(RED_LED);
    }
    else if (strcmp(led, "green") == 0) {
        togglePinValue(GREEN_LED);
    }
    else if (strcmp(led, "blue") == 0) {
        togglePinValue(BLUE_LED);
    }
    else {
        httpSetBody(response, HTTP_BAD_REQUEST, "text/plain; charset=utf-8", "led must be red, green or blue\n");
        return;
    }
    // Post/Redirect/Get so refreshing the page does not toggle again
    httpRedirect(response, HTTP_SEE_OTHER, "/");
}

// GET /api/status
static void handleStatus(const httpRequest* request, httpResponse* response) {
    uint8_t ip[4];
    getIpAddress(ip);
    response->contentType = "application/json";
    httpPrintf(response,
        "{\"ip\":\"%d.%d.%d.%d\",\"uptime_s\":%lu,\"mqtt\":\"%s\","
        "\"leds\":{\"red\":%s,\"green\":%s,\"blue\":%s}}\n",
        ip[0], ip[1], ip[2], ip[3], (unsigned long)(millis() / 1000), getMqttStateName(),
        getPinValue(RED_LED) ? "true" : "false",
        getPinValue(GREEN_LED) ? "true" : "false",
        getPinValue(BLUE_LED) ? "true" : "false");
}

// GET /about
static void handleAbout(const httpRequest* request, httpResponse* response) {
    httpSetBody(response, HTTP_OK, "text/html; charset=utf-8", ABOUT_HTML);
}

//=============================================================================
// PUBLIC FUNCTIONS
//=============================================================================

bool initHttpServerExample(void) {
    addHttpRoute(HTTP_METHOD_GET, "/", handleIndex);
    addHttpRoute(HTTP_METHOD_POST, "/led", handleLed);
    addHttpRoute(HTTP_METHOD_GET, "/api/status", handleStatus);
    addHttpRoute(HTTP_METHOD_GET, "/about", handleAbout);
    if (!initHttpServer(HTTP_PORT)) {
        putsUart0("HTTP: Could not start server\n");
        return false;
    }
    putsUart0("HTTP: Server listening on port 80\n");
    return true;
}

void runHttpServerExample(void) {
    runHttpServer();
}
