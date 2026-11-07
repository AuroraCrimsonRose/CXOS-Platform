// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/test_cxfs.c */
/* Aurora Tejeda / CATX Systems */
/*
 * CXFS on the host: what the fuzz target (fuzz_cxfs.c) found, kept.
 *
 * Every input the fuzzer crashed on is a file in regress/cxfs, and the corpus
 * it grows from is corpus/cxfs; this replays all of them through cxfs_one,
 * the same body the fuzzer runs, so a fixed bug that comes back fails here
 * in seconds without a fuzzing run. The real seeds are also held to reaching
 * the code: a harness that stopped mounting them would pass every input.
 */
#include "khost.h"
#include "cxfs_one.h"
#include "hostdisk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const inputs[] = {
#include "cxfs_inputs.h"
    NULL
};

static uint8_t *load(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n > 0 ? (size_t)n : 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

static const char *base_name(const char *p) {
    const char *b = p;
    for (; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

/* Every regression input and every corpus input: no sanitizer report, no
   broken rule. */
KH_TEST(cxfs_inputs_all_hold) {
    int n = 0, regress = 0;
    for (int i = 0; inputs[i]; i++) {
        size_t size;
        uint8_t *buf = load(inputs[i], &size);
        if (!buf) { kh_fail(__FILE__, __LINE__, inputs[i]); continue; }
        const char *broken = cxfs_one(buf, size);
        if (broken) {
            printf("    %s: %s\n", base_name(inputs[i]), broken);
            KH_CHECK(broken == NULL);
        }
        if (strstr(inputs[i], "regress")) regress++;
        free(buf);
        n++;
    }
    KH_CHECK(n >= 3);                       /* the seeds are there */
    KH_CHECK_EQ(regress, CXFS_NREGRESS);   /* and the list is the directory */
    hd_reset();
}

/* The volume the build installs mounts both ways and its files read back:
   the target reaches the code it is meant to. */
KH_TEST(cxfs_real_seed_reaches_the_code) {
    int seen = 0;
    for (int i = 0; inputs[i]; i++) {
        if (!strstr(inputs[i], "real-partition-meta")) continue;
        size_t size;
        uint8_t *buf = load(inputs[i], &size);
        KH_CHECK(buf != NULL);
        if (!buf) continue;
        KH_CHECK(cxfs_one(buf, size) == NULL);
        KH_CHECK(cxfs_one_last.mounted_root);
        KH_CHECK(cxfs_one_last.mounted_second);
        KH_CHECK(cxfs_one_last.listed >= 4);
        KH_CHECK(cxfs_one_last.files_read >= 1);
        KH_CHECK_EQ(cxfs_one_last.written_back, 2);
        free(buf);
        seen++;
    }
    KH_CHECK_EQ(seen, 1);
    hd_reset();
}

/* The oracle itself: a volume that fits its superblock but not its device is
   refused, and one past block 0 is never read for it. */
KH_TEST(cxfs_volume_larger_than_its_device_is_refused) {
    for (int i = 0; inputs[i]; i++) {
        if (!strstr(inputs[i], "real-wholedisk-meta")) continue;
        size_t size;
        uint8_t *buf = load(inputs[i], &size);
        KH_CHECK(buf != NULL);
        if (!buf) continue;
        KH_CHECK(cxfs_one(buf, size) == NULL);
        KH_CHECK(cxfs_one_last.mounted_root);
        /* one sector short of the volume */
        uint32_t sectors = (uint32_t)buf[0] | (uint32_t)buf[1] << 8 |
                           (uint32_t)buf[2] << 16 | (uint32_t)buf[3] << 24;
        sectors -= 1;
        for (int b = 0; b < 4; b++) buf[b] = (uint8_t)(sectors >> (8 * b));
        KH_CHECK(cxfs_one(buf, size) == NULL);
        KH_CHECK(!cxfs_one_last.mounted_root);
        KH_CHECK(!cxfs_one_last.mounted_second);
        free(buf);
    }
    hd_reset();
}
