/* /kernel/cpu/handle.c */
/* Aurora Tejeda / CATX Systems */
/* Pure per-process handle-table ops. See handle.h. */

#include "handle.h"
#include <stddef.h>

int handle_install(struct cap_handle *tbl, int max,
                   uint8_t type, uint8_t rights, void *object) {
    if (!tbl || type == HANDLE_NONE) return -1;
    for (int i = 0; i < max; i++) {
        if (tbl[i].type == HANDLE_NONE) {
            tbl[i].type   = type;
            tbl[i].rights = rights;
            tbl[i]._pad   = 0;
            tbl[i].object = object;
            return i;
        }
    }
    return -1;   /* table full */
}

struct cap_handle *handle_get(struct cap_handle *tbl, int max, int idx) {
    if (!tbl || idx < 0 || idx >= max) return NULL;
    if (tbl[idx].type == HANDLE_NONE)  return NULL;
    return &tbl[idx];
}

/* One release function per handle type, indexed by the type value. */
#define HANDLE_TYPE_MAX 8
static handle_release_fn releasers[HANDLE_TYPE_MAX];

void handle_set_release(uint8_t type, handle_release_fn fn) {
    if (type == HANDLE_NONE || type >= HANDLE_TYPE_MAX) return;
    releasers[type] = fn;
}

int handle_close(struct cap_handle *tbl, int max, int idx) {
    if (!tbl || idx < 0 || idx >= max)  return -1;
    if (tbl[idx].type == HANDLE_NONE)   return -1;

    /* release before blanking: the release function is handed the slot, and
       reads tbl[idx].object out of it. */
    if (tbl[idx].type < HANDLE_TYPE_MAX && releasers[tbl[idx].type])
        releasers[tbl[idx].type](&tbl[idx]);

    tbl[idx].type   = HANDLE_NONE;
    tbl[idx].rights = 0;
    tbl[idx].object = NULL;
    return 0;
}

void handle_release_all(struct cap_handle *tbl, int max) {
    if (!tbl) return;
    for (int i = 0; i < max; i++)
        if (tbl[i].type != HANDLE_NONE) handle_close(tbl, max, i);
}