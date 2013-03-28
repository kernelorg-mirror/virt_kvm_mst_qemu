/*
 * BER Output Visitor
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
#include "qapi/ber-output-visitor.h"
#include "qemu/queue.h"
#include "qemu-common.h"
#include "include/qapi/qmp/qerror.h"
#include "hw/hw.h"
#include "qapi/ber.h"
#include "qapi/visitor-impl.h"


#define CER_FRAGMENT_CHUNK_SIZE  1000

/*#define DEBUG_BER */

#ifdef DEBUG_BER
#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif


typedef struct QStackEntry {
    QEMUFile *qfile;
    bool is_list_head;
    BERLengthEncoding length_encoding;
    QTAILQ_ENTRY(QStackEntry) node;
} QStackEntry;

typedef QTAILQ_HEAD(QStack, QStackEntry) QStack;

struct BEROutputVisitor {
    Visitor visitor;
    QStack stack;
    QEMUFile *qfile;

    BERLengthEncoding length_encoding;
};

static void ber_output_type_bool(Visitor *v, bool *obj, const char *name,
                                 Error **errp);


static BEROutputVisitor *to_aov(Visitor *v)
{
    return container_of(v, BEROutputVisitor, visitor);
}

static void ber_output_push(BEROutputVisitor *qov, QEMUFile *qfile,
                            Error **errp)
{
    QStackEntry *e = g_malloc0(sizeof(*e));

    e->qfile = qfile;
    e->is_list_head = true;
    /* remember length encoding used at this point */
    e->length_encoding = qov->length_encoding;
    QTAILQ_INSERT_HEAD(&qov->stack, e, node);
}

static QEMUFile *ber_output_pop(BEROutputVisitor *qov)
{
    QStackEntry *e = QTAILQ_FIRST(&qov->stack);
    QEMUFile *qfile;

    QTAILQ_REMOVE(&qov->stack, e, node);
    qfile = e->qfile;
    /* switch back to length encoding used at this point */
    qov->length_encoding = e->length_encoding;
    g_free(e);

    return qfile;
}

static unsigned int ber_encode_type(uint8_t *buffer, uint32_t buflen,
                                    enum ber_type_tag ber_type,
                                    uint8_t ber_type_flags,
                                    Error **errp)
{
    unsigned int idx = 0;

    if (buflen < 1) {
        error_set(errp, QERR_BUFFER_OVERRUN);
        return 0;
    }

    if (ber_type > BER_TYPE_LONG_FORM) {
        int byte = sizeof(uint32_t);
        uint32_t mask = 0x7f << (7 * byte);
        bool do_write = false;

        buffer[0] = ber_type_flags | BER_TYPE_LONG_FORM;

        while (byte >= 0) {
            if (!do_write) {
                if ((mask & ber_type)) {
                    do_write = true;
                    if (1 + byte + 1 > buflen) {
                        error_set(errp, QERR_BUFFER_OVERRUN);
                        return 0;
                    }
                }
            }
            if (do_write) {
                buffer[1 + idx] = (ber_type >> (7 * byte)) & 0x7f;
                if (byte > 0) {
                    buffer[1 + idx] |= 0x80;
                }
                idx++;
            }
            byte--;
            mask = 0x7f << (7 * byte);
        }
    } else {
        buffer[0] = ber_type | ber_type_flags;
    }
    return 1 + idx;
}

static unsigned int ber_encode_len(uint8_t *buffer, uint32_t buflen,
                                   uint64_t len, Error **errp)
{
    uint64_t mask = 0xFF00000000000000ULL;
    int shift =  64 - 8;
    int c = 0;

    if (len <= 0x7f && buflen >= 1) {
        buffer[0] = len;
        return 1;
    }

    while (mask && (mask & len) == 0) {
        mask >>= 8;
        shift -= 8;
    }

    while (shift >= 0) {
        if (1 + c + 1 > buflen) {
            error_set(errp, QERR_BUFFER_OVERRUN);
            return 0;
        }
        buffer[1+c] = len >> shift;
        c++;
        shift -= 8;
    }

    buffer[0] = BER_LENGTH_LONG | c;

    return 1 + c;
}

static void ber_output_start_constructed(Visitor *v, uint32_t ber_type,
                                         Error **errp)
{
    BEROutputVisitor *aov = to_aov(v);
    uint8_t buf[20];
    unsigned int tag_bytes_written;

    switch (aov->length_encoding) {
    case BER_LENGTH_ENCODING_DEFINITE:
        ber_output_push(aov, aov->qfile, errp);
        if (error_is_set(errp)) {
            return;
        }
        aov->qfile = qemu_bufopen("w", NULL);
        break;
    case BER_LENGTH_ENCODING_INDEFINITE:
        ber_output_push(aov, aov->qfile, errp); /* needed for list support */
        if (error_is_set(errp)) {
            return;
        }
        tag_bytes_written = ber_encode_type(buf, sizeof(buf),
                                            ber_type, BER_TYPE_CONSTRUCTED,
                                            errp);
        if (error_is_set(errp)) {
            return;
        }
        buf[tag_bytes_written] = BER_LENGTH_INDEFINITE;
        if (qemu_write_bytes(aov->qfile, buf, 1 + tag_bytes_written) !=
            1 + tag_bytes_written) {
            error_setg(errp, "QEMUFile error: Error while writing "
                       "constructed type");
            return;
        }
    }
}

static void ber_output_constructed_ber_close(BEROutputVisitor *aov,
                                             QEMUFile *qfile,
                                             uint32_t ber_type,
                                             Error **errp)
{
    uint8_t buf[20];
    const QEMUSizedBuffer *qsb;
    uint64_t len;
    unsigned int num_bytes, tag_bytes_written;

    tag_bytes_written = ber_encode_type(buf, sizeof(buf),
                                        ber_type, BER_TYPE_CONSTRUCTED,
                                        errp);
    if (error_is_set(errp)) {
        return;
    }

    qsb = qemu_buf_get(aov->qfile);
    len = qsb_get_length(qsb);
    DPRINTF("%s:constructed type (0x%02x, %p) has length %ld bytes\n",
            __func__, ber_type, aov->qfile, len);

    num_bytes = ber_encode_len(&buf[tag_bytes_written],
                               sizeof(buf) - tag_bytes_written,
                               len, errp);
    if (error_is_set(errp)) {
        return;
    }

    if (qemu_write_bytes(qfile, buf, tag_bytes_written + num_bytes) !=
            tag_bytes_written + num_bytes ||
        qsb_qfile_write(qsb, qfile, 0, qsb_get_length(qsb)) !=
            qsb_get_length(qsb)) {
        error_setg(errp, "QEMUFile error: Error while writing buffer");
        return;
    }

    qemu_fclose(aov->qfile);
    aov->qfile = qfile;
    qemu_fflush(qfile);
}

static void ber_output_end_constructed(Visitor *v, uint32_t ber_type,
                                       Error **errp)
{
    BEROutputVisitor *aov = to_aov(v);
    uint8_t buf[20];
    QEMUFile *qfile = ber_output_pop(aov);

    DPRINTF("%s: end set/struct:\n", __func__);

    switch (aov->length_encoding) {
    case BER_LENGTH_ENCODING_DEFINITE:
        ber_output_constructed_ber_close(aov, qfile, ber_type, errp);
        break;

    case BER_LENGTH_ENCODING_INDEFINITE:
        buf[0] = BER_TYPE_EOC;
        buf[1] = 0;
        if (qemu_write_bytes(aov->qfile, buf, 2) != 2) {
            error_setg(errp, "QEMUFile error: Error while writing buffer "
                       "with BER_TYPE_EOC");
            return;
        }
        break;
    }
}

static void ber_output_start_struct(Visitor *v, void **obj, const char *kind,
                                    const char *name, size_t unused,
                                    Error **errp)
{
    ber_output_start_constructed(v, BER_TYPE_SEQUENCE, errp);
}

static void ber_output_end_struct(Visitor *v, Error **errp)
{
    ber_output_end_constructed(v, BER_TYPE_SEQUENCE, errp);
}

static void ber_output_start_optional(Visitor *v, bool *present,
                                      const char *name, Error **errp)
{
    ber_output_start_constructed(v, BER_TYPE_CUSTOM_OPTIONAL, errp);

    if (error_is_set(errp)) {
        return;
    }

    ber_output_type_bool(v, present, name, errp);
}

static void ber_output_end_optional(Visitor *v, Error **errp)
{
    ber_output_end_constructed(v, BER_TYPE_CUSTOM_OPTIONAL, errp);
}

static void ber_output_start_list(Visitor *v, const char *name,
                                  Error **errp)
{
    ber_output_start_constructed(v, BER_TYPE_CUSTOM_LIST, errp);
}

static GenericList *ber_output_next_list(Visitor *v, GenericList **listp,
                                         Error **errp)
{
    GenericList *list = *listp;
    BEROutputVisitor *bov = to_aov(v);
    QStackEntry *e = QTAILQ_FIRST(&bov->stack);

    assert(e);
    if (e->is_list_head) {
        e->is_list_head = false;
        return list;
    }

    return list ? list->next : NULL;
}

static void ber_output_end_list(Visitor *v, Error **errp)
{
    ber_output_end_constructed(v, BER_TYPE_CUSTOM_LIST, errp);
}

static void ber_output_fragment(Visitor *v, uint32_t ber_type,
                                uint8_t *buffer, size_t elem_count,
                                size_t elem_size, Error **errp)
{
    uint32_t offset = 0;
    bool fragmented = false;
    uint32_t chunk;
    unsigned int num_bytes, type_bytes;
    uint8_t buf[20];
    uint32_t chunk_size;
    BEROutputVisitor *aov = to_aov(v);
    size_t buflen = elem_count * elem_size;
    size_t n_elms;
#if __BYTE_ORDER == __LITTLE_ENDIAN
    size_t i;
    uint16_t *d16, *s16;
    uint32_t *d32, *s32;
    uint64_t *d64, *s64;
#endif
    uint8_t *conv_buffer = NULL;
    bool is_bigendian;
#if __BYTE_ORDER == __BIG_ENDIAN
    is_bigendian = true;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
    is_bigendian = false;
#else
# error Unsupported endianess
#endif

    switch (aov->length_encoding) {
    case BER_LENGTH_ENCODING_INDEFINITE:
        /* X.690 9.2 */
        fragmented = (buflen > CER_FRAGMENT_CHUNK_SIZE);
        chunk_size = 1000;
        break;
    case BER_LENGTH_ENCODING_DEFINITE:
        chunk_size = 0xffffffff;
        break;
    }

    if (fragmented) {
        ber_output_start_constructed(&aov->visitor, ber_type, errp);
        if (error_is_set(errp)) {
            return;
        }
    }

    if (elem_size != sizeof(uint8_t) && !is_bigendian) {
        /* intermediate buffer for endianess conversion */
        conv_buffer = g_malloc(chunk_size);
    }

    do {
        chunk = (buflen - offset > chunk_size) ? chunk_size : buflen - offset;

        /* calc how many >=2 byte-elements cleanly fit into the chunk */
        n_elms = chunk / elem_size;
        chunk = n_elms * elem_size;

        type_bytes = ber_encode_type(buf, sizeof(buf), ber_type, 0,
                                     errp);
        if (error_is_set(errp)) {
            goto error;
        }
        num_bytes = ber_encode_len(&buf[type_bytes], sizeof(buf) - type_bytes,
                                   chunk, errp);
        if (error_is_set(errp)) {
            goto error;
        }


        switch (elem_size) {
        case sizeof(uint8_t):
#if __BYTE_ORDER == __BIG_ENDIAN
        case sizeof(uint16_t):
        case sizeof(uint32_t):
        case sizeof(uint64_t):
#endif
            /* simple write, no endianess conversion */
            conv_buffer = &buffer[offset];
            break;
#if __BYTE_ORDER == __LITTLE_ENDIAN
        case sizeof(uint16_t):
            d16 = (uint16_t *)conv_buffer;
            s16 = (uint16_t *)&buffer[offset];
            for (i = 0; i < n_elms; i++) {
                d16[i] = cpu_to_be16(s16[i]);
            }
            break;
        case sizeof(uint32_t):
            d32 = (uint32_t *)conv_buffer;
            s32 = (uint32_t *)&buffer[offset];
            for (i = 0; i < n_elms; i++) {
                d32[i] = cpu_to_be32(s32[i]);
            }
            break;
        case sizeof(uint64_t):
            d64 = (uint64_t *)conv_buffer;
            s64 = (uint64_t *)&buffer[offset];
            for (i = 0; i < n_elms; i++) {
                d64[i] = cpu_to_be64(s64[i]);
            }
            break;
#endif
        default:
            error_setg(errp, "Illegal element size %" PRIu64, elem_size);
            goto error;
        }

        if (qemu_write_bytes(aov->qfile, buf, type_bytes + num_bytes) !=
            type_bytes + num_bytes ||
            qemu_write_bytes(aov->qfile, conv_buffer, chunk) != chunk) {
            error_setg(errp, "QEMUFile error: Error while writing buffer");
            goto error;
        }

        offset += chunk;
    } while (offset < buflen);

    if (fragmented) {
        ber_output_end_constructed(&aov->visitor, ber_type, errp);
    }

error:
    if (elem_size != sizeof(uint8_t) && !is_bigendian) {
        g_free(conv_buffer);
    }
}

static void ber_output_int(Visitor *v, int64_t val, uint8_t maxnumbytes,
                           Error **errp)
{
    uint8_t buf[20];
    int shift =  (maxnumbytes - 1) * 8;
    uint64_t mask = 0xFF80ULL << (shift - 8);
    bool exp_zeros;
    int c = 0;
    BEROutputVisitor *aov = to_aov(v);

    DPRINTF("%s: Writing int 0x%lx (len=%d)\n",
            __func__, val, maxnumbytes);

    /*
     * We encode ints with fixed-witdh so that they will use
     * the same number of bytes indepent of their value.
     * The 'universal' encoding would not encode a 32bit '0'
     * with 4 bytes, so this is an application-specific encoding.
     */
    buf[0] = BER_TYPE_CLASS_APPLICATION | BER_TYPE_PRIMITIVE |
             BER_TYPE_INTEGER;

    if (maxnumbytes > 1) {
        exp_zeros = ((mask & val) == 0) ? true : false;
        while (mask != 0xFF) {
            if (exp_zeros) {
                if ((mask & val) != 0) {
                    break;
                }
            } else {
                if ((mask & val) != mask) {
                    break;
                }
            }
            shift -= 8;
            mask >>= 8;
        }
    }

    while (shift >= 0) {
        buf[2+c] = val >> shift;
        c++;
        shift -= 8;
    }
    buf[1] = c;

    if (qemu_write_bytes(aov->qfile, buf, 1 + 1 + c) != 1 + 1 + c) {
        error_setg(errp, "QEMUFile error: Error while writing integer");
        return;
    }
}
static void ber_output_type_int(Visitor *v, int64_t *obj, const char *name,
                                Error **errp)
{
    ber_output_int(v, *obj, sizeof(*obj), errp);
}

static void ber_output_type_uint8(Visitor *v, uint8_t *obj,
                                    const char *name, Error **errp)
{
    ber_output_int(v, *obj, sizeof(*obj), errp);
}

static void ber_output_type_uint16(Visitor *v, uint16_t *obj,
                                     const char *name, Error **errp)
{
    ber_output_int(v, *obj, sizeof(*obj), errp);
}

static void ber_output_type_uint32(Visitor *v, uint32_t *obj,
                                     const char *name, Error **errp)
{
    ber_output_int(v, *obj, sizeof(*obj), errp);
}

static void ber_output_type_uint64(Visitor *v, uint64_t *obj,
                                     const char *name, Error **errp)
{
    ber_output_int(v, *obj, sizeof(*obj), errp);
}

static void ber_output_type_int8(Visitor *v, int8_t *obj,
                                   const char *name, Error **errp)
{
    ber_output_int(v, (int64_t)*obj, sizeof(*obj), errp);
}

static void ber_output_type_int16(Visitor *v, int16_t *obj,
                                    const char *name, Error **errp)
{
    ber_output_int(v, (int64_t)*obj, sizeof(*obj), errp);
}

static void ber_output_type_int32(Visitor *v, int32_t *obj,
                                    const char *name, Error **errp)
{
    ber_output_int(v, (int64_t)*obj, sizeof(*obj), errp);
}

static void ber_output_type_int64(Visitor *v, int64_t *obj,
                                    const char *name, Error **errp)
{
    ber_output_int(v, (int64_t)*obj, sizeof(*obj), errp);
}

static void ber_output_type_bool(Visitor *v, bool *obj, const char *name,
                                 Error **errp)
{
    bool b;

    b = *obj;

    ber_output_fragment(v, BER_TYPE_BOOLEAN, (uint8_t *)&b, sizeof(b),
                        sizeof(b), errp);
}

static void ber_output_type_str(Visitor *v, char **obj, const char *name,
                                Error **errp)
{
    DPRINTF("%s: Writing string %s, len = 0x%02x\n",
            __func__, *obj, (int)strlen(*obj));
    ber_output_fragment(v, BER_TYPE_IA5_STRING,
                        (uint8_t *)*obj,
                        *obj == NULL ? 0 : strlen(*obj), 1, errp);
}

static void ber_output_sized_buffer(Visitor *v, void **obj,
                                    const char *name, size_t elem_count,
                                    size_t elem_size, Error **errp)
{
    ber_output_fragment(v, BER_TYPE_OCTET_STRING,
                        *obj, elem_count, elem_size, errp);
}

static void ber_output_type_number(Visitor *v, double *obj, const char *name,
                                   Error **errp)
{
    BEROutputVisitor *aov = to_aov(v);
    GDoubleIEEE754 num;
    uint8_t first;
    struct ieee754_buffer number;

    /* encode it as fixed-width double */
    number.type = BER_TYPE_CLASS_APPLICATION | BER_TYPE_PRIMITIVE |
                  BER_TYPE_REAL;
    number.length = sizeof(number) - offsetof(struct ieee754_buffer, first);

    num.v_double = *obj;

    if (isnan(*obj)) {
        /* special encoding supported here; not found in spec. */
        first = 0x42;
    } else if (isinf(*obj)) {
        /* spec. 8.5.8 */
        if (num.mpn.sign) {
            first = 0x41; /* -oo */
        } else {
            first = 0x40; /* +oo */
        }
    } else {
        first = 0x80;
        if (num.mpn.sign) {
            first |= 0x40;
        }
        /* Base 2; 0 for scaling factor; 2nd and 3rd octet encode exp. */
        first |= 0x1;
    }

    number.first = first;
    number.exponent = cpu_to_be16(num.mpn.biased_exponent);
    number.mant_hi = cpu_to_be32(num.mpn.mantissa_high);
    number.mant_lo = cpu_to_be32(num.mpn.mantissa_low);

    if (qemu_write_bytes(aov->qfile, (uint8_t *)&number, sizeof(number)) !=
        sizeof(number)) {
        error_setg(errp, "QEMUFile error: Error while writing double.");
    }
}

void ber_output_visitor_cleanup(BEROutputVisitor *v)
{
    QStackEntry *e, *tmp;

    QTAILQ_FOREACH_SAFE(e, &v->stack, node, tmp) {
        QTAILQ_REMOVE(&v->stack, e, node);
        if (e->qfile) {
            qemu_fclose(e->qfile);
        }
        g_free(e);
    }

    g_free(v);
}


Visitor *ber_output_get_visitor(BEROutputVisitor *v)
{
    return &v->visitor;
}

BERLengthEncoding ber_output_set_length_encoding(BEROutputVisitor *v,
                                                 BERLengthEncoding le)
{
    BERLengthEncoding old = v->length_encoding;

    v->length_encoding = le;

    return old;
}

BEROutputVisitor *ber_output_visitor_new(QEMUFile *qfile,
                                         BERLengthEncoding le)
{
    BEROutputVisitor *v;

    v = g_malloc0(sizeof(*v));

    v->visitor.start_struct = ber_output_start_struct;
    v->visitor.end_struct = ber_output_end_struct;
    v->visitor.start_list = ber_output_start_list;
    v->visitor.next_list = ber_output_next_list;
    v->visitor.end_list = ber_output_end_list;
    v->visitor.start_optional = ber_output_start_optional;
    v->visitor.end_optional = ber_output_end_optional;
    v->visitor.type_int = ber_output_type_int;
    v->visitor.type_uint8 = ber_output_type_uint8;
    v->visitor.type_uint16 = ber_output_type_uint16;
    v->visitor.type_uint32 = ber_output_type_uint32;
    v->visitor.type_uint64 = ber_output_type_uint64;
    v->visitor.type_int8 = ber_output_type_int8;
    v->visitor.type_int16 = ber_output_type_int16;
    v->visitor.type_int32 = ber_output_type_int32;
    v->visitor.type_int64 = ber_output_type_int64;
    v->visitor.type_bool = ber_output_type_bool;
    v->visitor.type_str = ber_output_type_str;
    v->visitor.type_sized_buffer = ber_output_sized_buffer;
    v->visitor.type_number = ber_output_type_number;

    QTAILQ_INIT(&v->stack);
    v->qfile = qfile;
    v->length_encoding = le;

    return v;
}
