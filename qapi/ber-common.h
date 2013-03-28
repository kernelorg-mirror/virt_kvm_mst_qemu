/*
 * BER Visitor -- common code
 *
 * Copyright IBM, Corp. 2013
 *
 * Authors:
 *  Stefan Berger     <stefanb@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.1 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */
#ifndef __QAPI_BER_COMMON_H__
#define __QAPI_BER_COMMON_H__

#include <stdint.h>

#include "qemu/compiler.h"

struct ieee754_buffer {
    uint8_t type;
    uint8_t length;
    uint8_t first;
    uint16_t exponent;
    uint32_t mant_hi;
    uint32_t mant_lo;
} QEMU_PACKED;

#endif /* __QAPI_BER_COMMON_H__ */
