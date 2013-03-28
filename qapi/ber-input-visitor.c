/*
 * BER Input Visitor
 *
 * Copyright IBM, Corp. 2011, 2013
 * Copyright Red Hat, Inc. 2011
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *  Stefan Berger     <stefanb@us.ibm.com>
 *  Michael Tsirkin   <mst@redhat.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.1 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */

#include <math.h>
#include <glib.h>

#include "qemu-common.h"
#include "qapi/ber-common.h"
#include "qapi/ber-input-visitor.h"
#include "qemu/queue.h"
#include "qemu-common.h"
#include "hw/hw.h"
#include "qapi/ber.h"
#include "include/qapi/qmp/qerror.h"
#include "migration/qemu-file.h"
#include "qapi/visitor-impl.h"

#define AIV_STACK_SIZE 1024

/* whether to allow the parsing of primitives that are fragmented */
#define BER_ALLOW_FRAGMENTED_PRIMITIVES

/* #define DEBUG_BER */

#ifdef DEBUG_BER
#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

typedef struct StackEntry {
    uint64_t cur_pos;
    uint64_t counter;
} StackEntry;

struct BERInputVisitor {
    Visitor visitor;
    QEMUFile *qfile;
    uint64_t cur_pos;
    StackEntry stack[AIV_STACK_SIZE];
    int nb_stack;
    uint64_t max_allowed_buffer_size;
    uint64_t largest_needed_buffer;
};

static void ber_input_type_bool(Visitor *v, bool *obj, const char *name,
                                Error **errp);


static BERInputVisitor *to_biv(Visitor *v)
{
    return container_of(v, BERInputVisitor, visitor);
}

static void ber_input_push(BERInputVisitor *aiv,
                           uint64_t cur_pos, Error **errp)
{
    aiv->stack[aiv->nb_stack].cur_pos = cur_pos;
    aiv->stack[aiv->nb_stack].counter = 0;
    aiv->nb_stack++;

    if (aiv->nb_stack >= AIV_STACK_SIZE) {
        error_set(errp, QERR_BUFFER_OVERRUN);
    }
}

static uint64_t ber_input_pop(BERInputVisitor *aiv, Error **errp)
{
    aiv->nb_stack--;

    if (aiv->nb_stack < 0) {
        error_set(errp, QERR_BUFFER_OVERRUN);
        return 0;
    }

    return aiv->stack[aiv->nb_stack].cur_pos;
}

/*
 * Read a type tag from the stream. Up-to 32 bit type tags are supported
 * for reading and otherwise an error is returned. Anything larger than that
 * would not be reasonable and could only be abused.
 */
static uint32_t ber_read_type(BERInputVisitor *aiv, uint8_t *ber_type_flags,
                              Error **errp)
{
    uint32_t type;
    uint8_t byte;
    uint8_t ctr = 0;
    char buf[128];

    if (qemu_read_bytes(aiv->qfile, &byte, 1) != 1) {
        error_setg(errp, "QEMUFile has an error, error was '%s'",
                  "Error while reading type");
        return 0;
    }
    aiv->cur_pos++;
    type = byte;

    *ber_type_flags = type & (BER_TYPE_P_C_MASK | BER_TYPE_CLASS_MASK);

    if ((type & BER_TYPE_TAG_MASK) == BER_TYPE_LONG_FORM) {
        type = 0;
        while (true) {
            type <<= 7;
            if (qemu_read_bytes(aiv->qfile, &byte, 1) != 1) {
                error_setg(errp, "QEMUFile has an error, error was '%s'",
                          "Error while reading long type");
                return 0;
            }
            aiv->cur_pos++;

            type |= (byte & 0x7f);
            if ((byte & 0x80) == 0) {
                break;
            }
            ctr += 7; /* read 7 bits */
            if (ctr >= (sizeof(type) * 8)) {
                /* only support 32 bit length identifiers */
                snprintf(buf, sizeof(buf),
                         "type tag is larger than 32 bit (offset %" PRIu64
                         ")", aiv->cur_pos);
                error_setg(errp, "Data stream is invalid, error was '%s'", buf);
                return 0;
            }
        }
    } else {
        type &= BER_TYPE_TAG_MASK;
    }

    return type;
}

static uint64_t ber_read_length(BERInputVisitor *aiv, bool *is_indefinite,
                                Error **errp)
{
    uint8_t byte, c, int_len;
    uint64_t len = 0;
    QEMUFile *qfile = aiv->qfile;
    unsigned char int_array[sizeof(len)];
    char buf[128];

    *is_indefinite = false;

    if (qemu_read_bytes(qfile, &byte, 1) != 1) {
        error_setg(errp, "QEMUFile has an error, error was '%s'",
                  "Error while reading length indicator");
        return ~0x0ULL;
    }
    aiv->cur_pos++;

    if (byte == BER_LENGTH_INDEFINITE) {
        *is_indefinite = true;
        return ~0x0ULL;
    }

    if (!(byte & BER_LENGTH_LONG)) {
        len = byte;
    } else {
        int_len = byte & BER_LENGTH_MASK;
        if (int_len > sizeof(len)) {
            snprintf(buf, sizeof(buf),
                     "ASN.1 integer length field %d > %" PRIu64,
                     int_len, sizeof(len));
            /* Length can be up to 127 byte, but it seems
             * safe to assume any input will be < 1TB in length. */
            error_set(errp, QERR_INVALID_PARAMETER, buf);
            return ~0x0ULL;
        }
        if (qemu_read_bytes(qfile, int_array, int_len) != int_len) {
            error_setg(errp, "QEMUFile error: Error while reading length");
            return ~0x0ULL;
        }
        for (c = 0; c < int_len; c++) {
            len <<= 8;
            len |= int_array[c];
        }
        aiv->cur_pos += int_len;
    }

    if (len > aiv->max_allowed_buffer_size) {
        snprintf(buf, sizeof(buf),
                 "Length indicator (%"PRIu64") in input byte stream "
                 "exceeds maximum allowed length (%"PRIu64").",
                 len, aiv->max_allowed_buffer_size);
        error_setg(errp, "Data stream is invalid, error was '%s'", buf);
        return ~0x0ULL;
    }

    if (len > aiv->largest_needed_buffer) {
        aiv->largest_needed_buffer = len;
    }

    return len;
}

static uint64_t ber_peek_is_eoc(BERInputVisitor *biv, Error **errp)
{
    uint8_t buf[2];
    QEMUFile *qfile = biv->qfile;

    if (qemu_peek_bytes(qfile, buf, 2, 0) != 2) {
        error_setg(errp, "QEMUFile has an error, error was '%s'",
                   "Error while peeking for EOC");
        return ~0x0ULL;
    }

    if (buf[0] == BER_TYPE_EOC && buf[1] == 0) {
        return 1;
    }

    return 0;
}

static void ber_skip_bytes(BERInputVisitor *aiv, uint64_t to_skip,
                           Error **errp)
{
    uint8_t buf[1024];
    uint32_t skip;

    /* skip length bytes */
    while (to_skip > 0) {
        skip = MIN(to_skip, sizeof(buf));
        if (qemu_read_bytes(aiv->qfile, buf, skip) != skip) {
            error_setg(errp, "QEMUFile error: Error while skipping over bytes");
            return;
        }
        aiv->cur_pos += skip;
        to_skip -= skip;
    }
}

static void ber_skip_until_eoc(BERInputVisitor *aiv, Error **errp)
{
    uint32_t ber_type_tag;
    uint64_t length;
    bool is_indefinite;
    uint8_t ber_type_flags;
    uint64_t indefinite_nesting = 1;
    char buf[128];

    while (!error_is_set(errp)) {
        ber_type_tag = ber_read_type(aiv, &ber_type_flags, errp);
        if (error_is_set(errp)) {
            return;
        }

        length = ber_read_length(aiv, &is_indefinite, errp);
        if (error_is_set(errp)) {
            return;
        }
        if (ber_type_tag == BER_TYPE_EOC) {
            if (length) {
                snprintf(buf, sizeof(buf),
                         "ASN.1 EOC length field at offset %" PRIu64
                         " is invalid", aiv->cur_pos);
                error_set(errp, QERR_INVALID_PARAMETER, buf);
                return;
            }
            if (!indefinite_nesting) {
                snprintf(buf, sizeof(buf),
                         "ASN.1 EOC at offset %" PRIu64
                         "not within an indefinite length",
                         aiv->cur_pos);
                error_set(errp, QERR_INVALID_PARAMETER, buf);
                return;
            }
            DPRINTF("%s: found end! nesting=%" PRIdMAX
                    ", pos=%" PRIu64 "\n",
                    __func__, indefinite_nesting, aiv->cur_pos);
            if (!--indefinite_nesting) {
                return;
            }
        }
        if (is_indefinite) {
            if ((ber_type_flags & BER_TYPE_P_C_MASK) == BER_TYPE_PRIMITIVE) {
                snprintf(buf, sizeof(buf),
                         "ASN.1 indefinite length in a primitive type "
                         "at offset %" PRIu64,
                         aiv->cur_pos);
                error_set(errp, QERR_INVALID_PARAMETER, buf);
                return;
            }
            if (indefinite_nesting == ~0x0ULL) {
                snprintf(buf, sizeof(buf),
                         "ASN.1 indefinite nesting level is too large "
                         "(offset %" PRIu64 ")",
                         aiv->cur_pos);
                error_set(errp, QERR_INVALID_PARAMETER, buf);
                return;
            }
            ++indefinite_nesting;
        } else {
            DPRINTF("%s: skipping type '%s' of length "
                    "%" PRIu64 " at %" PRIu64 ".\n",
                    __func__, ber_type_to_str(ber_type_tag),
                    length, aiv->cur_pos);
            ber_skip_bytes(aiv, length, errp);
        }
    }
}

static void ber_input_start_constructed(Visitor *v, uint32_t exp_ber_type,
                                        uint8_t exp_ber_flags, void **obj,
                                        const char *kind, const char *name,
                                        size_t size, Error **errp)
{
    BERInputVisitor *aiv = to_biv(v);
    uint32_t ber_type_tag;
    uint8_t ber_type_flags;
    int64_t len;
    bool is_indefinite;
    char buf[128];

    ber_type_tag = ber_read_type(aiv, &ber_type_flags, errp);
    if (error_is_set(errp)) {
        return;
    }

    if (ber_type_tag != exp_ber_type || ber_type_flags != exp_ber_flags) {
        sprintf(buf, "%s at offset %" PRIu64,
                ber_type_to_str(exp_ber_type), aiv->cur_pos);

        error_set(errp, QERR_INVALID_PARAMETER_TYPE,
                  ber_type_to_str(ber_type_tag),
                  buf);
        return;
    }

    if ((ber_type_flags & BER_TYPE_P_C_MASK) == BER_TYPE_PRIMITIVE) {
        snprintf(buf, sizeof(buf), "primitive type (%s)",
                 ber_type_to_str(ber_type_tag));
        error_set(errp, QERR_INVALID_PARAMETER_TYPE,
                  buf, "constructed type");
        return;
    }

    len = ber_read_length(aiv, &is_indefinite, errp);
    if (error_is_set(errp)) {
        return;
    }

    if (!is_indefinite) {
        DPRINTF("%s: structure/set len: %" PRIi64 "\n", __func__, len);
        ber_input_push(aiv, aiv->cur_pos + len, errp);
    } else {
        DPRINTF("%s: indefinite length encoding!\n", __func__);
        ber_input_push(aiv, 0, errp);
    }

    if (error_is_set(errp)) {
        return;
    }

    if (size > 0 && *obj == NULL) {
        *obj = g_malloc0(size);
        DPRINTF("%s: for type '%s' allocated buffer at %p, "
                "size = %zu\n",
                __func__, ber_type_to_str(ber_type_tag), *obj, size);
    }
}

static void ber_input_end_constructed(Visitor *v, Error **errp)
{
    uint64_t new_pos;
    BERInputVisitor *aiv = to_biv(v);

    new_pos = ber_input_pop(aiv, errp);

    if (new_pos != 0) {
        DPRINTF("%s: new_pos = %" PRIu64 "\n", __func__, new_pos);
        aiv->cur_pos = new_pos;
    } else {
        DPRINTF("%s: searching for end...\n"
                "%s: cur_pos = %" PRIu64 "\n",
                __func__, __func__, aiv->cur_pos);
        ber_skip_until_eoc(aiv, errp);
    }
}

static void ber_input_start_struct(Visitor *v, void **obj, const char *kind,
                                   const char *name, size_t size, Error **errp)
{
    ber_input_start_constructed(v, BER_TYPE_SEQUENCE, BER_TYPE_CONSTRUCTED,
                                obj, kind, name, size, errp);
}

static void ber_input_end_struct(Visitor *v, Error **errp)
{
    ber_input_end_constructed(v, errp);
}

static void ber_input_start_optional(Visitor *v, bool *present,
                                     const char *name, Error **errp)
{
    ber_input_start_constructed(v, BER_TYPE_CUSTOM_OPTIONAL,
                                BER_TYPE_CONSTRUCTED,
                                NULL, "", name, 0, errp);
    if (error_is_set(errp)) {
        return;
    }

    ber_input_type_bool(v, present, name, errp);
}

static void ber_input_end_optional(Visitor *v, Error **errp)
{
    ber_input_end_constructed(v, errp);
}

static void ber_input_start_list(Visitor *v, const char *name,
                                 Error **errp)
{
    void *obj = NULL;
    ber_input_start_constructed(v, BER_TYPE_CUSTOM_LIST, BER_TYPE_CONSTRUCTED,
                                obj, NULL, name, 0, errp);
    g_free(obj);
}

static GenericList *ber_input_next_list(Visitor *v, GenericList **list,
                                        Error **errp)
{
    BERInputVisitor *biv = to_biv(v);
    GenericList *entry;
    StackEntry *se = &biv->stack[biv->nb_stack - 1];
    bool first = (se->counter++ == 0);

    if (se->cur_pos == 0) {
        /* indefinite lenght encoding is used */
        se = &biv->stack[biv->nb_stack];
        if (ber_peek_is_eoc(biv, errp) != 0) {
            return NULL;
        }
    } else if (se->cur_pos <= biv->cur_pos) {
        return NULL;
    }

    entry = g_malloc0(sizeof(*entry));
    if (first) {
        *list = entry;
    } else {
        (*list)->next = entry;
    }

    return entry;
}

static void ber_input_end_list(Visitor *v, Error **errp)
{
    ber_input_end_constructed(v, errp);
}

static void ber_input_integer(Visitor *v, uint8_t *obj, uint8_t maxbytes,
                              Error **errp)
{
    BERInputVisitor *aiv = to_biv(v);
    uint32_t ber_type_tag;
    uint8_t ber_type_flags;
    bool is_indefinite;
    uint64_t len;
    uint64_t val = 0;
    unsigned char int_array[sizeof(val)];
    int c;
    char buf[128], buf2[128];

    DPRINTF("%s: reading int to %p\n", __func__, obj);

    ber_type_tag = ber_read_type(aiv, &ber_type_flags, errp);
    if (error_is_set(errp)) {
        return;
    }

    DPRINTF("%s: got type: 0x%02x, expected 0x%02x\n",
            __func__, ber_type_tag, BER_TYPE_INTEGER);

    if (ber_type_tag != BER_TYPE_INTEGER ||
        ber_type_flags != (BER_TYPE_CLASS_APPLICATION|BER_TYPE_PRIMITIVE)) {
        snprintf(buf, sizeof(buf), "%s/%s/%s",
                 ber_type_class_to_str(ber_type_flags),
                 ber_type_pc_to_str(ber_type_flags),
                 ber_type_to_str(ber_type_tag));
        snprintf(buf2, sizeof(buf2), "%s/%s/%s",
                 ber_type_class_to_str(BER_TYPE_CLASS_APPLICATION),
                 ber_type_pc_to_str(BER_TYPE_PRIMITIVE),
                 ber_type_to_str(BER_TYPE_INTEGER));
        error_set(errp, QERR_INVALID_PARAMETER_TYPE, buf, buf2);
        return;
    }

    len = ber_read_length(aiv, &is_indefinite, errp);
    if (error_is_set(errp)) {
        return;
    }
    DPRINTF("%s: pos: %" PRIu64 " int len: %" PRIi64 "\n",
            __func__, aiv->cur_pos, len);

    if (is_indefinite) {
        error_set(errp, QERR_INVALID_PARAMETER_VALUE,
                  "ASN.1 int indicator is indefinite",
                  "[1..8]");
        return;
    }

    if (maxbytes > sizeof(val)) {
        snprintf(buf, sizeof(buf), "ASN.1 integers cannot have a length of "
                 "%" PRIi32 " bytes", maxbytes);
        error_set(errp, QERR_INVALID_PARAMETER_VALUE,
                  buf, "[1..8]");
        return;
    }

    if (len > maxbytes) {
        snprintf(buf, sizeof(buf), "ASN.1 integer length indicator %" PRIi64
                 " is larger than expected (%u bytes)",
                 len, maxbytes);
        error_set(errp, QERR_INVALID_PARAMETER_VALUE,
                  buf, "[1..8]");
        return;
    }

    if (qemu_read_bytes(aiv->qfile, int_array, len) != len) {
        error_setg(errp, "QEMUFile error: Error while reading integer");
        return;
    }

    for (c = 0; c < len ; c++) {
        val <<= 8;
        val |= int_array[c];
        if (c == 0 && (val & 0x80) == 0x80) {
            /* sign extend */
            val |= 0xFFFFFFFFFFFFFF00ULL;
        }
    }
    aiv->cur_pos += len;
    DPRINTF("%s: pos: %" PRIu64 " int: %" PRIu64 "\n",
            __func__, aiv->cur_pos, val);

#if __BYTE_ORDER == __LITTLE_ENDIAN
    memcpy(obj, &val, maxbytes);
#elif __BYTE_ORDER == __BIG_ENDIAN
    memcpy(obj, &((char *)&val)[sizeof(val) - maxbytes], maxbytes);
#endif
}

static void ber_input_type_int(Visitor *v, int64_t *obj, const char *name,
                               Error **errp)
{
    ber_input_integer(v, (uint8_t *)obj, sizeof(*obj), errp);
}

static void ber_input_type_uint8(Visitor *v, uint8_t *obj,
                                   const char *name, Error **errp)
{
    ber_input_integer(v, (uint8_t *)obj, sizeof(*obj), errp);
}

static void ber_input_type_uint16(Visitor *v, uint16_t *obj,
                                    const char *name, Error **errp)
{
    ber_input_integer(v, (uint8_t *)obj, sizeof(*obj), errp);
}

static void ber_input_type_uint32(Visitor *v, uint32_t *obj,
                                    const char *name, Error **errp)
{
    ber_input_integer(v, (uint8_t *)obj, sizeof(*obj), errp);
}

static void ber_input_type_uint64(Visitor *v, uint64_t *obj,
                                    const char *name, Error **errp)
{
    ber_input_integer(v, (uint8_t *)obj, sizeof(*obj), errp);
}

static void ber_input_type_int8(Visitor *v, int8_t *obj,
                                  const char *name, Error **errp)
{
    ber_input_integer(v, (uint8_t *)obj, sizeof(*obj), errp);
}

static void ber_input_type_int16(Visitor *v, int16_t *obj,
                                   const char *name, Error **errp)
{
    ber_input_integer(v, (uint8_t *)obj, sizeof(*obj), errp);
}

static void ber_input_type_int32(Visitor *v, int32_t *obj,
                                   const char *name, Error **errp)
{
    ber_input_integer(v, (uint8_t *)obj, sizeof(*obj), errp);
}

static void ber_input_type_int64(Visitor *v, int64_t *obj,
                                   const char *name, Error **errp)
{
    ber_input_integer(v, (uint8_t *)obj, sizeof(*obj), errp);
}

static void ber_input_type_bool(Visitor *v, bool *obj, const char *name,
                                Error **errp)
{
    BERInputVisitor *aiv = to_biv(v);
    uint32_t ber_type_tag;
    uint8_t ber_type_flags, byte;
    bool is_indefinite;
    uint64_t len;
    char buf[128];

    ber_type_tag = ber_read_type(aiv, &ber_type_flags, errp);
    if (error_is_set(errp)) {
        return;
    }

    if (ber_type_tag != BER_TYPE_BOOLEAN || ber_type_flags != 0) {
        error_set(errp, QERR_INVALID_PARAMETER_TYPE,
                  ber_type_to_str(ber_type_tag),
                  ber_type_to_str(BER_TYPE_BOOLEAN));
        return;
    }
    len = ber_read_length(aiv, &is_indefinite, errp);
    if (error_is_set(errp)) {
        return;
    }
    DPRINTF("%s: pos: %" PRIu64 " bool len: %" PRIi64 "\n",
            __func__, aiv->cur_pos, len);

    if (is_indefinite || len != 1) {
        snprintf(buf, sizeof(buf),
                 "ASN.1 bool length indicator at offset %" PRIu64
                 " is indefinite or != 1",
                 aiv->cur_pos);
        error_set(errp, QERR_INVALID_PARAMETER_VALUE,
                  buf, "1");
        return;
    }
    if (qemu_read_bytes(aiv->qfile, &byte, 1) != 1) {
        error_setg(errp, "QEMUFile error: Error while reading boolean");
        return;
    }
    aiv->cur_pos++;
    *obj = byte;

    DPRINTF("%s: pos: %" PRIu64 " bool: %d\n",
            __func__, aiv->cur_pos, *obj);
}

/* Function for recursive reading of fragmented primitives */
static uint32_t ber_input_fragment(BERInputVisitor *aiv,
                                   uint32_t exp_type_tag,
                                   uint8_t exp_type_flags,
                                   uint8_t **buffer,
                                   size_t *buffer_len, size_t elem_size,
                                   bool may_realloc,
                                   uint32_t offset, uint32_t nesting,
                                   bool indefinite, uint64_t max_pos,
                                   const char *name, Error **errp)
{
    uint32_t ber_type_tag;
    uint8_t ber_type_flags;
    uint32_t bytes_read = 0;
    bool is_indefinite;
    uint64_t len;
    char buf[128];
    uint8_t *conv_buffer;
    size_t num_elms;
#if __BYTE_ORDER == __LITTLE_ENDIAN
    size_t i;
    uint16_t *d16, *s16;
    uint32_t *d32, *s32;
    uint64_t *d64, *s64;
#endif
    bool is_bigendian;

#if __BYTE_ORDER == __BIG_ENDIAN
    is_bigendian = true;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
    is_bigendian = false;
#else
# error Unsupported endianess
#endif

    assert((exp_type_flags & BER_TYPE_CONSTRUCTED) == BER_TYPE_PRIMITIVE);

    ber_type_tag = ber_read_type(aiv, &ber_type_flags, errp);
    if (error_is_set(errp)) {
        return 0;
    }

    if (ber_type_tag != exp_type_tag) {
        error_set(errp, QERR_INVALID_PARAMETER_TYPE,
                  ber_type_to_str(ber_type_tag & BER_TYPE_TAG_MASK),
                  ber_type_to_str(exp_type_tag));
        return 0;
    }

    if ((ber_type_flags & BER_TYPE_CONSTRUCTED)) {
#ifndef BER_ALLOW_FRAGMENTED_PRIMITIVES
        error_setg(errp, "Data stream is invalid, error was '%s'",
                  "constructed encoding of primitive types is not supported");
        goto err_exit;
#else
        if (nesting == 1) {
            /* don't allow further nesting */
            error_setg(errp, "Data stream is invalid, error was invalid "
                       "nesting");
            goto err_exit;
        }
        len = ber_read_length(aiv, &is_indefinite, errp);
        if (error_is_set(errp)) {
            goto err_exit;
        }
        DPRINTF("%s: pos: %" PRIu64 " string len: %" PRIi64 "\n",
                __func__, aiv->cur_pos, len);

        if (!is_indefinite) {
            if ((*buffer) == NULL) {
                /* allocate buffer once; due to the ASN.1 overhead it
                 * will be bigger than what we need */
                *buffer = g_malloc0(len);
                *buffer_len = len;
                may_realloc = false;
            }
        }
        DPRINTF("%s: recursing now to read constructed type.\n"
                "%s: is_indefinite: %d\n",
                __func__, __func__, is_indefinite);
        bytes_read += ber_input_fragment(aiv, exp_type_tag, exp_type_flags,
                                         buffer, buffer_len, elem_size,
                                         may_realloc, offset, nesting + 1,
                                         is_indefinite, aiv->cur_pos + len,
                                         name, errp);
        return bytes_read;
#endif
    }

    while (true) {
        /* Would reading the length carry us beyond what we are allowed to
         * read?
         */
        if (!indefinite &&
            max_pos != 0 &&
            aiv->cur_pos + 1 > max_pos) {
            snprintf(buf, sizeof(buf),
                     "data stream would cause parsing beyond "
                     "allowed offset at %" PRIu64,
                     max_pos);
            /* input stream is malformed */
            error_setg(errp, "Data stream is invalid, error was '%s'", buf);
            goto err_exit;
        }

        /* not-constructed case */
        len = ber_read_length(aiv, &is_indefinite, errp);
        if (error_is_set(errp)) {
            goto err_exit;
        }
        DPRINTF("%s: pos: %" PRIu64 " string len: %" PRIi64 "\n",
                __func__, aiv->cur_pos, len);
        if (is_indefinite) {
            snprintf(buf, sizeof(buf),
                     "Got indefinite type length in primitive type (%s) at"
                     "offset %" PRIu64,
                     ber_type_to_str(ber_type_tag), aiv->cur_pos);
            error_set(errp, QERR_INVALID_PARAMETER, buf);
            goto err_exit;
        }
        /* if max_pos is not set, set it here */
        if (!indefinite && max_pos == 0) {
            max_pos = aiv->cur_pos + len;
        }

        /* Would reading the data carry us beyond what we are allowed to
         * read ?
         */
        if (!indefinite && aiv->cur_pos + len > max_pos) {
            /* input stream is malformed */
            snprintf(buf, sizeof(buf),
                     "data stream would cause parsing beyond "
                     "allowed offset at %" PRIu64,
                     max_pos);
            error_setg(errp, "Data stream is invalid, error was '%s'", buf);
            goto err_exit;
        }

        if (offset + len > *buffer_len) {
            if (!may_realloc) {
                snprintf(buf, sizeof(buf),
                         "given buffer is too small (%lu < %"PRIu64")",
                         (unsigned long)*buffer_len, offset + len);
                error_setg(errp, "Data stream is invalid, error was '%s'",
                           buf);
                goto err_exit;
            }
            /* allocate one more byte for strings, set to 0 */
            *buffer = g_realloc(*buffer, offset + len + 1);
            *buffer_len = offset + len;
            (*buffer)[offset+len] = 0;
        }

        num_elms = len / elem_size;
        if (num_elms * elem_size != len) {
            error_setg(errp, "Stream contains broken up element of size "
                       "%"PRIi64, elem_size);
            goto err_exit;
        }

        if (elem_size != sizeof(uint8_t) && !is_bigendian) {
            /* intermediate buffer for endianess conversion */
            conv_buffer = g_malloc(len);
        } else {
            conv_buffer = &((uint8_t *)*buffer)[offset];
        }

        if (qemu_read_bytes(aiv->qfile, conv_buffer, len) != len) {
            error_setg(errp, "QEMUFile error: Error while reading data");
            goto err_exit;
        }

        switch (elem_size) {
        case sizeof(uint8_t):
#if __BYTE_ORDER == __BIG_ENDIAN
        case sizeof(uint16_t):
        case sizeof(uint32_t):
        case sizeof(uint64_t):
#endif
            /* nothing to convert */
            break;
#if __BYTE_ORDER == __LITTLE_ENDIAN
        case sizeof(uint16_t):
            s16 = (uint16_t *)conv_buffer;
            d16 = (uint16_t *)&((uint8_t *)*buffer)[offset];
            for (i = 0; i < num_elms; i++) {
                d16[i] = be16_to_cpu(s16[i]);
            }
            break;
        case sizeof(uint32_t):
            s32 = (uint32_t *)conv_buffer;
            d32 = (uint32_t *)&((uint8_t *)*buffer)[offset];
            for (i = 0; i < num_elms; i++) {
                d32[i] = be32_to_cpu(s32[i]);
            }
            break;
        case sizeof(uint64_t):
            s64 = (uint64_t *)conv_buffer;
            d64 = (uint64_t *)&((uint8_t *)*buffer)[offset];
            for (i = 0; i < num_elms; i++) {
                d64[i] = be64_to_cpu(s64[i]);
            }
            break;
#endif
        }

        if (elem_size != sizeof(uint8_t) && !is_bigendian) {
            /* intermediate buffer for endianess conversion */
            g_free(conv_buffer);
        }

        offset += len;
        bytes_read += len;

        aiv->cur_pos += len;

        if (exp_type_tag == BER_TYPE_IA5_STRING) {
            DPRINTF("%s: pos: %" PRIu64 " string: %.*s\n",
                    __func__, aiv->cur_pos, offset, *buffer);
        }

        if (nesting == 0) {
            break;
        }

        /* indefinite length case: loop until we encounter EOC */
        if (indefinite) {
            ber_type_tag = ber_read_type(aiv, &ber_type_flags, errp);
            if (error_is_set(errp)) {
                goto err_exit;
            }

            if (ber_type_tag == BER_TYPE_EOC) {
                uint8_t byte;
                if (qemu_read_bytes(aiv->qfile, &byte, 1) != 1) {
                    error_setg(errp, "QEMUFile error: Error while reading "
                               "BER_TYPE_EOC length");
                    goto err_exit;
                }
                aiv->cur_pos++;

                if (byte != 0) {
                    snprintf(buf, sizeof(buf),
                             "ASN.1 EOC length field is invalid at offset "
                             "%" PRIu64,
                             aiv->cur_pos);
                    error_set(errp, QERR_INVALID_PARAMETER, buf);
                    goto err_exit;
                }
                return bytes_read;
            }

            if (ber_type_tag != exp_type_tag ||
                ber_type_flags != exp_type_flags) {
                snprintf(buf, sizeof(buf),
                         "ASN.1 type field or flags are wrong. Found "
                         "0x%x/%u, expected "
                         "0x%x/%u at offset %" PRIu64,
                         ber_type_tag, ber_type_flags,
                         exp_type_tag, exp_type_flags,
                         aiv->cur_pos);
                error_set(errp, QERR_INVALID_PARAMETER, buf);
                goto err_exit;
            }
            continue;
        }

        /* in definite length coding case; caller told us how far to read */
        if (aiv->cur_pos == max_pos) {
            return bytes_read;
        }

        ber_type_tag = ber_read_type(aiv, &ber_type_flags, errp);
        if (error_is_set(errp)) {
            goto err_exit;
        }

        if ((ber_type_flags & BER_TYPE_P_C_MASK) == BER_TYPE_CONSTRUCTED) {
            error_set(errp, QERR_INVALID_PARAMETER_TYPE,
                      "constructed BER type",
                      ber_type_to_str(exp_type_tag));
            goto err_exit;
        }

        if (ber_type_tag != exp_type_tag) {
            error_set(errp, QERR_INVALID_PARAMETER_TYPE,
                      ber_type_to_str(ber_type_tag & BER_TYPE_TAG_MASK),
                      ber_type_to_str(exp_type_tag));
            goto err_exit;
        }
    }
    return bytes_read;

err_exit:
    if (may_realloc) {
        g_free(*buffer);
        *buffer = NULL;
    }
    return 0;
}

static void ber_input_type_str(Visitor *v, char **obj, const char *name,
                               Error **errp)
{
    BERInputVisitor *aiv = to_biv(v);
    size_t buffer_len = 0;

    ber_input_fragment(aiv, BER_TYPE_IA5_STRING, 0,
                       (uint8_t **)obj, &buffer_len, 1, (*obj == NULL),
                       0, 0, false, 0, name, errp);

    if (!error_is_set(errp) && *obj == NULL) {
        /* adjust NULL string to "" */
        *obj = g_strdup("");
    }
}

static void ber_input_sized_buffer(Visitor *v, void **obj, const char *name,
                                   size_t num_elem, size_t elem_size,
                                   Error **errp)
{
    BERInputVisitor *aiv = to_biv(v);
    size_t len = num_elem * elem_size;

    if (*obj == NULL) {
        /*
         * Allocating the memory will prevent a smaller structure from
         * being allocated by ber_input_fragment.
         * Setting len = 0  would allow less bytes to be allocated than
         * expected by the caller, which may cause segfaults if the caller
         * then steps on unallocated memory.
         */
        *obj = g_malloc(len);
    }

    ber_input_fragment(aiv, BER_TYPE_OCTET_STRING, 0,
                       (uint8_t **)obj, &len, elem_size, (*obj == NULL),
                       0, 0, false, 0, name, errp);

#ifdef BER_DEBUG
    DPRINTF("%s: pos: %" PRIu64 " data at: %p data:\n",
            __func__, aiv->cur_pos, *obj);
    int i;
    for (i = 0; i < len; i++) {
        DPRINTF("%02x ", (*obj)[i]);
        if ((i & 0xf) == 0xf) {
            DPRINTF("\n");
        }
    }
    DPRINTF("\n");
#endif
}

static void ber_input_type_number(Visitor *v, double *obj, const char *name,
                                  Error **errp)
{
    BERInputVisitor *aiv = to_biv(v);
    uint32_t ber_type_tag;
    uint8_t ber_type_flags;
    uint32_t len;
    bool is_indefinite;
    char buf[128], buf2[128];
    GDoubleIEEE754 num;
    struct ieee754_buffer number;
    size_t to_read;

    ber_type_tag = ber_read_type(aiv, &ber_type_flags, errp);
    if (error_is_set(errp)) {
        return;
    }

    if (ber_type_tag != BER_TYPE_REAL ||
        ber_type_flags != (BER_TYPE_CLASS_APPLICATION|BER_TYPE_PRIMITIVE)) {
        snprintf(buf, sizeof(buf), "%s/%s/%s",
                 ber_type_class_to_str(ber_type_flags),
                 ber_type_pc_to_str(ber_type_flags),
                 ber_type_to_str(ber_type_tag));
        snprintf(buf2, sizeof(buf2), "%s/%s/%s",
                 ber_type_class_to_str(BER_TYPE_CLASS_APPLICATION),
                 ber_type_pc_to_str(BER_TYPE_PRIMITIVE),
                 ber_type_to_str(BER_TYPE_REAL));
        error_set(errp, QERR_INVALID_PARAMETER_TYPE, buf, buf2);
        return;
    }

    len = ber_read_length(aiv, &is_indefinite, errp);
    if (error_is_set(errp)) {
        return;
    }

    to_read = sizeof(number) - offsetof(struct ieee754_buffer, first);

    if (len != to_read) {
        snprintf(buf, sizeof(buf),
                 "Length indicator in input byte stream "
                 "of real has unexpected length %"PRIu32"; "
                 "expected %" PRIu64,
                 len, sizeof(buf));
        error_set(errp, QERR_INVALID_PARAMETER, buf);
        return;
    }

    if (is_indefinite) {
        snprintf(buf, sizeof(buf),
                 "ASN.1 indefinite length in a real type "
                 "at offset %" PRIu64,
                 aiv->cur_pos);
        error_set(errp, QERR_INVALID_PARAMETER, buf);
        return;
    }

    if (qemu_read_bytes(aiv->qfile, &number.first, to_read) != to_read) {
        error_setg(errp, "QEMUFile error: Error while reading real");
        return;
    }

    switch (number.first) {
    case 0x42:
        *obj = nan("NAN");
        break;
    case 0x41:
    case 0x40:
        num.mpn.sign = ((number.first & 0x1) != 0);
        num.mpn.biased_exponent = ~0;
        num.mpn.mantissa_low = 0;
        num.mpn.mantissa_high = 0;
        *obj = num.v_double;
        break;
    default:
        num.mpn.sign = ((number.first & 0x40) != 0);
        num.mpn.biased_exponent = be16_to_cpu(number.exponent);
        num.mpn.mantissa_low = be32_to_cpu(number.mant_lo);
        num.mpn.mantissa_high = be32_to_cpu(number.mant_hi);
        *obj = num.v_double;
    }
}

Visitor *ber_input_get_visitor(BERInputVisitor *v)
{
    return &v->visitor;
}

uint64_t ber_input_get_parser_position(BERInputVisitor *v)
{
    return v->cur_pos;
}

uint64_t ber_input_get_largest_needed_buffer(BERInputVisitor *v)
{
    return v->largest_needed_buffer;
}

void ber_input_visitor_cleanup(BERInputVisitor *v)
{
    g_free(v);
}

BERInputVisitor *ber_input_visitor_new(QEMUFile *qfile,
                                       uint64_t max_allowed_buffer_size)
{
    BERInputVisitor *v;

    v = g_malloc0(sizeof(*v));

    v->visitor.start_struct = ber_input_start_struct;
    v->visitor.end_struct = ber_input_end_struct;
    v->visitor.start_optional = ber_input_start_optional;
    v->visitor.end_optional = ber_input_end_optional;
    v->visitor.start_list = ber_input_start_list;
    v->visitor.next_list = ber_input_next_list;
    v->visitor.end_list = ber_input_end_list;
    v->visitor.type_int = ber_input_type_int;
    v->visitor.type_uint8 = ber_input_type_uint8;
    v->visitor.type_uint16 = ber_input_type_uint16;
    v->visitor.type_uint32 = ber_input_type_uint32;
    v->visitor.type_uint64 = ber_input_type_uint64;
    v->visitor.type_int8 = ber_input_type_int8;
    v->visitor.type_int16 = ber_input_type_int16;
    v->visitor.type_int32 = ber_input_type_int32;
    v->visitor.type_int64 = ber_input_type_int64;
    v->visitor.type_bool = ber_input_type_bool;
    v->visitor.type_str = ber_input_type_str;
    v->visitor.type_sized_buffer = ber_input_sized_buffer;
    v->visitor.type_number = ber_input_type_number;

    v->qfile = qfile;
    v->cur_pos = 0;
    v->max_allowed_buffer_size = max_allowed_buffer_size;
    v->largest_needed_buffer = 0;

    return v;
}
