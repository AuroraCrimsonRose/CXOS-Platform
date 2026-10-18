// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/drivers/iowin.c */
/* Aurora Tejeda / CATX Systems */
/* I/O windows (drivers/iowin.h). Host-tested with a recording port backend. */

#include "drivers/iowin.h"
#include "arch/pio.h"
#ifndef CXK_HOST_TEST
#include "logging.h"
#endif

uint32_t iowin_refusals = 0;

int iowin_port(struct iowin *w, uint32_t port, uint32_t size) {
    w->kind = IOWIN_NONE;
    if (!ARCH_HAS_PIO || size == 0 || port > 0x10000u || size > 0x10000u - port)
        return -1;
    w->base = port;
    w->size = size;
    w->warned = 0;
    w->kind = IOWIN_PORT;
    return 0;
}

int iowin_mmio(struct iowin *w, volatile void *base, uint32_t size) {
    w->kind = IOWIN_NONE;
    if (!base || size == 0 || (uintptr_t)base + (size - 1) < (uintptr_t)base)
        return -1;
    w->base = (uintptr_t)base;
    w->size = size;
    w->warned = 0;
    w->kind = IOWIN_MMIO;
    return 0;
}

/* The access [off, off + width) lies wholly inside an open window. Written so
   that neither side can overflow. */
static int inside(struct iowin *w, uint32_t off, uint32_t width) {
    if (w->kind != IOWIN_NONE && width <= w->size && off <= w->size - width)
        return 1;
    iowin_refusals++;
#ifndef CXK_HOST_TEST
    if (!w->warned)
        klog("IOWIN", SEV_WARN, "refused an access outside its window");
#endif
    w->warned = 1;
    return 0;
}

#if ARCH_HAS_PIO
#define PORT_IN(n, w, off)     arch_pio_in##n((uint16_t)((w)->base + (off)))
#define PORT_OUT(n, w, off, v) arch_pio_out##n((uint16_t)((w)->base + (off)), (v))
#else
#define PORT_IN(n, w, off)     ((uint##n##_t)~0u)
#define PORT_OUT(n, w, off, v) ((void)(v))
#endif

#define MMIO(n, w, off) (*(volatile uint##n##_t *)((w)->base + (off)))

#define IOWIN_ACCESSORS(n)                                                    \
uint##n##_t iowin_read##n(struct iowin *w, uint32_t off) {                    \
    if (!inside(w, off, n / 8)) return (uint##n##_t)~0u;                      \
    if (w->kind == IOWIN_PORT) return PORT_IN(n, w, off);                     \
    return MMIO(n, w, off);                                                   \
}                                                                             \
void iowin_write##n(struct iowin *w, uint32_t off, uint##n##_t v) {           \
    if (!inside(w, off, n / 8)) return;                                       \
    if (w->kind == IOWIN_PORT) PORT_OUT(n, w, off, v);                        \
    else MMIO(n, w, off) = v;                                                 \
}

IOWIN_ACCESSORS(8)
IOWIN_ACCESSORS(16)
IOWIN_ACCESSORS(32)
