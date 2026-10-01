/* /kernel/ktest.h */
/* Aurora Tejeda / CATX Systems LLC */
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

#endif