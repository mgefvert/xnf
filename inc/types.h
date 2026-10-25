/*
 * Fixed-width unsigned types for the freestanding 32-bit kernel.
 * These definitions rely on the configured i686 GCC data model, without a C runtime.
 */

#ifndef XNF_TYPES_H
#define XNF_TYPES_H

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;

#endif
