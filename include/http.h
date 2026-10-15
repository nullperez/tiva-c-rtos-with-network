/******************************************************************************
 * File:        http.h
 *
 * Author:      Giancarlo Perez
 *
 * Created:     10/8/26
 *
 * Description: Minimal HTTP/1.1 server (RFC 9110, RFC 9112)
 *
 *  - one request per connection, every response carries "Connection: close"
 *  - request bodies need Content-Length (chunked requests get 501)
 *  - only the Host, Content-Length, Content-Type and Transfer-Encoding headers
 *    are kept, other headers are read and ignored so long browser requests
 *    do not need a large buffer
 ******************************************************************************/

#ifndef HTTP_H_
#define HTTP_H_

//=============================================================================
// INCLUDES
//=============================================================================

#include "socket.h"
#include <stdint.h>
#include <stdbool.h>

//=============================================================================
// DEFINES AND MACROS
//=============================================================================

#define HTTP_PORT 80

/* Methods */
#define HTTP_METHOD_UNKNOWN 0
#define HTTP_METHOD_GET     1
#define HTTP_METHOD_HEAD    2
#define HTTP_METHOD_POST    3
#define HTTP_METHOD_PUT     4
#define HTTP_METHOD_DELETE  5

/* Status Codes */
#define HTTP_OK                     200
#define HTTP_NO_CONTENT             204
#define HTTP_SEE_OTHER              303
#define HTTP_BAD_REQUEST            400
#define HTTP_NOT_FOUND              404
#define HTTP_METHOD_NOT_ALLOWED     405
#define HTTP_REQUEST_TIMEOUT        408
#define HTTP_LENGTH_REQUIRED        411
#define HTTP_CONTENT_TOO_LARGE      413
#define HTTP_URI_TOO_LONG           414
#define HTTP_INTERNAL_ERROR         500
#define HTTP_NOT_IMPLEMENTED        501
#define HTTP_SERVICE_UNAVAILABLE    503
#define HTTP_VERSION_NOT_SUPPORTED  505

/* Config */
#define HTTP_MAX_CONNECTIONS 2      // needs MAX_SOCKETS >= this + 1 (listener)
#define HTTP_MAX_ROUTES 8
#define HTTP_MAX_LINE_LENGTH 200    // request line / header line
#define HTTP_MAX_TARGET_LENGTH 96   // path + query
#define HTTP_MAX_BODY_LENGTH 256    // request body
#define HTTP_MAX_CONTENT_TYPE 48
#define HTTP_RESPONSE_BUFFER_SIZE 640
#define HTTP_RECEIVE_TIMEOUT_MS 10000
#define HTTP_SEND_TIMEOUT_MS 30000

//=============================================================================
// TYPEDEFS AND GLOBALS
//=============================================================================

typedef struct _httpRequest {
    uint8_t method;
    const char* path;        // without the query, not percent-decoded
    const char* query;       // text after '?', "" if none
    const char* body;        // NUL terminated
    uint16_t bodyLength;
    const char* contentType; // "" if none
    uint8_t remoteIp[4];
} httpRequest;

typedef struct _httpResponse {
    uint16_t status;
    const char* contentType;
    const char* location;    // Location header for redirects, or NULL
    const uint8_t* body;     // may point to constant data, it is not copied
    uint32_t bodyLength;
    char buffer[HTTP_RESPONSE_BUFFER_SIZE]; // storage for generated bodies (httpPrintf)
    uint16_t bufferLength;
    bool overflow;           // httpPrintf ran out of buffer
} httpResponse;

typedef void (*http_handler_t)(const httpRequest* request, httpResponse* response);

//=============================================================================
// FUNCTION PROTOTYPES
//=============================================================================

// Server
bool initHttpServer(uint16_t port);
void stopHttpServer(void);
bool addHttpRoute(uint8_t method, const char* path, http_handler_t handler); // HEAD is served by GET routes
void runHttpServer(void); // call from the main loop

// Helpers for handlers, a response defaults to 200 text/html with an empty body
void httpSetBody(httpResponse* response, uint16_t status, const char* contentType, const char* body); // body is not copied
bool httpPrintf(httpResponse* response, const char* format, ...); // appends to the response buffer
void httpRedirect(httpResponse* response, uint16_t status, const char* location);
bool httpGetParam(const char* params, const char* name, char* value, uint16_t valueSize); // query string or form body
const char* httpGetStatusText(uint16_t status);
const char* httpGetMethodName(uint8_t method);

#endif
