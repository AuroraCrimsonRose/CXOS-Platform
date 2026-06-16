/* /CXLite/kernel/shell/shell.h */
/* Aurora Tejeda */

#ifndef SHELL_H
#define SHELL_H

/* run the interactive shell loop (never returns) */
void shell_run(void);

/* Read one line of input into `buf` (capacity `cap`), with backspace and
   command-history (arrow) editing. `use_history` enables up/down recall.
   Returns:
     1  = line entered (Enter), buf holds the (null-terminated) line
     0  = aborted with Ctrl+C (buf undefined)
   Used by the REPL and by multi-line input (e.g. write). */
int shell_read_line(char *buf, int cap, int use_history);

#endif