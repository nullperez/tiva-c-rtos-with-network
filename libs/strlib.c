/******************************************************************************
 * File:        strlib.c
 *
 * Author:      Giancarlo Perez
 *
 * Created:     12/2/24
 *
 * Description: -
 ******************************************************************************/

//=============================================================================
// INCLUDES
//=============================================================================

#include "strlib.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/*uint8_t asciiToUint8(const char str[]) {
    uint8_t data;
    if (str[0] == '0' && tolower(str[1]) == 'x')
        sscanf(str, "%hhx", &data);
    else
        sscanf(str, "%hhu", &data);
    return data;
}*/

//=============================================================================
// PUBLIC FUNCTIONS
//=============================================================================

char* str_char(const char* str, char c) {
    while (*str != 0) {
        if (*str == c) {
            return (char*)str;
        }
        str++;
    }
    return NULL;
}

//src MUST be null terminated
void str_copy(char dest[], const char* src) {
    uint32_t i;
    for (i = 0; src[i] != 0; i++) {
        dest[i] = src[i];
    }
    dest[i] = 0;
}


bool str_equal(const char* str1, const char* str2) {
    int i;
    if (str_length(str1) != str_length(str2)) {
        return false;
    }
    for (i = 0; str1[i] != 0; i++) {
        if (str1[i] != str2[i]) {
            return false;
        }
    }
    return true;
}

uint32_t str_length(const char str[]) {
    int c;
    for (c = 0; str[c] != 0; c++);
    return c;
}

char* str_tokenize(char* str, const char* delim) {
    static char *last = NULL;
    char* start;
    if (str == NULL) {
        str = last;
    }
    if (str == NULL) {
        return NULL;
    }
    while (*str && str_char(delim, *str)) {
        str++;
    }
    if (*str == '\0') {
        return NULL;
    }
    start = str;
    while (*str && !str_char(delim, *str)) {
        str++;
    }
    if (*str) {
        *str = '\0';
        last = str + 1;
    }
    else {
        last = NULL;
    }
    return start;
}

void str_to_lower(char* str) {

}

void str_to_upper(char* str) {

}

char to_lower(char c) {

    return c;
}

char to_upper(char c) {
    return c;
}

void to_string(uint32_t n, char* out, uint32_t b) {
    char num[10];
    uint32_t i = 0;
    uint64_t j = 1;
    uint32_t c;
    if (n == 0) {
        out[0] = '0';
        out[1] = 0;
        return;
    }
    for (c = n; c > 0; c /= b) {
        uint64_t r = ((n % (j*b))/j);
        char digit = (r < 10) ? (48 + r) : (65 + r - 10);
        num[i] = digit;
        j *= b;
        i++;
    }
    for (c = 0; c < i; c++) {
        out[c] = num[i-c-1];
    }
    out[c] = 0;
}

uint32_t to_uint32(const char* str, uint32_t base) {
    int32_t i;
    uint32_t n = 0;
    uint32_t p = 1;
    uint32_t len = str_length(str);
    char digit;
    for (i = len-1; i >= 0; i--) {
        digit = (str[i] >= '0' && str[i] <= '9') ? str[i]-48 : str[i]-55;
        n += (digit)*p;
        p*=base;
    }
    return n;
}

