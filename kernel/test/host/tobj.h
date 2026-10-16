// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/test/host/tobj.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The host tests' own object type, KOBJ_TEST, shared by every test file so
 * there is one registration whichever file runs first. Defined in
 * test_kobj.c. Each object counts its hooks.
 */
#ifndef KHOST_TOBJ_H
#define KHOST_TOBJ_H

#include "object/kobj.h"

#define TOBJ_R1     0x1u
#define TOBJ_R2     0x2u
#define TOBJ_R3     0x4u
#define TOBJ_RIGHTS (TOBJ_R1 | TOBJ_R2 | TOBJ_R3)   /* the type's rights_valid */

struct tobj {
    struct kobj hdr;
    int         last_handle_runs;
    int         destroy_runs;
};

/* Register KOBJ_TEST if no test has yet. */
void tobj_type(void);

/* Times kobj_panic has been called. */
extern int tobj_panics;

#endif
