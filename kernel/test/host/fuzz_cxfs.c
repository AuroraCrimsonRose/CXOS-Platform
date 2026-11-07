// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/fuzz_cxfs.c */
/* Aurora Tejeda / CATX Systems */
/*
 * The CXFS fuzz target (CX_SAFETY.md 3.2): cxfs_sb_validate and the mount
 * path that trusts what it passes, under libFuzzer, AddressSanitizer and
 * UndefinedBehaviorSanitizer. The input format and the oracle are in
 * cxfs_one.h; a broken rule is a crash, so libFuzzer keeps the input.
 *
 *   ninja -C build/hosttest cxfs_fuzz
 *   build/hosttest/cxfs_fuzz -max_len=300000 -timeout=10 \
 *       build/fuzz-cxfs kernel/test/host/corpus/cxfs
 *
 * (the first directory is where new inputs go, the second the seeds; on
 * Windows clang's lib/windows on PATH, as for khost_tests). Every crash it
 * finds becomes a test in test_cxfs.c.
 */
#include "cxfs_one.h"

#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    const char *broken = cxfs_one(data, size);
    if (broken) {
        fprintf(stderr, "cxfs_fuzz: %s\n", broken);
        abort();
    }
    return 0;
}
