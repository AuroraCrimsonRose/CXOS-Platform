/* /CXLite/kernel/shell/commands.h */
/* Aurora Tejeda */
/*
 * Shell command table and handlers. The shell (shell.c) provides the REPL
 * (prompt, line input, dispatch); this module provides WHAT commands exist
 * and what they do. Adding a command = one new row in the table here.
 */

#ifndef COMMANDS_H
#define COMMANDS_H

struct command {
    const char *name;
    void (*handler)(const char *args);
    const char *short_help;   /* one-liner for the `help` list */
    const char *detail;       /* long help for `help <command>`; "" = none */
};

/* the command table, exposed read-only so the shell can dispatch and
   (later) enumerate command names for things like tab-completion. */
extern const struct command commands[];
extern const unsigned commands_count;

#endif