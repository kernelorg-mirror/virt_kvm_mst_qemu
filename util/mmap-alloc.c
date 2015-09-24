/* 
 * Support for RAM backed by mmaped host memory.
 *
 * Copyright (c) 2015 Red Hat, Inc.
 *
 * Authors:
 *  Michael S. Tsirkin <mst@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 */
#include <qemu/mmap-alloc.h>
#include <sys/types.h>
#include <sys/mman.h>

void *qemu_ram_mmap(int fd, size_t size, size_t align)
{
    size_t total = size + align;
    void *ptr = mmap(0, total, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    size_t offset = QEMU_ALIGN_UP((uintptr_t)ptr, align) - (uintptr_t)ptr;
    void *ptr1;

    if (ptr == MAP_FAILED) {
        return NULL;
    }

    ptr1 = mmap(ptr + offset, size, PROT_READ | PROT_WRITE,
                MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE, fd, 0);
    if (ptr1 == MAP_FAILED) {
        munmap(ptr, total);
        return NULL;
    }

    ptr += offset;
    total -= offset;

    if (offset > 0) {
        munmap(ptr - offset, offset);
    }
    if (total > size + getpagesize()) {
        munmap(ptr + size + getpagesize(), total - size - getpagesize());
    }

    return ptr;
}

void qemu_ram_munmap(void *ptr, size_t size)
{
    if (ptr) {
        munmap(ptr, size + getpagesize());
    }
}
