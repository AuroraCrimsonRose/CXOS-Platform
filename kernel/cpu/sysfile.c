// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/sysfile.c */
/* Aurora Tejeda / CATX Systems */
/* SYS_FILE_OP - see sysfile.h. */

#include "sysfile.h"
#include "sched.h"
#include "object/htab.h"
#include "usermode.h"       /* user_ptr_readable / user_ptr_writable */
#include "cxfs.h"
#include "string.h"
#include "memman/ocache.h"
#include "cpu/int/idt.h"   /* kpanic */
#include <stddef.h>

/* ---- open files ------------------------------------------------------------
 * Open files come from their own cache (memman/ocache.h), never the heap: a
 * stale pointer to one can only alias another open file, and freeing one
 * that is not live panics. MAX_OPEN_FILES stays as a cap, and is what bounds
 * the graveyard below.
 *
 * An open file is a kernel object (KOBJ_FILE, object/kobj.h). Its lifetime is
 * its references': every handle holds one, and so does a file operation in
 * progress. Whether the process calls FILE_OP_CLOSE, calls SYS_HANDLE_CLOSE,
 * or exits and gets reaped, it goes back to its cache when the last one goes.
 *
 * What a handle may do is its RIGHTS (FOPEN_READ, FOPEN_WRITE), not the flags
 * the file was opened with: SYS_HANDLE_DUPLICATE can hand out a read-only handle on
 * a file opened read-write, and that handle must not write.
 */
#define MAX_OPEN_FILES 32

struct open_file {
    struct kobj hdr;     /* first: the handle table holds &hdr */
    uint32_t entry;      /* CXFS manifest id */
    uint32_t off;        /* current offset (see the INT32_MAX cap in the ABI) */
    uint32_t flags;      /* FOPEN_* the file was opened with */
    int      owner;      /* the thread (process) whose handle this is */
    int      doomed;     /* unlinked while open: delete at the last close */
};

static struct ocache of_cache;

static struct open_file *of_alloc(void) {
    if (ocache_live(&of_cache) >= MAX_OPEN_FILES) return NULL;
    struct open_file *f = ocache_alloc(&of_cache);   /* zeroed */
    if (!f) return NULL;
    if (kobj_init(&f->hdr, KOBJ_FILE) != 0) { ocache_free(&of_cache, f); return NULL; }
    f->owner = thread_current_id();
    return f;
}

/* Walks over the live open files. */
struct of_match { uint32_t entry; int doomed_only; int mark; };
static int of_match_fn(void *obj, void *ctx) {
    struct open_file *f = obj;
    struct of_match *m = ctx;
    if (f->entry != m->entry) return 0;
    if (m->mark) { f->doomed = 1; return 0; }
    return !m->doomed_only || f->doomed;
}

/* ---- unlinked while open ---------------------------------------------------
 * CXFS reuses the lowest free manifest id, and an open file reaches its data
 * by id with permission checked once, at open. So deleting an entry that is
 * still open would hand that open file to whatever is created next, another
 * user's file included. Every reference kernel defers the delete instead: a
 * Unix inode lives until its last reference, and NT marks the file
 * delete-pending until the last handle closes. CXK does the same. FILE_OP_UNLINK
 * on an open file marks it doomed, which hides its name from every lookup at
 * once, and the entry is deleted when the last open file on it is released.
 *
 * The release can happen in the reaper, inside the scheduler with interrupts
 * off, which is no place for disk I/O. So the release only queues the id, and
 * the delete runs at the start of the next SYS_FILE_OP, from anyone, and right
 * after a FILE_OP_CLOSE. A crash before that leaves the file in place: the
 * unlink simply did not happen, which is the safe direction to fail in.
 */
static uint32_t graveyard[MAX_OPEN_FILES];
static int      graves;

static int entry_open(uint32_t id) {
    struct of_match m = { id, 0, 0 };
    return ocache_walk(&of_cache, of_match_fn, &m);
}

/* Unlinked, and so invisible to lookups, but not deleted yet. */
static int entry_doomed(uint32_t id) {
    struct of_match m = { id, 1, 0 };
    if (ocache_walk(&of_cache, of_match_fn, &m)) return 1;
    for (int i = 0; i < graves; i++)
        if (graveyard[i] == id) return 1;
    return 0;
}

int sysfile_entry_doomed(uint32_t id) { return entry_doomed(id); }

static void bury_pending(void) {
    while (graves > 0) {
        uint32_t id = graveyard[--graves];
        cxfs_delete_entry(id);
    }
}

/* ---- advisory locks --------------------------------------------------------
 * SerenityOS's model (Kernel/FileSystem/Inode.cpp): a list of byte-range locks
 * per file, each owned by the open file that took it, removed when that open
 * file goes. Shared locks overlap freely; an exclusive one overlaps nothing
 * held by another open file. An open file's own locks never conflict with
 * each other, so re-locking a range converts it.
 *
 * Unlike the CXFS locks this replaces, nothing is on disk: a pid is a reused
 * thread slot, a lock written to disk survives a reboot, and a removable
 * volume's lock state would be whatever its author wanted.
 *
 * A range is [start, start + len), and len 0 runs to the end of the file
 * however far it grows, so (0, 0) is the whole file.
 */
#define MAX_FILE_LOCKS 64

struct file_lock {
    struct open_file *of;      /* NULL: slot free */
    uint32_t entry;
    uint32_t start;
    uint32_t len;
    uint32_t mode;             /* FLOCK_SHARED or FLOCK_EXCLUSIVE */
};
static struct file_lock locks[MAX_FILE_LOCKS];

/* What a thread blocked in FILE_OP_LOCK is waiting for. The record lives on
   that thread's own stack for as long as it waits, linked into `waiters`, so
   there is one per waiting thread and nothing sized by how many threads there
   could be. The deadlock check walks the list; a release wakes everyone on it.
   Unlocked, like `locks` itself: only thread context touches either, and
   the kernel is not preempted outside the preemption self-tests. One lock
   for both arrives with preemption or SMP. */
struct lock_wait {
    int      tid;
    int      seen;          /* visited by the current deadlock walk */
    struct open_file *of;
    uint32_t entry, start, len, mode;
    struct lock_wait *next;
};
static struct lock_wait *waiters;

static struct lock_wait *waiter_of(int tid) {
    for (struct lock_wait *w = waiters; w; w = w->next)
        if (w->tid == tid) return w;
    return NULL;
}

static int ranges_overlap(uint32_t s1, uint32_t l1, uint32_t s2, uint32_t l2) {
    uint64_t e1 = l1 ? (uint64_t)s1 + l1 : UINT64_MAX;
    uint64_t e2 = l2 ? (uint64_t)s2 + l2 : UINT64_MAX;
    return s1 < e2 && s2 < e1;
}

/* The first lock held by ANOTHER open file that this request collides with,
   starting the scan at `from`; -1 if none. */
static int lock_conflict(const struct open_file *of, uint32_t entry, uint32_t start,
                         uint32_t len, uint32_t mode, int from) {
    for (int i = from; i < MAX_FILE_LOCKS; i++) {
        const struct file_lock *l = &locks[i];
        if (!l->of || l->of == of || l->entry != entry) continue;
        if (!ranges_overlap(l->start, l->len, start, len)) continue;
        if (mode == FLOCK_EXCLUSIVE || l->mode == FLOCK_EXCLUSIVE) return i;
    }
    return -1;
}

/* Would thread `me` waiting for this lock never wake? It would if some holder
   of a conflicting lock is `me` (a second open file in the same process), or
   is itself waiting, directly or down a chain, on a lock `me` holds. Linux
   checks POSIX locks the same way and answers EDEADLK; Serenity does not check,
   and a program that gets it wrong hangs. */
static int lock_cycle(int me, const struct open_file *of, uint32_t entry, uint32_t start,
                      uint32_t len, uint32_t mode) {
    for (int i = lock_conflict(of, entry, start, len, mode, 0); i >= 0;
         i = lock_conflict(of, entry, start, len, mode, i + 1)) {
        int h = locks[i].of->owner;
        if (h == me) return 1;
        struct lock_wait *w = waiter_of(h);
        if (!w || w->seen) continue;
        w->seen = 1;
        if (lock_cycle(me, w->of, w->entry, w->start, w->len, w->mode)) return 1;
    }
    return 0;
}

static void lock_wake_all(void) {
    for (struct lock_wait *w = waiters; w; w = w->next) thread_unblock(w->tid);
}

/* Take or convert a lock. The caller has checked there is no conflict. */
static int lock_apply(struct open_file *of, uint32_t start, uint32_t len, uint32_t mode) {
    int free_slot = -1;
    for (int i = 0; i < MAX_FILE_LOCKS; i++) {
        struct file_lock *l = &locks[i];
        if (!l->of) { if (free_slot < 0) free_slot = i; continue; }
        if (l->of == of && l->start == start && l->len == len) {
            int downgrade = (l->mode == FLOCK_EXCLUSIVE && mode == FLOCK_SHARED);
            l->mode = mode;
            if (downgrade) lock_wake_all();
            return E_OK;
        }
    }
    if (free_slot < 0) return E_NOMEM;
    locks[free_slot].of    = of;
    locks[free_slot].entry = of->entry;
    locks[free_slot].start = start;
    locks[free_slot].len   = len;
    locks[free_slot].mode  = mode;
    return E_OK;
}

/* Drop this open file's locks that lie wholly inside [start, start+len). A
   lock that only partly overlaps is left alone: ranges are not split, which is
   also Serenity's rule, so unlock what was locked. (0, 0) drops them all. */
static void lock_drop(const struct open_file *of, uint32_t start, uint32_t len) {
    uint64_t end = len ? (uint64_t)start + len : UINT64_MAX;
    int any = 0;
    for (int i = 0; i < MAX_FILE_LOCKS; i++) {
        struct file_lock *l = &locks[i];
        if (l->of != of) continue;
        uint64_t lend = l->len ? (uint64_t)l->start + l->len : UINT64_MAX;
        if (l->start < start || lend > end) continue;
        l->of = NULL;
        any = 1;
    }
    if (any) lock_wake_all();
}

/* Releasing the slot is the whole lifetime rule: its locks go with it, and a
   doomed file whose last open this was is queued for deletion. It runs when
   the last reference goes, however the handles went - FILE_OP_CLOSE,
   SYS_HANDLE_CLOSE, or the reaper at exit. */
static void of_destroy(struct kobj *o) {
    struct open_file *f = (struct open_file *)o;
    uint32_t entry  = f->entry;
    int      doomed = f->doomed;
    lock_drop(f, 0, 0);
    ocache_free(&of_cache, f);
    if (doomed && !entry_open(entry) && graves < MAX_OPEN_FILES)
        graveyard[graves++] = entry;
}

static const struct kobj_ops file_ops = {
    .name         = "file",
    .rights_valid = FOPEN_READ | FOPEN_WRITE,
    .destroy      = of_destroy,
};

void sysfile_init(void) {
    if (ocache_init(&of_cache, "file", sizeof(struct open_file)) != 0)
        kpanic("sysfile_init: no open-file cache");
    for (int i = 0; i < MAX_FILE_LOCKS; i++) locks[i].of = NULL;
    waiters = NULL;
    graves = 0;
    kobj_type_register(KOBJ_FILE, &file_ops);
}

int sysfile_open_count(void) {
    return (int)ocache_live(&of_cache);
}

/* ---- error translation -----------------------------------------------------
 * CXFS has its own error space (CXFS_E_*, cxfs.h) and the ABI has another
 * (E_*, cxk_abi.h). They meet here and nowhere else, so a change to either one
 * has exactly one place to be reconciled.
 *
 * CXFS_E_FRAGMENT folds into E_NOMEM along with CXFS_E_NOSPACE: both mean the
 * write cannot be satisfied for want of space, and the difference between "no
 * blocks" and "no blocks in one run" is not something a user program can act on
 * differently. The kernel log keeps the distinction.
 */
static int abi_err(int cxfs_rc) {
    switch (cxfs_rc) {
        case CXFS_E_OK:       return E_OK;
        case CXFS_E_PERM:     return E_PERM;
        case CXFS_E_NOSPACE:  return E_NOMEM;
        case CXFS_E_FRAGMENT: return E_NOMEM;
        case CXFS_E_INVAL:    return E_INVAL;
        case CXFS_E_NOTFOUND: return E_NOENT;
        case CXFS_E_ISDIR:    return E_ISDIR;
        case CXFS_E_NOTDIR:   return E_NOTDIR;
        case CXFS_E_EXISTS:   return E_EXIST;
        case CXFS_E_FAIL:     return E_IO;
        default:              return (cxfs_rc < 0) ? E_IO : cxfs_rc;
    }
}

/* ---- copying from ring 3 --------------------------------------------------- */

/* Copy a NUL-terminated path in from user memory, bounded by `cap`.
   Validates one page at a time rather than one byte at a time: user_ptr_readable
   walks page tables, so per-byte would be a page-table walk per character.
   Returns the length, or a negative E_* code. */
static int copy_path_in(const char *upath, char *dst, uint32_t cap) {
    uint32_t addr = (uint32_t)upath;
    if (!addr) return E_FAULT;
    for (uint32_t i = 0; i < cap; i++) {
        if (i == 0 || (((addr + i) & 0xFFFu) == 0)) {       /* new page */
            if (!user_ptr_readable(addr + i, 1)) return E_FAULT;
        }
        char c = ((const char *)addr)[i];
        dst[i] = c;
        if (!c) return (int)i;
    }
    return E_RANGE;   /* no terminator inside cap bytes */
}

/* Resolve everything but the last component of `path`, so a create knows which
   directory to create in and under what name. Returns 0, or a negative E_*. */
static int resolve_parent(const char *path, uint32_t cwd,
                          uint32_t *parent, char *leaf, uint32_t leafcap) {
    uint32_t len = 0;
    while (path[len]) len++;
    if (len == 0) return E_INVAL;

    /* find the last '/' */
    int slash = -1;
    for (uint32_t i = 0; i < len; i++) if (path[i] == '/') slash = (int)i;

    if (slash < 0) {                       /* bare name: in the cwd */
        if (len >= leafcap) return E_RANGE;
        for (uint32_t i = 0; i <= len; i++) leaf[i] = path[i];
        *parent = cwd;
        return 0;
    }

    /* "/name" -> parent is root; "a/b/name" -> parent is the resolved prefix */
    uint32_t leaflen = len - (uint32_t)slash - 1;
    if (leaflen == 0)       return E_INVAL;   /* trailing slash */
    if (leaflen >= leafcap) return E_RANGE;
    for (uint32_t i = 0; i <= leaflen; i++) leaf[i] = path[slash + 1 + i];

    if (slash == 0) { *parent = 0; return 0; }   /* 0 = root */

    char dir[FILE_PATH_MAX];
    if ((uint32_t)slash >= sizeof dir) return E_RANGE;
    for (int i = 0; i < slash; i++) dir[i] = path[i];
    dir[slash] = '\0';

    int pid = cxfs_resolve(dir, cwd);
    if (pid < 0) return E_NOENT;
    *parent = (uint32_t)pid;
    return 0;
}

/* Does the path's LAST component be "." or ".."? Those resolve to a directory
   rather than to something inside it, so an operation that means "this named
   thing here" has to refuse them: rename(".", x) would otherwise rename the
   directory the caller is standing in, which is never what was meant. */
static int names_self(const char *path) {
    uint32_t len = 0;
    while (path[len]) len++;
    while (len > 1 && path[len - 1] == '/') len--;      /* ignore trailing slashes */
    if (len == 1 && path[0] == '.') return 1;
    if (len == 2 && path[0] == '.' && path[1] == '.') return 1;
    if (len >= 2 && path[len - 1] == '.' && path[len - 2] == '/') return 1;
    if (len >= 3 && path[len - 1] == '.' && path[len - 2] == '.' && path[len - 3] == '/') return 1;
    return 0;
}

/* Fill a user-facing file_stat from a CXFS entry. 64-bit fields are truncated
   to 32 on the way out - see the ABI note on the offset cap. */
static void fill_stat(const struct cxfs_entry *e, struct file_stat *st) {
    memset(st, 0, sizeof *st);
    st->id          = e->id;
    st->kind        = (e->type == CXFS_TYPE_DIR) ? FTYPE_DIR : FTYPE_FILE;
    st->size        = (e->size > 0x7FFFFFFFull) ? 0x7FFFFFFFu : (uint32_t)e->size;
    st->permissions = e->permissions;
    st->owner_uid   = e->owner_uid;
    st->created     = (uint32_t)e->created;
    st->modified    = (uint32_t)e->modified;
    st->accessed    = (uint32_t)e->accessed;
    strlcpy(st->name, e->name, FILE_NAME_MAX);
}

/* Look up one of the caller's open files, with a reference the caller drops,
   and the rights its handle carries. */
static struct open_file *of_ref(int32_t h, uint32_t *rights) {
    struct htab_info info;
    struct kobj *o = htab_ref(thread_htab(thread_current_id()), h, KOBJ_FILE, 0, &info);
    if (!o) return NULL;
    *rights = info.rights;
    return (struct open_file *)o;
}

/* The operations that act on an open file rather than on a path. */
static int names_handle(uint32_t op) {
    switch (op) {
        case FILE_OP_CLOSE: case FILE_OP_READ:  case FILE_OP_WRITE:
        case FILE_OP_SEEK:  case FILE_OP_TELL:  case FILE_OP_TRUNC:
        case FILE_OP_FSTAT: case FILE_OP_LOCK:  case FILE_OP_UNLOCK:
            return 1;
        default:
            return 0;
    }
}

/* ---- readdir ---------------------------------------------------------------
 * cxfs_list_dir is callback-driven with no index, so indexed access is done by
 * counting matches and keeping the one asked for. Safe to hold in statics for
 * the same reason CXFS uses static block buffers: nothing is inside CXFS twice
 * at once (see the note in cxfs.c).
 */
static uint32_t rd_want;
static uint32_t rd_seen;
static int      rd_found;
static struct cxfs_entry rd_entry;

static void rd_cb(const struct cxfs_entry *e) {
    if (entry_doomed(e->id)) return;                   /* already unlinked */
    if (rd_seen++ == rd_want && !rd_found) {
        rd_entry = *e;
        rd_found = 1;
    }
}

/* Resolve a path the way a program should see it: an entry unlinked while
   open is gone, though CXFS still holds it until the last close. */
static int lookup(const char *path, uint32_t cwd) {
    int r = cxfs_resolve(path, cwd);
    return (r >= 0 && entry_doomed((uint32_t)r)) ? -1 : r;
}

/* ---- the syscall ---------------------------------------------------------- */

/* One operation. `f` is the open file the handle names, held for the whole
   operation, and `rights` that handle's rights; both are 0 for an operation
   on a path. */
static int file_op(struct file_op_args a, struct open_file *f, uint32_t rights) {
    uint32_t cwd = thread_current_cwd();
    char path[FILE_PATH_MAX];
    int rc;

    switch (a.op) {

        case FILE_OP_OPEN: {
            if ((rc = copy_path_in(a.path, path, sizeof path)) < 0) return rc;
            if (!(a.flags & (FOPEN_READ | FOPEN_WRITE))) return E_INVAL;

            int id = lookup(path, cwd);

            if (id < 0) {
                if (!(a.flags & FOPEN_CREATE)) return E_NOENT;
                /* an unlinked file still open elsewhere holds its name until
                   its last close, as a delete-pending file does on NT */
                if (cxfs_resolve(path, cwd) >= 0) return E_EXIST;
                uint32_t parent;
                char leaf[FILE_NAME_MAX];
                if ((rc = resolve_parent(path, cwd, &parent, leaf, sizeof leaf)) < 0) return rc;
                id = cxfs_create_entry(parent, leaf, CXFS_TYPE_FILE);
                if (id < 0) return E_IO;
            } else {
                struct cxfs_entry e;
                if (cxfs_read_entry((uint32_t)id, &e) != 0) return E_IO;
                if (e.type == CXFS_TYPE_DIR) return E_ISDIR;
                if ((a.flags & FOPEN_TRUNC) && (a.flags & FOPEN_WRITE)) {
                    rc = cxfs_truncate((uint32_t)id, 0);
                    if (rc != CXFS_E_OK) return abi_err(rc);
                }
            }

            /* Take the handle slot LAST. Everything above can fail, and a slot
               claimed before a failure would be a handle leaked per failed
               open - the sort of thing a program retrying in a loop turns into
               an exhausted table. */
            struct open_file *nf = of_alloc();
            if (!nf) return E_NOMEM;
            nf->entry = (uint32_t)id;
            nf->flags = a.flags;
            nf->off   = 0;
            if (a.flags & FOPEN_APPEND) {
                struct cxfs_entry e;
                if (cxfs_read_entry(nf->entry, &e) == 0) nf->off = (uint32_t)e.size;
            }

            int32_t h = htab_install(thread_htab(thread_current_id()), &nf->hdr,
                                     (a.flags & (FOPEN_READ | FOPEN_WRITE)) | HRIGHT_DUPLICATE, 0);
            /* The open's own reference goes either way: on success the handle
               holds the file; on failure it goes back to its cache here. */
            kobj_unref(&nf->hdr);
            return h < 0 ? E_NOMEM : h;
        }

        case FILE_OP_CLOSE:
            /* The file itself goes when sys_file_op drops its reference, and
               the delete of an unlinked file runs after that. */
            if (htab_close(thread_htab(thread_current_id()), a.handle) != 0) return E_BADF;
            return E_OK;

        case FILE_OP_READ: {
            if (!(rights & FOPEN_READ)) return E_PERM;
            if (a.len == 0) return 0;
            if (a.len > 0x7FFFFFFFu) return E_RANGE;
            if (!user_ptr_writable((uint32_t)a.data, a.len)) return E_FAULT;

            rc = cxfs_read_at(f->entry, f->off, a.data, a.len);
            if (rc < 0) return abi_err(rc);
            f->off += (uint32_t)rc;
            return rc;
        }

        case FILE_OP_WRITE: {
            if (!(rights & FOPEN_WRITE)) return E_PERM;
            if (a.len == 0) return 0;
            if (a.len > 0x7FFFFFFFu) return E_RANGE;
            if (!user_ptr_readable((uint32_t)a.data, a.len)) return E_FAULT;
            /* the offset cap is the ABI's, so refuse rather than wrap */
            if (f->off > 0x7FFFFFFFu - a.len) return E_RANGE;

            /* APPEND means "at the end as it is now", not "at the end as it was
               when you opened it" - re-read the size each time, so two writers
               appending to one file do not overwrite each other. */
            if (f->flags & FOPEN_APPEND) {
                struct cxfs_entry e;
                if (cxfs_read_entry(f->entry, &e) != 0) return E_IO;
                f->off = (uint32_t)e.size;
            }

            rc = cxfs_write_at(f->entry, f->off, a.data, a.len);
            if (rc < 0) return abi_err(rc);
            f->off += (uint32_t)rc;
            return rc;
        }

        case FILE_OP_SEEK: {

            struct cxfs_entry e;
            if (cxfs_read_entry(f->entry, &e) != 0) return E_IO;
            uint32_t size = (e.size > 0x7FFFFFFFull) ? 0x7FFFFFFFu : (uint32_t)e.size;

            int64_t base;
            switch (a.flags) {
                case FSEEK_SET: base = 0;              break;
                case FSEEK_CUR: base = (int64_t)f->off; break;
                case FSEEK_END: base = (int64_t)size;  break;
                default: return E_INVAL;
            }
            int64_t pos = base + (int64_t)a.off;
            if (pos < 0 || pos > 0x7FFFFFFF) return E_RANGE;
            /* Seeking past EOF is allowed and allocates nothing: the hole only
               becomes real on the write that reaches it. */
            f->off = (uint32_t)pos;
            return (int)f->off;
        }

        case FILE_OP_TELL: {
            return (int)f->off;
        }

        case FILE_OP_TRUNC: {
            if (!(rights & FOPEN_WRITE)) return E_PERM;
            if (a.off < 0) return E_INVAL;
            return abi_err(cxfs_truncate(f->entry, (uint64_t)(uint32_t)a.off));
        }

        case FILE_OP_STAT:
        case FILE_OP_FSTAT: {
            uint32_t id;
            if (a.op == FILE_OP_STAT) {
                if ((rc = copy_path_in(a.path, path, sizeof path)) < 0) return rc;
                int r = lookup(path, cwd);
                if (r < 0) return E_NOENT;
                id = (uint32_t)r;
            } else {
                id = f->entry;
            }
            if (!user_ptr_writable((uint32_t)a.data, sizeof(struct file_stat))) return E_FAULT;
            struct cxfs_entry e;
            if (cxfs_read_entry(id, &e) != 0) return E_IO;
            fill_stat(&e, (struct file_stat *)a.data);
            return E_OK;
        }

        case FILE_OP_READDIR: {
            if ((rc = copy_path_in(a.path, path, sizeof path)) < 0) return rc;
            if (!user_ptr_writable((uint32_t)a.data, sizeof(struct file_stat))) return E_FAULT;

            int r = lookup(path, cwd);
            if (r < 0) return E_NOENT;
            struct cxfs_entry dir;
            if (cxfs_read_entry((uint32_t)r, &dir) != 0) return E_IO;
            if (dir.type != CXFS_TYPE_DIR) return E_NOTDIR;

            rd_want  = a.len;
            rd_seen  = 0;
            rd_found = 0;
            cxfs_list_dir((uint32_t)r, rd_cb);
            if (!rd_found) return 0;                 /* index past the end */
            fill_stat(&rd_entry, (struct file_stat *)a.data);
            return 1;
        }

        /* Commit what has been written to stable media.
         *
         * Takes no arguments and touches no handle: CXFS writes through, so
         * there is nothing buffered in the filesystem to pick out - what this
         * asks is that the DEVICE commit its cache, which is per-volume and
         * not per-file. Returning E_IO when the device cannot confirm is the
         * point; a caller told a write is durable when it is not is worse off
         * than one told nothing. */
        case FILE_OP_SYNC:
            return (cxfs_sync() == 0) ? E_OK : E_IO;

        /* Without FLOCK_WAIT a conflict is E_AGAIN at once. With it the thread
           sleeps until a release, then looks again, as Serenity's FlockBlocker
           does - unless waiting could never end, which is E_DEADLK. A
           conversion that conflicts leaves the lock already held unchanged. */
        case FILE_OP_LOCK: {
            uint32_t mode = a.flags & ~(uint32_t)FLOCK_WAIT;
            if (mode != FLOCK_SHARED && mode != FLOCK_EXCLUSIVE) return E_INVAL;
            if (a.off < 0) return E_INVAL;
            uint32_t start = (uint32_t)a.off;

            int me = thread_current_id();
            while (lock_conflict(f, f->entry, start, a.len, mode, 0) >= 0) {
                if (!(a.flags & FLOCK_WAIT)) return E_AGAIN;
                for (struct lock_wait *w = waiters; w; w = w->next) w->seen = 0;
                if (lock_cycle(me, f, f->entry, start, a.len, mode)) return E_DEADLK;

                struct lock_wait self = { .tid = me, .of = f, .entry = f->entry,
                                          .start = start, .len = a.len, .mode = mode,
                                          .next = waiters };
                waiters = &self;
                thread_wait();
                struct lock_wait **pp = &waiters;
                while (*pp != &self) pp = &(*pp)->next;
                *pp = self.next;
            }
            return lock_apply(f, start, a.len, mode);
        }

        case FILE_OP_UNLOCK: {
            if (a.off < 0) return E_INVAL;
            lock_drop(f, (uint32_t)a.off, a.len);
            return E_OK;
        }

        case FILE_OP_MKDIR: {
            if ((rc = copy_path_in(a.path, path, sizeof path)) < 0) return rc;
            if (cxfs_resolve(path, cwd) >= 0) return E_EXIST;
            uint32_t parent;
            char leaf[FILE_NAME_MAX];
            if ((rc = resolve_parent(path, cwd, &parent, leaf, sizeof leaf)) < 0) return rc;
            return (cxfs_create_entry(parent, leaf, CXFS_TYPE_DIR) < 0) ? E_IO : E_OK;
        }

        case FILE_OP_UNLINK: {
            if ((rc = copy_path_in(a.path, path, sizeof path)) < 0) return rc;
            int r = lookup(path, cwd);
            if (r < 0) return E_NOENT;
            if (r == 0) return E_INVAL;                    /* never the root */

            struct cxfs_entry e;
            if (cxfs_read_entry((uint32_t)r, &e) != 0) return E_IO;

            /* An unlink is a write to the entry, so it needs the entry's write
               permission - cxfs_delete_entry does not check, because its other
               caller is the kernel tidying up after itself. */
            if (thread_current_uid() != 0) {
                uint16_t need = (thread_current_uid() == e.owner_uid)
                                ? CXFS_PERM_OW : CXFS_PERM_TW;
                if (!(e.permissions & need)) return E_PERM;
            }
            if (e.type == CXFS_TYPE_DIR && cxfs_count_children((uint32_t)r) > 0)
                return E_INVAL;                            /* must be empty */
            if ((uint32_t)r == cwd) return E_INVAL;         /* not your own cwd */

            /* Still open somewhere: the name goes now, the entry at the last
               close (see "unlinked while open" above). */
            if (entry_open((uint32_t)r)) {
                struct of_match m = { (uint32_t)r, 0, 1 };
                ocache_walk(&of_cache, of_match_fn, &m);
                return E_OK;
            }
            return (cxfs_delete_entry((uint32_t)r) == 0) ? E_OK : E_IO;
        }

        case FILE_OP_RENAME: {
            if ((rc = copy_path_in(a.path, path, sizeof path)) < 0) return rc;
            char newname[FILE_NAME_MAX];
            if ((rc = copy_path_in(a.path2, newname, sizeof newname)) < 0) return rc;

            if (names_self(path)) return E_INVAL;   /* "." / ".." name a directory,
                                                       not an entry within it */
            int r = lookup(path, cwd);
            if (r < 0) return E_NOENT;
            if (r == 0) return E_INVAL;

            /* cxfs_rename deliberately does not check for collisions (its doc
               says the caller does), so check here - otherwise two entries in
               one directory share a name and lookups become order-dependent. */
            struct cxfs_entry e;
            if (cxfs_read_entry((uint32_t)r, &e) != 0) return E_IO;
            int clash = cxfs_find_in_dir(e.parent_id, newname);
            if (clash >= 0 && clash != r) return E_EXIST;

            return (cxfs_rename((uint32_t)r, newname) == 0) ? E_OK : E_INVAL;
        }

        case FILE_OP_CHDIR: {
            if ((rc = copy_path_in(a.path, path, sizeof path)) < 0) return rc;
            int r = lookup(path, cwd);
            if (r < 0) return E_NOENT;
            struct cxfs_entry e;
            if (cxfs_read_entry((uint32_t)r, &e) != 0) return E_IO;
            if (e.type != CXFS_TYPE_DIR) return E_NOTDIR;
            thread_set_cwd(thread_current_id(), (uint32_t)r);
            return E_OK;
        }

        case FILE_OP_GETCWD: {
            if (a.len == 0) return E_RANGE;
            if (a.len > FILE_PATH_MAX) a.len = FILE_PATH_MAX;
            if (!user_ptr_writable((uint32_t)a.data, a.len)) return E_FAULT;
            cxfs_path_of(cwd, path, (int)a.len);
            uint32_t n = 0;
            while (n + 1 < a.len && path[n]) n++;
            memcpy(a.data, path, n);
            ((char *)a.data)[n] = '\0';
            return (int)n;
        }

        default:
            return E_INVAL;
    }
}

int sys_file_op(const struct file_op_args *ua) {
    if (!user_ptr_readable((uint32_t)ua, sizeof *ua)) return E_FAULT;
    struct file_op_args a = *ua;          /* copy in: never re-read user memory */

    if (!cxfs_is_mounted()) return E_IO;
    bury_pending();

    /* The handle is resolved once, here, and the reference held until the
       operation returns: a lock wait blocks, and the file must not be freed
       under it, on this CPU or another. */
    struct open_file *f = NULL;
    uint32_t rights = 0;
    if (names_handle(a.op) && !(f = of_ref(a.handle, &rights))) return E_BADF;

    int rc = file_op(a, f, rights);

    if (f) kobj_unref(&f->hdr);
    if (a.op == FILE_OP_CLOSE) bury_pending();   /* the last close of an unlinked file */
    return rc;
}
