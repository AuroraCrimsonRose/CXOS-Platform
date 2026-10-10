// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/exec.c */
/* Aurora Tejeda / CATX Systems */
/* cxex_exec: verify a signed image, decide its capabilities, and start it as a
   ring-3 scheduler thread. Non-blocking - returns the new pid. See exec.h. */

#include "exec.h"
#include "cxex.h"
#include "cxex_verify.h"
#include "caps.h"
#include "spawn.h"
#include "keyvault.h"
#include "logging.h"

/* policy layer: identity (CXEX type) + trust -> capability set. Consulted once
   here at the handoff; a valid signature does not itself grant authority. */
uint32_t caps_for(uint16_t type_code, int trusted) {
    if (!trusted) return 0;
    if (type_code == CXEX_TYPE_OS)     return GRANT_OS_BASELINE;      /* the broker */
    if (type_code == CXEX_TYPE_SYSTEM) return GRANT_SYSTEM_BASELINE;  /* OS-owned */
    if (type_code == CXEX_TYPE_USER)   return 0;                      /* an app */
    return 0;
}

/* The minimum trust a given tier may be launched at.
 *
 * Anything the operating system is MADE OF - the kernel, the boot chain, the
 * executive, a system program - is platform-signed or it does not run. There
 * is no publisher good enough to ship a replacement executive: a machine that
 * accepted one would be a machine whose owner could be changed by adding a
 * file to a directory.
 *
 * A user's program is the one place a second signer is allowed, and even there
 * the key has to be in the vault. What is refused today is everything below
 * that - unsigned, and signed by someone this machine has never been told to
 * believe.
 *
 * THE ADMIN OVERRIDE GOES HERE, and cannot be built yet. The intent is that a
 * user holding administrative permission may choose to run an unverified or
 * unsigned .xuex anyway, and that choice is theirs to make. There is no
 * account model to hang it on - uid.h has SYSTEM and users, nothing creates a
 * user, and nothing logs in - so the policy refuses for now rather than
 * pretending a permission exists. When accounts arrive, this is the one
 * function that changes. */
/* What a verification result admits as. Normally that is the result itself:
 * a trust level, or a negative error that refuses the image.
 *
 * A DEVELOPMENT kernel (built with -DDEV_UNSIGNED=ON) differs in exactly one
 * case: an image carrying no signature at all is admitted, at platform trust,
 * so an unsigned test build can boot its own executive and supervisor. Only
 * CXEX_VERIFY_UNSIGNED - never a tampered image, a forged or wrong signature,
 * or a file that is not a CXEX. The flag skips signing; it does not ignore
 * corruption. Every such admission is logged, so a dev kernel can never quietly
 * look like a release one.
 *
 * Kept a separate, pure function so the self-tests can hold both kinds of
 * build to their rule. */
int exec_admit(int verified) {
#ifdef CXK_DEV_UNSIGNED
    if (verified == CXEX_VERIFY_UNSIGNED) {
        klog("EXEC", SEV_WARN, "dev kernel: admitting an UNSIGNED image");
        return CX_TRUST_PLATFORM;
    }
#endif
    return verified;
}

static int min_trust_for(uint16_t type_code) {
    if (type_code == CXEX_TYPE_USER) return CX_TRUST_PUBLISHER;
    return CX_TRUST_PLATFORM;
}

int cxex_exec_as(const uint8_t *file, size_t len, uint32_t caps, struct endpoint *broker,
                 const char *args, uint32_t args_len) {
    /* INTEGRITY FIRST, before this code forms any opinion about the image.
       keyvault_trust_of establishes that the bytes are what the signer signed,
       and only then says who that was - so a negative here means unsigned,
       tampered, or not a CXEX at all, and all three are one answer: the
       signature check refused it. Parsing the header first instead would make
       a text file report "malformed" rather than "refused", which is the
       loader volunteering its opinion of bytes nobody has vouched for. */
    int trust = exec_admit(keyvault_trust_of(file, len));
    if (trust < 0) return CXEX_EXEC_VERIFY_FAILED;

    /* POLICY: only OS/SYSTEM/USER executables run in ring 3, and the tier
       decides how much signature the image needed. The header is parsed here
       rather than above because by now it is known to be signed bytes. */
    struct cxex_header h;
    if (cxex_parse_header(file, len, &h) != 0) return CXEX_EXEC_LOAD_FAILED;
    if (h.type_code != CXEX_TYPE_OS &&
        h.type_code != CXEX_TYPE_SYSTEM &&
        h.type_code != CXEX_TYPE_USER)
        return CXEX_EXEC_BAD_TYPE;

    if (trust < min_trust_for(h.type_code)) {
        klog("EXEC", SEV_WARN, "refused: signer not trusted for this tier");
        klog_child(keyvault_trust_name(trust));
        return CXEX_EXEC_VERIFY_FAILED;
    }

    /* Start it as a normal ring-3 thread: its own address space, kernel stack,
       and capability tier. The scheduler handles its CR3/esp0 like any process -
       no special-casing. */
    int pid = proc_start(file, len, caps, broker, args, args_len);
    return pid;   /* >= 0 pid, or a negative error */
}

int cxex_exec(const uint8_t *file, size_t len) {
    /* The kernel launching something by itself: nobody to attenuate from, so
       the image's own tier decides. A root executive has no broker (NULL).
       The header is parsed twice on this path - once here for the tier, once
       inside cxex_exec_as - which is a few hundred bytes of work on a path
       taken once per boot, and the alternative is duplicating the whole
       verify-and-check sequence to save it. */
    struct cxex_header h;
    if (cxex_parse_header(file, len, &h) != 0) return CXEX_EXEC_LOAD_FAILED;
    return cxex_exec_as(file, len, caps_for(h.type_code, 1 /* verified */), NULL, NULL, 0);
}