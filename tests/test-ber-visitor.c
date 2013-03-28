/*
 * BER Output Visitor unit-tests.
 *
 * Copyright (C) 2011 Red Hat Inc.
 * Copyright (C) 2011, 2012, 2013 IBM Corporation
 *
 * Authors:
 *  Luiz Capitulino <lcapitulino@redhat.com>
 *  Stefan Berger <stefanb@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include <glib.h>

#include "qemu-common.h"
#include "qapi/ber-output-visitor.h"
#include "qapi/ber-input-visitor.h"
#include "hw/hw.h"
#include "include/qapi/visitor.h"


typedef struct TestExpResult {
    const uint8_t *exp;
    size_t exp_len;
} TestExpResult;

typedef struct TestInputOutputVisitorData {
    QEMUFile *qoutfile;
    BEROutputVisitor *bov;
    Visitor *ov;

    QEMUFile *qinfile;
    BERInputVisitor *biv;
    Visitor *iv;

    const TestExpResult *ter;
} TestInputOutputVisitor;

static void test_visitor_dump(const uint8_t *exp, size_t exp_len,
                              const uint8_t *act, size_t act_len)
{
    size_t i;

    fprintf(stderr, "\nExpected output:");

    for (i = 0; i < exp_len; i++) {
        if ((i % 0x8) == 0) {
            fprintf(stderr, "\n    ");
        }
        fprintf(stderr, "0x%02x, ", exp[i]);
    }

    fprintf(stderr, "\nActual output:");

    for (i = 0; i < act_len; i++) {
        if ((i % 0x8) == 0) {
            fprintf(stderr, "\n    ");
        }
        fprintf(stderr, "0x%02x, ", act[i]);
    }
    fprintf(stderr, "\n");
}

static void test_visitor_check_result(const uint8_t *exp, size_t exp_len,
                                      const uint8_t *act, size_t act_len)
{
    size_t i;

    if (exp_len != act_len) {
        test_visitor_dump(exp, exp_len, act, act_len);
    }

    for (i = 0; i < exp_len; i++) {
        if (exp[i] != act[i]) {
            test_visitor_dump(exp, exp_len, act, act_len);
        }
    }
}

static void visitor_output_setup(TestInputOutputVisitor *data,
                                 BERLengthEncoding ber_length_encoding,
                                 const TestExpResult *ter)
{
    data->ter = ter;

    data->qoutfile = qemu_bufopen("w", NULL);

    data->bov = ber_output_visitor_new(data->qoutfile, ber_length_encoding);
    g_assert(data->bov != NULL);

    data->ov = ber_output_get_visitor(data->bov);
    g_assert(data->ov != NULL);
}

static void visitor_output_setup_indefinite(TestInputOutputVisitor *data,
                                           const void *results)
{
    const TestExpResult *ter = results;

    visitor_output_setup(data, BER_LENGTH_ENCODING_INDEFINITE, ter);
}

static void visitor_output_setup_definite(TestInputOutputVisitor *data,
                                          const void *results)
{
    const TestExpResult *ter = results;

    visitor_output_setup(data, BER_LENGTH_ENCODING_DEFINITE, ter);
}

static void visitor_input_setup(TestInputOutputVisitor *data)
{
    const QEMUSizedBuffer *qsb = qemu_buf_get(data->qoutfile);
    QEMUSizedBuffer *new_qsb = qsb_clone(qsb);
    unsigned char *buffer = NULL;
    g_assert(new_qsb != NULL);

    qsb_get_buffer(qsb, 0, qsb_get_length(qsb), &buffer);
    test_visitor_check_result(data->ter->exp, data->ter->exp_len,
                              buffer, qsb_get_length(qsb));
    g_free(buffer);

    data->qinfile = qemu_bufopen("r", new_qsb);
    g_assert(data->qinfile != NULL);

    data->biv = ber_input_visitor_new(data->qinfile, ~0);
    g_assert(data->biv != NULL);

    data->iv = ber_input_get_visitor(data->biv);
    g_assert(data->iv != NULL);
}

static void visitor_output_teardown(TestInputOutputVisitor *data,
                                    const void *unused)
{
    ber_output_visitor_cleanup(data->bov);
    data->bov = NULL;
    data->ov = NULL;

    ber_input_visitor_cleanup(data->biv);
    data->biv = NULL;
    data->iv = NULL;

    if (data->qinfile) {
        qemu_fclose(data->qinfile);
    }
    if (data->qoutfile) {
        qemu_fclose(data->qoutfile);
    }
}

static const uint8_t test_visitor_int_exp[] = {
    0x42, 0x01, 0xd6,
};

static void test_visitor_out_int(TestInputOutputVisitor *data,
                                 const void *unused)
{
    int64_t value_in = -42, value_out = 0;
    Error *errp = NULL;

    visit_type_int(data->ov, &value_in, NULL, &errp);
    g_assert(error_is_set(&errp) == 0);

    visitor_input_setup(data);

    visit_type_int(data->iv, &value_out, NULL, &errp);
    g_assert_cmpint(value_out, ==, -42);
}

static const uint8_t test_visitor_boolean_exp[] = {
    0x01, 0x01, 0x01,
};

static void test_visitor_out_boolean(TestInputOutputVisitor *data,
                                     const void *unused)
{
    Error *errp = NULL;
    bool value_in = true, value_out = false;

    visit_type_bool(data->ov, &value_in, NULL, &errp);
    g_assert(error_is_set(&errp) == 0);

    visitor_input_setup(data);

    visit_type_bool(data->iv, &value_out, NULL, &errp);
    g_assert_cmpint(value_out, ==, true);

}

static const uint8_t test_visitor_string_exp[] = {
    0x16, 0x07, 0x51, 0x20, 0x45, 0x20, 0x4d, 0x20,
    0x55,
};

static void test_visitor_out_string(TestInputOutputVisitor *data,
                                    const void *unused)
{
    char *string_in = (char *) "Q E M U", *string_out = NULL;
    Error *errp = NULL;

    visit_type_str(data->ov, &string_in, NULL, &errp);
    g_assert(error_is_set(&errp) == 0);

    visitor_input_setup(data);

    visit_type_str(data->iv, &string_out, NULL, &errp);

    g_assert_cmpstr(string_out, ==, string_in);
    g_free(string_out);
}

static const uint8_t test_visitor_no_string_exp[] = {
    0x16, 0x00,
};

static void test_visitor_out_no_string(TestInputOutputVisitor *data,
                                       const void *unused)
{
    char *string_in = NULL, *string_out = NULL;
    Error *errp = NULL;

    /* A null string should return "" */
    visit_type_str(data->ov, &string_in, NULL, &errp);
    g_assert(error_is_set(&errp) == 0);
    g_assert(qsb_get_length(qemu_buf_get(data->qoutfile)) == 2);

    visitor_input_setup(data);

    visit_type_str(data->iv, &string_out, NULL, &errp);
    g_assert_cmpstr(string_out, ==, "");

    g_free(string_out);
}

#define SIMPLE_STRUCT_ARRAY_SIZE 5

typedef struct SimpleStruct {
    int64_t integer;
    bool boolean;
    char *string;
    uint32_t data[SIMPLE_STRUCT_ARRAY_SIZE];
    uint16_t *extdata;
} SimpleStruct;

static void visit_type_SimpleStruct(Visitor *v, SimpleStruct **obj,
                                    const char *name, Error **errp)
{
    uint32_t *data;
    uint16_t *extdata;
    visit_start_struct(v, (void **)obj, "SimpleStruct", name,
                       sizeof(SimpleStruct), errp);

    visit_type_int(v, &(*obj)->integer, "integer", errp);
    visit_type_bool(v, &(*obj)->boolean, "boolean", errp);
    visit_type_str(v, &(*obj)->string, "string", errp);

    data = (uint32_t *)&(*obj)->data;

    visit_type_sized_buffer(v, (void **)&data, "data",
                            SIMPLE_STRUCT_ARRAY_SIZE, sizeof(uint32_t), errp);

    extdata = (uint16_t *)(*obj)->extdata;

    visit_type_sized_buffer(v, (void **)&extdata, "extdata",
                            SIMPLE_STRUCT_ARRAY_SIZE, sizeof(uint16_t), errp);

    (*obj)->extdata = extdata;

    visit_end_struct(v, errp);
}

static const uint8_t test_visitor_struct_exp_indef[] = {
    0x30, 0x80, 0x42, 0x01, 0x2a, 0x01, 0x01, 0x00,
    0x16, 0x03, 0x66, 0x6f, 0x6f, 0x04, 0x14, 0x00,
    0x00, 0x11, 0x11, 0x00, 0x00, 0x22, 0x22, 0x00,
    0x00, 0x33, 0x33, 0x00, 0x00, 0x44, 0x44, 0x00,
    0x00, 0x55, 0x55, 0x04, 0x0a, 0x00, 0x01, 0x00,
    0x02, 0x00, 0x03, 0x00, 0x04, 0x00, 0x05, 0x00,
    0x00,
};

static const uint8_t test_visitor_struct_exp_def[] = {
    0x30, 0x2d, 0x42, 0x01, 0x2a, 0x01, 0x01, 0x00,
    0x16, 0x03, 0x66, 0x6f, 0x6f, 0x04, 0x14, 0x00,
    0x00, 0x11, 0x11, 0x00, 0x00, 0x22, 0x22, 0x00,
    0x00, 0x33, 0x33, 0x00, 0x00, 0x44, 0x44, 0x00,
    0x00, 0x55, 0x55, 0x04, 0x0a, 0x00, 0x01, 0x00,
    0x02, 0x00, 0x03, 0x00, 0x04, 0x00, 0x05,
};


static void test_visitor_out_struct(TestInputOutputVisitor *data,
                                    const void *unused)
{
    uint16_t extdata[] = {1, 2, 3, 4, 5};
    SimpleStruct test_struct = {
        .integer = 42,
        .boolean = false,
        .string = (char *)"foo",
        .data = {
            0x1111, 0x2222, 0x3333, 0x4444, 0x5555
        },
        .extdata = extdata,
    };
    SimpleStruct *p_in = &test_struct, *p_out = NULL;
    Error *errp = NULL;
    int i;

    visit_type_SimpleStruct(data->ov, &p_in, NULL, &errp);
    g_assert(!error_is_set(&errp));

    visitor_input_setup(data);

    visit_type_SimpleStruct(data->iv, &p_out, NULL, &errp);

    g_assert_cmpint(p_out->integer, ==, 42);
    g_assert_cmpint(p_out->boolean, ==, 0);
    g_assert_cmpstr(p_out->string, ==, "foo");

    for (i = 0; i < ARRAY_SIZE(test_struct.data); i++) {
        g_assert_cmpint(p_out->data[i], ==, (i + 1) * 0x1111);
    }

    for (i = 0; i < ARRAY_SIZE(extdata); i++) {
        g_assert_cmpint(p_out->extdata[i], ==, (i + 1));
    }

    g_free(p_out->extdata);
    g_free(p_out->string);
    g_free(p_out);
}

typedef struct NestedStruct {
    int64_t integer;
    bool boolean;
    SimpleStruct simpleStruct[2];
    char *string;
} NestedStruct;

static void visit_type_NestedStruct(Visitor *v, NestedStruct **obj,
                                    const char *name, Error **errp)
{
    unsigned int i;

    visit_start_struct(v, (void **)obj, "NestedStruct", name,
                       sizeof(NestedStruct), errp);

    visit_type_int(v, &(*obj)->integer, "integer", errp);
    visit_type_bool(v, &(*obj)->boolean, "boolean", errp);

    for (i = 0; i < ARRAY_SIZE((*obj)->simpleStruct); i++) {
        SimpleStruct *simple = &(*obj)->simpleStruct[i];
        visit_type_SimpleStruct(v, &simple, "SimpleStruct", errp);
    }

    visit_type_str(v, &(*obj)->string, "string", errp);

    visit_end_struct(v, errp);
}

static const uint8_t test_visitor_nested_struct_exp_indef[] = {
    0x30, 0x80, 0x42, 0x01, 0x2a, 0x01, 0x01, 0x01,
    0x30, 0x80, 0x42, 0x02, 0x03, 0xe8, 0x01, 0x01,
    0x00, 0x16, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f,
    0x04, 0x14, 0x00, 0x00, 0x11, 0x11, 0x00, 0x00,
    0x22, 0x22, 0x00, 0x00, 0x33, 0x33, 0x00, 0x00,
    0x44, 0x44, 0x00, 0x00, 0x55, 0x55, 0x04, 0x0a,
    0x00, 0x05, 0x00, 0x04, 0x00, 0x03, 0x00, 0x02,
    0x00, 0x01, 0x00, 0x00, 0x30, 0x80, 0x42, 0x02,
    0x07, 0xd0, 0x01, 0x01, 0x00, 0x16, 0x06, 0x77,
    0x6f, 0x72, 0x6c, 0x64, 0x21, 0x04, 0x14, 0x11,
    0x11, 0x11, 0x11, 0x22, 0x22, 0x22, 0x22, 0x33,
    0x33, 0x33, 0x33, 0x44, 0x44, 0x44, 0x44, 0x55,
    0x55, 0x55, 0x55, 0x04, 0x0a, 0x00, 0x55, 0x00,
    0x44, 0x00, 0x33, 0x00, 0x22, 0x00, 0x11, 0x00,
    0x00, 0x16, 0x03, 0x62, 0x61, 0x72, 0x00, 0x00,
};

static const uint8_t test_visitor_nested_struct_exp_def[] = {
    0x30, 0x70, 0x42, 0x01, 0x2a, 0x01, 0x01, 0x01,
    0x30, 0x30, 0x42, 0x02, 0x03, 0xe8, 0x01, 0x01,
    0x00, 0x16, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f,
    0x04, 0x14, 0x00, 0x00, 0x11, 0x11, 0x00, 0x00,
    0x22, 0x22, 0x00, 0x00, 0x33, 0x33, 0x00, 0x00,
    0x44, 0x44, 0x00, 0x00, 0x55, 0x55, 0x04, 0x0a,
    0x00, 0x05, 0x00, 0x04, 0x00, 0x03, 0x00, 0x02,
    0x00, 0x01, 0x30, 0x31, 0x42, 0x02, 0x07, 0xd0,
    0x01, 0x01, 0x00, 0x16, 0x06, 0x77, 0x6f, 0x72,
    0x6c, 0x64, 0x21, 0x04, 0x14, 0x11, 0x11, 0x11,
    0x11, 0x22, 0x22, 0x22, 0x22, 0x33, 0x33, 0x33,
    0x33, 0x44, 0x44, 0x44, 0x44, 0x55, 0x55, 0x55,
    0x55, 0x04, 0x0a, 0x00, 0x55, 0x00, 0x44, 0x00,
    0x33, 0x00, 0x22, 0x00, 0x11, 0x16, 0x03, 0x62,
    0x61, 0x72,
};

static void test_visitor_out_nested_struct(TestInputOutputVisitor *data,
                                           const void *unused)
{
    uint16_t extdata1[] = { 5, 4, 3, 2, 1};
    uint16_t extdata2[] = { 0x55, 0x44, 0x33, 0x22, 0x11};
    NestedStruct nested_struct = {
        .integer = 42,
        .boolean = true,
        .simpleStruct = {
            {
                .integer = 1000,
                .boolean = false,
                .string = (char *)"Hello",
                .data = {
                    0x1111, 0x2222, 0x3333, 0x4444, 0x5555
                },
                .extdata = extdata1,
            }, {
                .integer = 2000,
                .boolean = false,
                .string = (char *)"world!",
                .data = {
                    0x11111111, 0x22222222, 0x33333333, 0x44444444,
                    0x55555555
                },
                .extdata = extdata2,
            },
        },
        .string = (char *)"bar",
    };
    NestedStruct *p_in = &nested_struct, *p_out = NULL;
    Error *errp = NULL;
    unsigned int i;

    visit_type_NestedStruct(data->ov, &p_in, NULL, &errp);
    g_assert(!error_is_set(&errp));

    visitor_input_setup(data);

    visit_type_NestedStruct(data->iv, &p_out, NULL, &errp);

    g_assert_cmpint(p_out->integer, ==, 42);
    g_assert_cmpint(p_out->boolean, ==, true);

    g_assert_cmpint(p_out->simpleStruct[0].integer, ==, 1000);
    g_assert_cmpint(p_out->simpleStruct[0].boolean, ==, 0);
    g_assert_cmpstr(p_out->simpleStruct[0].string , ==, "Hello");
    for (i = 0; i < ARRAY_SIZE(nested_struct.simpleStruct[0].data); i++) {
        g_assert_cmpint(p_out->simpleStruct[0].data[i], ==, (i + 1) * 0x1111);
    }
    for (i = 0; i < ARRAY_SIZE(extdata1); i++) {
        g_assert_cmpint(p_out->simpleStruct[0].extdata[i], ==, (5 - i));
    }

    g_assert_cmpint(p_out->simpleStruct[1].integer, ==, 2000);
    g_assert_cmpint(p_out->simpleStruct[1].boolean, ==, 0);
    g_assert_cmpstr(p_out->simpleStruct[1].string , ==, "world!");
    for (i = 0; i < ARRAY_SIZE(nested_struct.simpleStruct[1].data); i++) {
        g_assert_cmpint(p_out->simpleStruct[1].data[i], ==,
                        (i + 1) * 0x11111111);
    }
    for (i = 0; i < ARRAY_SIZE(extdata2); i++) {
        g_assert_cmpint(p_out->simpleStruct[1].extdata[i], ==, (5 - i) * 0x11);
    }

    g_assert_cmpstr(p_out->string, ==, "bar");

    g_free(p_out->string);
    for (i = 0; i < ARRAY_SIZE(p_out->simpleStruct); i++) {
        g_free(p_out->simpleStruct[i].string);
        g_free(p_out->simpleStruct[i].extdata);
    }
    g_free(p_out);
}

typedef struct Optional {
    int64_t integer;
    bool has_array;
    SimpleStruct simpleStruct[2];
    char *string;
} Optional;

static void visit_type_Optional(Visitor *v, Optional **obj,
                                const char *name, Error **errp)
{
    unsigned int i;

    visit_start_struct(v, (void **)obj, "Optional", name,
                       sizeof(Optional), errp);

    visit_type_int(v, &(*obj)->integer, "integer", errp);

    visit_start_optional(v, &(*obj)->has_array, "has_array", errp);

    if ((*obj)->has_array) {
        for (i = 0; i < ARRAY_SIZE((*obj)->simpleStruct); i++) {
            SimpleStruct *simple = &(*obj)->simpleStruct[i];
            visit_type_SimpleStruct(v, &simple, "SimpleStruct", errp);
        }
    }

    visit_end_optional(v, errp);

    visit_type_str(v, &(*obj)->string, "string", errp);

    visit_end_struct(v, errp);
}

static const uint8_t test_visitor_optional_exp_indef[] = {
    0x30, 0x80, 0x42, 0x01, 0x2a, 0x3f, 0x21, 0x80,
    0x01, 0x01, 0x01, 0x30, 0x80, 0x42, 0x02, 0x03,
    0xe8, 0x01, 0x01, 0x00, 0x16, 0x05, 0x48, 0x65,
    0x6c, 0x6c, 0x6f, 0x04, 0x14, 0x00, 0x00, 0x11,
    0x11, 0x00, 0x00, 0x22, 0x22, 0x00, 0x00, 0x33,
    0x33, 0x00, 0x00, 0x44, 0x44, 0x00, 0x00, 0x55,
    0x55, 0x04, 0x0a, 0x00, 0x05, 0x00, 0x04, 0x00,
    0x03, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x30,
    0x80, 0x42, 0x02, 0x07, 0xd0, 0x01, 0x01, 0x00,
    0x16, 0x06, 0x77, 0x6f, 0x72, 0x6c, 0x64, 0x21,
    0x04, 0x14, 0x11, 0x11, 0x11, 0x11, 0x22, 0x22,
    0x22, 0x22, 0x33, 0x33, 0x33, 0x33, 0x44, 0x44,
    0x44, 0x44, 0x55, 0x55, 0x55, 0x55, 0x04, 0x0a,
    0x00, 0x55, 0x00, 0x44, 0x00, 0x33, 0x00, 0x22,
    0x00, 0x11, 0x00, 0x00, 0x00, 0x00, 0x16, 0x03,
    0x62, 0x61, 0x72, 0x00, 0x00,
};

static const uint8_t test_visitor_optional_exp_def[] = {
    0x30, 0x73, 0x42, 0x01, 0x2a, 0x3f, 0x21, 0x68,
    0x01, 0x01, 0x01, 0x30, 0x30, 0x42, 0x02, 0x03,
    0xe8, 0x01, 0x01, 0x00, 0x16, 0x05, 0x48, 0x65,
    0x6c, 0x6c, 0x6f, 0x04, 0x14, 0x00, 0x00, 0x11,
    0x11, 0x00, 0x00, 0x22, 0x22, 0x00, 0x00, 0x33,
    0x33, 0x00, 0x00, 0x44, 0x44, 0x00, 0x00, 0x55,
    0x55, 0x04, 0x0a, 0x00, 0x05, 0x00, 0x04, 0x00,
    0x03, 0x00, 0x02, 0x00, 0x01, 0x30, 0x31, 0x42,
    0x02, 0x07, 0xd0, 0x01, 0x01, 0x00, 0x16, 0x06,
    0x77, 0x6f, 0x72, 0x6c, 0x64, 0x21, 0x04, 0x14,
    0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x22, 0x22,
    0x33, 0x33, 0x33, 0x33, 0x44, 0x44, 0x44, 0x44,
    0x55, 0x55, 0x55, 0x55, 0x04, 0x0a, 0x00, 0x55,
    0x00, 0x44, 0x00, 0x33, 0x00, 0x22, 0x00, 0x11,
    0x16, 0x03, 0x62, 0x61, 0x72,
};

static void test_visitor_out_optional(TestInputOutputVisitor *data,
                                      const void *unused)
{
    uint16_t extdata1[] = { 5, 4, 3, 2, 1};
    uint16_t extdata2[] = { 0x55, 0x44, 0x33, 0x22, 0x11};
    Optional optional_struct = {
        .integer = 42,
        .has_array = true,
        .simpleStruct = {
            {
                .integer = 1000,
                .boolean = false,
                .string = (char *)"Hello",
                .data = {
                    0x1111, 0x2222, 0x3333, 0x4444, 0x5555
                },
                .extdata = extdata1,
            }, {
                .integer = 2000,
                .boolean = false,
                .string = (char *)"world!",
                .data = {
                    0x11111111, 0x22222222, 0x33333333, 0x44444444,
                    0x55555555
                },
                .extdata = extdata2,
            },
        },
        .string = (char *)"bar",
    };
    Optional *p_in = &optional_struct, *p_out = NULL;
    Error *errp = NULL;
    unsigned int i;

    visit_type_Optional(data->ov, &p_in, NULL, &errp);
    g_assert(!error_is_set(&errp));

    visitor_input_setup(data);

    visit_type_Optional(data->iv, &p_out, NULL, &errp);

    g_assert_cmpint(p_out->integer, ==, 42);
    g_assert_cmpint(p_out->has_array, ==, true);

    g_assert_cmpint(p_out->simpleStruct[0].integer, ==, 1000);
    g_assert_cmpint(p_out->simpleStruct[0].boolean, ==, 0);
    g_assert_cmpstr(p_out->simpleStruct[0].string , ==, "Hello");
    for (i = 0; i < ARRAY_SIZE(optional_struct.simpleStruct[0].data); i++) {
        g_assert_cmpint(p_out->simpleStruct[0].data[i], ==, (i + 1) * 0x1111);
    }
    for (i = 0; i < ARRAY_SIZE(extdata1); i++) {
        g_assert_cmpint(p_out->simpleStruct[0].extdata[i], ==, (5 - i));
    }

    g_assert_cmpint(p_out->simpleStruct[1].integer, ==, 2000);
    g_assert_cmpint(p_out->simpleStruct[1].boolean, ==, 0);
    g_assert_cmpstr(p_out->simpleStruct[1].string , ==, "world!");
    for (i = 0; i < ARRAY_SIZE(optional_struct.simpleStruct[1].data); i++) {
        g_assert_cmpint(p_out->simpleStruct[1].data[i], ==,
                        (i + 1) * 0x11111111);
    }
    for (i = 0; i < ARRAY_SIZE(extdata2); i++) {
        g_assert_cmpint(p_out->simpleStruct[1].extdata[i], ==, (5 - i) * 0x11);
    }

    g_assert_cmpstr(p_out->string, ==, "bar");

    g_free(p_out->string);
    for (i = 0; i < ARRAY_SIZE(p_out->simpleStruct); i++) {
        g_free(p_out->simpleStruct[i].string);
        g_free(p_out->simpleStruct[i].extdata);
    }
    g_free(p_out);
}

typedef struct SimpleStringList {
    char *value;
    struct SimpleStringList *next;
} SimpleStringList;

static void visit_type_SimpleStringList(Visitor *m, SimpleStringList ** obj,
                                  const char *name, Error **errp)
{
    GenericList *i, **head = (GenericList **)obj;

    visit_start_list(m, name, errp);

    for (*head = i = visit_next_list(m, head, errp);
         i;
         i = visit_next_list(m, &i, errp)) {
        SimpleStringList *native_i = (SimpleStringList *)i;
        visit_type_str(m, &native_i->value, NULL, errp);
    }

    visit_end_list(m, errp);
}

static const uint8_t test_visitor_list_exp_indef[] = {
    0x3f, 0x20, 0x80, 0x16, 0x05, 0x68, 0x65, 0x6c,
    0x6c, 0x6f, 0x16, 0x05, 0x77, 0x6f, 0x72, 0x6c,
    0x64, 0x00, 0x00,
};

static const uint8_t test_visitor_list_exp_def[] = {
    0x3f, 0x20, 0x0e, 0x16, 0x05, 0x68, 0x65, 0x6c,
    0x6c, 0x6f, 0x16, 0x05, 0x77, 0x6f, 0x72, 0x6c,
    0x64,
};

static void test_visitor_out_list(TestInputOutputVisitor *data,
                                  const void *unused)
{
    SimpleStringList test_list = {
        .value = (char *)"hello",
        .next = &(SimpleStringList) {
            .value = (char *)"world",
            .next = NULL,
        },
    };
    SimpleStringList *p_in = &test_list, *p_out = NULL, *iter = NULL, *_iter;
    Error *errp = NULL;

    visit_type_SimpleStringList(data->ov, &p_in, "SimpleStringList", &errp);
    g_assert(!error_is_set(&errp));

    visitor_input_setup(data);

    visit_type_SimpleStringList(data->iv, &p_out, "SimpleStringList", &errp);
    g_assert(!error_is_set(&errp));

    iter = p_out;
    g_assert_cmpstr(iter->value, ==, "hello");
    iter = iter->next;
    g_assert_cmpstr(iter->value, ==, "world");

    for (iter = p_out; iter; iter = _iter) {
        _iter = iter->next;
        g_free(iter->value);
        g_free(iter);
    }
}

typedef struct SimpleStructList {
    SimpleStruct *value;
    struct SimpleStructList *next;
} SimpleStructList;

static void visit_type_SimpleStructList(Visitor *m, SimpleStructList ** obj,
                                        const char *name, Error **errp)
{
    GenericList *i, **head = (GenericList **)obj;

    visit_start_list(m, name, errp);

    for (*head = i = visit_next_list(m, head, errp);
         i;
         i = visit_next_list(m, &i, errp)) {
        SimpleStructList *native_i = (SimpleStructList *)i;
        visit_type_SimpleStruct(m, &native_i->value, NULL, errp);
    }

    visit_end_list(m, errp);
}

static const uint8_t test_visitor_simplestruct_list_exp_indef[] = {
    0x3f, 0x20, 0x80, 0x30, 0x80, 0x42, 0x02, 0x03,
    0xe8, 0x01, 0x01, 0x00, 0x16, 0x05, 0x68, 0x65,
    0x6c, 0x6c, 0x6f, 0x04, 0x14, 0x00, 0x00, 0x11,
    0x11, 0x00, 0x00, 0x22, 0x22, 0x00, 0x00, 0x33,
    0x33, 0x00, 0x00, 0x44, 0x44, 0x00, 0x00, 0x55,
    0x55, 0x04, 0x0a, 0x00, 0x05, 0x00, 0x04, 0x00,
    0x03, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x30,
    0x80, 0x42, 0x02, 0x07, 0xd0, 0x01, 0x01, 0x01,
    0x16, 0x05, 0x77, 0x6f, 0x72, 0x6c, 0x64, 0x04,
    0x14, 0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x22,
    0x22, 0x33, 0x33, 0x33, 0x33, 0x44, 0x44, 0x44,
    0x44, 0x55, 0x55, 0x55, 0x55, 0x04, 0x0a, 0x00,
    0x55, 0x00, 0x44, 0x00, 0x33, 0x00, 0x22, 0x00,
    0x11, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t test_visitor_simplestruct_list_exp_def[] = {
    0x3f, 0x20, 0x64, 0x30, 0x30, 0x42, 0x02, 0x03,
    0xe8, 0x01, 0x01, 0x00, 0x16, 0x05, 0x68, 0x65,
    0x6c, 0x6c, 0x6f, 0x04, 0x14, 0x00, 0x00, 0x11,
    0x11, 0x00, 0x00, 0x22, 0x22, 0x00, 0x00, 0x33,
    0x33, 0x00, 0x00, 0x44, 0x44, 0x00, 0x00, 0x55,
    0x55, 0x04, 0x0a, 0x00, 0x05, 0x00, 0x04, 0x00,
    0x03, 0x00, 0x02, 0x00, 0x01, 0x30, 0x30, 0x42,
    0x02, 0x07, 0xd0, 0x01, 0x01, 0x01, 0x16, 0x05,
    0x77, 0x6f, 0x72, 0x6c, 0x64, 0x04, 0x14, 0x11,
    0x11, 0x11, 0x11, 0x22, 0x22, 0x22, 0x22, 0x33,
    0x33, 0x33, 0x33, 0x44, 0x44, 0x44, 0x44, 0x55,
    0x55, 0x55, 0x55, 0x04, 0x0a, 0x00, 0x55, 0x00,
    0x44, 0x00, 0x33, 0x00, 0x22, 0x00, 0x11,
};

static void test_visitor_out_simplestruct_list(TestInputOutputVisitor *data,
                                               const void *unused)
{
    uint16_t extdata1[] = { 5, 4, 3, 2, 1};
    uint16_t extdata2[] = { 0x55, 0x44, 0x33, 0x22, 0x11};
    SimpleStructList test_list = {
        .value = &(SimpleStruct) {
            .string = (char *)"hello",
            .integer = 1000,
            .boolean = false,
            .data = {
                0x1111, 0x2222, 0x3333, 0x4444, 0x5555
            },
            .extdata = extdata1,
        },
        .next = &(SimpleStructList) {
            .value = &(SimpleStruct) {
                .string = (char *)"world",
                .integer = 2000,
                .boolean = true,
                .data = {
                    0x11111111, 0x22222222, 0x33333333, 0x44444444,
                    0x55555555
                },
                .extdata = extdata2,
            },
            .next = NULL,
        },
    };
    SimpleStructList *p_in = &test_list, *p_out = NULL, *iter = NULL, *_iter;
    Error *errp = NULL;
    size_t i;

    visit_type_SimpleStructList(data->ov, &p_in, "SimpleStructList", &errp);
    g_assert(!error_is_set(&errp));

    visitor_input_setup(data);

    visit_type_SimpleStructList(data->iv, &p_out, "SimpleStructList", &errp);
    g_assert(!error_is_set(&errp));

    iter = p_out;
    g_assert_cmpstr(iter->value->string, ==, "hello");
    for (i = 0; i < ARRAY_SIZE(test_list.value->data); i++) {
        g_assert_cmpint(iter->value->data[i], ==, (i + 1) * 0x1111);
    }
    for (i = 0; i < ARRAY_SIZE(extdata1); i++) {
        g_assert_cmpint(iter->value->extdata[i], ==, (5 - i));
    }

    iter = iter->next;
    g_assert_cmpstr(iter->value->string, ==, "world");
    for (i = 0; i < ARRAY_SIZE(test_list.next->value->data); i++) {
        g_assert_cmpint(iter->value->data[i], ==, (i + 1) * 0x11111111);
    }
    for (i = 0; i < ARRAY_SIZE(extdata2); i++) {
        g_assert_cmpint(iter->value->extdata[i], ==, (5 - i) * 0x11);
    }

    for (iter = p_out; iter; iter = _iter) {
        _iter = iter->next;
        g_free(iter->value->string);
        g_free(iter->value->extdata);
        g_free(iter->value);
        g_free(iter);
    }
}

static void test_visitor_test_add(const char *testpath,
                         BERLengthEncoding ber_length_encoding,
                         const TestExpResult *ter,
                         void (*test_func)(TestInputOutputVisitor *data,
                                           const void *user_data))
{
    char path[64];
    void (*setup)(TestInputOutputVisitor *data, const void *unused);

    switch (ber_length_encoding) {
    case BER_LENGTH_ENCODING_DEFINITE:
        snprintf(path, sizeof(path), "/definite%s", testpath);
        setup = visitor_output_setup_definite;
        break;
    default:
        snprintf(path, sizeof(path), "/indefinite%s", testpath);
        setup = visitor_output_setup_indefinite;
        break;
    }

    g_test_add(path, TestInputOutputVisitor, ter, setup,
               test_func, visitor_output_teardown);
}

struct test_data {
    const char *path;
    void (*test_func)(TestInputOutputVisitor *, const void *);
    TestExpResult ter[2];
};

#define _TEST_CASE(FUNC, POST1, POST2, PATH) \
    {\
        .path = PATH,\
        .test_func = test_visitor_out_## FUNC,\
        .ter = {\
            {\
                .exp = test_visitor_## FUNC ## _exp ## POST1,\
                .exp_len = sizeof(test_visitor_## FUNC ##_exp ## POST1),\
            }, {\
                .exp = test_visitor_## FUNC ##_exp ## POST2,\
                .exp_len = sizeof(test_visitor_## FUNC ##_exp ## POST2),\
            } \
        } \
    }

#define TEST_CASE(PATH, FUNC) \
    _TEST_CASE(FUNC, _indef, _def, PATH)

#define TEST_CASE_SIMPLE(PATH, FUNC) \
    _TEST_CASE(FUNC, , , PATH)

static const struct test_data tests[] = {
    TEST_CASE_SIMPLE("/visitor/output/int", int),
    TEST_CASE_SIMPLE("/visitor/output/bool", boolean),
    TEST_CASE_SIMPLE("/visitor/output/string", string),
    TEST_CASE_SIMPLE("/visitor/output/no-string", no_string),
    TEST_CASE("/visitor/output/struct", struct),
    TEST_CASE("/visitor/output/optional", optional),
    TEST_CASE("/visitor/output/nested-struct", nested_struct),
    TEST_CASE("/visitor/output/optional", optional),
    TEST_CASE("/visitor/output/list", list),
    TEST_CASE("/visitor/output/simplestruct_list", simplestruct_list),
};

int main(int argc, char **argv)
{
    BERLengthEncoding length_encoding[] =
        {BER_LENGTH_ENCODING_INDEFINITE, BER_LENGTH_ENCODING_DEFINITE};
    int i;
    int j;

    g_test_init(&argc, &argv, NULL);

    for (i = 0; i < ARRAY_SIZE(length_encoding); i++) {
        for (j = 0; j < ARRAY_SIZE(tests); j++) {
            test_visitor_test_add(tests[j].path, length_encoding[i],
                                  &tests[j].ter[i],
                                  tests[j].test_func);
        }
    }

    g_test_run();

    return 0;
}
