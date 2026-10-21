#ifndef _AARCH64_STDINT_H
#define _AARCH64_STDINT_H

typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long               int64_t;

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long      uint64_t;

typedef unsigned long      uintptr_t;
typedef long               intptr_t;

#define UINT8_MAX   0xFF
#define UINT16_MAX  0xFFFF
#define UINT32_MAX  0xFFFFFFFFu
#define UINT64_MAX  0xFFFFFFFFFFFFFFFFul
#define INT64_MAX   9223372036854775807l
#define INT64_MIN   (-INT64_MAX - 1)

#endif
