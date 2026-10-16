// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/runner.c */
/* Aurora Tejeda / CATX Systems */
/*
 * Runs every registered host test, or those whose name contains the first
 * argument. Prints one line per failed check and a summary line that the
 * DevKit's KernelHostTests reads:
 *
 *   host tests: all N passed
 *   host tests: F of N failed
 *
 * The exit code is 0 only when every selected test passed AND at least one
 * test ran: a filter that matches nothing is a mistake, not a success.
 */
#include "khost.h"

#include <stdio.h>
#include <string.h>

static struct kh_test *head;
static int current_failed;

void kh_register(struct kh_test *t) {
    /* Constructors run in an unspecified order; keep the list sorted by name
       so the output is the same on every host. */
    struct kh_test **at = &head;
    while (*at && strcmp((*at)->name, t->name) < 0) at = &(*at)->next;
    t->next = *at;
    *at = t;
}

void kh_fail(const char *file, int line, const char *what) {
    printf("    FAIL %s:%d: %s\n", file, line, what);
    current_failed = 1;
}

void kh_fail_eq(const char *file, int line, const char *what,
                uint64_t got, uint64_t want) {
    printf("    FAIL %s:%d: %s (got 0x%llx, want 0x%llx)\n", file, line, what,
           (unsigned long long)got, (unsigned long long)want);
    current_failed = 1;
}

int main(int argc, char **argv) {
    const char *filter = argc > 1 ? argv[1] : NULL;
    int ran = 0, failed = 0;

    for (struct kh_test *t = head; t; t = t->next) {
        if (filter && !strstr(t->name, filter)) continue;
        current_failed = 0;
        t->fn();
        ran++;
        if (current_failed) {
            failed++;
            printf("[FAIL] %s\n", t->name);
        }
    }

    if (ran == 0) {
        printf("host tests: none matched%s%s\n", filter ? " " : "", filter ? filter : "");
        return 2;
    }
    if (failed) {
        printf("host tests: %d of %d failed\n", failed, ran);
        return 1;
    }
    printf("host tests: all %d passed\n", ran);
    return 0;
}
