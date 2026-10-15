/******************************************************************************
 * File:        http_server_example.h
 *
 * Author:      Giancarlo Perez
 *
 * Created:     10/8/26
 *
 * Description: Example web server built on http.c
 ******************************************************************************/

#ifndef HTTP_SERVER_EXAMPLE_H_
#define HTTP_SERVER_EXAMPLE_H_

#include <stdbool.h>

bool initHttpServerExample(void); // registers the routes and listens on port 80
void runHttpServerExample(void);  // call from the main loop

#endif
