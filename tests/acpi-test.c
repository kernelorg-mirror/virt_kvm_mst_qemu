/*
 * Boot order test cases.
 *
 * Copyright (c) 2013 Red Hat Inc.
 *
 * Authors:
 *  Markus Armbruster <armbru@redhat.com>,
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include <string.h>
#include <stdio.h>
#include <glib.h>
#include "libqtest.h"

typedef struct {
    const char *args;
    uint64_t expected_boot;
    uint64_t expected_reboot;
} boot_order_test;

#define LOW(x) ((x) & 0xff)
#define HIGH(x) ((x) >> 8)

#define SIGNATURE 0xdead
#define SIGNATURE_OFFSET 0x10
#define BOOT_SECTOR_ADDRESS 0x7c00

static uint8_t boot_sector[0x200] = {
    /* 7c00: mov $0xdead,%ax */
    [0x00] = 0xb8,
    [0x01] = LOW(SIGNATURE),
    [0x02] = HIGH(SIGNATURE),
    /* 7c03:  mov %ax,0x7c10 */
    [0x03] = 0xa3,
    [0x04] = LOW(BOOT_SECTOR_ADDRESS + SIGNATURE_OFFSET),
    [0x05] = HIGH(BOOT_SECTOR_ADDRESS + SIGNATURE_OFFSET),
    /* 7c06: hlt */
    [0x06] = 0xf4,                           
    /* 7c07: jmp 0x7c06=0x7c09-3   */
    [0x07] = 0xeb,
    [0x08] = LOW(-3),
    /* We mov 0xdead here: set value to make debugging easier */
    [SIGNATURE_OFFSET] = LOW(0xface),
    [SIGNATURE_OFFSET + 1] = HIGH(0xface),
    /* End of boot sector marker */
    [0x1FE] = 0x55,
    [0x1FF] = 0xAA,
};

static void test_acpi(void)
{
    const char *disk = "tests/acpi-test-disk.raw";
    FILE *f = fopen(disk, "w");
    char *args;
    uint8_t signature_low;
    uint8_t signature_high;
    uint16_t signature;
    int i;

    fwrite(boot_sector, 1, sizeof boot_sector, f);
    fclose(f);

    args = g_strdup_printf("-net none -display none %s", disk);
    qtest_start(args);

   /* Wait at most 10 seconds */
#define TEST_DELAY (1 * G_USEC_PER_SEC / 10)
#define TEST_CYCLES (10 * G_USEC_PER_SEC / TEST_DELAY)

    for (i = 0; i < TEST_CYCLES; ++i) {
        signature_low = readb(SIGNATURE_OFFSET);
        signature_high = readl(SIGNATURE_OFFSET + 1);
        signature = (signature_high << 8) | signature_low;
        if (signature == SIGNATURE) {
            break;
        }
        g_usleep(TEST_DELAY);
    }
    g_assert_cmphex(signature, ==, SIGNATURE);
    qtest_quit(global_qtest);
    g_free(args);
}

int main(int argc, char *argv[])
{
    const char *arch = qtest_get_arch();

    g_test_init(&argc, &argv, NULL);

    if (strcmp(arch, "i386") == 0 || strcmp(arch, "x86_64") == 0) {
        qtest_add_func("acpi/pc", test_acpi);
    }
    return g_test_run();
}
