/*
 * QTest test cases for virtio balloon device
 *
 * Copyright (c) 2024 Gao Shiyuan <gaoshiyuan@baidu.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "libqos/pci-pc.h"
#include "libqos/malloc-pc.h"
#include "libqos/virtio-pci.h"
#include "standard-headers/linux/virtio_balloon.h"

#define BALLOON_TIMEOUT_US  (30 * 1000 * 1000)

#define BALLOON_PAGE_SIZE   (1 << VIRTIO_BALLOON_PFN_SHIFT)

/*
 * https://gitlab.com/qemu-project/qemu/-/issues/2576
 * Used to trigger:
 *   virtio_address_space_lookup: Assertion `mrs.mr' failed.
 */
static void oss_fuzz_71649(void)
{
    QTestState *s = qtest_init("-device virtio-balloon -machine q35"
                               " -nodefaults");

    qtest_outl(s, 0xcf8, 0x80000890);
    qtest_outl(s, 0xcfc, 0x2);
    qtest_outl(s, 0xcf8, 0x80000891);
    qtest_inl(s, 0xcfc);
    qtest_quit(s);
}

static void query_stats(void)
{
    QTestState *s = qtest_init("-device virtio-balloon,id=balloon"
                               " -nodefaults");
    QDict *ret = qtest_qmp_assert_success_ref(
        s,
        "{ 'execute': 'qom-get', 'arguments': "     \
        "{ 'path': '/machine/peripheral/balloon', " \
        "  'property': 'guest-stats' } }");
    QDict *stats = qdict_get_qdict(ret, "stats");

    /* We expect 1 entry in the dict for each known kernel stat */
    assert(qdict_size(stats) == VIRTIO_BALLOON_S_NR);

    qobject_unref(ret);
    qtest_quit(s);
}

/*
 * Common setup for tests that need virtio feature negotiation
 * and virtqueue access on a PCI balloon device.
 */
typedef struct {
    QTestState *qts;
    QGuestAllocator alloc;
    QPCIBus *pcibus;
    QVirtioPCIDevice *dev;
} TestState;

static void balloon_pci_setup(TestState *s, const char *extra_args)
{
    char *cmdline = g_strdup_printf("-machine q35 -nodefaults"
                                   " -device virtio-balloon,addr=04.0%s%s",
                                   extra_args ? "," : "",
                                   extra_args ? extra_args : "");
    QPCIAddress addr = { .devfn = QPCI_DEVFN(4, 0) };

    s->qts = qtest_init(cmdline);
    g_free(cmdline);

    pc_alloc_init(&s->alloc, s->qts, 0);
    s->pcibus = qpci_new_pc(s->qts, &s->alloc);
    s->dev = virtio_pci_new(s->pcibus, &addr);
    g_assert_nonnull(s->dev);

    qvirtio_pci_device_enable(s->dev);
    qvirtio_start_device(&s->dev->vdev);
}

static void balloon_pci_teardown(TestState *s, QVirtQueue **vqs,
                                 int nvqs)
{
    int i;

    for (i = 0; i < nvqs; i++) {
        qvirtqueue_cleanup(s->dev->vdev.bus, vqs[i], &s->alloc);
    }
    qvirtio_pci_device_disable(s->dev);
    g_free(s->dev);
    qpci_free_pc(s->pcibus);
    alloc_destroy(&s->alloc);
    qtest_quit(s->qts);
}

/*
 * Negotiate features, set up virtqueues, and mark driver OK.
 * Returns negotiated features.
 */
static uint64_t balloon_negotiate_and_setup(TestState *s,
                                            uint64_t wanted,
                                            QVirtQueue **vqs, int nvqs)
{
    uint64_t features;

    features = qvirtio_get_features(&s->dev->vdev);
    g_assert(features & (1ull << VIRTIO_F_VERSION_1));
    features &= ~QVIRTIO_F_BAD_FEATURE;
    features &= wanted;
    qvirtio_set_features(&s->dev->vdev, features);

    for (int i = 0; i < nvqs; i++) {
        vqs[i] = qvirtqueue_setup(&s->dev->vdev, &s->alloc, i);
    }

    qvirtio_set_driver_ok(&s->dev->vdev);
    return features;
}

/*
 * VIRTIO_BALLOON_F_DEVICE_INIT_ON_INFLATE: inflate pages and verify
 * that the device returns a bitmap with bits set, and that the pages
 * are actually zeroed.
 */
static void test_inflate_device_init(void)
{
    TestState s;
    QVirtQueue *vqs[3];
    uint64_t features;
    uint32_t free_head;
    uint32_t used_len;
    uint64_t pfn_addr, bitmap_addr;
    uint32_t pfn;
    uint8_t bitmap;
    uint8_t pattern[4096];

    balloon_pci_setup(&s, "device-init-on-inflate=on");

    features = balloon_negotiate_and_setup(&s,
        ~0ull & ~(1ull << VIRTIO_BALLOON_F_PAGE_POISON),
        vqs, 3);
    g_assert(features & (1ull << VIRTIO_BALLOON_F_DEVICE_INIT_ON_INFLATE));

    uint64_t test_gpa = guest_alloc(&s.alloc, BALLOON_PAGE_SIZE);
    uint32_t test_pfn = test_gpa >> VIRTIO_BALLOON_PFN_SHIFT;

    /* Fill a guest page with non-zero data */
    memset(pattern, 0xAA, sizeof(pattern));
    qtest_memwrite(s.qts, test_gpa, pattern, sizeof(pattern));

    /* Build descriptor chain: out (PFN array) + in (bitmap) */
    pfn_addr = guest_alloc(&s.alloc, sizeof(pfn));
    bitmap_addr = guest_alloc(&s.alloc, 1);

    pfn = cpu_to_le32(test_pfn);
    qtest_memwrite(s.qts, pfn_addr, &pfn, sizeof(pfn));
    qtest_memset(s.qts, bitmap_addr, 0, 1);

    free_head = qvirtqueue_add(s.qts, vqs[0], pfn_addr, sizeof(pfn),
                               false, true);
    qvirtqueue_add(s.qts, vqs[0], bitmap_addr, 1, true, false);
    qvirtqueue_kick(s.qts, &s.dev->vdev, vqs[0], free_head);

    qvirtio_wait_used_elem(s.qts, &s.dev->vdev, vqs[0], free_head,
                           &used_len, BALLOON_TIMEOUT_US);

    /* 1 PFN -> bitmap is DIV_ROUND_UP(1, 8) = 1 byte */
    g_assert_cmpuint(used_len, ==, 1);

    /* The page should now be zeroed */
    qtest_memread(s.qts, test_gpa, pattern, sizeof(pattern));
    for (int i = 0; i < 4096; i++) {
        if (pattern[i] != 0) {
            g_test_message("Page not zeroed at offset %d: 0x%02x", i,
                           pattern[i]);
            g_assert_cmpuint(pattern[i], ==, 0);
        }
    }

    /* Bitmap bit 0 should be set */
    qtest_memread(s.qts, bitmap_addr, &bitmap, 1);
    g_assert_cmphex(bitmap & 1, ==, 1);

    guest_free(&s.alloc, pfn_addr);
    guest_free(&s.alloc, bitmap_addr);
    balloon_pci_teardown(&s, vqs, 3);
}

/*
 * Baseline: inflate without DEVICE_INIT_ON_INFLATE.
 * No bitmap is returned, used length is 0.
 */
static void test_inflate_no_device_init(void)
{
    TestState s;
    QVirtQueue *vqs[3];
    uint64_t features;
    uint32_t free_head;
    uint32_t used_len;
    uint64_t pfn_addr;
    uint32_t pfn;

    balloon_pci_setup(&s, NULL);

    features = balloon_negotiate_and_setup(&s, ~0ull, vqs, 3);
    g_assert(!(features & (1ull << VIRTIO_BALLOON_F_DEVICE_INIT_ON_INFLATE)));

    uint64_t test_gpa = guest_alloc(&s.alloc, BALLOON_PAGE_SIZE);
    uint32_t test_pfn = test_gpa >> VIRTIO_BALLOON_PFN_SHIFT;

    pfn_addr = guest_alloc(&s.alloc, sizeof(pfn));
    pfn = cpu_to_le32(test_pfn);
    qtest_memwrite(s.qts, pfn_addr, &pfn, sizeof(pfn));

    free_head = qvirtqueue_add(s.qts, vqs[0], pfn_addr, sizeof(pfn),
                               false, false);
    qvirtqueue_kick(s.qts, &s.dev->vdev, vqs[0], free_head);

    qvirtio_wait_used_elem(s.qts, &s.dev->vdev, vqs[0], free_head,
                           &used_len, BALLOON_TIMEOUT_US);

    g_assert_cmpuint(used_len, ==, 0);

    guest_free(&s.alloc, pfn_addr);
    balloon_pci_teardown(&s, vqs, 3);
}

/*
 * VIRTIO_BALLOON_F_DEVICE_INIT_REPORTED: report pages and verify
 * they are zeroed and used_len reflects the discarded size.
 *
 * VQ layout without free-page-hint:
 *   0=inflate, 1=deflate, 2=stats, 3=reporting
 */
static void test_report_device_init(void)
{
    TestState s;
    QVirtQueue *vqs[4];
    uint64_t features;
    uint32_t free_head;
    uint32_t used_len;
    uint8_t pattern[4096];

    balloon_pci_setup(&s,
                      "free-page-reporting=on,x-device-init-reported=on");

    features = balloon_negotiate_and_setup(&s,
        ~0ull & ~(1ull << VIRTIO_BALLOON_F_PAGE_POISON),
        vqs, 4);
    g_assert(features & (1ull << VIRTIO_BALLOON_F_REPORTING));
    g_assert(features & (1ull << VIRTIO_BALLOON_F_DEVICE_INIT_REPORTED));

    uint64_t test_gpa = guest_alloc(&s.alloc, BALLOON_PAGE_SIZE);

    /* Fill the target page with non-zero data */
    memset(pattern, 0xBB, sizeof(pattern));
    qtest_memwrite(s.qts, test_gpa, pattern, sizeof(pattern));

    /* Reporting VQ uses writable (in) descriptors */
    free_head = qvirtqueue_add(s.qts, vqs[3], test_gpa, 4096, true, false);
    qvirtqueue_kick(s.qts, &s.dev->vdev, vqs[3], free_head);

    qvirtio_wait_used_elem(s.qts, &s.dev->vdev, vqs[3], free_head,
                           &used_len, BALLOON_TIMEOUT_US);

    /* Discard should succeed: used_len == page size */
    g_assert_cmpuint(used_len, ==, 4096);

    /* Page should be zeroed */
    qtest_memread(s.qts, test_gpa, pattern, sizeof(pattern));
    for (int i = 0; i < 4096; i++) {
        if (pattern[i] != 0) {
            g_test_message("Page not zeroed at offset %d: 0x%02x", i,
                           pattern[i]);
            g_assert_cmpuint(pattern[i], ==, 0);
        }
    }

    balloon_pci_teardown(&s, vqs, 4);
}

/*
 * Baseline: reporting without DEVICE_INIT_REPORTED.
 * used_len should be 0.
 */
static void test_report_no_device_init(void)
{
    TestState s;
    QVirtQueue *vqs[4];
    uint64_t features;
    uint32_t free_head;
    uint32_t used_len;

    balloon_pci_setup(&s,
                      "free-page-reporting=on,x-device-init-reported=off");

    features = balloon_negotiate_and_setup(&s, ~0ull, vqs, 4);
    g_assert(features & (1ull << VIRTIO_BALLOON_F_REPORTING));
    g_assert(!(features & (1ull << VIRTIO_BALLOON_F_DEVICE_INIT_REPORTED)));

    uint64_t test_gpa = guest_alloc(&s.alloc, BALLOON_PAGE_SIZE);

    free_head = qvirtqueue_add(s.qts, vqs[3], test_gpa, 4096, true, false);
    qvirtqueue_kick(s.qts, &s.dev->vdev, vqs[3], free_head);

    qvirtio_wait_used_elem(s.qts, &s.dev->vdev, vqs[3], free_head,
                           &used_len, BALLOON_TIMEOUT_US);

    g_assert_cmpuint(used_len, ==, 0);

    balloon_pci_teardown(&s, vqs, 4);
}

/*
 * Feature negotiation logic for x-device-init-reported:
 *   auto: offered when free-page-reporting is on
 *   off:  never offered
 */
static void test_device_init_reported_negotiation(void)
{
    uint64_t features;
    TestState s;

    /* auto + reporting=on -> feature offered */
    balloon_pci_setup(&s,
                      "free-page-reporting=on,x-device-init-reported=auto");
    features = qvirtio_get_features(&s.dev->vdev);
    g_assert(features & (1ull << VIRTIO_BALLOON_F_DEVICE_INIT_REPORTED));
    qvirtio_pci_device_disable(s.dev);
    g_free(s.dev);
    qpci_free_pc(s.pcibus);
    alloc_destroy(&s.alloc);
    qtest_quit(s.qts);

    /* auto + reporting=off -> feature not offered */
    balloon_pci_setup(&s,
                      "free-page-reporting=off,x-device-init-reported=auto");
    features = qvirtio_get_features(&s.dev->vdev);
    g_assert(!(features & (1ull << VIRTIO_BALLOON_F_DEVICE_INIT_REPORTED)));
    qvirtio_pci_device_disable(s.dev);
    g_free(s.dev);
    qpci_free_pc(s.pcibus);
    alloc_destroy(&s.alloc);
    qtest_quit(s.qts);

    /* off + reporting=on -> feature not offered */
    balloon_pci_setup(&s,
                      "free-page-reporting=on,x-device-init-reported=off");
    features = qvirtio_get_features(&s.dev->vdev);
    g_assert(!(features & (1ull << VIRTIO_BALLOON_F_DEVICE_INIT_REPORTED)));
    qvirtio_pci_device_disable(s.dev);
    g_free(s.dev);
    qpci_free_pc(s.pcibus);
    alloc_destroy(&s.alloc);
    qtest_quit(s.qts);
}

/*
 * Inflate multiple PFNs and verify the bitmap has a bit set for each.
 */
static void test_inflate_device_init_multi(void)
{
    TestState s;
    QVirtQueue *vqs[3];
    uint64_t features;
    uint32_t free_head;
    uint32_t used_len;
    uint64_t pfn_addr, bitmap_addr;
    uint32_t pfns[4];
    uint8_t bitmap;
    uint8_t pattern[4096];
    int i;

    balloon_pci_setup(&s, "device-init-on-inflate=on");

    features = balloon_negotiate_and_setup(&s,
        ~0ull & ~(1ull << VIRTIO_BALLOON_F_PAGE_POISON),
        vqs, 3);
    g_assert(features & (1ull << VIRTIO_BALLOON_F_DEVICE_INIT_ON_INFLATE));

    uint64_t test_gpa = guest_alloc(&s.alloc, 4 * BALLOON_PAGE_SIZE);

    /* Fill 4 consecutive guest pages */
    memset(pattern, 0xCC, sizeof(pattern));
    for (i = 0; i < 4; i++) {
        qtest_memwrite(s.qts, test_gpa + i * BALLOON_PAGE_SIZE,
                       pattern, sizeof(pattern));
    }

    pfn_addr = guest_alloc(&s.alloc, sizeof(pfns));
    bitmap_addr = guest_alloc(&s.alloc, 1);

    for (i = 0; i < 4; i++) {
        pfns[i] = cpu_to_le32((test_gpa >> VIRTIO_BALLOON_PFN_SHIFT) + i);
    }
    qtest_memwrite(s.qts, pfn_addr, pfns, sizeof(pfns));
    qtest_memset(s.qts, bitmap_addr, 0, 1);

    free_head = qvirtqueue_add(s.qts, vqs[0], pfn_addr, sizeof(pfns),
                               false, true);
    qvirtqueue_add(s.qts, vqs[0], bitmap_addr, 1, true, false);
    qvirtqueue_kick(s.qts, &s.dev->vdev, vqs[0], free_head);

    qvirtio_wait_used_elem(s.qts, &s.dev->vdev, vqs[0], free_head,
                           &used_len, BALLOON_TIMEOUT_US);

    /* 4 PFNs -> bitmap is DIV_ROUND_UP(4, 8) = 1 byte */
    g_assert_cmpuint(used_len, ==, 1);

    /* All 4 pages should be zeroed */
    for (i = 0; i < 4; i++) {
        qtest_memread(s.qts, test_gpa + i * BALLOON_PAGE_SIZE,
                      pattern, sizeof(pattern));
        for (int j = 0; j < 4096; j++) {
            if (pattern[j] != 0) {
                g_test_message("Page %d not zeroed at offset %d: 0x%02x",
                               i, j, pattern[j]);
                g_assert_cmpuint(pattern[j], ==, 0);
            }
        }
    }

    /* Low 4 bits of bitmap should all be set */
    qtest_memread(s.qts, bitmap_addr, &bitmap, 1);
    g_assert_cmphex(bitmap & 0xf, ==, 0xf);

    guest_free(&s.alloc, pfn_addr);
    guest_free(&s.alloc, bitmap_addr);
    balloon_pci_teardown(&s, vqs, 3);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("virtio-balloon/oss_fuzz_71649", oss_fuzz_71649);
    qtest_add_func("virtio-balloon/query-stats", query_stats);
    qtest_add_func("virtio-balloon/inflate-device-init",
                   test_inflate_device_init);
    qtest_add_func("virtio-balloon/inflate-no-device-init",
                   test_inflate_no_device_init);
    qtest_add_func("virtio-balloon/inflate-device-init-multi",
                   test_inflate_device_init_multi);
    qtest_add_func("virtio-balloon/report-device-init",
                   test_report_device_init);
    qtest_add_func("virtio-balloon/report-no-device-init",
                   test_report_no_device_init);
    qtest_add_func("virtio-balloon/device-init-reported-negotiation",
                   test_device_init_reported_negotiation);

    return g_test_run();
}
