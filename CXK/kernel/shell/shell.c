/* /CXLite/kernel/shell/shell.c */
/* Aurora Tejeda */
/*
 * Shell REPL: prompt, line input (history + editing), dispatch.
 * Commands live in commands.c (via commands.h). The line editor is exposed
 * as shell_read_line() so multi-line input (write) can reuse it.
 */

#include "shell.h"
#include "commands.h"
#include "console.h"
#include "keyboard.h"
#include "string.h"
#include "vga.h"

#define CMD_BUF_SIZE 128
#define PROMPT "CXK> "

/* ---- command history: a ring of the last HIST_SIZE commands ---- */
#define HIST_SIZE 16
static char hist[HIST_SIZE][CMD_BUF_SIZE];
static int  hist_count = 0;
static int  hist_next  = 0;

static void hist_add(const char *line) {
    if (line[0] == '\0') return;
    if (hist_count > 0) {
        int last = (hist_next - 1 + HIST_SIZE) % HIST_SIZE;
        if (strcmp(hist[last], line) == 0) return;
    }
    strlcpy(hist[hist_next], line, CMD_BUF_SIZE);
    hist_next = (hist_next + 1) % HIST_SIZE;
    if (hist_count < HIST_SIZE) hist_count++;
}

static const char *hist_get(int back) {
    if (back < 1 || back > hist_count) return 0;
    int idx = (hist_next - back + HIST_SIZE) % HIST_SIZE;
    return hist[idx];
}

static void erase_line(int len) {
    for (int i = 0; i < len; i++) console_putc('\b');
}

/* Read one line with editing (backspace, history, mid-line cursor movement).
   Returns 1 on Enter, 0 on Ctrl+C abort.

   Editing model (uses only console_putc + '\b', so it's backend-agnostic):
     buf[0..len)  = the line contents
     pos          = cursor position within the line (0..len)
   On any change we "repaint the tail": from the cursor, reprint buf[pos..len),
   print a space to erase a just-deleted trailing char, then backspace back to
   pos. Moving left/right is just backspaces / reprints of single chars. */
int shell_read_line(char *buf, int cap, int use_history) {
    int len = 0;        /* number of chars in buf */
    int pos = 0;        /* cursor position within buf */
    int browse = 0;

    for (;;) {
        unsigned char c = (unsigned char)keyboard_getchar_blocking();

        if (c == KEY_CTRL_C) {
            console_print("^C\n");
            return 0;
        }
        else if (c == '\n') {
            /* move cursor to end so the newline lands after the whole line */
            while (pos < len) { console_putc(buf[pos]); pos++; }
            console_putc('\n');
            break;
        }
        else if (c == '\b') {
            if (pos > 0) {
                /* delete char before cursor: shift tail left */
                for (int i = pos - 1; i < len - 1; i++) buf[i] = buf[i + 1];
                len--;
                pos--;
                /* repaint: move left one, reprint tail, erase last, reposition */
                console_putc('\b');
                for (int i = pos; i < len; i++) console_putc(buf[i]);
                console_putc(' ');                       /* erase old last char */
                for (int i = len; i >= pos; i--) console_putc('\b');
            }
        }
        else if (c == KEY_LEFT) {
            if (pos > 0) { pos--; console_putc('\b'); }
        }
        else if (c == KEY_RIGHT) {
            if (pos < len) { console_putc(buf[pos]); pos++; }
        }
        else if (c == KEY_HOME) {
            while (pos > 0) { pos--; console_putc('\b'); }
        }
        else if (c == KEY_END) {
            while (pos < len) { console_putc(buf[pos]); pos++; }
        }
        else if (use_history && c == KEY_UP) {
            if (browse < hist_count) {
                const char *h = hist_get(browse + 1);
                if (h) {
                    browse++;
                    /* move to end, then erase whole line */
                    while (pos < len) { console_putc(buf[pos]); pos++; }
                    erase_line(len);
                    len = 0; pos = 0;
                    for (int i = 0; h[i] && len < cap - 1; i++) {
                        buf[len++] = h[i]; console_putc(h[i]);
                    }
                    pos = len;
                }
            }
        }
        else if (use_history && c == KEY_DOWN) {
            if (browse > 0) {
                browse--;
                while (pos < len) { console_putc(buf[pos]); pos++; }
                erase_line(len);
                len = 0; pos = 0;
                if (browse > 0) {
                    const char *h = hist_get(browse);
                    if (h) for (int i = 0; h[i] && len < cap - 1; i++) {
                        buf[len++] = h[i]; console_putc(h[i]);
                    }
                }
                pos = len;
            }
        }
        else if (c == KEY_PGUP)     { console_scroll_up(VGA_HEIGHT - 1); }
        else if (c == KEY_PGDN)     { console_scroll_down(VGA_HEIGHT - 1); }
        else if (c == KEY_SHIFT_UP) { console_scroll_up(1); }
        else if (c == KEY_SHIFT_DN) { console_scroll_down(1); }
        else if (c >= 0x80) {
            /* other special keys - ignore */
        }
        else if (len < cap - 1) {
            /* insert printable char at pos: shift tail right */
            for (int i = len; i > pos; i--) buf[i] = buf[i - 1];
            buf[pos] = (char)c;
            len++;
            /* paint: print from pos to end, then backspace to just after the
               inserted char */
            for (int i = pos; i < len; i++) console_putc(buf[i]);
            for (int i = len; i > pos + 1; i--) console_putc('\b');
            pos++;
            browse = 0;
        }
    }

    buf[len] = '\0';
    return 1;
}

static void shell_execute(char *line) {
    char *args = line;
    while (*args && *args != ' ') args++;
    if (*args == ' ') {
        *args = '\0';
        args++;
        while (*args == ' ') args++;
    }

    if (line[0] == '\0') return;

    for (unsigned i = 0; i < commands_count; i++) {
        if (strcasecmp(line, commands[i].name) == 0) {
            /* Batch the command's output so multi-line results render in a
               single screen redraw instead of scrolling once per line. The
               matching end_batch (after the handler) is guaranteed to run, so
               no command can leave rendering deferred.

               EXCEPTION: interactive / live-updating commands (their own
               render loops with input polling) must render continuously, so
               they are NOT batched. */
            /* Batch the command's output so multi-line results render in a
               single screen redraw instead of scrolling once per line. The
               matching end_batch (after the handler) always runs, so no command
               can leave rendering deferred.

               EXCEPTION - commands listed here are NOT batched because they
               either render continuously or BLOCK while producing output, and
               batching would defer their text until they return (e.g. you'd see
               "Sleeping..." only AFTER waking). Any command that loops, waits,
               or streams output over time MUST be added to this list. */
            int interactive =
                (strcmp(commands[i].name, "spin") == 0)   ||  /* live render loop */
                (strcmp(commands[i].name, "uptime") == 0) ||  /* live -p updates */
                (strcmp(commands[i].name, "sleep") == 0)  ||  /* blocks while idling */
                (strcmp(commands[i].name, "write") == 0)  ||  /* multi-line input prompt */
                (strcmp(commands[i].name, "cat") == 0)    ||  /* may page / read a key */
                (strcmp(commands[i].name, "arping") == 0)    ||  /* blocks polling for ARP reply */
                (strcmp(commands[i].name, "ping") == 0)    ||  /* blocks polling for ICMP reply */
                (strcmp(commands[i].name, "threads") == 0)    ||  /* loops yielding between threads */
                (strcmp(commands[i].name, "preempt") == 0)    ||  /* timer-driven multitasking demo */
                (strcmp(commands[i].name, "proc") == 0);       /* yields to a ring-3 process */

            if (interactive) {
                commands[i].handler(args);
            } else {
                console_begin_batch();
                commands[i].handler(args);
                console_end_batch();
            }
            return;
        }
    }

    console_set_color(VGA_LIGHT_RED, VGA_BLACK);
    console_print("Unknown command: ");
    console_print(line);
    console_putc('\n');
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

void shell_run(void) {
    char buf[CMD_BUF_SIZE];

    for (;;) {
        console_set_color(VGA_YELLOW, VGA_BLACK);
        console_print(PROMPT);
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);

        if (!shell_read_line(buf, CMD_BUF_SIZE, 1)) {
            continue;   /* Ctrl+C: discard, fresh prompt */
        }
        hist_add(buf);
        shell_execute(buf);
    }
}