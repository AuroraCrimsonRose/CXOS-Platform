/* /kernel/cpu/sysfile.c */
/* Aurora Tejeda / CATX Systems */
/* SYS_FILE_OP - see sysfile.h. */

#include "sysfile.h"
#include "sched.h"
#include "handle.h"
#include "usermode.h"       /* user_ptr_readable / user_ptr_writable */
#include "cxfs.h"
#include "string.h"
#include <stddef.h>

/* ---- open files ------------------------------------------------------------
 * A fixed pool rather than kmalloc per open: an open file is 16 bytes, the
 * handle table caps a process at CXK_MAX_HANDLES anyway, and a pool cannot
 * fragment the heap or fail an allocation half way through an open.
 *
 * The pool slot is what a HANDLE_FILE handle points at. Its lifetime is the
 * handle's, enforced by the releaser registered in sysfile_init: whether the
 * process calls FILE_OP_CLOSE, calls SYS_HANDLE_CLOSE, or exits and gets
 * reaped, the slot comes back.
 */
#define MAX_OPEN_FILES 32

struct open_file {
    int      used;
    uint32_t entry;      /* CXFS manifest id */
    uint32_t off;        /* current offset (see the INT32_MAX cap in the ABI) */
    uint32_t flags;      /* FOPEN_* the file was opened with */
};

static struct open_file open_files[MAX_OPEN_FILES];

static struct open_file *of_alloc(void) {
    for (int i = 0; i < MAX_OPEN_FILES; i++)
        if (!open_files[i].used) {
            open_files[i].used  = 1;
            open_files[i].entry = 0;
            open_files[i].off   = 0;
            open_files[i].flags = 0;
            return &open_files[i];
        }
    return NULL;
}

static void of_release(struct cap_handle *h) {
    struct open_file *f = (struct open_file *)h->object;
    if (f >= open_files && f < open_files + MAX_OPEN_FILES) f->used = 0;
}

void sysfile_init(void) {
    for (int i = 0; i < MAX_OPEN_FILES; i++) open_files[i].used = 0;
    handle_set_release(HANDLE_FILE, of_release);
}

int sysfile_open_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_OPEN_FILES; i++) if (open_files[i].used) n++;
    return n;
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
        case CXFS_E_LOCKED:   return E_AGAIN;
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

/* look up one of the caller's open files */
static struct open_file *of_from_handle(int h) {
    struct cap_handle *ch = thread_handle_get(thread_current_id(), h);
    if (!ch || ch->type != HANDLE_FILE) return NULL;
    return (struct open_file *)ch->object;
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
    if (rd_seen++ == rd_want && !rd_found) {
        rd_entry = *e;
        rd_found = 1;
    }
}

/* ---- the syscall ---------------------------------------------------------- */

int sys_file_op(const struct file_op_args *ua) {
    if (!user_ptr_readable((uint32_t)ua, sizeof *ua)) return E_FAULT;
    struct file_op_args a = *ua;          /* copy in: never re-read user memory */

    if (!cxfs_is_mounted()) return E_IO;

    uint32_t cwd = thread_current_cwd();
    char path[FILE_PATH_MAX];
    int rc;

    switch (a.op) {

        case FILE_OP_OPEN: {
            if ((rc = copy_path_in(a.path, path, sizeof path)) < 0) return rc;
            if (!(a.flags & (FOPEN_READ | FOPEN_WRITE))) return E_INVAL;

            int id = cxfs_resolve(path, cwd);

            if (id < 0) {
                if (!(a.flags & FOPEN_CREATE)) return E_NOENT;
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
            struct open_file *f = of_alloc();
            if (!f) return E_NOMEM;
            f->entry = (uint32_t)id;
            f->flags = a.flags;
            f->off   = 0;
            if (a.flags & FOPEN_APPEND) {
                struct cxfs_entry e;
                if (cxfs_read_entry(f->entry, &e) == 0) f->off = (uint32_t)e.size;
            }

            int h = thread_handle_install(thread_current_id(), HANDLE_FILE,
                                          (uint8_t)(a.flags & (FOPEN_READ | FOPEN_WRITE)), f);
            if (h < 0) { f->used = 0; return E_NOMEM; }
            return h;
        }

        case FILE_OP_CLOSE:
            if (!of_from_handle(a.handle)) return E_BADF;
            return (thread_handle_close(thread_current_id(), a.handle) == 0) ? E_OK : E_BADF;

        case FILE_OP_READ: {
            struct open_file *f = of_from_handle(a.handle);
            if (!f) return E_BADF;
            if (!(f->flags & FOPEN_READ)) return E_PERM;
            if (a.len == 0) return 0;
            if (a.len > 0x7FFFFFFFu) return E_RANGE;
            if (!user_ptr_writable((uint32_t)a.data, a.len)) return E_FAULT;

            rc = cxfs_read_at(f->entry, f->off, a.data, a.len);
            if (rc < 0) return abi_err(rc);
            f->off += (uint32_t)rc;
            return rc;
        }

        case FILE_OP_WRITE: {
            struct open_file *f = of_from_handle(a.handle);
            if (!f) return E_BADF;
            if (!(f->flags & FOPEN_WRITE)) return E_PERM;
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
            struct open_file *f = of_from_handle(a.handle);
            if (!f) return E_BADF;

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
            struct open_file *f = of_from_handle(a.handle);
            if (!f) return E_BADF;
            return (int)f->off;
        }

        case FILE_OP_TRUNC: {
            struct open_file *f = of_from_handle(a.handle);
            if (!f) return E_BADF;
            if (!(f->flags & FOPEN_WRITE)) return E_PERM;
            if (a.off < 0) return E_INVAL;
            return abi_err(cxfs_truncate(f->entry, (uint64_t)(uint32_t)a.off));
        }

        case FILE_OP_STAT:
        case FILE_OP_FSTAT: {
            uint32_t id;
            if (a.op == FILE_OP_STAT) {
                if ((rc = copy_path_in(a.path, path, sizeof path)) < 0) return rc;
                int r = cxfs_resolve(path, cwd);
                if (r < 0) return E_NOENT;
                id = (uint32_t)r;
            } else {
                struct open_file *f = of_from_handle(a.handle);
                if (!f) return E_BADF;
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

            int r = cxfs_resolve(path, cwd);
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
            int r = cxfs_resolve(path, cwd);
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
            if (cxfs_is_locked((uint32_t)r)) return E_AGAIN;
            if ((uint32_t)r == cwd) return E_INVAL;         /* not your own cwd */

            return (cxfs_delete_entry((uint32_t)r) == 0) ? E_OK : E_IO;
        }

        case FILE_OP_RENAME: {
            if ((rc = copy_path_in(a.path, path, sizeof path)) < 0) return rc;
            char newname[FILE_NAME_MAX];
            if ((rc = copy_path_in(a.path2, newname, sizeof newname)) < 0) return rc;

            if (names_self(path)) return E_INVAL;   /* "." / ".." name a directory,
                                                       not an entry within it */
            int r = cxfs_resolve(path, cwd);
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
            int r = cxfs_resolve(path, cwd);
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
