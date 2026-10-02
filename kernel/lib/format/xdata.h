/* /kernel/lib/format/xdata.h */
/* Aurora Tejeda / CATX Systems */
/*
 * The X Data reader, called from C.
 *
 * There is no C implementation behind this header. The kernel links
 * os/std/xdata.xfxn itself - compiled with `cxk compile --object` - so the
 * kernel, the supervisor and (through its port) the build all read X Data with
 * the same code, and cannot disagree about what a document means. It is the
 * kernel's first code written in X.
 *
 * That works because X's calling convention is cdecl: arguments pushed right
 * to left, the caller pops them, the result comes back in eax, and ebx, esi and
 * edi are preserved. An X `bool` is 0 or 1 in eax, so it is an int here.
 *
 * These prototypes must match the `fn` declarations in xdata.xfxn by hand -
 * nothing checks them - and so must struct xd_val. X lays every field out on
 * a 4-byte boundary, which for five u32s is exactly what C does too.
 *
 * The reader keeps its error position in globals, so it is not reentrant. The
 * kernel reads X Data once, at boot, before anything else could call it.
 */
#ifndef XDATA_H
#define XDATA_H

#include <stdint.h>

#define XD_OK         0
#define XD_E_SYNTAX  (-1)
#define XD_E_STRING  (-2)
#define XD_E_DEPTH   (-3)
#define XD_E_DUPKEY  (-4)
#define XD_E_SEP     (-5)
#define XD_E_RANGE   (-6)
#define XD_E_TYPE    (-7)
#define XD_E_SPACE   (-8)

#define XD_STR   1
#define XD_INT   2
#define XD_BOOL  3
#define XD_SYM   4
#define XD_LIST  5
#define XD_REC   6

struct xd_val {
    uint32_t kind;
    uint32_t at;        /* byte offset of the value (for a list/record, its body) */
    uint32_t len;
    uint32_t tag_at;
    uint32_t tag_len;
};

extern int32_t  xd_err;
extern uint32_t xd_err_at;

int32_t     xd_check(const char *s, uint32_t len);
const char *xd_strerror(int32_t code);
uint32_t    xd_line(const char *s, uint32_t at);
void        xd_root(const char *s, uint32_t len, struct xd_val *out);
int32_t     xd_get(const char *s, struct xd_val *rec, const char *key, struct xd_val *out);
int32_t     xd_entry_at(const char *s, struct xd_val *rec, uint32_t index,
                        struct xd_val *key, struct xd_val *val);
int32_t     xd_u32(const char *s, struct xd_val *v, uint32_t *out);
int32_t     xd_sym_is(const char *s, struct xd_val *v, const char *name);

#endif
