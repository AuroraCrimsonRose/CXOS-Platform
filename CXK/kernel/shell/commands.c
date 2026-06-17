/* /CXLite/kernel/shell/commands.c */
/* Aurora Tejeda */
/* All shell command handlers and the command table. */

#include "commands.h"
#include "usermode.h"
#include "sched.h"
#include "console.h"
#include "demo.h"
#include "pci.h"
#include "ahci.h"
#include "ohci.h"
#include "e1000.h"
#include "netif.h"
#include "arp.h"
#include "ip.h"
#include "icmp.h"
#include "disk.h"
#include "vga.h"
#include "string.h"
#include "timer.h"
#include "power.h"
#include "pmm.h"
#include "heap.h"
#include "ata.h"
#include "cxfs.h"
#include "rtc.h"
#include "keyboard.h"
#include "shell.h"
#include <stdint.h>

/* E820 memory map left by the bootloader (raw buffer = all regions). */
#define E820_BUFFER       0x1000
#define E820_ENTRY_COUNT  0x1600

struct e820_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t flags;
} __attribute__((packed));

static const char *e820_type_name(uint32_t t) {
    switch (t) {
        case 1: return "Usable";
        case 2: return "Reserved";
        case 3: return "ACPI reclaim";
        case 4: return "ACPI NVS";
        case 5: return "Bad";
        default: return "Unknown";
    }
}

/* ---- command handlers ---- */
/* each takes the argument tail (text after the command word) */

static void cmd_help(const char *args);   /* fwd decl: needs the table */

static void cmd_clear(const char *args) {
    (void)args;
    console_clear();
}

static void cmd_echo(const char *args) {
    console_print(args);
    console_putc('\n');
}

static void cmd_ver(const char *args) {
    (void)args;
    console_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    console_print("CXK");
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    console_print(" - V0.0.1.10 - Made by Aurora Tejeda\n");
}

static void cmd_meminfo(const char *args) {
    (void)args;
    uint16_t count = *(volatile uint16_t *)E820_ENTRY_COUNT;
    struct e820_entry *e = (struct e820_entry *)E820_BUFFER;

    if (count == 0) {
        console_print("No E820 memory map available.\n");
        return;
    }

    console_set_color(VGA_WHITE, VGA_BLACK);
    console_print("BASE             LENGTH           TYPE\n");
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    uint64_t total_usable = 0;
    for (int i = 0; i < count; i++) {
        /* base (show as two 32-bit halves: high then low) */
        console_print_hex((uint32_t)(e[i].base >> 32));
        console_print_hex((uint32_t)(e[i].base & 0xFFFFFFFF));
        console_print(" ");
        console_print_hex((uint32_t)(e[i].length >> 32));
        console_print_hex((uint32_t)(e[i].length & 0xFFFFFFFF));
        console_print(" ");
        console_print(e820_type_name(e[i].type));
        console_putc('\n');

        if (e[i].type == 1) total_usable += e[i].length;
    }

    console_print("Total usable: ");
    console_print_dec((uint32_t)(total_usable / (1024 * 1024)));
    console_print(" MB\n");
}

/* print the uptime as DD:HH:MM:SS:mmm (no trailing newline).
   returns the number of characters printed (so it can be erased for -p). */
static int uptime_print(void) {
    uint32_t ms_total = timer_ticks();   /* 1 tick = 1 ms at 1000 Hz */
    /* NOTE: uint32_t ms wraps after ~49.7 days of uptime. */

    uint32_t ms  = ms_total % 1000;
    uint32_t s   = ms_total / 1000;
    uint32_t ss  = s % 60;
    uint32_t m   = s / 60;
    uint32_t mm  = m % 60;
    uint32_t h   = m / 60;
    uint32_t hh  = h % 24;
    uint32_t dd  = h / 24;

    console_print_dec(dd / 10); console_print_dec(dd % 10); console_putc(':');
    console_print_dec(hh / 10); console_print_dec(hh % 10); console_putc(':');
    console_print_dec(mm / 10); console_print_dec(mm % 10); console_putc(':');
    console_print_dec(ss / 10); console_print_dec(ss % 10); console_putc(':');
    console_print_dec((ms / 100) % 10);
    console_print_dec((ms / 10) % 10);
    console_print_dec(ms % 10);
    return 15;   /* DD:HH:MM:SS:mmm = 15 chars */
}

static void cmd_uptime(const char *args) {
    int persist = 0;
    while (args[0] == '-') {
        const char *f = args + 1;
        while (*f && *f != ' ') { if (*f == 'p') persist = 1; f++; }
        while (*args && *args != ' ') args++;
        while (*args == ' ') args++;
    }

    if (!persist) {
        uptime_print();
        console_putc('\n');
        return;
    }

    /* -p: live update in place until Ctrl+C */
    console_print("Live uptime (Ctrl+C to stop):\n");
    for (;;) {
        int n = uptime_print();
        /* poll for Ctrl+C a few times while waiting ~100 ms between refreshes */
        for (int i = 0; i < 10; i++) {
            char c = keyboard_getchar();   /* non-blocking */
            if ((unsigned char)c == KEY_CTRL_C) {
                console_print("\n^C\n");
                return;
            }
            timer_sleep(10);               /* 10 ms x 10 = ~100 ms refresh */
        }
        /* erase the printed value to redraw in place */
        for (int i = 0; i < n; i++) console_putc('\b');
    }
}

static void cmd_reboot(const char *args) {
    (void)args;
    console_print("Rebooting...\n");
    power_reboot();
}

static void cmd_shutdown(const char *args) {
    (void)args;
    power_shutdown();
}

static void cmd_sleep(const char *args) {
    while (*args == ' ') args++;
    /* optional <seconds>: idle for that long (C1). no arg: S1-or-C1 until key. */
    if (*args >= '0' && *args <= '9') {
        uint32_t secs = 0;
        while (*args >= '0' && *args <= '9') { secs = secs * 10 + (uint32_t)(*args - '0'); args++; }
        console_print("Sleeping ");
        console_print_dec(secs);
        console_print("s (press a key to wake early)...\n");
        power_sleep(secs * 1000);   /* ms */
        console_print("Awake.\n");
        return;
    }
    power_sleep(0);   /* indefinite: S1 if available, else C1, until keypress */
}

static void cmd_pmm(const char *args) {
    (void)args;
    uint32_t total = pmm_total_pages();
    uint32_t used  = pmm_used_pages();
    uint32_t freep = pmm_free_count();

    console_print("Physical memory (4 KB pages):\n");
    console_print("  total: "); console_print_dec(total);
    console_print(" pages ("); console_print_dec(total * 4 / 1024);
    console_print(" MB)\n");
    console_print("  used:  "); console_print_dec(used);
    console_print(" pages ("); console_print_dec(used * 4 / 1024);
    console_print(" MB)\n");
    console_print("  free:  "); console_print_dec(freep);
    console_print(" pages ("); console_print_dec(freep * 4 / 1024);
    console_print(" MB)\n");
}

static void cmd_heaptest(const char *args) {
    (void)args;
    console_print("Allocating a=64, b=128, c=32 bytes...\n");
    void *a = kmalloc(64);
    void *b = kmalloc(128);
    void *c = kmalloc(32);
    console_print("  a = "); console_print_hex((uint32_t)a); console_putc('\n');
    console_print("  b = "); console_print_hex((uint32_t)b); console_putc('\n');
    console_print("  c = "); console_print_hex((uint32_t)c); console_putc('\n');

    console_print("Freeing b, then allocating d=100 (should reuse b's space)...\n");
    kfree(b);
    void *d = kmalloc(100);
    console_print("  d = "); console_print_hex((uint32_t)d); console_putc('\n');

    kfree(a); kfree(c); kfree(d);
    console_print("Freed all. Heap used: ");
    console_print_dec(heap_bytes_used());
    console_print(" bytes\n");
}

static void cmd_disktest(const char *args) {
    uint8_t drive = 1;   /* default: ata primary slave = filesystem disk */

    /* optional drive number argument (0-3) */
    while (*args == ' ') args++;
    if (*args >= '0' && *args <= '3') drive = (uint8_t)(*args - '0');

    if (!ata_present(drive)) {
        console_set_color(VGA_LIGHT_RED, VGA_BLACK);
        console_print("Drive ");
        console_print_dec(drive);
        console_print(" not present. (Try 'disks' to see what's detected.)\n");
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    console_set_color(VGA_YELLOW, VGA_BLACK);
    console_print("WARNING: this WRITES to drive ");
    console_print_dec(drive);
    console_print(" at LBA 100 (destroys data there).\n");
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    /* build a recognizable pattern */
    static uint8_t wbuf[512];
    static uint8_t rbuf[512];
    for (int i = 0; i < 512; i++) wbuf[i] = (uint8_t)(i & 0xFF);
    /* drop a readable signature at the start so it's easy to spot in hex */
    const char *sig = "CXK-DISKTEST!";
    for (int i = 0; sig[i]; i++) wbuf[i] = (uint8_t)sig[i];

    console_print("Writing test pattern to LBA 100...\n");
    if (ata_write(drive, 100, 1, wbuf) != 0) {
        console_print("Write failed.\n"); return;
    }

    console_print("Reading it back...\n");
    for (int i = 0; i < 512; i++) rbuf[i] = 0;
    if (ata_read(drive, 100, 1, rbuf) != 0) {
        console_print("Read failed.\n"); return;
    }

    /* verify */
    int ok = 1;
    for (int i = 0; i < 512; i++) if (wbuf[i] != rbuf[i]) { ok = 0; break; }

    if (ok) {
        console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        console_print("Round-trip OK - data matches!\n");
    } else {
        console_set_color(VGA_LIGHT_RED, VGA_BLACK);
        console_print("Mismatch - read data differs from written.\n");
    }
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

static void cmd_format(const char *args) {
    (void)args;
    console_print("Formatting filesystem disk as CXFS...\n");
    if (cxfs_format() == 0) {
        console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        console_print("Format complete. CXFS ready.\n");
    } else {
        console_set_color(VGA_LIGHT_RED, VGA_BLACK);
        console_print("Format failed (disk present?).\n");
    }
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

static void cmd_fsinfo(const char *args) {
    (void)args;
    if (!cxfs_is_mounted()) {
        console_print("No CXFS mounted. Run 'format' first.\n");
        return;
    }
    const struct cxfs_superblock *s = cxfs_get_superblock();
    console_print("CXFS filesystem:\n");
    console_print("  version:        "); console_print_dec(s->version); console_putc('\n');
    console_print("  block size:     "); console_print_dec(s->block_size); console_print(" bytes\n");
    console_print("  total blocks:   "); console_print_dec(s->total_blocks); console_putc('\n');
    console_print("  bitmap start:   "); console_print_dec(s->bitmap_start); console_putc('\n');
    console_print("  manifest start: "); console_print_dec(s->manifest_start); console_putc('\n');
    console_print("  manifest count: "); console_print_dec(s->manifest_count); console_putc('\n');
    console_print("  data start:     "); console_print_dec(s->data_start); console_putc('\n');
    console_print("  reserved blocks:"); console_print_dec(s->reserved_blocks); console_putc('\n');
}

static void cmd_fstest(const char *args) {
    (void)args;
    if (!cxfs_is_mounted()) { console_print("No CXFS mounted.\n"); return; }

    console_print("Free data blocks before: ");
    console_print_dec(cxfs_free_blocks()); console_putc('\n');

    /* allocate three blocks */
    uint32_t b1 = cxfs_alloc_block();
    uint32_t b2 = cxfs_alloc_block();
    uint32_t b3 = cxfs_alloc_block();
    console_print("Allocated blocks: ");
    console_print_dec(b1); console_print(", ");
    console_print_dec(b2); console_print(", ");
    console_print_dec(b3); console_putc('\n');

    console_print("Free after alloc:  ");
    console_print_dec(cxfs_free_blocks()); console_putc('\n');

    /* free the middle one, re-alloc (should reuse b2) */
    cxfs_free_block(b2);
    uint32_t b4 = cxfs_alloc_block();
    console_print("Freed "); console_print_dec(b2);
    console_print(", re-allocated "); console_print_dec(b4);
    console_print(b4 == b2 ? " (reused!)\n" : " (different)\n");

    /* clean up */
    cxfs_free_block(b1); cxfs_free_block(b3); cxfs_free_block(b4);
    console_print("Freed all. Free now: ");
    console_print_dec(cxfs_free_blocks()); console_putc('\n');

    /* manifest: allocate an entry slot */
    int id = cxfs_alloc_entry();
    console_print("First free manifest slot: ");
    if (id < 0) console_print("none\n");
    else { console_print_dec((uint32_t)id); console_putc('\n'); }
}

/* ---- current working directory (shell state) ---- */
/* tracked by entry id; the manifest is the source of truth (per CXFS design).
   pwd reconstructs the path string by walking parent_ids back to root. */

/* case-insensitive ASCII string compare (local to the shell commands) */
static int ci_equals(const char *a, const char *b) {
    return strcasecmp(a, b) == 0;   /* uses the shared library strcasecmp */
}

static uint32_t cwd_id = 0;   /* 0 = root */

/* print a name, then pad with spaces to column `width` (one entry per line).
   if the name is longer than `width`, just emit a single trailing space
   (ragged) so the line doesn't blow past the screen edge. */
#define LS_NAME_COL 20
static void print_padded_name(const char *name) {
    int n = 0;
    while (name[n]) n++;
    console_print(name);
    if (n < LS_NAME_COL) {
        for (int i = n; i < LS_NAME_COL; i++) console_putc(' ');
    } else {
        console_putc(' ');
    }
}

/* callback for plain ls: one entry per line, dirs marked [DIR] in light blue */
static void ls_print_cb(const struct cxfs_entry *e) {
    if (e->type == CXFS_TYPE_DIR) {
        console_set_color(VGA_LIGHT_BLUE, VGA_BLACK);
        print_padded_name(e->name);
        console_print("[DIR]");
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    } else {
        console_print(e->name);
    }
    console_putc('\n');
}

/* detailed (ls -l) callback: NAME  id N  parent N  size N B / [DIR] */
static void ls_long_cb(const struct cxfs_entry *e) {
    if (e->type == CXFS_TYPE_DIR) {
        console_set_color(VGA_LIGHT_BLUE, VGA_BLACK);
        print_padded_name(e->name);
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    } else {
        print_padded_name(e->name);
    }
    console_print("id ");      console_print_dec(e->id);
    console_print("    parent ");  console_print_dec(e->parent_id);
    console_print("    ");
    if (e->type == CXFS_TYPE_DIR) {
        console_set_color(VGA_LIGHT_BLUE, VGA_BLACK);
        console_print("[DIR]");
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    } else {
        console_print("size ");
        console_print_dec(e->size);
        console_print(" B");
    }
    console_putc('\n');
}

static void cmd_pwd(const char *args) {
    (void)args;
    if (!cxfs_is_mounted()) { console_print("No filesystem mounted.\n"); return; }
    char path[256];
    cxfs_path_of(cwd_id, path, sizeof(path));
    console_print(path);
    console_putc('\n');
}

static void cmd_cd(const char *args) {
    if (!cxfs_is_mounted()) { console_print("No filesystem mounted.\n"); return; }
    if (args[0] == '\0') { cwd_id = 0; return; }   /* cd with no arg -> root */

    int id = cxfs_resolve(args, cwd_id);
    if (id < 0) {
        console_set_color(VGA_LIGHT_RED, VGA_BLACK);
        console_print("No such directory: ");
        console_print(args); console_putc('\n');
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }
    /* must be a directory */
    struct cxfs_entry e;
    if (cxfs_read_entry((uint32_t)id, &e) != 0 || e.type != CXFS_TYPE_DIR) {
        console_set_color(VGA_LIGHT_RED, VGA_BLACK);
        console_print("Not a directory: ");
        console_print(args); console_putc('\n');
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }
    cwd_id = (uint32_t)id;
}

static void cmd_mkdir(const char *args) {
    if (!cxfs_is_mounted()) { console_print("No filesystem mounted.\n"); return; }
    if (args[0] == '\0') { console_print("Usage: mkdir <name>\n"); return; }

    /* resolve the parent: if the arg has a slash, split off the last component;
       otherwise create directly in the current dir. v1: simple - require the
       name to be a single component in the current directory, OR an absolute
       path whose parent already exists. Keep it simple: create in cwd. */
    int id = cxfs_create_entry(cwd_id, args, CXFS_TYPE_DIR);
    if (id < 0) {
        console_set_color(VGA_LIGHT_RED, VGA_BLACK);
        console_print("mkdir failed (exists, bad name, or full): ");
        console_print(args); console_putc('\n');
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }
}

static void cmd_ls(const char *args) {
    if (!cxfs_is_mounted()) { console_print("No filesystem mounted.\n"); return; }

    int long_form = 0;

    /* parse a leading "-l" flag, then optional path */
    while (args[0] == '-') {
        const char *f = args + 1;
        while (*f && *f != ' ') {
            if (*f == 'l') long_form = 1;
            f++;
        }
        /* advance args past this flag token */
        while (*args && *args != ' ') args++;
        while (*args == ' ') args++;
    }

    uint32_t target = cwd_id;
    if (args[0] != '\0') {
        int id = cxfs_resolve(args, cwd_id);
        if (id < 0) {
            console_set_color(VGA_LIGHT_RED, VGA_BLACK);
            console_print("No such path: "); console_print(args); console_putc('\n');
            console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            return;
        }
        target = (uint32_t)id;
    }

    if (long_form) {
        cxfs_list_dir(target, ls_long_cb);
    } else {
        cxfs_list_dir(target, ls_print_cb);
    }
}

static void cmd_write(const char *args) {
    if (!cxfs_is_mounted()) { console_print("No filesystem mounted.\n"); return; }

    int force = 0;
    /* parse optional -f flag */
    while (args[0] == '-') {
        const char *fl = args + 1;
        while (*fl && *fl != ' ') { if (*fl == 'f') force = 1; fl++; }
        while (*args && *args != ' ') args++;
        while (*args == ' ') args++;
    }

    /* first token = filename, rest (after one space) = content */
    char name[CXFS_NAME_LEN];
    int n = 0;
    while (args[n] && args[n] != ' ' && n < CXFS_NAME_LEN - 1) { name[n] = args[n]; n++; }
    name[n] = '\0';
    if (n == 0) { console_print("Usage: write [-f] <name> <content>\n"); return; }

    const char *content = args + n;
    while (*content == ' ') content++;       /* skip the space(s) before content */

    /* find or create the file in the current directory */
    int id = cxfs_find_in_dir(cwd_id, name);
    if (id < 0) {
        id = cxfs_create_entry(cwd_id, name, CXFS_TYPE_FILE);
        if (id < 0) {
            console_set_color(VGA_LIGHT_RED, VGA_BLACK);
            console_print("Could not create file (bad name or full).\n");
            console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            return;
        }
    } else {
        /* exists: if it has content, require -f */
        struct cxfs_entry e;
        cxfs_read_entry((uint32_t)id, &e);
        if (e.type != CXFS_TYPE_FILE) {
            console_set_color(VGA_LIGHT_RED, VGA_BLACK);
            console_print("Not a file: "); console_print(name); console_putc('\n');
            console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            return;
        }
        if (e.size > 0 && !force) {
            console_set_color(VGA_YELLOW, VGA_BLACK);
            console_print("File has content. Use 'write -f' to overwrite.\n");
            console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            return;
        }
    }

    /* --- INLINE content provided: single-line write (original behavior) --- */
    if (content[0] != '\0') {
        uint32_t len = strlen(content);
        if (cxfs_write_file((uint32_t)id, content, len) != 0) {
            console_set_color(VGA_LIGHT_RED, VGA_BLACK);
            console_print("Write failed (content too large for v1?).\n");
            console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            return;
        }
        console_print("Wrote "); console_print_dec(len); console_print(" bytes.\n");
        return;
    }

    /* --- NO inline content: multi-line mode --- */
    /* Read lines after a '> ' prompt, joining with '\n', until a line that is
       exactly "<<DONE>>" (case-insensitive). Ctrl+C aborts (writes nothing). */
    console_print("Enter text. End with a line: <<DONE>>  (Ctrl+C aborts)\n");

    static char content_buf[4096];
    char line[256];
    uint32_t total = 0;
    int first = 1;

    for (;;) {
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        console_print("> ");
        if (!shell_read_line(line, sizeof(line), 0)) {   /* Ctrl+C */
            console_print("Aborted (nothing written).\n");
            return;
        }
        /* terminator check: case-insensitive "<<DONE>>" */
        if (ci_equals(line, "<<DONE>>")) break;

        uint32_t llen = strlen(line);
        /* +1 for the joining newline (except before the first line) */
        if (total + llen + (first ? 0 : 1) >= sizeof(content_buf)) {
            console_set_color(VGA_YELLOW, VGA_BLACK);
            console_print("Buffer full - ending input here.\n");
            console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            break;
        }
        if (!first) content_buf[total++] = '\n';
        for (uint32_t i = 0; i < llen; i++) content_buf[total++] = line[i];
        first = 0;
    }

    if (cxfs_write_file((uint32_t)id, content_buf, total) != 0) {
        console_set_color(VGA_LIGHT_RED, VGA_BLACK);
        console_print("Write failed (content too large for v1?).\n");
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }
    console_print("Wrote "); console_print_dec(total); console_print(" bytes.\n");
}

static void cmd_cat(const char *args) {
    if (!cxfs_is_mounted()) { console_print("No filesystem mounted.\n"); return; }
    if (args[0] == '\0') { console_print("Usage: cat <path>\n"); return; }

    int id = cxfs_resolve(args, cwd_id);
    if (id < 0) {
        console_set_color(VGA_LIGHT_RED, VGA_BLACK);
        console_print("No such file: "); console_print(args); console_putc('\n');
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }
    struct cxfs_entry e;
    cxfs_read_entry((uint32_t)id, &e);
    if (e.type != CXFS_TYPE_FILE) {
        console_set_color(VGA_LIGHT_RED, VGA_BLACK);
        console_print("Not a file: "); console_print(args); console_putc('\n');
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    static char buf[4096];
    int got = cxfs_read_file((uint32_t)id, buf, sizeof(buf) - 1);
    if (got < 0) { console_print("Read failed.\n"); return; }
    buf[got] = '\0';
    console_print(buf);
    console_putc('\n');
}

/* ---- shared helpers for rn/rm/rmdir flags ---- */

struct delflags { int force, recursive, dryrun, interactive; };

/* parse leading -f/-r/-n/-i flag tokens; advance *pp past them. */
static void parse_flags(const char **pp, struct delflags *fl) {
    fl->force = fl->recursive = fl->dryrun = fl->interactive = 0;
    const char *p = *pp;
    while (p[0] == '-') {
        const char *c = p + 1;
        while (*c && *c != ' ') {
            if (*c == 'f') fl->force = 1;
            else if (*c == 'r') fl->recursive = 1;
            else if (*c == 'n') fl->dryrun = 1;
            else if (*c == 'i') fl->interactive = 1;
            c++;
        }
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
    }
    *pp = p;
}

/* ask y/n; returns 1 for yes. reads one key (blocking). */
static int confirm_yn(const char *prompt) {
    console_print(prompt);
    console_print(" (y/n): ");
    char c = keyboard_getchar_blocking();
    console_putc(c);
    console_putc('\n');
    return (c == 'y' || c == 'Y');
}

static void cmd_rn(const char *args) {
    if (!cxfs_is_mounted()) { console_print("No filesystem mounted.\n"); return; }
    struct delflags fl;
    parse_flags(&args, &fl);

    /* args now: <path> <newname> */
    char path[128]; int n = 0;
    while (args[n] && args[n] != ' ' && n < 127) { path[n] = args[n]; n++; }
    path[n] = '\0';
    const char *newname = args + n;
    while (*newname == ' ') newname++;
    if (path[0] == '\0' || newname[0] == '\0') {
        console_print("Usage: rn [-f] [-n] [-i] <path> <newname>\n"); return;
    }

    int id = cxfs_resolve(path, cwd_id);
    if (id < 0) { console_print("No such entry: "); console_print(path); console_putc('\n'); return; }

    /* read entry for parent + report */
    struct cxfs_entry e;
    cxfs_read_entry((uint32_t)id, &e);

    /* collision check in the same parent */
    int existing = cxfs_find_in_dir(e.parent_id, newname);

    if (fl.dryrun) {
        console_print("[dry-run] would rename '"); console_print(e.name);
        console_print("' -> '"); console_print(newname); console_print("'");
        if (existing >= 0) console_print(" (replacing existing)");
        console_putc('\n');
        return;
    }

    if (existing >= 0 && (uint32_t)existing != (uint32_t)id) {
        if (!fl.force) {
            console_set_color(VGA_YELLOW, VGA_BLACK);
            console_print("Name '"); console_print(newname);
            console_print("' already exists. Use -f to replace.\n");
            console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            return;
        }
    }

    if (fl.interactive) {
        console_print("Rename '"); console_print(e.name);
        console_print("' to '"); console_print(newname); console_print("'");
        if (existing >= 0) console_print(" (replacing existing)");
        if (!confirm_yn("?")) { console_print("Cancelled.\n"); return; }
    }

    /* if forcing over an existing target, delete it first */
    if (existing >= 0 && (uint32_t)existing != (uint32_t)id) cxfs_delete_entry((uint32_t)existing);

    if (cxfs_rename((uint32_t)id, newname) != 0) {
        console_set_color(VGA_LIGHT_RED, VGA_BLACK);
        console_print("Rename failed (bad name?).\n");
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }
}

static void cmd_mv(const char *args) {
    if (!cxfs_is_mounted()) { console_print("No filesystem mounted.\n"); return; }
    struct delflags fl;
    parse_flags(&args, &fl);

    char src[128]; int n = 0;
    while (args[n] && args[n] != ' ' && n < 127) { src[n] = args[n]; n++; }
    src[n] = '\0';
    const char *dst = args + n;
    while (*dst == ' ') dst++;
    if (src[0] == '\0' || dst[0] == '\0') { console_print("Usage: mv [-f] <source> <destdir>\n"); return; }

    int sid = cxfs_resolve(src, cwd_id);
    if (sid < 0) { console_print("No such source: "); console_print(src); console_putc('\n'); return; }
    int did = cxfs_resolve(dst, cwd_id);
    if (did < 0) { console_print("No such destination: "); console_print(dst); console_putc('\n'); return; }

    struct cxfs_entry se;
    cxfs_read_entry((uint32_t)sid, &se);

    /* collision: does destination already have something with this name? */
    int existing = cxfs_find_in_dir((uint32_t)did, se.name);
    if (existing >= 0) {
        if (!fl.force) {
            console_set_color(VGA_YELLOW, VGA_BLACK);
            console_print("Destination already has '"); console_print(se.name);
            console_print("'. Use -f to replace.\n");
            console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            return;
        }
        cxfs_delete_entry((uint32_t)existing);
    }

    if (cxfs_move((uint32_t)sid, (uint32_t)did) != 0) {
        console_set_color(VGA_LIGHT_RED, VGA_BLACK);
        console_print("Move failed (into itself, or not a directory?).\n");
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }
}

/* recursively delete a directory subtree (files + subdirs).
   Repeatedly finds one child and deletes it (recursing into subdirs first),
   until the directory has no children left. */
static void delete_subtree(uint32_t dir_id) {
    const struct cxfs_superblock *sbp = cxfs_get_superblock();
    for (;;) {
        int child = -1;
        struct cxfs_entry ce;
        for (uint32_t id = 0; id < sbp->manifest_count; id++) {
            if (cxfs_read_entry(id, &ce) != 0) continue;
            if (ce.type == CXFS_TYPE_FREE) continue;
            if (ce.parent_id == dir_id && id != dir_id) { child = (int)id; break; }
        }
        if (child < 0) break;                 /* no more children */
        cxfs_read_entry((uint32_t)child, &ce);
        if (ce.type == CXFS_TYPE_DIR) delete_subtree((uint32_t)child);
        cxfs_delete_entry((uint32_t)child);
    }
}

/* collect/print names that would be removed (for dry-run / interactive). */
static void list_subtree(uint32_t dir_id, int depth) {
    struct cxfs_entry ce;
    const struct cxfs_superblock *sbp = cxfs_get_superblock();
    for (uint32_t id = 0; id < sbp->manifest_count; id++) {
        if (cxfs_read_entry(id, &ce) != 0) continue;
        if (ce.type == CXFS_TYPE_FREE) continue;
        if (ce.parent_id == dir_id && id != dir_id) {
            for (int d = 0; d < depth; d++) console_print("  ");
            console_print("  "); console_print(ce.name);
            if (ce.type == CXFS_TYPE_DIR) console_print("/");
            console_putc('\n');
            if (ce.type == CXFS_TYPE_DIR) list_subtree(id, depth + 1);
        }
    }
}

static void cmd_rm(const char *args) {
    if (!cxfs_is_mounted()) { console_print("No filesystem mounted.\n"); return; }
    struct delflags fl;
    parse_flags(&args, &fl);

    /* rm -r / -rf: operate on FILES in the current directory (no descent). */
    if (fl.recursive && args[0] == '\0') {
        /* gather candidate files in cwd */
        if (fl.dryrun) {
            console_print("[dry-run] would remove files in current dir:\n");
        }
        /* iterate cwd's children, files only */
        const struct cxfs_superblock *sbp = cxfs_get_superblock();
        struct cxfs_entry ce;
        /* interactive confirm once for the batch */
        if (fl.interactive && !fl.dryrun) {
            console_print("Remove files in current directory");
            console_print(fl.force ? " (including non-empty)" : " (empty only)");
            if (!confirm_yn("?")) { console_print("Cancelled.\n"); return; }
        }
        for (uint32_t id = 0; id < sbp->manifest_count; id++) {
            if (cxfs_read_entry(id, &ce) != 0) continue;
            if (ce.type != CXFS_TYPE_FILE) continue;
            if (ce.parent_id != cwd_id) continue;
            int empty = (ce.size == 0);
            if (!empty && !fl.force) continue;     /* skip non-empty unless -f */
            if (fl.dryrun) { console_print("  "); console_print(ce.name); console_putc('\n'); continue; }
            cxfs_delete_entry(id);
        }
        return;
    }

    /* single-file rm */
    if (args[0] == '\0') { console_print("Usage: rm [-r] [-f] [-n] [-i] <file>\n"); return; }
    int id = cxfs_resolve(args, cwd_id);
    if (id < 0) { console_print("No such file: "); console_print(args); console_putc('\n'); return; }

    struct cxfs_entry e;
    cxfs_read_entry((uint32_t)id, &e);
    if (e.type != CXFS_TYPE_FILE) {
        console_set_color(VGA_LIGHT_RED, VGA_BLACK);
        console_print("Not a file (use rmdir for directories): "); console_print(args); console_putc('\n');
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    if (fl.dryrun) {
        console_print("[dry-run] would remove file: "); console_print(e.name); console_putc('\n');
        return;
    }
    if (e.size > 0 && !fl.force) {
        console_set_color(VGA_YELLOW, VGA_BLACK);
        console_print("File has content. Use -f to remove.\n");
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }
    if (fl.interactive) {
        console_print("Remove file '"); console_print(e.name); console_print("'");
        if (!confirm_yn("?")) { console_print("Cancelled.\n"); return; }
    }
    cxfs_delete_entry((uint32_t)id);
}

static void cmd_rmdir(const char *args) {
    if (!cxfs_is_mounted()) { console_print("No filesystem mounted.\n"); return; }
    struct delflags fl;
    parse_flags(&args, &fl);

    if (args[0] == '\0') { console_print("Usage: rmdir [-r] [-f] [-n] [-i] <dir>\n"); return; }
    int id = cxfs_resolve(args, cwd_id);
    if (id < 0) { console_print("No such directory: "); console_print(args); console_putc('\n'); return; }

    struct cxfs_entry e;
    cxfs_read_entry((uint32_t)id, &e);
    if (e.type != CXFS_TYPE_DIR) {
        console_set_color(VGA_LIGHT_RED, VGA_BLACK);
        console_print("Not a directory (use rm for files): "); console_print(args); console_putc('\n');
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }
    if ((uint32_t)id == cwd_id) { console_print("Cannot remove the current directory.\n"); return; }

    int children = cxfs_count_children((uint32_t)id);

    /* dry-run: show what would be removed */
    if (fl.dryrun) {
        console_print("[dry-run] would remove directory: "); console_print(e.name); console_print("/\n");
        if (children > 0 && fl.recursive) list_subtree((uint32_t)id, 0);
        else if (children > 0) console_print("  (not empty - needs -rf)\n");
        return;
    }

    /* empty directory: simple case */
    if (children == 0) {
        if (fl.interactive && !confirm_yn("Remove empty directory?")) { console_print("Cancelled.\n"); return; }
        cxfs_delete_entry((uint32_t)id);
        return;
    }

    /* non-empty: need -r (empties recursively) or -rf (everything) */
    if (!fl.recursive) {
        console_set_color(VGA_YELLOW, VGA_BLACK);
        console_print("Directory not empty. Use -r (empty subdirs) or -rf (everything).\n");
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    if (fl.recursive && !fl.force) {
        /* -r without -f: only remove if the subtree is entirely empty dirs.
           if any files exist anywhere below, refuse. */
        /* simple policy: if there are files in the subtree, refuse. */
        console_print("Recursive empty-only remove. (Files present will block removal.)\n");
        /* attempt: only works if no files; we let delete_subtree handle dirs,
           but guard by checking for files first via list is complex - v1:
           require -rf to remove anything containing files. */
        console_set_color(VGA_YELLOW, VGA_BLACK);
        console_print("For v1, use -rf to remove a non-empty directory tree.\n");
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }

    /* -rf: nuke the whole subtree */
    if (fl.interactive) {
        console_print("This will remove:\n");
        list_subtree((uint32_t)id, 0);
        if (!confirm_yn("Remove directory and ALL contents?")) { console_print("Cancelled.\n"); return; }
    }
    delete_subtree((uint32_t)id);
    cxfs_delete_entry((uint32_t)id);
}

/* print a number zero-padded to 2 digits */
static void print_2d(uint32_t v) {
    console_print_dec((v / 10) % 10);
    console_print_dec(v % 10);
}

/* print the date as YYYY-MM-DD */
static void print_date(const struct rtc_time *t) {
    console_print_dec((t->year / 1000) % 10);
    console_print_dec((t->year / 100) % 10);
    console_print_dec((t->year / 10) % 10);
    console_print_dec(t->year % 10);
    console_putc('-');
    print_2d(t->month);
    console_putc('-');
    print_2d(t->day);
}

/* print the time as HH:MM:SS (24-hour) */
static void print_time(const struct rtc_time *t) {
    print_2d(t->hour);
    console_putc(':');
    print_2d(t->minute);
    console_putc(':');
    print_2d(t->second);
}

static void cmd_date(const char *args) {
    (void)args;
    struct rtc_time t;
    rtc_read(&t);
    print_date(&t);
    console_putc('\n');
}

static void cmd_time(const char *args) {
    (void)args;
    struct rtc_time t;
    rtc_read(&t);
    print_time(&t);
    console_putc('\n');
}

static void cmd_datetime(const char *args) {
    (void)args;
    struct rtc_time t;
    rtc_read(&t);
    print_date(&t);
    console_putc(' ');
    print_time(&t);
    console_putc('\n');
}

/* print a 2-digit hex byte */
static void print_hex2(uint8_t v) {
    const char *d = "0123456789abcdef";
    console_putc(d[(v >> 4) & 0xF]);
    console_putc(d[v & 0xF]);
}
/* print a 4-digit hex word */
static void print_hex4(uint16_t v) {
    print_hex2((uint8_t)(v >> 8));
    print_hex2((uint8_t)(v & 0xFF));
}

static void cmd_ahci(const char *args) {
    (void)args;
    int found = 0;
    for (int p = 0; p < 32; p++) {
        if (!ahci_present(p)) continue;
        found++;
        console_print("Port ");
        console_print_dec(p);
        console_print(": ");
        console_print(ahci_model(p));
        console_print("  sectors=");
        console_print_dec((uint32_t)ahci_sectors(p));
        console_putc('\n');
    }
    if (!found) { console_print("No AHCI disks detected.\n"); return; }
    for (int p = 0; p < 32; p++) {
        if (!ahci_present(p)) continue;
        static uint8_t sec[512];
        int r = ahci_read(p, 0, 1, sec);
        if (r == DISK_OK) {
            console_print("Port ");
            console_print_dec(p);
            console_print(" sector 0 sig[510,511]: ");
            print_hex2(sec[510]); console_putc(' '); print_hex2(sec[511]);
            console_print(" (expect 55 aa if bootable)\n");
        } else {
            console_print("Port ");
            console_print_dec(p);
            console_print(": read FAILED (");
            console_print(disk_err_str(r));
            console_print(")\n");
        }
        break;
    }
}

static void cmd_lspci(const char *args) {
    (void)args;
    unsigned n = pci_device_count();
    if (n == 0) { console_print("No PCI devices found.\n"); return; }
    for (unsigned i = 0; i < n; i++) {
        const struct pci_device *d = pci_get_device(i);
        print_hex2(d->bus); console_putc(':');
        print_hex2(d->slot); console_putc('.');
        console_putc("01234567"[d->func & 7]);
        console_print("  ");
        print_hex4(d->vendor_id); console_putc(':');
        print_hex4(d->device_id);
        console_print("  ");
        console_print(pci_class_name(d->class_code, d->subclass, d->prog_if));
        console_putc('\n');
    }
    console_print("Total: ");
    console_print_dec(n);
    console_print(" devices\n");
}

static void cmd_spin(const char *args) {
    (void)args;
    demo_spin();
}

static void cmd_textsize(const char *args) {
    while (*args == ' ') args++;
    if (args[0] == '8') {
        console_set_font(1);
        console_print("Text size: 8x8 (small).\n");
    } else if (args[0] == '1' && args[1] == '6') {
        console_set_font(0);
        console_print("Text size: 8x16 (large).\n");
    } else {
        console_print("Usage: textsize <8|16>\n");
        console_print("(Only affects graphics mode; text mode has a fixed font.)\n");
    }
}

static void cmd_disks(const char *args) {
    (void)args;
    unsigned n = disk_count();
    if (n == 0) {
        console_set_color(VGA_YELLOW, VGA_BLACK);
        console_print("No disks registered.\n");
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }
    console_begin_batch();
    /* header */
    console_set_color(VGA_DARK_GREY, VGA_BLACK);
    console_print("ID    NAME        DRIVER  CAPACITY   MODEL\n");
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    for (unsigned i = 0; i < n; i++) {
        const struct disk *d = disk_get(i);

        /* ID as 0xNN */
        console_print("0x"); print_hex2(d->id);
        console_print("  ");

        /* NAME padded to 12 */
        int col = 0;
        console_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        for (const char *p = d->name; *p; p++) { console_putc(*p); col++; }
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        while (col < 12) { console_putc(' '); col++; }

        /* DRIVER padded to 8 */
        col = 0;
        const char *drv = disk_driver_name(d->driver);
        for (const char *p = drv; *p; p++) { console_putc(*p); col++; }
        while (col < 8) { console_putc(' '); col++; }

        /* CAPACITY padded to 11 */
        char cap[24];
        disk_capacity_str(d->sectors, cap, sizeof(cap));
        col = 0;
        for (const char *p = cap; *p; p++) { console_putc(*p); col++; }
        while (col < 11) { console_putc(' '); col++; }

        /* MODEL */
        console_print(d->model);
        console_putc('\n');
    }

    console_print("Use 'dskset <name>' or 'dskset -i 0xNN' to select the CXFS volume.\n");
    console_end_batch();
}

/* parse a hex byte like "0x0A" or "0a" -> value; returns -1 on bad input */
static int parse_hex_byte(const char *s) {
    while (*s == ' ') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    int val = 0, digits = 0;
    while (*s && *s != ' ') {
        char c = *s++;
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        val = val * 16 + d;
        digits++;
    }
    if (digits == 0 || val > 255) return -1;
    return val;
}

static void cmd_dskset(const char *args) {
    while (*args == ' ') args++;

    /* no argument: show current selection */
    if (*args == '\0') {
        const struct disk *d = disk_find_by_id(cxfs_get_id());
        console_print("CXFS volume: ");
        if (d) { console_print(d->name); console_print(" (ID 0x"); print_hex2(d->id); console_print(")\n"); }
        else   { console_print("(none selected)\n"); }
        console_print("Usage: dskset <name>   or   dskset -i 0xNN   (see 'disks')\n");
        return;
    }

    const struct disk *d = 0;

    /* -i 0xNN : select by hex id */
    if (args[0] == '-' && args[1] == 'i') {
        const char *p = args + 2;
        int id = parse_hex_byte(p);
        if (id < 0) { console_print("Bad ID. Usage: dskset -i 0xNN\n"); return; }
        d = disk_find_by_id((uint8_t)id);
        if (!d) {
            console_set_color(VGA_LIGHT_RED, VGA_BLACK);
            console_print("No disk with that ID. Run 'disks'.\n");
            console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            return;
        }
    } else {
        /* select by name */
        d = disk_find_by_name(args);
        if (!d) {
            console_set_color(VGA_LIGHT_RED, VGA_BLACK);
            console_print("No disk named '"); console_print(args);
            console_print("'. Run 'disks' to see names.\n");
            console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            return;
        }
    }

    cxfs_set_id(d->id);
    console_print("CXFS now targets ");
    console_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    console_print(d->name);
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    console_print(".\n");
    console_set_color(VGA_YELLOW, VGA_BLACK);
    console_print("(Remounting: ");
    if (cxfs_mount() == 0) console_print("found an existing CXFS here.)\n");
    else                   console_print("no CXFS yet - 'format' to create one.)\n");
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

static void cmd_usb(const char *args) {
    (void)args;
    if (!ohci_present()) {
        console_print("No OHCI USB controller found.\n");
        console_print("(OHCI handles USB 1.x devices. EHCI/xHCI not driven yet.)\n");
        return;
    }
    int np = ohci_port_count();
    console_print("OHCI USB controller: ");
    console_print_dec((uint32_t)np);
    console_print(" root port(s)\n");
    for (int i = 0; i < np; i++) {
        console_print("  Port ");
        console_print_dec((uint32_t)i);
        console_print(": ");
        if (ohci_port_connected(i)) {
            console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
            console_print("device connected");
            console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            /* Stage 2: try to enumerate it and show what it is */
            if (ohci_enumerate(i)) {
                console_print("\n      ID ");
                print_hex4(ohci_dev_vendor());
                console_putc(':');
                print_hex4(ohci_dev_product());
                /* prefer the interface class - the device class is often 0x00
                   (per-interface) for mass storage and other class-per-if devices */
                uint8_t cls = ohci_dev_class();
                if (cls == 0x00 && ohci_dev_if_valid()) cls = ohci_dev_if_class();
                console_print("  class ");
                print_hex2(cls);
                if (cls == 0x08) console_print(" (Mass Storage)");
                else if (cls == 0x03) console_print(" (HID)");
                else if (cls == 0x09) console_print(" (Hub)");
                else if (cls == 0x00) console_print(" (per-interface)");
                /* if it's mass storage, show the bulk endpoints (stage 3 needs them) */
                if (ohci_dev_if_valid() && ohci_dev_if_class() == 0x08) {
                    console_print("\n      Bulk-Only Transport, ep IN 0x");
                    print_hex2(ohci_dev_ep_in());
                    console_print(" / ep OUT 0x");
                    print_hex2(ohci_dev_ep_out());
                }
            } else {
                console_print("\n      (enumeration failed)");
            }
        } else {
            console_set_color(VGA_DARK_GREY, VGA_BLACK);
            console_print("(empty)");
            console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        }
        console_putc('\n');
    }
    /* if a mass-storage device came up, note it's in the disk table */
    if (ohci_storage_ready()) {
        console_print("USB mass storage ready as a disk - see 'disks' (USB0).\n");
    }
}

static void cmd_net(const char *args) {
    (void)args;
    if (!e1000_present()) {
        console_print("No network interface found.\n");
        console_print("(e1000 NIC expected. Develop networking in QEMU with -device e1000.)\n");
        return;
    }
    const uint8_t *m = e1000_mac();
    console_print("Interface: eth0 (Intel e1000)\n");
    console_print("  MAC: ");
    for (int i = 0; i < 6; i++) {
        print_hex2(m[i]);
        if (i < 5) console_putc(':');
    }
    console_putc('\n');
    console_print("  Link: ");
    if (e1000_link_up()) {
        console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        console_print("UP");
    } else {
        console_set_color(VGA_BROWN, VGA_BLACK);
        console_print("DOWN");
    }
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    console_print(" (10 Mbps FDX target)\n");

    /* IP configuration */
    const struct net_config *c = netif_cfg();
    char b[16];
    netif_ip_str(c->ip, b);      console_print("  IP:      "); console_print(b); console_putc('\n');
    netif_ip_str(c->mask, b);    console_print("  Mask:    "); console_print(b); console_putc('\n');
    netif_ip_str(c->gateway, b); console_print("  Gateway: "); console_print(b); console_putc('\n');
    netif_ip_str(c->dns1, b);    console_print("  DNS1:    "); console_print(b); console_putc('\n');
    netif_ip_str(c->dns2, b);    console_print("  DNS2:    "); console_print(b); console_putc('\n');
}

/* ipset <field> <a.b.c.d> - set a network config field (no validation). */
static void cmd_ipset(const char *args) {
    while (*args == ' ') args++;
    /* read the field keyword */
    char field[12]; int fi = 0;
    while (*args && *args != ' ' && fi < 11) field[fi++] = *args++;
    field[fi] = '\0';
    while (*args == ' ') args++;

    if (field[0] == '\0') {
        console_print("Usage: ipset <ip|mask|gateway|dns1|dns2|dns> <a.b.c.d> [<a.b.c.d>]\n");
        console_print("  e.g. ipset ip 10.0.2.15   |   ipset dns 1.1.1.1 1.0.0.1\n");
        console_print("  (values are taken as-is; you are responsible for correctness)\n");
        return;
    }

    ip4_t a;
    if (!netif_parse_ip(args, a)) {
        console_print("Could not parse an address from: ");
        console_print(args);
        console_putc('\n');
        return;
    }

    if (strcmp(field, "ip") == 0)            { netif_set_ip(a);      console_print("IP set.\n"); }
    else if (strcmp(field, "mask") == 0)     { netif_set_mask(a);    console_print("Mask set.\n"); }
    else if (strcmp(field, "gateway") == 0 ||
             strcmp(field, "gw") == 0)       { netif_set_gateway(a); console_print("Gateway set.\n"); }
    else if (strcmp(field, "dns1") == 0)     { ip4_t d2; for(int i=0;i<4;i++)d2[i]=netif_cfg()->dns2[i]; netif_set_dns(a, d2); console_print("DNS1 set.\n"); }
    else if (strcmp(field, "dns2") == 0)     { ip4_t d1; for(int i=0;i<4;i++)d1[i]=netif_cfg()->dns1[i]; netif_set_dns(d1, a); console_print("DNS2 set.\n"); }
    else if (strcmp(field, "dns") == 0) {
        /* dns <primary> [secondary] */
        ip4_t d2; for(int i=0;i<4;i++) d2[i]=netif_cfg()->dns2[i];
        while (*args && *args != ' ') args++;   /* skip primary */
        while (*args == ' ') args++;
        if (*args) netif_parse_ip(args, d2);    /* optional secondary */
        netif_set_dns(a, d2);
        console_print("DNS set.\n");
    }
    else {
        console_print("Unknown field. Use: ip, mask, gateway, dns1, dns2, or dns.\n");
    }
}

/* ping <a.b.c.d> [count] - ICMP echo (Stage 3 milestone) */
static void cmd_ping(const char *args) {
    while (*args == ' ') args++;
    if (*args == '\0') {
        console_print("Usage: ping <a.b.c.d> [count]\n");
        return;
    }
    if (!netif_ready()) {
        console_print("Network interface not ready.\n");
        return;
    }
    ip4_t target;
    if (!netif_parse_ip(args, target)) {
        console_print("Could not parse address.\n");
        return;
    }
    /* optional count after the address (default 4) */
    int count = 4;
    const char *p = args;
    while (*p && *p != ' ') p++;        /* skip the address */
    while (*p == ' ') p++;
    if (*p >= '0' && *p <= '9') {
        int n = 0;
        while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; }
        if (n > 0 && n <= 100) count = n;
    }

    char b[16]; netif_ip_str(target, b);
    console_print("PING ");
    console_print(b);
    console_print(" - 32 bytes of data:\n");

    int sent = 0, recvd = 0;
    for (int i = 0; i < count; i++) {
        uint32_t rtt = 0;
        sent++;
        if (icmp_ping(target, (uint16_t)(i + 1), &rtt)) {
            recvd++;
            console_print("  reply from ");
            console_print(b);
            console_print("  seq=");
            console_print_dec((uint32_t)(i + 1));
            console_print("  time=");
            console_print_dec(rtt);
            console_print(" ms\n");
        } else {
            console_set_color(VGA_BROWN, VGA_BLACK);
            console_print("  request timed out  seq=");
            console_print_dec((uint32_t)(i + 1));
            console_putc('\n');
            console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        }
        if (i + 1 < count) timer_sleep(500);   /* 0.5s between pings */
    }

    console_print("--- ");
    console_print(b);
    console_print(" ping statistics ---\n  ");
    console_print_dec((uint32_t)sent);
    console_print(" sent, ");
    console_print_dec((uint32_t)recvd);
    console_print(" received, ");
    console_print_dec((uint32_t)(sent - recvd));
    console_print(" lost\n");
}
static void cmd_arping(const char *args) {
    while (*args == ' ') args++;
    if (*args == '\0') {
        console_print("Usage: arping <a.b.c.d>\n");
        return;
    }
    if (!netif_ready()) {
        console_print("Network interface not ready.\n");
        return;
    }
    ip4_t target;
    if (!netif_parse_ip(args, target)) {
        console_print("Could not parse address.\n");
        return;
    }
    char b[16]; netif_ip_str(target, b);
    console_print("ARP who-has ");
    console_print(b);
    console_print(" ... ");

    uint8_t mac[6];
    if (arp_resolve(target, mac)) {
        console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        console_print("is at ");
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        for (int i = 0; i < 6; i++) { print_hex2(mac[i]); if (i < 5) console_putc(':'); }
        console_putc('\n');
    } else {
        console_set_color(VGA_BROWN, VGA_BLACK);
        console_print("no reply (timeout)\n");
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }
}

/* ---- command table ---- */

/* usermode - Ring 3 Stage 1 test: drop to user mode, run a tiny routine that
   makes syscalls (write + exit), and return to the shell. */
static void cmd_usermode(const char *args) {
    (void)args;
    console_print("Entering ring 3...\n");
    int r = usermode_test();
    console_print("Returned to kernel (ring 0). exit code = ");
    console_print_dec((uint32_t)r);
    console_putc('\n');
}

/* two cooperative kernel threads that yield back and forth - Checkpoint 1
   proof that context switching works. */
static volatile int demo_done_a, demo_done_b;

static void demo_thread_a(void) {
    for (int i = 0; i < 5; i++) {
        console_print("  [thread A] tick ");
        console_print_dec((uint32_t)i);
        console_putc('\n');
        yield();
    }
    demo_done_a = 1;
}
static void demo_thread_b(void) {
    for (int i = 0; i < 5; i++) {
        console_print("  [thread B] tock ");
        console_print_dec((uint32_t)i);
        console_putc('\n');
        yield();
    }
    demo_done_b = 1;
}

/* proc - create a scheduler-managed ring-3 PROCESS (checkpoint 3a.2):
   a process that runs in user mode, syscalls, exits, and is reaped. */
extern uint8_t user_blob_start[];
extern uint8_t user_blob_end[];

static void cmd_proc(const char *args) {
    (void)args;
    uint32_t blen = (uint32_t)(user_blob_end - user_blob_start);
    console_print("Creating a ring-3 process via the scheduler...\n");
    int pid = process_create_ring3("userproc", user_blob_start, blen,
                                   "  [ring3 proc] hello from a scheduled user process!\n");
    if (pid < 0) { console_print("process_create failed.\n"); return; }
    console_print("Created pid ");
    console_print_dec((uint32_t)pid);
    console_print("; yielding to it...\n");
    for (int i = 0; i < 20; i++) yield();
    console_print("Back in shell. Process ran in ring 3 and exited cleanly.\n");
}

/* ---- preemptive demo (Checkpoint 2): threads that DON'T yield ---- */
static volatile int pre_done_a, pre_done_b;

static void preempt_thread_a(void) {
    /* busy work with NO yield - only the timer can switch us out */
    for (uint32_t i = 0; i < 5; i++) {
        for (volatile uint32_t spin = 0; spin < 8000000; spin++) { }
        console_print("  [preempt A] step ");
        console_print_dec(i);
        console_putc('\n');
    }
    pre_done_a = 1;
    thread_exit();   /* finished - leave cleanly */
}
static void preempt_thread_b(void) {
    for (uint32_t i = 0; i < 5; i++) {
        for (volatile uint32_t spin = 0; spin < 8000000; spin++) { }
        console_print("  [preempt B] step ");
        console_print_dec(i);
        console_putc('\n');
    }
    pre_done_b = 1;
    thread_exit();   /* finished - leave cleanly */
}

static void cmd_preempt(const char *args) {
    (void)args;
    pre_done_a = pre_done_b = 0;
    console_print("Starting two threads that never yield...\n");
    console_print("(only the timer can switch between them)\n");
    int a = thread_create("preA", preempt_thread_a);
    int b = thread_create("preB", preempt_thread_b);
    if (a < 0 || b < 0) { console_print("thread_create failed.\n"); return; }

    sched_preempt_enable(5);   /* switch every ~5 timer ticks */

    /* main thread waits (also preemptible) until both demo threads finish */
    while (!(pre_done_a && pre_done_b)) {
        for (volatile uint32_t spin = 0; spin < 1000000; spin++) { }
    }

    sched_preempt_disable();
    console_print("Both threads finished under PREEMPTION (no yields).\n");
    console_print("Context switches were driven entirely by the timer.\n");
}

static void cmd_threads(const char *args) {
    (void)args;
    demo_done_a = demo_done_b = 0;
    console_print("Creating two cooperative threads...\n");
    int a = thread_create("demoA", demo_thread_a);
    int b = thread_create("demoB", demo_thread_b);
    if (a < 0 || b < 0) { console_print("thread_create failed.\n"); return; }
    /* yield among them until both finish */
    while (!(demo_done_a && demo_done_b)) yield();
    console_print("Both threads finished. Context switching works.\n");
}

const struct command commands[] = {
    { "proc",     cmd_proc,     "create a scheduled ring-3 process (proc model)",
      "proc - create a user-mode process managed by the scheduler; it runs in\n"
      "ring 3, makes syscalls, exits, and is reaped (checkpoint 3a.2).\n" },

    { "preempt",  cmd_preempt,  "preemptive multitasking demo (timer-driven)",
      "preempt - run two threads that never yield; the timer preempts them.\n"
      "Proves timer-driven preemptive context switching (checkpoint 2).\n" },

    { "threads",  cmd_threads,  "run two cooperative kernel threads (ctx-switch test)",
      "threads - create two kernel threads that yield back and forth,\n"
      "proving the context-switch mechanism (process model checkpoint 1).\n" },

    { "usermode", cmd_usermode,  "ring 3 test: enter user mode, syscall, return",
      "usermode - drop into ring 3, run a user routine that uses syscalls,\n"
      "and return to the kernel. The Stage-1 proof that the privilege\n"
      "boundary and syscall gate work.\n" },

    { "help",     cmd_help,     "show this command list",
      "help - list commands, or show detail for one\n"
      "Usage: help [command]\n"
      "  [command]   optional; if given, shows detailed help for it\n" },

    { "ahci",     cmd_ahci,     "list AHCI SATA disks and read-test sector 0",
      "ahci - show SATA disks found via the AHCI controller\n"
      "Usage: ahci\n"
      "Lists each disk (model, sector count) and reads sector 0 as a test.\n" },

    { "net",      cmd_net,      "show the network interface (e1000) and MAC",
      "net - show the network interface, MAC address, and link status\n"
      "Usage: net\n"
      "Stage 1: link layer (the NIC). ARP, IP, and ping come in later stages.\n" },

    { "ipset",    cmd_ipset,    "set network config (ip/mask/gateway/dns)",
      "ipset - set a network configuration field (no validation)\n"
      "Usage: ipset ip 10.0.2.15\n"
      "       ipset mask 255.255.255.0\n"
      "       ipset gateway 10.0.2.2\n"
      "       ipset dns 1.1.1.1 1.0.0.1   (primary + optional secondary)\n"
      "Values are stored exactly as entered - correctness is up to you.\n" },

    { "arping",   cmd_arping,   "resolve an IP to a MAC via ARP",
      "arping - send an ARP request and show the resolved MAC\n"
      "Usage: arping <a.b.c.d>\n"
      "e.g. arping 10.0.2.2  (the gateway). Proves link-layer send/receive.\n" },

    { "ping",     cmd_ping,     "send ICMP echo requests to a host",
      "ping - send ICMP echo requests (the network 'are you there?' test)\n"
      "Usage: ping <a.b.c.d> [count]\n"
      "  count   optional, 1-100 (default 4)\n"
      "e.g. ping 10.0.2.2   (the QEMU gateway)\n" },

    { "usb",      cmd_usb,      "show USB (OHCI) controller and port status",
      "usb - show the OHCI USB controller and which ports have devices\n"
      "Usage: usb\n"
      "Stage 1: detects the controller and connected ports. Device enumeration\n"
      "and USB storage are not implemented yet.\n" },

    { "lspci",    cmd_lspci,    "list PCI devices (bus:slot.func vendor:device class)",
      "lspci - enumerate and list all PCI devices\n"
      "Usage: lspci\n"
      "Shows bus location, vendor:device IDs, and device class for each.\n" },

    { "spin",     cmd_spin,     "spinning 3D dodecahedron demo (Ctrl+C to exit)",
      "spin - render a rotating wireframe dodecahedron on the framebuffer\n"
      "Usage: spin\n"
      "Graphics mode only. Press Ctrl+C to return to the shell.\n" },

    { "textsize", cmd_textsize, "set graphics font size (8 or 16)",
      "textsize - change the console font size (graphics mode only)\n"
      "Usage: textsize <8|16>\n"
      "  8  = compact 8x8 font (more rows)\n"
      "  16 = large 8x16 font (more readable)\n"
      "Has no effect in VGA text mode (fixed font).\n" },

    { "clear",    cmd_clear,    "clear the screen", "" },

    { "echo",     cmd_echo,     "print the rest of the line",
      "echo - print text to the screen\n"
      "Usage: echo <text>\n"
      "  <text>   everything after 'echo' is printed verbatim\n" },

    { "ver",      cmd_ver,      "show version info", "" },

    { "meminfo",  cmd_meminfo,  "show the memory map",
      "meminfo - display the physical memory map (from BIOS E820)\n"
      "Usage: meminfo\n"
      "Lists each memory region, one per line:\n"
      "  BASE     starting physical address of the region\n"
      "  LENGTH   size of the region in bytes\n"
      "  TYPE     Usable, Reserved, ACPI reclaim, ACPI NVS, or Bad\n"
      "Ends with the total usable RAM in megabytes.\n" },

    { "uptime",   cmd_uptime,   "show time since boot",
      "uptime - time elapsed since the kernel started\n"
      "Usage: uptime [-p]\n"
      "Format: DD:HH:MM:SS:mmm\n"
      "  DD days, HH hours, MM minutes, SS seconds, mmm milliseconds\n"
      "  -p  live mode: updates in place until Ctrl+C\n" },

    { "date",     cmd_date,     "show the current date",
      "date - show the current date from the real-time clock\n"
      "Usage: date\n"
      "Format: YYYY-MM-DD\n" },

    { "time",     cmd_time,     "show the current time",
      "time - show the current time from the real-time clock\n"
      "Usage: time\n"
      "Format: HH:MM:SS (24-hour)\n" },

    { "datetime", cmd_datetime, "show date and time",
      "datetime - show the current date and time\n"
      "Usage: datetime\n"
      "Format: YYYY-MM-DD HH:MM:SS\n" },

    { "pmm",      cmd_pmm,      "show physical memory stats",
      "pmm - physical memory manager statistics\n"
      "Usage: pmm\n"
      "Shows total, used, and free physical memory, counted in 4 KB pages\n"
      "and megabytes. Free memory is what the PMM can hand out.\n" },

    { "heaptest", cmd_heaptest, "test kmalloc/kfree",
      "heaptest - exercise the kernel heap\n"
      "Usage: heaptest\n"
      "Allocates a few blocks, frees one, reallocates to show reuse, then\n"
      "frees everything. Prints addresses so you can see the allocator work.\n" },

    { "disks",    cmd_disks,    "list all disks (any driver) with ID, name, capacity",
      "disks - list every registered disk across all drivers\n"
      "Usage: disks\n"
      "Shows each disk's hex ID, assigned name (HDD0, SSD0, EXT-HDD0, ...),\n"
      "the driver communicating with it, capacity, and model.\n" },

    { "dskset",   cmd_dskset,   "select the CXFS volume by name or ID",
      "dskset - choose which disk CXFS operates on\n"
      "Usage: dskset <name>      e.g. dskset HDD0\n"
      "       dskset -i 0xNN     e.g. dskset -i 0x01\n"
      "       dskset             show the current selection\n"
      "Names and IDs come from the 'disks' command. After selecting, CXFS\n"
      "tries to mount an existing filesystem; use 'format' to create one.\n" },

    { "disktest", cmd_disktest, "test ATA disk read/write",
      "disktest - read/write test on an ATA disk\n"
      "Usage: disktest [drive 0-3]   (default 1 = filesystem disk)\n"
      "WARNING: writes to LBA 100 on the chosen drive (destroys data there).\n"
      "Writes a known pattern to sector (LBA) 100 of the filesystem disk,\n"
      "reads it back, and verifies it matches. Open the filesystem image in\n"
      "a hex editor afterward to see the pattern at offset 100*512 = 0xC800.\n" },

    { "format",   cmd_format,   "format the disk as CXFS",
      "format - initialize the filesystem disk with a blank CXFS\n"
      "Usage: format\n"
      "WARNING: erases the filesystem disk. Writes the CXFS superblock,\n"
      "bitmap, manifest, and an empty root directory.\n" },

    { "fsinfo",   cmd_fsinfo,   "show CXFS filesystem info",
      "fsinfo - display the mounted CXFS layout\n"
      "Usage: fsinfo\n"
      "Shows version, block size, and the on-disk layout of the manifest,\n"
      "bitmap, and data regions.\n" },

    { "fstest",   cmd_fstest,   "test CXFS block/entry allocation",
      "fstest - exercise the CXFS allocator\n"
      "Usage: fstest\n"
      "Allocates and frees data blocks (showing reuse) and finds a free\n"
      "manifest slot. Verifies the on-disk bitmap allocator works.\n" },

    { "pwd",      cmd_pwd,      "print working directory",
      "pwd - print the current directory path\n"
      "Usage: pwd\n" },

    { "cd",       cmd_cd,       "change directory",
      "cd - change the current directory\n"
      "Usage: cd <path>\n"
      "Absolute (/a/b) or relative paths; '.' and '..' supported.\n"
      "cd with no argument returns to root.\n" },

    { "mkdir",    cmd_mkdir,    "make a directory",
      "mkdir - create a directory in the current directory\n"
      "Usage: mkdir <name>\n"
      "Spaces in the name become underscores; '/' is not allowed.\n" },

    { "ls",       cmd_ls,       "list directory contents",
      "ls - list entries in a directory\n"
      "Usage: ls [-l] [path]\n"
      "Lists the current directory, or the given path. Directories shown\n"
      "in cyan with a trailing slash.\n"
      "  -l   long form: show type, id, parent, and size per entry.\n" },

    { "write",    cmd_write,    "write text to a file",
      "write - create or overwrite a file with text\n"
      "Usage: write [-f] <name> [content]\n"
      "With inline content: stores it as a single line.\n"
      "With no content: enters multi-line mode - type lines after the '>'\n"
      "prompt and finish with a line containing <<DONE>> (case-insensitive).\n"
      "Ctrl+C aborts multi-line entry (nothing written).\n"
      "If the file already has content, -f is required to overwrite.\n" },

    { "cat",      cmd_cat,      "display file contents",
      "cat - print a file's contents\n"
      "Usage: cat <path>\n" },

    { "rn",       cmd_rn,       "rename a file or directory",
      "rn - rename an entry in place\n"
      "Usage: rn [-f] [-n] [-i] <path> <newname>\n"
      "  -f  replace an existing target (destroys it)\n"
      "  -n  dry-run: show what would happen, do nothing\n"
      "  -i  interactive: confirm before acting\n" },

    { "mv",       cmd_mv,       "move a file or directory",
      "mv - move an entry into another directory\n"
      "Usage: mv [-f] <source> <destdir>\n"
      "Changes the entry's parent (data does not move). Works on files\n"
      "and directories; cannot move a directory into its own descendant.\n"
      "  -f  replace a colliding entry at the destination\n" },

    { "rm",       cmd_rm,       "remove file(s)",
      "rm - remove files (not directories)\n"
      "Usage: rm [-r] [-f] [-n] [-i] <file>\n"
      "  (no flags) remove an empty file\n"
      "  -f  remove even if it has content\n"
      "  -r  operate on all files in the current directory (no descent)\n"
      "  -rf force-remove all files in the current directory\n"
      "  -n  dry-run; -i  interactive confirm\n" },

    { "rmdir",    cmd_rmdir,    "remove a directory",
      "rmdir - remove a directory\n"
      "Usage: rmdir [-r] [-f] [-n] [-i] <dir>\n"
      "  (no flags) remove only if empty\n"
      "  -rf remove the directory and everything in it (recursive)\n"
      "  -n  dry-run; -i  interactive confirm\n" },

    { "reboot",   cmd_reboot,   "restart the machine",
      "reboot - restart the computer\n"
      "Usage: reboot\n"
      "Uses the ACPI reset register if available, else the keyboard controller.\n" },
    { "shutdown", cmd_shutdown, "power off the machine (ACPI S5)",
      "shutdown - power off the computer\n"
      "Usage: shutdown\n"
      "Uses ACPI S5 soft-off where supported. (No --firmware option: returning\n"
      "to firmware setup is a UEFI feature, unavailable on legacy BIOS boot.)\n" },
    { "sleep",    cmd_sleep,    "low-power CPU idle until a key (or N seconds)",
      "sleep - enter a low-power CPU idle state (C1)\n"
      "Usage: sleep            idle until a key is pressed\n"
      "       sleep <seconds>  idle for that many seconds (wakes early on a key)\n"
      "Note: the CPU halts to save power and wakes on any interrupt. (ACPI S1\n"
      "is detected at boot but not used yet - waking it needs more ACPI work.)\n" },
};

const unsigned commands_count = sizeof(commands) / sizeof(commands[0]);
#define NUM_COMMANDS commands_count

static void cmd_help(const char *args) {
    /* `help` with no argument: list all commands with short help */
    if (args[0] == '\0') {
        console_begin_batch();
        console_print("Available commands:\n");
        for (unsigned i = 0; i < NUM_COMMANDS; i++) {
            console_print("  ");
            console_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
            console_print(commands[i].name);
            console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            int pad = 10 - (int)strlen(commands[i].name);
            for (int p = 0; p < pad; p++) console_putc(' ');
            console_print("- ");
            console_print(commands[i].short_help);
            console_putc('\n');
        }
        console_print("Type 'help <command>' for details.\n");
        console_end_batch();
        return;
    }

    /* `help <command>`: show that command's detail (or short help if none) */
    for (unsigned i = 0; i < NUM_COMMANDS; i++) {
        if (strcasecmp(args, commands[i].name) == 0) {
            if (commands[i].detail[0] != '\0') {
                console_print(commands[i].detail);
            } else {
                /* no detailed help — fall back to the short description */
                console_print(commands[i].name);
                console_print(" - ");
                console_print(commands[i].short_help);
                console_putc('\n');
            }
            return;
        }
    }

    console_set_color(VGA_LIGHT_RED, VGA_BLACK);
    console_print("No such command: ");
    console_print(args);
    console_putc('\n');
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}