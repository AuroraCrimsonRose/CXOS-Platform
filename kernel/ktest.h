// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/ktest.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Kernel self-tests. kmain() brings up the subsystems, then calls ktest_run()
 * to exercise them and report pass/fail. Keeping the tests here (rather than in
 * kmain) keeps the boot entry point clean and makes the tests easy to extend or
 * gate behind a build flag later.
 *
 * Assumes all subsystems are already initialized by kmain: console, gdt, idt,
 * pmm, paging, heap, scheduler, timer, usermode.
 */

#ifndef KTEST_H
#define KTEST_H

/* run all kernel self-tests, logging concise pass/fail lines to the console. */
void ktest_run(void);

/* A narrowed boot (roadmap 2.8a′, `cxk test sabotage --narrow`): when the
 * system volume holds KTEST_ONLY_PATH with at least one pattern in it, only
 * the self-tests it names run; the rest are reported skipped. One pattern a
 * line, a test's name exactly, or its start followed by '*'; blank lines and
 * lines starting '#' are ignored, and so is a file larger than
 * KTEST_ONLY_CAP (every test runs). os/std/test.xfxn reads the same file by
 * the same rules for the X tests, and the kernel refuses a skip of a test
 * the file does not let go. A gate boot runs everything: `cxk verify-boot`
 * fails a boot with a skip unless told --narrowed. */
#define KTEST_ONLY_PATH "/System/Config/ktest.only"
#define KTEST_ONLY_CAP  2048u

/* 1 if the test called `name` runs in this boot. */
int  ktest_wanted(const char *name);
/* 1 if some test whose name starts `start` might run. */
int  ktest_wanted_prefix(const char *start);
/* Reports the test called `name` skipped: a result line of its own. */
void ktest_skip(const char *name);

#endif