// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/uid.c */
/* Aurora Tejeda / CATX Systems */
/* User / system identity - reports the current process's owning UID. */

#include "uid.h"
#include "sched.h"

/* current_uid() reads the owning UID of the currently scheduled process. The
   scheduler exposes it via thread_current_uid(); before the scheduler is up (or
   for the bare kernel boot context) there is no "process" yet, so we report
   SYSTEM - the kernel itself is the machine identity. */
uid_t current_uid(void) {
    return thread_current_uid();
}

int is_system(void) {
    return current_uid() == UID_SYSTEM;
}

const char *uid_name(uid_t uid) {
    if (uid == UID_SYSTEM)  return "SYSTEM";
    if (uid == UID_INVALID) return "(none)";
    /* No account layer yet: render a stable generic label. A real name table
       arrives with the user/account subsystem. */
    static char buf[16];
    /* build "user<N>" without snprintf (freestanding) */
    int p = 0;
    buf[p++] = 'u'; buf[p++] = 's'; buf[p++] = 'e'; buf[p++] = 'r';
    /* decimal of uid */
    char tmp[10]; int t = 0;
    uint32_t v = uid;
    if (v == 0) tmp[t++] = '0';
    while (v && t < 10) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
    while (t > 0 && p < (int)sizeof(buf) - 1) buf[p++] = tmp[--t];
    buf[p] = '\0';
    return buf;
}