/* /kernel/ktest_loader.h */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/*
 * Adversarial cases for the CXEX runtime loader (kernel security review §14,
 * and the Phase 1 items in docs/planning/HARDENING_PLAN.md).
 *
 * Kept out of ktest.c because it is already 960 lines and these bring their own
 * image builder with them.
 */

#ifndef KTEST_LOADER_H
#define KTEST_LOADER_H

/* Every malformed image the loader must refuse, plus one well-formed image it
   must accept. 1 = all cases behaved, 0 = at least one did not. */
int ktest_loader_adversarial(void);

/* A read-only ring-3 page: user_ptr_readable must accept it and
   user_ptr_writable must refuse it. */
int ktest_user_ptr_writability(void);

#endif
