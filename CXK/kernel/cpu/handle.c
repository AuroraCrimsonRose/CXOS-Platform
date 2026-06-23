/* /CXK/kernel/cpu/handle.c */
/* Aurora Tejeda / CATX SYSTEMS LLC */
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

int handle_close(struct cap_handle *tbl, int max, int idx) {
    if (!tbl || idx < 0 || idx >= max)  return -1;
    if (tbl[idx].type == HANDLE_NONE)   return -1;
    tbl[idx].type   = HANDLE_NONE;
    tbl[idx].rights = 0;
    tbl[idx].object = NULL;
    return 0;
}