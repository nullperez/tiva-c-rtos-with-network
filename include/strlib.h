/******************************************************************************
 * File:        strlib.h
 *
 * Author:      Giancarlo Perez
 *
 * Created:     12/2/24
 *
 * Description: -
 ******************************************************************************/

#ifndef _STRLIB_H
#define _STRLIB_H

//=============================================================================
// INCLUDES
//=============================================================================

#include <stdbool.h>
#include <stdint.h>

//=============================================================================
// DEFINES AND MACROS
//=============================================================================

//=============================================================================
// TYPEDEFS AND GLOBALS
//=============================================================================

//=============================================================================
// FUNCTION PROTOTYPES
//=============================================================================

char* str_char(const char* str, char c);
void str_copy(char dest[], const char* src);
bool str_equal(const char* str1, const char* str2);
uint32_t str_length(const char str[]);
char* str_tokenize(char* str, const char* delim);
void str_to_lower(char* str);
void str_to_upper(char* str);
char to_lower(char c);
char to_upper(char c);
void to_string(uint32_t n, char* out, uint32_t b);
uint32_t to_uint32(const char* str, uint32_t base);

#endif
