// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/test_ocache.c */
/* Aurora Tejeda / CATX Systems */
/*
 * The object caches (memman/ocache.h), on the host.
 *
 * The window is an arena here. A page the allocator gives back is poisoned
 * for AddressSanitizer, so touching a returned slab ends the run, which is
 * the host's version of the fault the kernel takes on an unmapped page. The
 * backend also counts its pages, so a test can require that every slab went
 * back.
 */
#include "khost.h"
#include "memman/ocache.h"
#include <string.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define OC_ASAN 1
#endif
#endif
#ifdef OC_ASAN
void __asan_poison_memory_region(void const volatile *addr, size_t size);
void __asan_unpoison_memory_region(void const volatile *addr, size_t size);
#define OC_POISON(a, n)   __asan_poison_memory_region((a), (n))
#define OC_UNPOISON(a, n) __asan_unpoison_memory_region((a), (n))
#else
#define OC_POISON(a, n)   ((void)(a), (void)(n))
#define OC_UNPOISON(a, n) ((void)(a), (void)(n))
#endif

#define WINDOW_BYTES ((size_t)OCACHE_SLOTS * OCACHE_SPAN)
#define WINDOW_PAGES (WINDOW_BYTES / OCACHE_PAGE)

static uint8_t   arena_raw[WINDOW_BYTES + OCACHE_SPAN];
static uintptr_t window;
static uint8_t   mapped[WINDOW_PAGES];
static int       backend_errors;   /* a page mapped twice, or unmapped unmapped */
static int       fail_maps;        /* the next map fails while set */

static int         oc_panics;
static const char *oc_last;

void ocache_panic(const char *why) { oc_panics++; oc_last = why; }

uintptr_t ocache_window(void) {
    if (!window) {
        window = ((uintptr_t)arena_raw + OCACHE_SPAN - 1) & ~(uintptr_t)(OCACHE_SPAN - 1);
        OC_POISON((void *)window, WINDOW_BYTES);
    }
    return window;
}

int ocache_map(uintptr_t va, uint32_t pages) {
    if (fail_maps) return -1;
    for (uint32_t p = 0; p < pages; p++) {
        size_t i = (va - window) / OCACHE_PAGE + p;
        if (mapped[i]) backend_errors++;
        mapped[i] = 1;
        OC_UNPOISON((void *)(va + p * OCACHE_PAGE), OCACHE_PAGE);
        /* fresh pages are not zero, so nothing may count on it */
        uint8_t *b = (uint8_t *)(va + p * OCACHE_PAGE);
        for (uint32_t k = 0; k < OCACHE_PAGE; k++) b[k] = 0xA5;
    }
    return 0;
}

void ocache_unmap(uintptr_t va, uint32_t pages) {
    for (uint32_t p = 0; p < pages; p++) {
        size_t i = (va - window) / OCACHE_PAGE + p;
        if (!mapped[i]) backend_errors++;
        mapped[i] = 0;
        OC_POISON((void *)(va + p * OCACHE_PAGE), OCACHE_PAGE);
    }
}

static uint32_t mapped_pages(void) {
    uint32_t n = 0;
    for (size_t i = 0; i < WINDOW_PAGES; i++) n += mapped[i];
    return n;
}

static size_t span_of(const void *p) {
    return ((uintptr_t)p - ocache_window()) / OCACHE_SPAN;
}

static int all_zero(const uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) if (p[i]) return 0;
    return 1;
}

KH_TEST(ocache_sizing) {
    struct ocache c;
    KH_CHECK_EQ(ocache_init(&c, "t", 48), 0);       /* an open file's size */
    KH_CHECK_EQ(c.size, 48); KH_CHECK_EQ(c.pages, 1); KH_CHECK_EQ(c.per_slab, 64);

    KH_CHECK_EQ(ocache_init(&c, "t", 100), 0);      /* rounds up to 16 */
    KH_CHECK_EQ(c.size, 112); KH_CHECK_EQ(c.pages, 1); KH_CHECK_EQ(c.per_slab, 36);

    KH_CHECK_EQ(ocache_init(&c, "t", 4140), 0);     /* an endpoint's size */
    KH_CHECK_EQ(c.size, 4144); KH_CHECK_EQ(c.pages, 8); KH_CHECK_EQ(c.per_slab, 7);

    KH_CHECK_EQ(ocache_init(&c, "t", OCACHE_SPAN), 0);
    KH_CHECK_EQ(c.pages, 8); KH_CHECK_EQ(c.per_slab, 1);

    KH_CHECK(ocache_init(&c, "t", OCACHE_SPAN + 1) < 0);
    KH_CHECK(ocache_init(&c, "t", 0) < 0);
}

/* Objects are distinct, aligned, zeroed and in the window; slabs come and go
   with them, and every page goes back. */
KH_TEST(ocache_slabs_come_and_go) {
    struct ocache c;
    KH_CHECK_EQ(ocache_init(&c, "t", 200), 0);
    uint32_t per = c.per_slab, n = per * 3 + 1;
    uint8_t *o[256];
    KH_CHECK(n <= 256);

    for (uint32_t i = 0; i < n; i++) {
        o[i] = ocache_alloc(&c);
        KH_CHECK(o[i] != NULL);
        if (!o[i]) return;
        KH_CHECK_EQ((uintptr_t)o[i] % OCACHE_ALIGN, 0);
        KH_CHECK(all_zero(o[i], c.size));
        KH_CHECK(ocache_owns(&c, o[i]));
        for (uint32_t j = 0; j < i; j++) KH_CHECK(o[j] != o[i]);
        for (uint32_t k = 0; k < c.size; k++) o[i][k] = (uint8_t)i;
    }
    KH_CHECK_EQ(ocache_live(&c), n);
    KH_CHECK_EQ(ocache_slabs(&c), 4);
    KH_CHECK_EQ(mapped_pages(), 4 * c.pages);

    /* each object kept its own bytes: none overlaps another */
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t k = 0; k < c.size; k++)
            if (o[i][k] != (uint8_t)i) { KH_CHECK(!"objects overlap"); break; }

    for (uint32_t i = 0; i < n; i++) ocache_free(&c, o[i]);
    KH_CHECK_EQ(ocache_live(&c), 0);
    KH_CHECK_EQ(ocache_slabs(&c), 0);
    KH_CHECK_EQ(mapped_pages(), 0);
    KH_CHECK(!ocache_owns(&c, o[0]));
    KH_CHECK_EQ(backend_errors, 0);
    KH_CHECK_EQ(oc_panics, 0);
}

/* Anything but a live object of this cache is refused, and refusing changes
   nothing. */
KH_TEST(ocache_refuses_bad_frees) {
    struct ocache a, b;
    KH_CHECK_EQ(ocache_init(&a, "a", 64), 0);
    KH_CHECK_EQ(ocache_init(&b, "b", 64), 0);
    uint8_t *x = ocache_alloc(&a), *y = ocache_alloc(&a), *z = ocache_alloc(&b);
    KH_CHECK(x && y && z);
    if (!x || !y || !z) return;
    int p0 = oc_panics;

    ocache_free(&a, x);
    ocache_free(&a, x);                     /* twice */
    KH_CHECK_EQ(oc_panics, p0 + 1);
    KH_CHECK(oc_last && strstr(oc_last, "already free"));
    ocache_free(&a, y + 8);                 /* inside an object */
    KH_CHECK_EQ(oc_panics, p0 + 2);
    KH_CHECK(oc_last && strstr(oc_last, "not the start"));
    ocache_free(&a, z);                     /* another cache's */
    KH_CHECK_EQ(oc_panics, p0 + 3);
    KH_CHECK(oc_last && strstr(oc_last, "another cache"));
    static uint8_t outside[64];
    ocache_free(&a, outside);               /* not the window at all */
    KH_CHECK_EQ(oc_panics, p0 + 4);
    KH_CHECK(oc_last && strstr(oc_last, "not an object cache address"));

    KH_CHECK_EQ(ocache_live(&a), 1);
    KH_CHECK_EQ(ocache_live(&b), 1);
    KH_CHECK(ocache_owns(&a, y));
    KH_CHECK(ocache_owns(&b, z));

    ocache_free(&a, y);
    ocache_free(&b, z);
    KH_CHECK_EQ(oc_panics, p0 + 4);
    KH_CHECK_EQ(mapped_pages(), 0);
    oc_panics = p0;
}

/* A write into a freed object is found when the object is handed out again,
   and the new owner still gets it zeroed. */
KH_TEST(ocache_catches_write_after_free) {
    struct ocache c;
    KH_CHECK_EQ(ocache_init(&c, "t", 64), 0);
    uint8_t *a = ocache_alloc(&c), *b = ocache_alloc(&c);   /* b keeps the slab */
    KH_CHECK(a && b);
    if (!a || !b) return;
    int p0 = oc_panics;

    ocache_free(&c, a);
    KH_CHECK_EQ(a[5], OCACHE_POISON);       /* freed memory is filled */
    a[5] = 0;                               /* the stale write */
    uint8_t *again = ocache_alloc(&c);
    KH_CHECK(again == a);                   /* lowest free object first */
    KH_CHECK_EQ(oc_panics, p0 + 1);
    KH_CHECK(oc_last && strstr(oc_last, "written after free"));
    KH_CHECK(again && all_zero(again, c.size));

    /* and a clean reuse is quiet */
    ocache_free(&c, again);
    again = ocache_alloc(&c);
    KH_CHECK_EQ(oc_panics, p0 + 1);

    ocache_free(&c, again);
    ocache_free(&c, b);
    KH_CHECK_EQ(mapped_pages(), 0);
    oc_panics = p0;
}

/* Two caches of the same size never share a span, so a freed object of one
   is never handed out by the other. */
KH_TEST(ocache_types_never_share_a_slab) {
    struct ocache a, b;
    KH_CHECK_EQ(ocache_init(&a, "a", 96), 0);
    KH_CHECK_EQ(ocache_init(&b, "b", 96), 0);
    void *pa[80], *pb[80];
    for (int i = 0; i < 80; i++) { pa[i] = ocache_alloc(&a); pb[i] = ocache_alloc(&b); }
    for (int i = 0; i < 80; i++)
        for (int j = 0; j < 80; j++)
            KH_CHECK(span_of(pa[i]) != span_of(pb[j]));

    void *gone = pa[3];
    ocache_free(&a, gone);
    void *nb = ocache_alloc(&b);
    KH_CHECK(nb != gone);
    KH_CHECK(span_of(nb) != span_of(pa[0]));

    for (int i = 0; i < 80; i++) { if (i != 3) ocache_free(&a, pa[i]); ocache_free(&b, pb[i]); }
    ocache_free(&b, nb);
    KH_CHECK_EQ(mapped_pages(), 0);
    KH_CHECK_EQ(oc_panics, 0);
}

/* Running out of pages or of spans is a NULL, and leaves nothing claimed. */
KH_TEST(ocache_running_out) {
    struct ocache c;
    KH_CHECK_EQ(ocache_init(&c, "t", OCACHE_SPAN), 0);     /* one per span */

    fail_maps = 1;
    KH_CHECK(ocache_alloc(&c) == NULL);
    fail_maps = 0;
    KH_CHECK_EQ(ocache_slabs(&c), 0);

    static void *o[OCACHE_SLOTS];
    for (int i = 0; i < OCACHE_SLOTS; i++) {
        o[i] = ocache_alloc(&c);
        KH_CHECK(o[i] != NULL);    /* every span, so the failed map gave its back */
    }
    KH_CHECK(ocache_alloc(&c) == NULL);
    KH_CHECK_EQ(ocache_slabs(&c), OCACHE_SLOTS);

    struct ocache d;
    KH_CHECK_EQ(ocache_init(&d, "d", 32), 0);
    KH_CHECK(ocache_alloc(&d) == NULL);                    /* no span left for d */

    for (int i = 0; i < OCACHE_SLOTS; i++) if (o[i]) ocache_free(&c, o[i]);
    KH_CHECK_EQ(mapped_pages(), 0);
    void *x = ocache_alloc(&d);
    KH_CHECK(x != NULL);
    if (x) ocache_free(&d, x);
    KH_CHECK_EQ(mapped_pages(), 0);
    KH_CHECK_EQ(backend_errors, 0);
}

static int count_live(void *obj, void *ctx) {
    (void)obj;
    (*(int *)ctx)++;
    return 0;
}
static int stop_at(void *obj, void *ctx) {
    return obj == ctx ? 7 : 0;
}

KH_TEST(ocache_walk_visits_the_live) {
    struct ocache c;
    KH_CHECK_EQ(ocache_init(&c, "t", 512), 0);
    void *o[20];
    for (int i = 0; i < 20; i++) o[i] = ocache_alloc(&c);
    for (int i = 0; i < 20; i += 3) ocache_free(&c, o[i]);   /* 7 freed */

    int n = 0;
    KH_CHECK_EQ(ocache_walk(&c, count_live, &n), 0);
    KH_CHECK_EQ(n, 13);
    KH_CHECK_EQ(ocache_walk(&c, stop_at, o[1]), 7);
    KH_CHECK_EQ(ocache_walk(&c, stop_at, o[0]), 0);          /* a freed one is not visited */

    for (int i = 0; i < 20; i++) if (i % 3) ocache_free(&c, o[i]);
    KH_CHECK_EQ(mapped_pages(), 0);
}

/* A model check: random allocations and frees over three caches. Each live
   object holds its own tag in every byte, and still does when freed; the
   slab count always equals the spans the live objects occupy. */
#define MODEL_MAX 300
KH_TEST(ocache_model) {
    static const uint32_t sizes[3] = { 40, 700, 4140 };
    struct ocache c[3];
    for (int k = 0; k < 3; k++) KH_CHECK_EQ(ocache_init(&c[k], "m", sizes[k]), 0);

    static uint8_t *obj[MODEL_MAX];
    static int      kind[MODEL_MAX];
    static uint8_t  tag[MODEL_MAX];
    int n = 0;
    uint64_t seed = 0x0CAC4E;
    int p0 = oc_panics;

    for (int step = 0; step < 20000; step++) {
        uint32_t r = kh_rand(&seed);
        if (n < MODEL_MAX && (n == 0 || r % 100 < 55)) {
            int k = (int)(kh_rand(&seed) % 3);
            uint8_t *p = ocache_alloc(&c[k]);
            KH_CHECK(p != NULL);
            if (!p) break;
            for (int i = 0; i < n; i++) KH_CHECK(obj[i] != p);
            KH_CHECK(all_zero(p, c[k].size));
            obj[n] = p; kind[n] = k; tag[n] = (uint8_t)(step | 1);
            for (uint32_t b = 0; b < c[k].size; b++) p[b] = tag[n];
            n++;
        } else {
            int i = (int)(kh_rand(&seed) % (uint32_t)n);
            for (uint32_t b = 0; b < c[kind[i]].size; b++)
                if (obj[i][b] != tag[i]) { KH_CHECK(!"object overwritten"); break; }
            ocache_free(&c[kind[i]], obj[i]);
            n--;
            obj[i] = obj[n]; kind[i] = kind[n]; tag[i] = tag[n];
        }

        if (step % 97 == 0) {
            for (int k = 0; k < 3; k++) {
                uint32_t live = 0, spans = 0;
                static uint8_t seen[OCACHE_SLOTS];
                for (int s = 0; s < OCACHE_SLOTS; s++) seen[s] = 0;
                for (int i = 0; i < n; i++)
                    if (kind[i] == k) {
                        live++;
                        if (!seen[span_of(obj[i])]++) spans++;
                    }
                KH_CHECK_EQ(ocache_live(&c[k]), live);
                KH_CHECK_EQ(ocache_slabs(&c[k]), spans);
            }
        }
    }
    while (n > 0) { n--; ocache_free(&c[kind[n]], obj[n]); }
    for (int k = 0; k < 3; k++) KH_CHECK_EQ(ocache_slabs(&c[k]), 0);
    KH_CHECK_EQ(mapped_pages(), 0);
    KH_CHECK_EQ(oc_panics, p0);
    KH_CHECK_EQ(backend_errors, 0);
}
