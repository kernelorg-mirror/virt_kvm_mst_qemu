/*
 * ASN.1 Basic Encoding Rules Common functions
 *
 * Copyright IBM, Corp. 2011, 2013
 * Copyright Red Hat, Inc. 2011
 *
 * Authors:
 *  Stefan Berger     <stefanb@us.ibm.com>
 *  Michael Tsirkin   <mst@redhat.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.1 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */
#ifndef QAPI_BER_H
#define QAPI_BER_H

/*
 * This is a subset of BER for QEMU use.
 * QEMU will use the DER encoding always with one extension from
 * CER: SET and SEQUENCE types can have indefinite-length encoding
 * if the encoding is not all immediately available.
 *
 * We assume that SET encodings can be available or not available,
 * and that SEQUENCE encodings are available unless a SEQUENCE includes
 * a non-available SET.
 *
 * The last is an extension to allow an arbitrarily large SET
 * to be produced online without knowing the length in advance.
 *
 * All types used shall be universal, with explicit tagging, to simplify
 * use by external tools.
 */


#define BER_TYPE_CLASS_SHIFT  6
#define BER_TYPE_PC_SHIFT     5

typedef enum ber_type_class {
    BER_TYPE_CLASS_UNIVERSAL = 0x0 << BER_TYPE_CLASS_SHIFT,
    BER_TYPE_CLASS_APPLICATION = 0x1 << BER_TYPE_CLASS_SHIFT,
    BER_TYPE_CLASS_CONTENT_SPECIFIC = 0x2 << BER_TYPE_CLASS_SHIFT,
    BER_TYPE_CLASS_PRIVATE = 0x3 << BER_TYPE_CLASS_SHIFT,
    BER_TYPE_CLASS_MASK = 0x3 << BER_TYPE_CLASS_SHIFT /* Mask to get class */
} BERTypeClass;

/* P/C bit */
typedef enum ber_type_p_c {
    BER_TYPE_PRIMITIVE = 0x0 << BER_TYPE_PC_SHIFT,
    BER_TYPE_CONSTRUCTED = 0x1 << BER_TYPE_PC_SHIFT,
    BER_TYPE_P_C_MASK = 0x1 << BER_TYPE_PC_SHIFT /* Mask to get P/C bit */
} BERTypePC;

typedef enum ber_type_tag {
    BER_TYPE_EOC              /*  P        0       0*/,
    BER_TYPE_BOOLEAN          /*  P        1       1*/,
    BER_TYPE_INTEGER          /*  P        2       2*/,
    BER_TYPE_BIT_STRING       /*  P/C      3       3*/,
    BER_TYPE_OCTET_STRING     /*  P/C      4       4*/,
    BER_TYPE_NULL             /*  P        5       5*/,
    BER_TYPE_OBJECT_ID        /*  P        6       6*/,
    BER_TYPE_OBJECT_DESC      /*  P        7       7*/,
    BER_TYPE_EXTERNAL         /*  C        8       8*/,
    BER_TYPE_REAL             /*  P        9       9*/,
    BER_TYPE_ENUMERATED       /*  P        10      A*/,
    BER_TYPE_EMBEDDED         /*  C        11      B*/,
    BER_TYPE_UTF8_STRING      /*  P/C      12      C*/,
    BER_TYPE_RELATIVE_OID     /*  P        13      D*/,
    BER_TYPE_UNUSED_0xE       /*                    */,
    BER_TYPE_UNUSED_0xF       /*                    */,
    BER_TYPE_SEQUENCE         /*  C        16      10*/,
    BER_TYPE_SET              /*  C        17      11*/,
    BER_TYPE_NUMERIC_STRING   /*  P/C      18      12*/,
    BER_TYPE_PRINTABLE_STRING /*  P/C      19      13*/,
    BER_TYPE_T61STRING        /*  P/C      20      14*/,
    BER_TYPE_VIDEOTEX_STRING  /*  P/C      21      15*/,
    BER_TYPE_IA5_STRING       /*  P/C      22      16*/,
    BER_TYPE_UTCTIME          /*  P/C      23      17*/,
    BER_TYPE_GENERALIZED_TIME /*  P/C      24      18*/,
    BER_TYPE_GRAPHIC_STRING   /*  P/C      25      19*/,
    BER_TYPE_VISIBLE_STRING   /*  P/C      26      1A*/,
    BER_TYPE_GENERAL_STRING   /*  P/C      27      1B*/,
    BER_TYPE_UNIVERSAL_STRING /*  P/C      28      1C*/,
    BER_TYPE_CHARACTER_STRING /*  P/C      29      1D*/,
    BER_TYPE_BMP_STRING       /*  P/C      30      1E*/,
    BER_TYPE_LONG_FORM        /*  -        31      1F*/,
    BER_TYPE_TAG_MASK = 0x1f /* Mask to get tag */,
    BER_TYPE_CUSTOM_LIST = 0x20,
    BER_TYPE_CUSTOM_OPTIONAL = 0x21,
} BERTypeTag;

typedef enum ber_length {
    /* Special length values */
    BER_LENGTH_INDEFINITE = 0x1 << 7,
    BER_LENGTH_RESERVED = 0xFF,
    /* Anything else is either short or long */
    BER_LENGTH_SHORT = 0x0 << 7,
    BER_LENGTH_LONG = 0x1 << 7,
    BER_LENGTH_SHORT_LONG_MASK = 0x1 << 7,
    BER_LENGTH_MASK = 0x7F,
} BERLength;

typedef enum ber_length_encoding {
    BER_LENGTH_ENCODING_DEFINITE = 0, /* used by DER */
    BER_LENGTH_ENCODING_INDEFINITE = 1, /* used by CER */
} BERLengthEncoding;

const char *ber_type_to_str(uint8_t ber_type);
const char *ber_type_pc_to_str(enum ber_type_class ber_type_flags);
const char *ber_type_class_to_str(enum ber_type_class ber_type_flags);

#endif /* QAPI_BER_H */

