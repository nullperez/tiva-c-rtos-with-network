/******************************************************************************
 * File:        http.c
 *
 * Author:      Giancarlo Perez
 *
 * Created:     10/8/26
 *
 * Description: Minimal HTTP/1.1 server (RFC 9110, RFC 9112), see http.h
 ******************************************************************************/

//=============================================================================
// INCLUDES
//=============================================================================

#include "log.h"
#include "http.h"
#include "tcp.h"
#include "clock.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

//=============================================================================
// DEFINES AND MACROS
//=============================================================================

#define HTTP_HEADER_BUFFER_SIZE 192
#define HTTP_ALLOW_BUFFER_SIZE 32

/* Connection States */
#define HTTP_CONN_FREE 0
#define HTTP_CONN_REQUEST_LINE 1
#define HTTP_CONN_HEADERS 2
#define HTTP_CONN_BODY 3
#define HTTP_CONN_SENDING 4
#define HTTP_CONN_CLOSING 5

//=============================================================================
// TYPEDEFS AND GLOBALS
//=============================================================================

typedef struct _httpRoute {
    uint8_t method;
    const char* path;
    http_handler_t handler;
} httpRoute;

typedef struct _httpConnection {
    socket* sock;
    uint8_t state;
    uint32_t lastActivity;
    // request parser
    char line[HTTP_MAX_LINE_LENGTH + 1];
    uint16_t lineLength;
    bool lineTooLong;
    uint8_t method;
    char target[HTTP_MAX_TARGET_LENGTH + 1];
    char contentType[HTTP_MAX_CONTENT_TYPE + 1];
    char body[HTTP_MAX_BODY_LENGTH + 1];
    uint16_t bodyLength;
    uint32_t contentLength;
    bool hasContentLength;
    bool hasHost;
    bool http11;
    bool hasTransferEncoding;
    // response
    char header[HTTP_HEADER_BUFFER_SIZE];
    char allow[HTTP_ALLOW_BUFFER_SIZE];
    uint16_t headerLength;
    uint16_t headerSent;
    uint32_t bodySent;
    bool headOnly;
    httpResponse response;
} httpConnection;

static httpConnection connections[HTTP_MAX_CONNECTIONS];
static httpRoute routes[HTTP_MAX_ROUTES];
static uint8_t routeCount = 0;
static socket* listener = NULL;

//=============================================================================
// STATIC FUNCTIONS
//=============================================================================

static void httpStartResponse(httpConnection* c);

static char lowerCase(char c) {
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static bool equalsIgnoreCase(const char* a, const char* b) {
    while (*a && *b) {
        if (lowerCase(*a) != lowerCase(*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static bool isTokenChar(char c) {
    // RFC 9110 5.6.2 tchar
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c != 0 && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static int8_t hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static uint8_t parseMethod(const char* str) {
    if (strcmp(str, "GET") == 0) return HTTP_METHOD_GET;
    if (strcmp(str, "HEAD") == 0) return HTTP_METHOD_HEAD;
    if (strcmp(str, "POST") == 0) return HTTP_METHOD_POST;
    if (strcmp(str, "PUT") == 0) return HTTP_METHOD_PUT;
    if (strcmp(str, "DELETE") == 0) return HTTP_METHOD_DELETE;
    return HTTP_METHOD_UNKNOWN;
}

static httpConnection* findConnection(socket* s) {
    uint8_t i;
    for (i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
        if (connections[i].state != HTTP_CONN_FREE && connections[i].sock == s) {
            return &connections[i];
        }
    }
    return NULL;
}

static httpConnection* allocConnection(socket* s) {
    uint8_t i;
    for (i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
        httpConnection* c = &connections[i];
        if (c->state == HTTP_CONN_FREE) {
            memset(c, 0, sizeof(httpConnection));
            c->sock = s;
            c->state = HTTP_CONN_REQUEST_LINE;
            c->lastActivity = millis();
            return c;
        }
    }
    return NULL;
}

static void httpResetResponse(httpResponse* response) {
    response->status = HTTP_OK;
    response->contentType = "text/html; charset=utf-8";
    response->location = NULL;
    response->body = NULL;
    response->bodyLength = 0;
    response->bufferLength = 0;
    response->buffer[0] = '\0';
    response->overflow = false;
}

// Prepares a plain text error response
static void httpSetError(httpConnection* c, uint16_t status) {
    httpResponse* res = &c->response;
    httpResetResponse(res);
    res->status = status;
    res->contentType = "text/plain; charset=utf-8";
    httpPrintf(res, "%u %s\n", status, httpGetStatusText(status));
    c->headOnly = (c->method == HTTP_METHOD_HEAD);
}

// Answers with an error status and closes the connection
static void httpFail(httpConnection* c, uint16_t status) {
    httpSetError(c, status);
    c->allow[0] = '\0';
    httpStartResponse(c);
}

static void httpDispatch(httpConnection* c) {
    httpRequest request;
    httpResponse* res = &c->response;
    http_handler_t handler = NULL;
    uint8_t routeMethod;
    bool pathFound = false;
    bool allowsGet = false;
    char* query;
    uint8_t i;

    // split the target into path and query
    query = strchr(c->target, '?');
    if (query != NULL) {
        *query++ = '\0';
    }
    else {
        query = c->target + strlen(c->target);
    }
    request.method = c->method;
    request.path = c->target;
    request.query = query;
    request.body = c->body;
    request.bodyLength = c->bodyLength;
    request.contentType = c->contentType;
    copyIpAddress(request.remoteIp, c->sock->remoteIpAddress);

    // HEAD is answered like GET without the body (RFC 9110 9.3.2)
    c->headOnly = (c->method == HTTP_METHOD_HEAD);
    routeMethod = c->headOnly ? HTTP_METHOD_GET : c->method;
    c->allow[0] = '\0';
    for (i = 0; i < routeCount; i++) {
        if (strcmp(routes[i].path, request.path) == 0) {
            pathFound = true;
            if (routes[i].method == routeMethod) {
                handler = routes[i].handler;
            }
            if (strlen(c->allow) + 10 < HTTP_ALLOW_BUFFER_SIZE) {
                if (c->allow[0] != '\0') {
                    strcat(c->allow, ", ");
                }
                strcat(c->allow, httpGetMethodName(routes[i].method));
            }
            allowsGet = allowsGet || (routes[i].method == HTTP_METHOD_GET);
        }
    }
    if (allowsGet && strlen(c->allow) + 6 < HTTP_ALLOW_BUFFER_SIZE) {
        strcat(c->allow, ", HEAD");
    }

    if (logEnabled(LOG_HTTP)) {
        static const char* const methods[] = { "?", "GET", "HEAD", "POST", "PUT", "DELETE" };
        logPrintf(LOG_HTTP, "%u.%u.%u.%u %s %s%s", request.remoteIp[0], request.remoteIp[1], request.remoteIp[2],
                  request.remoteIp[3], c->method < 6 ? methods[c->method] : "?", c->target,
                  handler != NULL ? "" : (pathFound ? " -> 405" : " -> 404"));
    }
    if (handler != NULL) {
        httpResetResponse(res);
        handler(&request, res);
        if (res->overflow) {
            httpFail(c, HTTP_INTERNAL_ERROR);
            return;
        }
        c->allow[0] = '\0';
        httpStartResponse(c);
    }
    else if (pathFound) {
        // 405 must list the allowed methods (RFC 9110 15.5.6), c->allow holds them
        httpSetError(c, HTTP_METHOD_NOT_ALLOWED);
        httpStartResponse(c);
    }
    else {
        httpFail(c, HTTP_NOT_FOUND);
    }
}

static void httpSendPending(httpConnection* c) {
    httpResponse* res = &c->response;
    uint16_t n;
    if (c->state != HTTP_CONN_SENDING) {
        return;
    }
    while (c->headerSent < c->headerLength) {
        n = socketSendTcp(c->sock, (const uint8_t*)c->header + c->headerSent, c->headerLength - c->headerSent);
        if (n == 0) {
            return; // send buffer full, continue from runHttpServer
        }
        c->headerSent += n;
        c->lastActivity = millis();
    }
    if (!c->headOnly) {
        while (c->bodySent < res->bodyLength) {
            uint32_t remaining = res->bodyLength - c->bodySent;
            n = socketSendTcp(c->sock, res->body + c->bodySent, (remaining > 0xFFFF) ? 0xFFFF : remaining);
            if (n == 0) {
                return;
            }
            c->bodySent += n;
            c->lastActivity = millis();
        }
    }
    // everything is queued, the FIN follows the data
    c->state = HTTP_CONN_CLOSING;
    socketCloseTcp(c->sock);
}

// Appends formatted text to the header buffer, n is the current length
// Returns the new length, which is >= HTTP_HEADER_BUFFER_SIZE once it overflows
static int appendHeader(httpConnection* c, int n, const char* format, ...) {
    va_list args;
    int written;
    if (n < 0 || n >= HTTP_HEADER_BUFFER_SIZE) {
        return HTTP_HEADER_BUFFER_SIZE;
    }
    va_start(args, format);
    written = vsnprintf(c->header + n, HTTP_HEADER_BUFFER_SIZE - n, format, args);
    va_end(args);
    return (written < 0) ? HTTP_HEADER_BUFFER_SIZE : n + written;
}

static void httpStartResponse(httpConnection* c) {
    httpResponse* res = &c->response;
    int n = 0;
    bool noBody = (res->status == HTTP_NO_CONTENT) || (res->status < 200);
    n = appendHeader(c, n, "HTTP/1.1 %u %s\r\n", res->status, httpGetStatusText(res->status));
    if (!noBody) {
        if (res->bodyLength > 0 && res->contentType != NULL) {
            n = appendHeader(c, n, "Content-Type: %s\r\n", res->contentType);
        }
        n = appendHeader(c, n, "Content-Length: %lu\r\n", (unsigned long)res->bodyLength);
    }
    else {
        res->bodyLength = 0; // 204 has no content and no Content-Length (RFC 9110 8.6)
    }
    if (res->location != NULL) {
        n = appendHeader(c, n, "Location: %s\r\n", res->location);
    }
    if (c->allow[0] != '\0') {
        n = appendHeader(c, n, "Allow: %s\r\n", c->allow);
    }
    // no Date header: the device has no clock (RFC 9110 6.6.1)
    n = appendHeader(c, n, "Connection: close\r\nCache-Control: no-store\r\n\r\n");
    if (n >= HTTP_HEADER_BUFFER_SIZE) {
        // headers did not fit, send a bare error instead
        static const char fallback[] = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        strcpy(c->header, fallback);
        n = sizeof(fallback) - 1;
        res->bodyLength = 0;
    }
    c->headerLength = n;
    c->headerSent = 0;
    c->bodySent = 0;
    c->state = HTTP_CONN_SENDING;
    c->lastActivity = millis();
    httpSendPending(c);
}

static void httpParseRequestLine(httpConnection* c) {
    char* method = c->line;
    char* target;
    char* version;
    // RFC 9112 2.2: ignore empty lines before the request line
    if (c->lineLength == 0 && !c->lineTooLong) {
        return;
    }
    if (c->lineTooLong) {
        httpFail(c, HTTP_URI_TOO_LONG);
        return;
    }
    target = strchr(method, ' ');
    if (target == NULL) {
        httpFail(c, HTTP_BAD_REQUEST);
        return;
    }
    *target++ = '\0';
    version = strchr(target, ' ');
    if (version == NULL) {
        httpFail(c, HTTP_BAD_REQUEST);
        return;
    }
    *version++ = '\0';
    c->method = parseMethod(method);
    if (strncmp(version, "HTTP/", 5) != 0) {
        httpFail(c, HTTP_BAD_REQUEST);
        return;
    }
    if (strcmp(version, "HTTP/1.1") == 0) {
        c->http11 = true;
    }
    else if (strcmp(version, "HTTP/1.0") != 0) {
        httpFail(c, HTTP_VERSION_NOT_SUPPORTED);
        return;
    }
    if (c->method == HTTP_METHOD_UNKNOWN) {
        httpFail(c, HTTP_NOT_IMPLEMENTED);
        return;
    }
    if (target[0] != '/') {
        httpFail(c, HTTP_BAD_REQUEST); // only origin-form is supported
        return;
    }
    if (strlen(target) > HTTP_MAX_TARGET_LENGTH) {
        httpFail(c, HTTP_URI_TOO_LONG);
        return;
    }
    strcpy(c->target, target);
    c->state = HTTP_CONN_HEADERS;
}

static void httpHeadersComplete(httpConnection* c) {
    if (c->http11 && !c->hasHost) {
        httpFail(c, HTTP_BAD_REQUEST); // RFC 9112 3.2
        return;
    }
    if (c->hasTransferEncoding) {
        httpFail(c, HTTP_NOT_IMPLEMENTED); // chunked request bodies are not supported
        return;
    }
    if (!c->hasContentLength && (c->method == HTTP_METHOD_POST || c->method == HTTP_METHOD_PUT)) {
        c->contentLength = 0; // no body
    }
    if (c->contentLength > HTTP_MAX_BODY_LENGTH) {
        httpFail(c, HTTP_CONTENT_TOO_LARGE);
        return;
    }
    if (c->contentLength > 0) {
        c->state = HTTP_CONN_BODY;
    }
    else {
        httpDispatch(c);
    }
}

static void httpParseHeader(httpConnection* c) {
    char* name = c->line;
    char* value;
    char* end;
    if (c->lineTooLong) {
        return; // too long to keep, the headers this server uses are short
    }
    if (c->lineLength == 0) {
        httpHeadersComplete(c);
        return;
    }
    value = strchr(name, ':');
    if (value == NULL || value == name) {
        httpFail(c, HTTP_BAD_REQUEST);
        return;
    }
    *value++ = '\0';
    // field name is a token, whitespace before the colon is not allowed (RFC 9112 5.1)
    for (end = name; *end; end++) {
        if (!isTokenChar(*end)) {
            httpFail(c, HTTP_BAD_REQUEST);
            return;
        }
    }
    // trim optional whitespace around the value
    while (*value == ' ' || *value == '\t') {
        value++;
    }
    end = value + strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t')) {
        *--end = '\0';
    }
    if (equalsIgnoreCase(name, "Host")) {
        c->hasHost = true;
    }
    else if (equalsIgnoreCase(name, "Content-Length")) {
        uint32_t length = 0;
        char* p = value;
        if (*p == '\0') {
            httpFail(c, HTTP_BAD_REQUEST);
            return;
        }
        for (; *p; p++) {
            if (*p < '0' || *p > '9' || length > 100000) {
                httpFail(c, HTTP_BAD_REQUEST);
                return;
            }
            length = length * 10 + (*p - '0');
        }
        if (c->hasContentLength && length != c->contentLength) {
            httpFail(c, HTTP_BAD_REQUEST); // conflicting lengths (RFC 9112 6.3)
            return;
        }
        c->hasContentLength = true;
        c->contentLength = length;
    }
    else if (equalsIgnoreCase(name, "Content-Type")) {
        strncpy(c->contentType, value, HTTP_MAX_CONTENT_TYPE);
        c->contentType[HTTP_MAX_CONTENT_TYPE] = '\0';
    }
    else if (equalsIgnoreCase(name, "Transfer-Encoding")) {
        c->hasTransferEncoding = true;
    }
}

static void httpReceive(httpConnection* c, const uint8_t* data, uint16_t length) {
    uint16_t i = 0;
    c->lastActivity = millis();
    while (i < length && c->state >= HTTP_CONN_REQUEST_LINE && c->state <= HTTP_CONN_BODY) {
        if (c->state == HTTP_CONN_BODY) {
            uint32_t needed = c->contentLength - c->bodyLength;
            uint32_t available = (uint32_t)(length - i);
            uint16_t n = (available < needed) ? available : needed;
            memcpy(c->body + c->bodyLength, data + i, n);
            c->bodyLength += n;
            i += n;
            if (c->bodyLength == c->contentLength) {
                c->body[c->bodyLength] = '\0';
                httpDispatch(c);
            }
            continue;
        }
        char ch = (char)data[i++];
        if (ch == '\n') {
            if (c->lineLength > 0 && c->line[c->lineLength - 1] == '\r') {
                c->lineLength--;
            }
            c->line[c->lineLength] = '\0';
            if (c->state == HTTP_CONN_REQUEST_LINE) {
                httpParseRequestLine(c);
            }
            else {
                httpParseHeader(c);
            }
            c->lineLength = 0;
            c->lineTooLong = false;
        }
        else if (c->lineLength < HTTP_MAX_LINE_LENGTH) {
            c->line[c->lineLength++] = ch;
        }
        else {
            c->lineTooLong = true;
        }
    }
    // extra bytes (pipelined requests) are ignored, the connection closes after one response
}

static void httpRecvCallback(socket* s, const uint8_t* data, uint16_t length) {
    httpConnection* c = findConnection(s);
    if (c != NULL) {
        httpReceive(c, data, length);
    }
}

static void httpEventCallback(socket* s, uint8_t event) {
    httpConnection* c = findConnection(s);
    switch (event) {
    case SOCKET_EVENT_ACCEPTED:
        if (allocConnection(s) == NULL) {
            socketAbortTcp(s); // all connection slots busy
        }
        break;
    case SOCKET_EVENT_PEER_CLOSED:
        // client finished sending, if the request is incomplete it never will be
        if (c != NULL && c->state >= HTTP_CONN_REQUEST_LINE && c->state <= HTTP_CONN_BODY) {
            c->state = HTTP_CONN_CLOSING;
            socketCloseTcp(s);
        }
        break;
    case SOCKET_EVENT_CLOSED:
        if (s == listener) {
            listener = NULL;
        }
        if (c != NULL) {
            c->state = HTTP_CONN_FREE;
            c->sock = NULL;
        }
        break;
    }
}

//=============================================================================
// PUBLIC FUNCTIONS
//=============================================================================

bool initHttpServer(uint16_t port) {
    uint8_t i;
    if (listener != NULL) {
        return false;
    }
    for (i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
        connections[i].state = HTTP_CONN_FREE;
        connections[i].sock = NULL;
    }
    listener = newSocket(SOCKET_STREAM);
    if (listener == NULL) {
        return false;
    }
    // accepted sockets inherit these callbacks
    listener->recvCallback = httpRecvCallback;
    listener->eventCallback = httpEventCallback;
    listener->errorCallback = NULL;
    if (!socketListenTcp(listener, port)) {
        deleteSocket(listener);
        listener = NULL;
        return false;
    }
    return true;
}

void stopHttpServer(void) {
    uint8_t i;
    if (listener != NULL) {
        socketCloseTcp(listener); // CLOSED event clears listener
        listener = NULL;
    }
    for (i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
        if (connections[i].state != HTTP_CONN_FREE) {
            socketAbortTcp(connections[i].sock);
        }
    }
}

bool addHttpRoute(uint8_t method, const char* path, http_handler_t handler) {
    if (routeCount >= HTTP_MAX_ROUTES || handler == NULL || path == NULL || method == HTTP_METHOD_HEAD) {
        return false;
    }
    routes[routeCount].method = method;
    routes[routeCount].path = path;
    routes[routeCount].handler = handler;
    routeCount++;
    return true;
}

void runHttpServer(void) {
    uint8_t i;
    uint32_t now = millis();
    for (i = 0; i < HTTP_MAX_CONNECTIONS; i++) {
        httpConnection* c = &connections[i];
        switch (c->state) {
        case HTTP_CONN_REQUEST_LINE:
        case HTTP_CONN_HEADERS:
        case HTTP_CONN_BODY:
            if (now - c->lastActivity > HTTP_RECEIVE_TIMEOUT_MS) {
                httpFail(c, HTTP_REQUEST_TIMEOUT);
            }
            break;
        case HTTP_CONN_SENDING:
            httpSendPending(c);
            if (c->state == HTTP_CONN_SENDING && now - c->lastActivity > HTTP_SEND_TIMEOUT_MS) {
                socketAbortTcp(c->sock);
            }
            break;
        default:
            break;
        }
    }
}

void httpSetBody(httpResponse* response, uint16_t status, const char* contentType, const char* body) {
    response->status = status;
    response->contentType = contentType;
    response->body = (const uint8_t*)body;
    response->bodyLength = (body != NULL) ? strlen(body) : 0;
}

bool httpPrintf(httpResponse* response, const char* format, ...) {
    va_list args;
    int n;
    uint16_t space = HTTP_RESPONSE_BUFFER_SIZE - response->bufferLength;
    va_start(args, format);
    n = vsnprintf(response->buffer + response->bufferLength, space, format, args);
    va_end(args);
    if (n < 0 || n >= space) {
        response->overflow = true;
        response->buffer[response->bufferLength] = '\0';
        return false;
    }
    response->bufferLength += n;
    response->body = (const uint8_t*)response->buffer;
    response->bodyLength = response->bufferLength;
    return true;
}

void httpRedirect(httpResponse* response, uint16_t status, const char* location) {
    response->status = status;
    response->location = location;
    response->body = NULL;
    response->bodyLength = 0;
}

// Finds name in "a=1&b=2" and copies its percent-decoded value
bool httpGetParam(const char* params, const char* name, char* value, uint16_t valueSize) {
    uint16_t nameLength = strlen(name);
    const char* p = params;
    while (p != NULL && *p != '\0') {
        const char* end = strchr(p, '&');
        const char* pairEnd = (end != NULL) ? end : p + strlen(p);
        if ((uint16_t)(pairEnd - p) > nameLength && strncmp(p, name, nameLength) == 0 && p[nameLength] == '=') {
            const char* v = p + nameLength + 1;
            uint16_t i = 0;
            while (v < pairEnd && i + 1 < valueSize) {
                if (*v == '+') {
                    value[i++] = ' ';
                    v++;
                }
                else if (*v == '%' && v + 2 < pairEnd && hexValue(v[1]) >= 0 && hexValue(v[2]) >= 0) {
                    value[i++] = (char)((hexValue(v[1]) << 4) | hexValue(v[2]));
                    v += 3;
                }
                else {
                    value[i++] = *v++;
                }
            }
            value[i] = '\0';
            return true;
        }
        p = (end != NULL) ? end + 1 : NULL;
    }
    return false;
}

const char* httpGetStatusText(uint16_t status) {
    switch (status) {
    case HTTP_OK:                    return "OK";
    case HTTP_NO_CONTENT:            return "No Content";
    case HTTP_SEE_OTHER:             return "See Other";
    case HTTP_BAD_REQUEST:           return "Bad Request";
    case HTTP_NOT_FOUND:             return "Not Found";
    case HTTP_METHOD_NOT_ALLOWED:    return "Method Not Allowed";
    case HTTP_REQUEST_TIMEOUT:       return "Request Timeout";
    case HTTP_LENGTH_REQUIRED:       return "Length Required";
    case HTTP_CONTENT_TOO_LARGE:     return "Content Too Large";
    case HTTP_URI_TOO_LONG:          return "URI Too Long";
    case HTTP_INTERNAL_ERROR:        return "Internal Server Error";
    case HTTP_NOT_IMPLEMENTED:       return "Not Implemented";
    case HTTP_SERVICE_UNAVAILABLE:   return "Service Unavailable";
    case HTTP_VERSION_NOT_SUPPORTED: return "HTTP Version Not Supported";
    default:                         return "Unknown";
    }
}

const char* httpGetMethodName(uint8_t method) {
    switch (method) {
    case HTTP_METHOD_GET:    return "GET";
    case HTTP_METHOD_HEAD:   return "HEAD";
    case HTTP_METHOD_POST:   return "POST";
    case HTTP_METHOD_PUT:    return "PUT";
    case HTTP_METHOD_DELETE: return "DELETE";
    default:                 return "UNKNOWN";
    }
}
