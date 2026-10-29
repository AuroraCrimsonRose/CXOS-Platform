// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/cpu/sysfile.c */
/* Aurora Tejeda / CATX Systems */
/* SYS_FILE_* - see sysfile.h. */

#include "sysfile.h"
#include "sched.h"
#include "object/htab.h"
#include "usermode.h"       /* user_ptr_readable / user_ptr_writable */
#include "cxfs.h"
#include "string.h"
#include "memman/ocache.h"
#include "panic.h"
#include <stddef.h>

/* ---- open files ------------------------------------------------------------
 * Open files come from their own cache (memman/ocache.h), never the heap: a
 * stale pointer to one can only alias another open file, and freeing one
 * that is not live panics. MAX_OPEN_FILES stays as a cap, and is what bounds
 * the graveyard below.
 *
 * An open file is a kernel object (KOBJ_FILE, object/kobj.h). Its lifetime is
 * its references': every handle holds one, and so does a file operation in
 * progress. Whether the process calls SYS_FILE_CLOSE, calls SYS_HANDLE_CLOSE,
 * or exits and gets reaped, it goes back to its cache when the last one goes.
 *
 * What a handle may do is its RIGHTS (FOPEN_READ, FOPEN_WRITE), not the flags
 * the file was opened with: SYS_HANDLE_DUPLICATE can hand out a read-only handle on
 * a file opened read-write, and that handle must not write.
 */
#define MAX_OPEN_FILES 32

/* Offsets, sizes and lock ranges are u64 in the ABI (since ABI 10). A position
   stays at or below INT64_MAX, so a seek's signed arithmetic and start + len
   both stay in range; CXFS refuses a file past 2^32 blocks well before it. A
   read or write moves at most FILE_IO_MAX bytes, since its count comes back in
   an i32; asking for more is a short count, as at the end of a file. */
#define FILE_OFF_MAX ((uint64_t)INT64_MAX)
#define FILE_IO_MAX  0x7FFFFFFFu

struct open_file {
    struct kobj hdr;     /* first: the handle table holds &hdr */
    uint32_t entry;      /* CXFS manifest id */
    uint64_t off;        /* current offset, at most FILE_OFF_MAX */
    uint32_t flags;      /* FOPEN_* the file was opened with */
    int      doomed;     /* unlinked while open: delete at the last close */
};

static struct ocache of_cache;

static struct open_file *of_alloc(void) {
    if (ocache_live(&of_cache) >= MAX_OPEN_FILES) return NULL;
    struct open_file *f = ocache_alloc(&of_cache);   /* zeroed */
    if (!f) return NULL;
    if (kobj_init(&f->hdr, KOBJ_FILE) != 0) { ocache_free(&of_cache, f); return NULL; }
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
 * delete-pending until the last handle closes. CXK does the same. SYS_FILE_UNLINK
 * on an open file marks it doomed, which hides its name from every lookup at
 * once, and the entry is deleted when the last open file on it is released.
 *
 * The release can happen in the reaper, inside the scheduler with interrupts
 * off, which is no place for disk I/O. So the release only queues the id, and
 * the delete runs at the start of the next file syscall, from anyone, and right
 * after a SYS_FILE_CLOSE. A crash before that leaves the file in place: the
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
 * Owned by the open file, not a thread or a process: an open file's handle can
 * travel in a message (1.3b'), and whoever holds it then holds its locks and
 * can unlock them, as with XNU's OFD locks and Haiku's flock. Each lock also
 * remembers the thread that took it, only for the deadlock check; once the
 * handle travels that is unknown (XNU kern_lockf.c: a lock is "confined" to
 * one thread until its file is shared), and the check passes over it.
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
    uint64_t start;
    uint64_t len;
    uint32_t mode;             /* FLOCK_SHARED or FLOCK_EXCLUSIVE */
    int      taker;            /* the thread that took it; TAKER_UNKNOWN once
                                  its handle has travelled, for good (XNU does
                                  not confine a shared lock again either) */
};
static struct file_lock locks[MAX_FILE_LOCKS];

/* Not a thread id: thread 0 is the boot thread. */
#define TAKER_UNKNOWN (-1)

/* What a thread blocked in SYS_FILE_LOCK is waiting for. The record lives on
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
    uint32_t entry, mode;
    uint64_t start, len;
    struct lock_wait *next;
};
static struct lock_wait *waiters;

static struct lock_wait *waiter_of(int tid) {
    for (struct lock_wait *w = waiters; w; w = w->next)
        if (w->tid == tid) return w;
    return NULL;
}

/* Starts and lengths are at most FILE_OFF_MAX, so start + len cannot wrap. */
static int ranges_overlap(uint64_t s1, uint64_t l1, uint64_t s2, uint64_t l2) {
    uint64_t e1 = l1 ? s1 + l1 : UINT64_MAX;
    uint64_t e2 = l2 ? s2 + l2 : UINT64_MAX;
    return s1 < e2 && s2 < e1;
}

/* The first lock held by ANOTHER open file that this request collides with,
   starting the scan at `from`; -1 if none. */
static int lock_conflict(const struct open_file *of, uint32_t entry, uint64_t start,
                         uint64_t len, uint32_t mode, int from) {
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
   and a program that gets it wrong hangs. A lock whose taker is unknown ends
   that chain: whoever holds its handle now could be anyone, so the check
   cannot tell and says no, as XNU's does for a lock that is not confined.
   TAKER_UNKNOWN is never `me` and no waiter's tid, so it needs no case of
   its own. A real cycle through it hangs, never a false E_DEADLOCK. */
static int lock_cycle(int me, const struct open_file *of, uint32_t entry, uint64_t start,
                      uint64_t len, uint32_t mode) {
    for (int i = lock_conflict(of, entry, start, len, mode, 0); i >= 0;
         i = lock_conflict(of, entry, start, len, mode, i + 1)) {
        int h = locks[i].taker;
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
static int lock_apply(struct open_file *of, uint64_t start, uint64_t len, uint32_t mode) {
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
    if (free_slot < 0) return E_NO_RESOURCES;
    locks[free_slot].of    = of;
    locks[free_slot].entry = of->entry;
    locks[free_slot].start = start;
    locks[free_slot].len   = len;
    locks[free_slot].mode  = mode;
    locks[free_slot].taker = thread_current_id();
    return E_OK;
}

/* Its handle is on the way to someone else: who holds each of its locks is
   no longer known. Under the sender's handle-table lock, so it only writes. */
static void of_travel(struct kobj *o) {
    for (int i = 0; i < MAX_FILE_LOCKS; i++)
        if (locks[i].of == (struct open_file *)o) locks[i].taker = TAKER_UNKNOWN;
}

/* Drop this open file's locks that lie wholly inside [start, start+len). A
   lock that only partly overlaps is left alone: ranges are not split, which is
   also Serenity's rule, so unlock what was locked. (0, 0) drops them all. */
static void lock_drop(const struct open_file *of, uint64_t start, uint64_t len) {
    uint64_t end = len ? start + len : UINT64_MAX;
    int any = 0;
    for (int i = 0; i < MAX_FILE_LOCKS; i++) {
        struct file_lock *l = &locks[i];
        if (l->of != of) continue;
        uint64_t lend = l->len ? l->start + l->len : UINT64_MAX;
        if (l->start < start || lend > end) continue;
        l->of = NULL;
        any = 1;
    }
    if (any) lock_wake_all();
}

/* Releasing the slot is the whole lifetime rule: its locks go with it, and a
   doomed file whose last open this was is queued for deletion. It runs when
   the last reference goes, however the handles went - SYS_FILE_CLOSE,
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
    /* Its locks go with it (above). */
    .transfer     = 1,
    .travel       = of_travel,
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

int sysfile_lock_waiting(void) {
    int n = 0;
    for (struct lock_wait *w = waiters; w; w = w->next) n++;
    return n;
}

/* ---- error translation -----------------------------------------------------
 * CXFS has its own error space (CXFS_E_*, cxfs.h) and the ABI has another
 * (E_*, cxk_abi.h). They meet here and nowhere else, so a change to either one
 * has exactly one place to be reconciled.
 *
 * CXFS_E_FRAGMENT folds into E_NO_SPACE along with CXFS_E_NOSPACE: both mean the
 * write cannot be satisfied for want of space, and the difference between "no
 * blocks" and "no blocks in one run" is not something a user program can act on
 * differently. The kernel log keeps the distinction.
 */
static int abi_err(int cxfs_rc) {
    switch (cxfs_rc) {
        case CXFS_E_OK:       return E_OK;
        case CXFS_E_PERM:     return E_ACCESS_DENIED;
        case CXFS_E_NOSPACE:  return E_NO_SPACE;
        case CXFS_E_FRAGMENT: return E_NO_SPACE;
        case CXFS_E_INVAL:    return E_INVALID_ARGS;
        case CXFS_E_NOTFOUND: return E_NOT_FOUND;
        case CXFS_E_ISDIR:    return E_IS_DIR;
        case CXFS_E_NOTDIR:   return E_NOT_DIR;
        case CXFS_E_EXISTS:   return E_ALREADY_EXISTS;
        case CXFS_E_FAIL:     return E_IO;
        default:              return (cxfs_rc < 0) ? E_IO : cxfs_rc;
    }
}

/* ---- copying from ring 3 --------------------------------------------------- */

/* Copy a NUL-terminated path in from user memory, bounded by `cap`.
   Validates one page at a time rather than one byte at a time: user_ptr_readable
   walks page tables, so per-byte would be a page-table walk per character.
   Returns the length, or a negative E_* code. */
static int copy_path_in(uint64_t addr, char *dst, uint32_t cap) {
    if (!addr) return E_BAD_ADDRESS;
    for (uint32_t i = 0; i < cap; i++) {
        if (i == 0 || (((addr + i) & 0xFFFu) == 0)) {       /* new page */
            if (!user_ptr_readable(addr + i, 1)) return E_BAD_ADDRESS;
        }
        char c = ((const char *)user_ptr(addr))[i];
        dst[i] = c;
        if (!c) return (int)i;
    }
    return E_OUT_OF_RANGE;   /* no terminator inside cap bytes */
}

/* Resolve everything but the last component of `path`, so a create knows which
   directory to create in and under what name. Returns 0, or a negative E_*. */
static int resolve_parent(const char *path, uint32_t cwd,
                          uint32_t *parent, char *leaf, uint32_t leafcap) {
    uint32_t len = 0;
    while (path[len]) len++;
    if (len == 0) return E_INVALID_ARGS;

    /* find the last '/' */
    int slash = -1;
    for (uint32_t i = 0; i < len; i++) if (path[i] == '/') slash = (int)i;

    if (slash < 0) {                       /* bare name: in the cwd */
        if (len >= leafcap) return E_OUT_OF_RANGE;
        for (uint32_t i = 0; i <= len; i++) leaf[i] = path[i];
        *parent = cwd;
        return 0;
    }

    /* "/name" -> parent is root; "a/b/name" -> parent is the resolved prefix */
    uint32_t leaflen = len - (uint32_t)slash - 1;
    if (leaflen == 0)       return E_INVALID_ARGS;   /* trailing slash */
    if (leaflen >= leafcap) return E_OUT_OF_RANGE;
    for (uint32_t i = 0; i <= leaflen; i++) leaf[i] = path[slash + 1 + i];

    if (slash == 0) { *parent = 0; return 0; }   /* 0 = root */

    char dir[FILE_PATH_MAX];
    if ((uint32_t)slash >= sizeof dir) return E_OUT_OF_RANGE;
    for (int i = 0; i < slash; i++) dir[i] = path[i];
    dir[slash] = '\0';

    int pid = cxfs_resolve(dir, cwd);
    if (pid < 0) return E_NOT_FOUND;
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

/* Fill a user-facing file_stat from a CXFS entry. The timestamps are truncated
   to 32 bits on the way out (the ABI note on file_stat); the size is whole. */
static void fill_stat(const struct cxfs_entry *e, struct file_stat *st) {
    memset(st, 0, sizeof *st);
    st->id          = e->id;
    st->kind        = (e->type == CXFS_TYPE_DIR) ? FTYPE_DIR : FTYPE_FILE;
    st->size        = e->size;
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

/* ---- the syscalls ----------------------------------------------------------
 * One function per syscall, each called after the GRANT_DISK check. Every one
 * starts the same way: the volume must be mounted, and the delete of a file
 * whose last handle went since the previous call runs first.
 */
static int fs_enter(void) {
    if (!cxfs_is_mounted()) return E_IO;
    bury_pending();
    return 0;
}

/* The calls on an open file resolve the handle once, here, and hold the
   reference until the operation returns (file_leave): a lock wait blocks, and
   the file must not be freed under it, on this CPU or another. */
static int file_enter(int32_t h, struct open_file **f, uint32_t *rights) {
    int rc = fs_enter();
    if (rc < 0) return rc;
    if (!(*f = of_ref(h, rights))) return E_BAD_HANDLE;
    return 0;
}

static int file_leave(struct open_file *f, int rc) {
    kobj_unref(&f->hdr);
    return rc;
}

int sys_file_open(uint64_t upath, uint32_t flags) {
    uint32_t cwd = thread_current_cwd();
    char path[FILE_PATH_MAX];
    int rc;

    if ((rc = fs_enter()) < 0) return rc;
    if ((rc = copy_path_in(upath, path, sizeof path)) < 0) return rc;
    if (!(flags & (FOPEN_READ | FOPEN_WRITE))) return E_INVALID_ARGS;

    int id = lookup(path, cwd);

    if (id < 0) {
        if (!(flags & FOPEN_CREATE)) return E_NOT_FOUND;
        /* an unlinked file still open elsewhere holds its name until
           its last close, as a delete-pending file does on NT */
        if (cxfs_resolve(path, cwd) >= 0) return E_ALREADY_EXISTS;
        uint32_t parent;
        char leaf[FILE_NAME_MAX];
        if ((rc = resolve_parent(path, cwd, &parent, leaf, sizeof leaf)) < 0) return rc;
        id = cxfs_create_entry(parent, leaf, CXFS_TYPE_FILE);
        if (id < 0) return E_IO;
    } else {
        struct cxfs_entry e;
        if (cxfs_read_entry((uint32_t)id, &e) != 0) return E_IO;
        if (e.type == CXFS_TYPE_DIR) return E_IS_DIR;
        if ((flags & FOPEN_TRUNC) && (flags & FOPEN_WRITE)) {
            rc = cxfs_truncate((uint32_t)id, 0);
            if (rc != CXFS_E_OK) return abi_err(rc);
        }
    }

    /* Take the handle slot LAST. Everything above can fail, and a slot
       claimed before a failure would be a handle leaked per failed
       open - the sort of thing a program retrying in a loop turns into
       an exhausted table. */
    struct open_file *nf = of_alloc();
    if (!nf) return E_NO_MEMORY;
    nf->entry = (uint32_t)id;
    nf->flags = flags;
    nf->off   = 0;
    if (flags & FOPEN_APPEND) {
        struct cxfs_entry e;
        if (cxfs_read_entry(nf->entry, &e) == 0) nf->off = e.size;
    }

    int32_t h = htab_install(thread_htab(thread_current_id()), &nf->hdr,
                             (flags & (FOPEN_READ | FOPEN_WRITE)) | HRIGHT_DUPLICATE, 0);
    /* The open's own reference goes either way: on success the handle
       holds the file; on failure it goes back to its cache here. */
    kobj_unref(&nf->hdr);
    return h < 0 ? E_NO_RESOURCES : h;
}

int sys_file_close(int32_t h) {
    struct open_file *f;
    uint32_t rights;
    int rc = file_enter(h, &f, &rights);
    if (rc < 0) return rc;
    /* The file itself goes when file_leave drops the last reference, and the
       delete of an unlinked file runs after that. */
    rc = (htab_close(thread_htab(thread_current_id()), h) != 0) ? E_BAD_HANDLE : E_OK;
    file_leave(f, rc);
    bury_pending();
    return rc;
}

static int file_read(struct open_file *f, uint32_t rights, uint64_t buf, uint64_t n) {
    if (!(rights & FOPEN_READ)) return E_ACCESS_DENIED;
    if (n == 0) return 0;
    if (n > FILE_IO_MAX) n = FILE_IO_MAX;   /* a short count is normal */
    if (!user_ptr_writable(buf, (uint32_t)n)) return E_BAD_ADDRESS;

    int rc = cxfs_read_at(f->entry, f->off, user_ptr(buf), (uint32_t)n);
    if (rc < 0) return abi_err(rc);
    f->off += (uint32_t)rc;
    return rc;
}

int sys_file_read(int32_t h, uint64_t buf, uint64_t n) {
    struct open_file *f;
    uint32_t rights;
    int rc = file_enter(h, &f, &rights);
    return rc < 0 ? rc : file_leave(f, file_read(f, rights, buf, n));
}

static int file_write(struct open_file *f, uint32_t rights, uint64_t buf, uint64_t n) {
    if (!(rights & FOPEN_WRITE)) return E_ACCESS_DENIED;
    if (n == 0) return 0;
    if (n > FILE_IO_MAX) n = FILE_IO_MAX;   /* a short count is normal */
    if (!user_ptr_readable(buf, (uint32_t)n)) return E_BAD_ADDRESS;
    /* refuse rather than wrap: no offset passes FILE_OFF_MAX */
    if (f->off > FILE_OFF_MAX - n) return E_OUT_OF_RANGE;

    /* APPEND means "at the end as it is now", not "at the end as it was
       when you opened it" - re-read the size each time, so two writers
       appending to one file do not overwrite each other. */
    if (f->flags & FOPEN_APPEND) {
        struct cxfs_entry e;
        if (cxfs_read_entry(f->entry, &e) != 0) return E_IO;
        f->off = e.size;
        if (f->off > FILE_OFF_MAX - n) return E_OUT_OF_RANGE;
    }

    int rc = cxfs_write_at(f->entry, f->off, user_ptr(buf), (uint32_t)n);
    if (rc < 0) return abi_err(rc);
    f->off += (uint32_t)rc;
    return rc;
}

int sys_file_write(int32_t h, uint64_t buf, uint64_t n) {
    struct open_file *f;
    uint32_t rights;
    int rc = file_enter(h, &f, &rights);
    return rc < 0 ? rc : file_leave(f, file_write(f, rights, buf, n));
}

/* The new offset goes to *pos (a user address, 0 for none), checked before the
   offset moves, so a bad pointer leaves the file where it was. */
static int file_seek(struct open_file *f, int64_t off, uint32_t origin, uint64_t pos) {
    if (pos && !user_ptr_writable(pos, sizeof(uint64_t))) return E_BAD_ADDRESS;
    struct cxfs_entry e;
    if (cxfs_read_entry(f->entry, &e) != 0) return E_IO;

    int64_t base;
    switch (origin) {
        case FSEEK_SET: base = 0;                break;
        case FSEEK_CUR: base = (int64_t)f->off;  break;
        case FSEEK_END: base = (int64_t)e.size;  break;   /* CXFS: under 2^44 */
        default: return E_INVALID_ARGS;
    }
    int64_t to;
    if (__builtin_add_overflow(base, off, &to) || to < 0) return E_OUT_OF_RANGE;
    /* Seeking past EOF is allowed and allocates nothing: the hole only
       becomes real on the write that reaches it. */
    f->off = (uint64_t)to;
    if (pos) *(uint64_t *)user_ptr(pos) = f->off;
    return E_OK;
}

int sys_file_seek(int32_t h, int64_t off, uint32_t origin, uint64_t pos) {
    struct open_file *f;
    uint32_t rights;
    int rc = file_enter(h, &f, &rights);
    return rc < 0 ? rc : file_leave(f, file_seek(f, off, origin, pos));
}

int sys_file_tell(int32_t h, uint64_t pos) {
    struct open_file *f;
    uint32_t rights;
    int rc = file_enter(h, &f, &rights);
    if (rc < 0) return rc;
    if (user_copy_out(pos, &f->off, sizeof f->off) != 0) return file_leave(f, E_BAD_ADDRESS);
    return file_leave(f, E_OK);
}

static int file_truncate(struct open_file *f, uint32_t rights, uint64_t size) {
    if (!(rights & FOPEN_WRITE)) return E_ACCESS_DENIED;
    if (size > FILE_OFF_MAX) return E_OUT_OF_RANGE;
    return abi_err(cxfs_truncate(f->entry, size));
}

int sys_file_truncate(int32_t h, uint64_t size) {
    struct open_file *f;
    uint32_t rights;
    int rc = file_enter(h, &f, &rights);
    return rc < 0 ? rc : file_leave(f, file_truncate(f, rights, size));
}

/* STAT and FSTAT: entry `id` into the caller's file_stat at `st`. */
static int stat_out(uint32_t id, uint64_t st) {
    if (!user_ptr_writable(st, sizeof(struct file_stat))) return E_BAD_ADDRESS;
    struct cxfs_entry e;
    if (cxfs_read_entry(id, &e) != 0) return E_IO;
    fill_stat(&e, (struct file_stat *)user_ptr(st));
    return E_OK;
}

int sys_file_stat(uint64_t upath, uint64_t st) {
    char path[FILE_PATH_MAX];
    int rc;
    if ((rc = fs_enter()) < 0) return rc;
    if ((rc = copy_path_in(upath, path, sizeof path)) < 0) return rc;
    int r = lookup(path, thread_current_cwd());
    if (r < 0) return E_NOT_FOUND;
    return stat_out((uint32_t)r, st);
}

int sys_file_fstat(int32_t h, uint64_t st) {
    struct open_file *f;
    uint32_t rights;
    int rc = file_enter(h, &f, &rights);
    return rc < 0 ? rc : file_leave(f, stat_out(f->entry, st));
}

int sys_file_readdir(uint64_t upath, uint32_t index, uint64_t st) {
    char path[FILE_PATH_MAX];
    int rc;
    if ((rc = fs_enter()) < 0) return rc;
    if ((rc = copy_path_in(upath, path, sizeof path)) < 0) return rc;
    if (!user_ptr_writable(st, sizeof(struct file_stat))) return E_BAD_ADDRESS;

    int r = lookup(path, thread_current_cwd());
    if (r < 0) return E_NOT_FOUND;
    struct cxfs_entry dir;
    if (cxfs_read_entry((uint32_t)r, &dir) != 0) return E_IO;
    if (dir.type != CXFS_TYPE_DIR) return E_NOT_DIR;

    rd_want  = index;
    rd_seen  = 0;
    rd_found = 0;
    cxfs_list_dir((uint32_t)r, rd_cb);
    if (!rd_found) return 0;                 /* index past the end */
    fill_stat(&rd_entry, (struct file_stat *)user_ptr(st));
    return 1;
}

int sys_file_mkdir(uint64_t upath) {
    uint32_t cwd = thread_current_cwd();
    char path[FILE_PATH_MAX];
    int rc;
    if ((rc = fs_enter()) < 0) return rc;
    if ((rc = copy_path_in(upath, path, sizeof path)) < 0) return rc;
    if (cxfs_resolve(path, cwd) >= 0) return E_ALREADY_EXISTS;
    uint32_t parent;
    char leaf[FILE_NAME_MAX];
    if ((rc = resolve_parent(path, cwd, &parent, leaf, sizeof leaf)) < 0) return rc;
    return (cxfs_create_entry(parent, leaf, CXFS_TYPE_DIR) < 0) ? E_IO : E_OK;
}

int sys_file_unlink(uint64_t upath) {
    uint32_t cwd = thread_current_cwd();
    char path[FILE_PATH_MAX];
    int rc;
    if ((rc = fs_enter()) < 0) return rc;
    if ((rc = copy_path_in(upath, path, sizeof path)) < 0) return rc;
    int r = lookup(path, cwd);
    if (r < 0) return E_NOT_FOUND;
    if (r == 0) return E_INVALID_ARGS;                    /* never the root */

    struct cxfs_entry e;
    if (cxfs_read_entry((uint32_t)r, &e) != 0) return E_IO;

    /* An unlink is a write to the entry, so it needs the entry's write
       permission - cxfs_delete_entry does not check, because its other
       caller is the kernel tidying up after itself. */
    if (thread_current_uid() != 0) {
        uint16_t need = (thread_current_uid() == e.owner_uid)
                        ? CXFS_PERM_OW : CXFS_PERM_TW;
        if (!(e.permissions & need)) return E_ACCESS_DENIED;
    }
    if (e.type == CXFS_TYPE_DIR && cxfs_count_children((uint32_t)r) > 0)
        return E_INVALID_ARGS;                            /* must be empty */
    if ((uint32_t)r == cwd) return E_INVALID_ARGS;         /* not your own cwd */

    /* Still open somewhere: the name goes now, the entry at the last
       close (see "unlinked while open" above). */
    if (entry_open((uint32_t)r)) {
        struct of_match m = { (uint32_t)r, 0, 1 };
        ocache_walk(&of_cache, of_match_fn, &m);
        return E_OK;
    }
    return (cxfs_delete_entry((uint32_t)r) == 0) ? E_OK : E_IO;
}

int sys_file_rename(uint64_t upath, uint64_t unewname) {
    char path[FILE_PATH_MAX];
    char newname[FILE_NAME_MAX];
    int rc;
    if ((rc = fs_enter()) < 0) return rc;
    if ((rc = copy_path_in(upath, path, sizeof path)) < 0) return rc;
    if ((rc = copy_path_in(unewname, newname, sizeof newname)) < 0) return rc;

    if (names_self(path)) return E_INVALID_ARGS;   /* "." / ".." name a directory,
                                               not an entry within it */
    int r = lookup(path, thread_current_cwd());
    if (r < 0) return E_NOT_FOUND;
    if (r == 0) return E_INVALID_ARGS;

    /* cxfs_rename deliberately does not check for collisions (its doc
       says the caller does), so check here - otherwise two entries in
       one directory share a name and lookups become order-dependent. */
    struct cxfs_entry e;
    if (cxfs_read_entry((uint32_t)r, &e) != 0) return E_IO;
    int clash = cxfs_find_in_dir(e.parent_id, newname);
    if (clash >= 0 && clash != r) return E_ALREADY_EXISTS;

    return (cxfs_rename((uint32_t)r, newname) == 0) ? E_OK : E_INVALID_ARGS;
}

int sys_file_chdir(uint64_t upath) {
    char path[FILE_PATH_MAX];
    int rc;
    if ((rc = fs_enter()) < 0) return rc;
    if ((rc = copy_path_in(upath, path, sizeof path)) < 0) return rc;
    int r = lookup(path, thread_current_cwd());
    if (r < 0) return E_NOT_FOUND;
    struct cxfs_entry e;
    if (cxfs_read_entry((uint32_t)r, &e) != 0) return E_IO;
    if (e.type != CXFS_TYPE_DIR) return E_NOT_DIR;
    thread_set_cwd(thread_current_id(), (uint32_t)r);
    return E_OK;
}

int sys_file_getcwd(uint64_t buf, uint64_t cap) {
    char path[FILE_PATH_MAX];
    int rc;
    if ((rc = fs_enter()) < 0) return rc;
    if (cap == 0) return E_BUFFER_TOO_SMALL;
    if (cap > FILE_PATH_MAX) cap = FILE_PATH_MAX;
    if (!user_ptr_writable(buf, (uint32_t)cap)) return E_BAD_ADDRESS;
    cxfs_path_of(thread_current_cwd(), path, (int)cap);
    uint32_t n = 0;
    while (n + 1 < cap && path[n]) n++;
    memcpy(user_ptr(buf), path, n);
    ((char *)user_ptr(buf))[n] = '\0';
    return (int)n;
}

/* Commit what has been written to stable media.
 *
 * Takes no arguments and touches no handle: CXFS writes through, so
 * there is nothing buffered in the filesystem to pick out - what this
 * asks is that the DEVICE commit its cache, which is per-volume and
 * not per-file. Returning E_IO when the device cannot confirm is the
 * point; a caller told a write is durable when it is not is worse off
 * than one told nothing. */
int sys_file_sync(void) {
    int rc = fs_enter();
    if (rc < 0) return rc;
    return (cxfs_sync() == 0) ? E_OK : E_IO;
}

/* Without FLOCK_WAIT a conflict is E_BUSY at once. With it the thread
   sleeps until a release, then looks again, as Serenity's FlockBlocker
   does - unless waiting could never end, which is E_DEADLOCK. A
   conversion that conflicts leaves the lock already held unchanged. */
static int file_lock(struct open_file *f, uint64_t start, uint64_t len, uint32_t flags) {
    uint32_t mode = flags & ~(uint32_t)FLOCK_WAIT;
    if (mode != FLOCK_SHARED && mode != FLOCK_EXCLUSIVE) return E_INVALID_ARGS;
    if (start > FILE_OFF_MAX || len > FILE_OFF_MAX) return E_OUT_OF_RANGE;

    int me = thread_current_id();
    while (lock_conflict(f, f->entry, start, len, mode, 0) >= 0) {
        if (!(flags & FLOCK_WAIT)) return E_BUSY;
        for (struct lock_wait *w = waiters; w; w = w->next) w->seen = 0;
        if (lock_cycle(me, f, f->entry, start, len, mode)) return E_DEADLOCK;

        struct lock_wait self = { .tid = me, .of = f, .entry = f->entry,
                                  .start = start, .len = len, .mode = mode,
                                  .next = waiters };
        waiters = &self;
        thread_wait();
        struct lock_wait **pp = &waiters;
        while (*pp != &self) pp = &(*pp)->next;
        *pp = self.next;
        if (thread_killed()) return E_CANCELED;
    }
    return lock_apply(f, start, len, mode);
}

/* The range and mode arrive in a struct file_lock_args: two u64s and a mode
   are six registers on x86-32, one more than a syscall has. */
int sys_file_lock(int32_t h, uint64_t args) {
    struct open_file *f;
    uint32_t rights;
    int rc = file_enter(h, &f, &rights);
    if (rc < 0) return rc;
    struct file_lock_args a;
    if (user_copy_in(&a, args, sizeof a) != 0) return file_leave(f, E_BAD_ADDRESS);
    if (a.reserved != 0) return file_leave(f, E_INVALID_ARGS);
    return file_leave(f, file_lock(f, a.start, a.len, a.mode));
}

int sys_file_unlock(int32_t h, uint64_t start, uint64_t len) {
    struct open_file *f;
    uint32_t rights;
    int rc = file_enter(h, &f, &rights);
    if (rc < 0) return rc;
    if (start > FILE_OFF_MAX || len > FILE_OFF_MAX) return file_leave(f, E_OUT_OF_RANGE);
    lock_drop(f, start, len);
    return file_leave(f, E_OK);
}
