/* /CXLite/kernel/drivers/console.c */
/* Aurora Tejeda */
/*
 * Console driver with scrollback - TWO BACKENDS.
 *
 * The history buffer (a ring of lines of char+color cells) is the source of
 * truth and is backend-independent. Rendering goes to ONE of:
 *   - the framebuffer (graphics mode, via fb_draw_char) when fb_active(), or
 *   - VGA text memory (via vga_put_cell) as the fallback.
 * Screen dimensions are computed from whichever backend is active (and, in
 * graphics mode, from the current font height - so font size is switchable).
 */

#include "console.h"
#include "vga.h"
#include "fb.h"

#define TAB_WIDTH   4
#define HIST_LINES  500          /* scrollback depth */
#define MAX_COLS    128          /* widest console we support (1024/8) */

/* a cell = char (low byte) + VGA color attribute (high byte) */
static uint16_t hist[HIST_LINES][MAX_COLS];

static int total_lines = 1;
static int top_line    = 0;
static int cur_line    = 0;
static int cur_x       = 0;
static int view_offset = 0;
static uint8_t cur_attr = 0;

/* active screen geometry (set by recompute_geometry) */
static int scr_cols = VGA_WIDTH;
static int scr_rows = VGA_HEIGHT;
static int use_fb   = 0;         /* 1 = framebuffer backend, 0 = VGA text */

/* 16 VGA colors -> RGB (for the framebuffer backend) */
static const uint32_t vga_palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};

/* recompute scr_cols/scr_rows from the active backend + font */
static void recompute_geometry(void) {
    if (fb_active()) {
        use_fb = 1;
        int cols = (int)(fb_width()  / FB_CHAR_W);
        int rows = (int)(fb_height() / fb_font_height());
        if (cols > MAX_COLS) cols = MAX_COLS;
        scr_cols = cols;
        scr_rows = rows;
    } else {
        use_fb = 0;
        scr_cols = VGA_WIDTH;
        scr_rows = VGA_HEIGHT;
    }
}

static int ring_index(int logical) { return (top_line + logical) % HIST_LINES; }
static int cur_logical(void)       { return total_lines - 1; }

/* the screen row (0..scr_rows-1) where the cursor/current line currently sits */
static int screen_row_of_current(void) {
    int visible_lines = cur_logical() + 1;
    return (visible_lines < scr_rows) ? cur_logical() : (scr_rows - 1);
}

static void blank_line(int ring) {
    uint16_t blank = (uint16_t)' ' | ((uint16_t)cur_attr << 8);
    for (int x = 0; x < MAX_COLS; x++) hist[ring][x] = blank;
}

/* draw one cell to the screen at (col,row), via whichever backend is active */
static void draw_cell(int col, int row, char ch, uint8_t attr) {
    if (use_fb) {
        uint32_t fg = fb_rgb((vga_palette[attr & 0x0F] >> 16) & 0xFF,
                             (vga_palette[attr & 0x0F] >> 8) & 0xFF,
                              vga_palette[attr & 0x0F] & 0xFF);
        uint32_t bg = fb_rgb((vga_palette[(attr >> 4) & 0x0F] >> 16) & 0xFF,
                             (vga_palette[(attr >> 4) & 0x0F] >> 8) & 0xFF,
                              vga_palette[(attr >> 4) & 0x0F] & 0xFF);
        fb_draw_char((uint32_t)(col * FB_CHAR_W),
                     (uint32_t)(row * (int)fb_font_height()),
                     ch, fg, bg);
    } else {
        vga_put_cell(col, row, ch, attr);
    }
}

/* full redraw of the visible window at the current view_offset */
/* map a screen row (0..scr_rows-1) to a logical history line, matching how
   the console places lines. when the screen isn't full and we're at the live
   bottom, lines are top-anchored (logical line N at screen row N); otherwise
   the window is bottom-anchored at the newest line minus view_offset. */
static int screen_row_to_logical(int row) {
    int visible_lines = cur_logical() + 1;
    if (visible_lines <= scr_rows && view_offset == 0) {
        return row;                       /* not full: screen row == logical */
    }
    int bottom = cur_logical() - view_offset;
    int top = bottom - (scr_rows - 1);
    return top + row;
}

static void render(void) {
    for (int row = 0; row < scr_rows; row++) {
        int logical = screen_row_to_logical(row);
        if (logical < 0 || logical > cur_logical()) {
            for (int x = 0; x < scr_cols; x++) draw_cell(x, row, ' ', cur_attr);
        } else {
            int ring = ring_index(logical);
            for (int x = 0; x < scr_cols; x++) {
                uint16_t cell = hist[ring][x];
                draw_cell(x, row, (char)(cell & 0xFF), (uint8_t)(cell >> 8));
            }
        }
    }
}

/* cheap: redraw just the current line to its screen row */
static void render_current_line(void) {
    if (view_offset != 0) return;
    int visible_lines = cur_logical() + 1;
    int row = (visible_lines < scr_rows) ? cur_logical() : (scr_rows - 1);
    int ring = cur_line;
    for (int x = 0; x < scr_cols; x++) {
        uint16_t cell = hist[ring][x];
        draw_cell(x, row, (char)(cell & 0xFF), (uint8_t)(cell >> 8));
    }
}

/* render a specific screen row from history (used to erase the cursor by
   redrawing the row underneath it). row is a screen row 0..scr_rows-1. */
static void render_screen_row(int row) {
    int logical = screen_row_to_logical(row);
    if (logical < 0 || logical > cur_logical()) {
        for (int x = 0; x < scr_cols; x++) draw_cell(x, row, ' ', cur_attr);
    } else {
        int ring = ring_index(logical);
        for (int x = 0; x < scr_cols; x++) {
            uint16_t cell = hist[ring][x];
            draw_cell(x, row, (char)(cell & 0xFF), (uint8_t)(cell >> 8));
        }
    }
}

/* cursor: the VGA backend uses the hardware cursor; the framebuffer backend
   draws a block cursor at the current position, erasing its previous spot. */
static int last_cur_row = -1;   /* last fb cursor row drawn (-1 = none) */

/* batch mode: while >0, console_putc updates the text grid + cursor position
   but skips the expensive per-character scroll/render. console_print uses this
   to print a whole string then render ONCE, instead of scrolling the screen
   once per output line (which made 'help' and other multi-line output crawl). */
static int batch_depth = 0;
/* during a batch we track whether the content scrolled and which screen rows
   were touched, so end_batch can render minimally (just the affected rows)
   instead of always doing a full-screen render - the latter made short output
   like the prompt expensive. */
static int batch_scrolled = 0;
static int batch_start_row = 0;

static void update_cursor(void) {
    if (!use_fb) {
        if (view_offset == 0) {
            int row = scr_rows - 1;
            int visible_lines = cur_logical() + 1;
            if (visible_lines < scr_rows) row = cur_logical();
            vga_set_cursor(cur_x, row);
        }
        return;
    }

    /* erase the cursor at its previous row (redraw that row from history) */
    if (last_cur_row >= 0 && last_cur_row < scr_rows)
        render_screen_row(last_cur_row);
    last_cur_row = -1;

    /* don't show a cursor while scrolled up into history */
    if (view_offset != 0) return;

    int visible_lines = cur_logical() + 1;
    int row = (visible_lines < scr_rows) ? cur_logical() : (scr_rows - 1);

    uint32_t fg = vga_palette[cur_attr & 0x0F];
    uint32_t cursor_color = fb_rgb((uint8_t)((fg >> 16) & 0xFF),
                                   (uint8_t)((fg >> 8) & 0xFF),
                                   (uint8_t)(fg & 0xFF));
    uint32_t fh = fb_font_height();
    fb_fill_rect((uint32_t)(cur_x * FB_CHAR_W),
                 (uint32_t)(row * (int)fh) + fh - 2,
                 FB_CHAR_W, 2, cursor_color);
    last_cur_row = row;
}

static void new_line(void) {
    cur_x = 0;
    if (total_lines < HIST_LINES) {
        total_lines++;
        cur_line = ring_index(cur_logical());
    } else {
        top_line = (top_line + 1) % HIST_LINES;
        cur_line = ring_index(cur_logical());
    }
    blank_line(cur_line);
}

void console_set_color(uint8_t fg, uint8_t bg) {
    cur_attr = VGA_ATTR(fg, bg);
}

void console_clear(void) {
    total_lines = 1;
    top_line = 0;
    cur_line = 0;
    cur_x = 0;
    view_offset = 0;
    blank_line(cur_line);
    if (use_fb) fb_clear(vga_palette[(cur_attr >> 4) & 0x0F]);
    else        vga_clear(cur_attr);
    /* If we're inside a batch (e.g. the shell dispatcher wrapping `clear`),
       skip the render here - the batch's end will render the final state once,
       avoiding a redundant full-screen redraw. The fb_clear above already
       wiped the screen, so nothing is shown stale in the meantime. */
    if (batch_depth == 0) {
        render();
        update_cursor();
    }
}

void console_init(void) {
    cur_attr = VGA_ATTR(VGA_LIGHT_GREY, VGA_BLACK);
    recompute_geometry();
    if (!use_fb) vga_enable_cursor();
    console_clear();
}

/* switch the graphics font live, recompute geometry, re-render. no-op in
   text mode (VGA has a fixed font). */
void console_set_font(int small) {
    if (!fb_active()) return;
    fb_set_font(small ? FB_FONT_8X8 : FB_FONT_8X16);
    recompute_geometry();
    /* clamp cursor column into the new width */
    if (cur_x >= scr_cols) cur_x = scr_cols - 1;
    fb_clear(vga_palette[(cur_attr >> 4) & 0x0F]);
    render();
}

void console_putc(char c) {
    if (view_offset != 0) view_offset = 0;

    /* Erase the previously-drawn cursor up front, while last_cur_row still
       refers to the current (pre-change) screen layout. The cursor is a baked-in
       pixel bar, so it must be cleared before we scroll/render or it leaves a
       trail. update_cursor() repaints it fresh at the end. */
    if (use_fb && !batch_depth && last_cur_row >= 0 && last_cur_row < scr_rows) {
        render_screen_row(last_cur_row);
        last_cur_row = -1;
    }

    int shifted = 0;

    switch (c) {
        case '\n':
            new_line();
            shifted = 1;
            break;
        case '\r':
            cur_x = 0;
            break;
        case '\b':
            /* non-destructive: move the cursor left only. (The line editor
               manages character deletion itself by reprinting + spacing; if
               the console also erased here, the editor's reposition backspaces
               would wipe the very characters just typed.) */
            if (cur_x > 0) cur_x--;
            break;
        case '\t':
            cur_x = (cur_x + TAB_WIDTH) & ~(TAB_WIDTH - 1);
            if (cur_x >= scr_cols) { new_line(); shifted = 1; }
            break;
        default:
            hist[cur_line][cur_x] = (uint16_t)(unsigned char)c | ((uint16_t)cur_attr << 8);
            if (++cur_x >= scr_cols) { new_line(); shifted = 1; }
            break;
    }

    /* On a line shift past a full screen: in graphics mode, scroll the
       framebuffer up by one text row (fast pixel copy) and draw just the new
       bottom line - instead of re-rendering every glyph (which made commands
       that print many lines crawl). Text mode's full render is cheap, keep it. */
    /* Erase the previously-drawn cursor first (redraw its row from history),
       so it's never copied by a scroll or left behind by a render. The cursor
       is then repainted fresh by update_cursor() at the end. */
    /* In batch mode, skip rendering here - we only update the text grid and
       cursor position. Record whether a scroll happened so end_batch can choose
       a full render (scrolled) vs a cheap partial render (didn't). */
    if (batch_depth) {
        if (shifted && total_lines > scr_rows) batch_scrolled = 1;
        return;
    }

    if (shifted && total_lines > scr_rows) {
        if (use_fb) {
            uint32_t bg = vga_palette[(cur_attr >> 4) & 0x0F];
            fb_scroll_up(fb_font_height(), bg);
            render_current_line();                   /* draw the new bottom line */
        } else {
            render();
        }
    } else {
        render_current_line();
    }
    update_cursor();
}

void console_print(const char *s) {
    /* Batch the string so it renders once. Track the start row + whether it
       scrolled, so we render minimally: a full render only if the screen
       scrolled, otherwise just the rows from where we started to where we are.
       (Short output like the prompt then costs a couple of rows, not a full
       screen redraw.) */
    int outer = (batch_depth == 0);
    if (outer) {
        batch_scrolled = 0;
        batch_start_row = screen_row_of_current();
    }
    batch_depth++;
    while (*s) console_putc(*s++);
    batch_depth--;

    if (batch_depth == 0) {
        if (batch_scrolled) {
            render();
        } else {
            int end_row = screen_row_of_current();
            int from = batch_start_row, to = end_row;
            if (from > to) { int t = from; from = to; to = t; }
            for (int r = from; r <= to && r < scr_rows; r++)
                if (r >= 0) render_screen_row(r);
        }
        update_cursor();
    }
}

/* Public batch controls: a command that emits multi-line output via a mix of
   console_putc and console_print can wrap its whole output in these to get a
   single screen redraw at the end instead of one scroll per line. Nestable. */
void console_begin_batch(void) {
    if (batch_depth == 0) {
        batch_scrolled = 0;
        batch_start_row = screen_row_of_current();
    }
    batch_depth++;
}
void console_end_batch(void) {
    if (batch_depth > 0) batch_depth--;
    if (batch_depth == 0) {
        if (batch_scrolled) {
            render();
        } else {
            int end_row = screen_row_of_current();
            int from = batch_start_row, to = end_row;
            if (from > to) { int t = from; from = to; to = t; }
            for (int r = from; r <= to && r < scr_rows; r++)
                if (r >= 0) render_screen_row(r);
        }
        update_cursor();
    }
}

/* set the cursor to a specific column on the current line and redraw it there.
   used by the line editor for mid-line cursor movement. */
void console_set_cursor_col(int col) {
    if (col < 0) col = 0;
    if (col >= scr_cols) col = scr_cols - 1;
    cur_x = col;
    update_cursor();
}

void console_print_hex(uint32_t v) {
    const char *digits = "0123456789ABCDEF";
    console_print("0x");
    for (int i = 28; i >= 0; i -= 4)
        console_putc(digits[(v >> i) & 0xF]);
}

void console_print_dec(uint32_t v) {
    char buf[11];
    int i = 0;
    if (v == 0) { console_putc('0'); return; }
    while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i > 0) console_putc(buf[--i]);
}

/* ---- scrollback controls ---- */

static int max_offset(void) {
    int m = cur_logical() + 1 - scr_rows;
    return m < 0 ? 0 : m;
}

void console_scroll_up(int lines) {
    view_offset += lines;
    if (view_offset > max_offset()) view_offset = max_offset();
    render();
    if (!use_fb) {
        if (view_offset != 0) vga_set_cursor(VGA_WIDTH, VGA_HEIGHT);
        else update_cursor();
    }
}

void console_scroll_down(int lines) {
    view_offset -= lines;
    if (view_offset < 0) view_offset = 0;
    render();
    update_cursor();
}

void console_scroll_reset(void) {
    if (view_offset != 0) {
        view_offset = 0;
        render();
        update_cursor();
    }
}