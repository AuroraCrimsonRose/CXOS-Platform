// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/uid.h */
/* Aurora Tejeda / CATX Systems */
/*
 * User / system identity - the foundation for CXFS v2 permissions.
 *
 * This module establishes *who* a process runs as. It is deliberately minimal:
 * it provides the User-0 = SYSTEM convention, lets each process carry an owning
 * UID, and reports the current process's UID. It does NOT yet define user
 * accounts, evaluate permissions, or enforce anything - those build on top of
 * this once the rest of the CXFS v2 model lands.
 *
 * Convention (per the CXFS v2 design):
 *   - UID 0 is SYSTEM: the machine identity / OS execution context. It is NOT a
 *     human account. Kernel threads and the boot context run as SYSTEM.
 *   - Human users have UIDs >= 1, assigned later by the user/account layer.
 *   - Privilege is ultimately determined by role/permission, NOT by the numeric
 *     UID - UID 0 being SYSTEM is a convention, not a magic privilege number.
 *     (For now SYSTEM is the only context with implicit override authority.)
 */

#ifndef UID_H
#define UID_H

#include <stdint.h>

typedef uint32_t uid_t;

#define UID_SYSTEM   0u           /* the OS / machine identity (User 0) */
#define UID_FIRST_USER 1u         /* lowest UID a human user may have */
#define UID_INVALID  0xFFFFFFFFu  /* sentinel for "no/unknown user" */

/* Invariant: a USER is always UID >= 1. UID 0 (SYSTEM) is the machine identity,
   never a human account, and a user process can never be created as - or
   changed to - UID 0. uid_is_user() is the one predicate that expresses this. */
static inline int uid_is_user(uid_t uid) {
    return uid >= UID_FIRST_USER && uid != UID_INVALID;
}

/* The UID the currently running process owns. Kernel context = UID_SYSTEM. */
uid_t current_uid(void);

/* Is the current context SYSTEM (User 0)? SYSTEM may override protections
   (e.g. write OS-critical files, override locks). */
int   is_system(void);

/* Human-readable name for a UID (e.g. "SYSTEM" for 0). For diagnostics/shell.
   Until a real account layer exists, non-system UIDs render as "user<N>". */
const char *uid_name(uid_t uid);

#endif