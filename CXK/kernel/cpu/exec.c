/* /CXK/kernel/cpu/exec.c */
/* Aurora Tejeda / CATX SYSTEMS LLC */
/* cxex_exec: verify a signed image, decide its capabilities, and start it as a
   ring-3 scheduler thread. Non-blocking - returns the new pid. See exec.h. */

#include "exec.h"
#include "cxex.h"
#include "cxex_verify.h"
#include "caps.h"
#include "spawn.h"

/* policy layer: identity (CXEX type) + trust -> capability set. Consulted once
   here at the handoff; a valid signature does not itself grant authority. */
uint32_t caps_for(uint16_t type_code, int trusted) {
    if (!trusted) return 0;
    if (type_code == CXEX_TYPE_OS)   return CAP_OS_BASELINE;  /* broker executive */
    if (type_code == CXEX_TYPE_USER) return 0;                /* apps: capability-less */
    return 0;
}

int cxex_exec_as(const uint8_t *file, size_t len, uint32_t caps, struct endpoint *broker,
                 const char *args, uint32_t args_len) {
    /* IDENTITY + INTEGRITY: only run images signed by the trusted key. */
    if (cxex_verify_trusted(file, len) != CXEX_VERIFY_OK)
        return CXEX_EXEC_VERIFY_FAILED;

    /* POLICY: only OS/USER executables run in ring 3. */
    struct cxex_header h;
    if (cxex_parse_header(file, len, &h) != 0) return CXEX_EXEC_LOAD_FAILED;
    if (h.type_code != CXEX_TYPE_OS && h.type_code != CXEX_TYPE_USER)
        return CXEX_EXEC_BAD_TYPE;

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