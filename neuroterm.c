/* neuroterm: a small X11 terminal whose every cell is drawn by Neuronte.
 *
 * It is meant as a rescue terminal: the thing that still starts when the
 * main terminal does not. So it has few parts. One process, plain Xlib,
 * a pty, the libvterm parser and the Neuronte nets compiled in, the weight
 * files inside the program. It reads no configuration file, opens no font
 * file, and never asks the X server for a font: no text request, no font
 * cursor, no input method. Finished pixels go to the window with XPutImage.
 *
 * MIT licence. See LICENSE.
 */
#define _GNU_SOURCE

#include "glyphs.h"

#include "vterm.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define VERSION "0.1.0"
#define PAD 2          /* pixels between the window edge and the cells */
#define MIN_CELL_W 4
#define MAX_CELL_W 64

/* ---- state ---- */

static Display *dpy;
static Window win;
static GC gc;
static Visual *visual;
static int depth;
static XImage *image;
static uint32_t *fb;       /* win_w * win_h, 0xFFRRGGBB */
static int win_w, win_h;
static int cell_w = 10, cell_h = 20;
static int cols = 80, rows = 24;
static int focused, exposed;
static Atom a_wm_protocols, a_wm_delete, a_net_wm_name, a_utf8, a_targets, a_clipboard, a_incr,
    a_paste_prop, a_text, a_net_wm_pid;

static VTerm *vt;
static VTermScreen *vts;
static VTermState *vtstate;
static int pty = -1;
static pid_t child = -1;
static int child_gone, hold;
static int wake_pipe[2] = {-1, -1};

static uint32_t default_fg = 0xd8d8d8, default_bg = 0x101014;
static VTermPos cursor;
static int cursor_visible = 1, cursor_shape = VTERM_PROP_CURSORSHAPE_BLOCK;
static int altscreen, mouse_mode, focus_report;

static unsigned char *dirty; /* one flag per row */
static int dirty_len, all_dirty = 1;

/* bytes waiting to go to the child */
static char *out_buf;
static size_t out_len, out_cap;

/* ---- small helpers ---- */

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("neuroterm: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        die("out of memory");
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p)
        die("out of memory");
    return p;
}

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static size_t utf8_put(uint32_t c, char *out)
{
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (char)(0xc0 | c >> 6);
        out[1] = (char)(0x80 | (c & 0x3f));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (char)(0xe0 | c >> 12);
        out[1] = (char)(0x80 | ((c >> 6) & 0x3f));
        out[2] = (char)(0x80 | (c & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | c >> 18);
    out[1] = (char)(0x80 | ((c >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3f));
    out[3] = (char)(0x80 | (c & 0x3f));
    return 4;
}

static void to_child(const char *bytes, size_t len)
{
    if (pty < 0 || !len)
        return;
    if (out_len + len > out_cap) {
        out_cap = (out_len + len) * 2;
        out_buf = xrealloc(out_buf, out_cap);
    }
    memcpy(out_buf + out_len, bytes, len);
    out_len += len;
}

static void vt_output(const char *bytes, size_t len, void *user)
{
    (void)user;
    to_child(bytes, len);
}

static void mark_row(int row)
{
    if (row >= 0 && row < dirty_len)
        dirty[row] = 1;
}

/* ---- scrollback ---- */

struct sb_line {
    int n;
    VTermScreenCell cells[];
};

static struct sb_line **sb;
static int sb_cap = 10000, sb_len, sb_head;
static int scroll_off; /* lines the view is scrolled back */
static VTermScreenCell blank_cell;

static struct sb_line *sb_at(int i)
{
    return sb[(sb_head + i) % sb_cap];
}

/* selection, in lines counted from the oldest scrollback line */
static struct {
    int active, dragging, mode; /* mode 0 characters, 1 words, 2 lines */
    int a_line, a_col;          /* where the drag began */
    int b_line, b_col;          /* where it is now */
    int a_end_col;              /* end of the first word or line */
} sel;
static char *primary_text, *clipboard_text;

static void sel_clear(void)
{
    if (sel.active) {
        sel.active = 0;
        all_dirty = 1;
    }
    sel.dragging = 0;
}

static int cell_is_blank(const VTermScreenCell *c)
{
    return (c->chars[0] == 0 || c->chars[0] == ' ') && VTERM_COLOR_IS_DEFAULT_BG(&c->bg) &&
           !c->attrs.reverse && !c->attrs.underline;
}

static int sb_push(int n, const VTermScreenCell *cells, void *user)
{
    (void)user;
    if (sb_cap <= 0)
        return 0;
    while (n > 0 && cell_is_blank(&cells[n - 1]))
        n--;
    struct sb_line *line = xmalloc(sizeof *line + (size_t)n * sizeof line->cells[0]);
    line->n = n;
    memcpy(line->cells, cells, (size_t)n * sizeof line->cells[0]);
    if (sb_len == sb_cap) {
        free(sb[sb_head]);
        sb[sb_head] = line;
        sb_head = (sb_head + 1) % sb_cap;
        if (sel.active) { /* every line number dropped by one */
            sel.a_line--;
            sel.b_line--;
            if (sel.a_line < 0 || sel.b_line < 0)
                sel_clear();
        }
    } else {
        sb[(sb_head + sb_len) % sb_cap] = line;
        sb_len++;
        if (scroll_off > 0 && scroll_off < sb_len)
            scroll_off++; /* keep the view on the same lines */
    }
    return 1;
}

static int sb_pop(int n, VTermScreenCell *cells, void *user)
{
    (void)user;
    if (!sb_len)
        return 0;
    struct sb_line *line = sb_at(sb_len - 1);
    for (int i = 0; i < n; i++)
        cells[i] = i < line->n ? line->cells[i] : blank_cell;
    free(line);
    sb_len--;
    if (scroll_off > sb_len)
        scroll_off = sb_len;
    sel_clear();
    return 1;
}

static int sb_clear(void *user)
{
    (void)user;
    for (int i = 0; i < sb_len; i++)
        free(sb_at(i));
    sb_len = sb_head = scroll_off = 0;
    sel_clear();
    all_dirty = 1;
    return 1;
}

/* The cell on line `line` (0 is the oldest scrollback line). */
static void cell_at(int line, int col, VTermScreenCell *out)
{
    if (line < sb_len) {
        struct sb_line *l = line >= 0 ? sb_at(line) : NULL;
        *out = l && col < l->n ? l->cells[col] : blank_cell;
        return;
    }
    VTermPos pos = {line - sb_len, col};
    if (pos.row >= rows || col >= cols || !vterm_screen_get_cell(vts, pos, out))
        *out = blank_cell;
}

static int top_line(void)
{
    return sb_len - scroll_off;
}

static void scroll_view(int delta)
{
    int to = clampi(scroll_off + delta, 0, sb_len);
    if (to != scroll_off) {
        scroll_off = to;
        all_dirty = 1;
    }
}

/* ---- selection ---- */

static void sel_range(int *l0, int *c0, int *l1, int *c1)
{
    int forward = sel.b_line > sel.a_line || (sel.b_line == sel.a_line && sel.b_col >= sel.a_col);
    if (forward) {
        *l0 = sel.a_line;
        *c0 = sel.a_col;
        *l1 = sel.b_line;
        *c1 = sel.b_col;
        if (sel.mode && sel.b_line == sel.a_line && *c1 < sel.a_end_col)
            *c1 = sel.a_end_col;
    } else {
        *l0 = sel.b_line;
        *c0 = sel.b_col;
        *l1 = sel.a_line;
        *c1 = sel.mode ? sel.a_end_col : sel.a_col;
    }
    if (sel.mode == 2) {
        *c0 = 0;
        *c1 = cols - 1;
    }
}

static int selected(int line, int col)
{
    if (!sel.active)
        return 0;
    int l0, c0, l1, c1;
    sel_range(&l0, &c0, &l1, &c1);
    if (line < l0 || line > l1)
        return 0;
    if (line == l0 && col < c0)
        return 0;
    if (line == l1 && col > c1)
        return 0;
    return 1;
}

static int is_word_cell(int line, int col)
{
    VTermScreenCell c;
    cell_at(line, col, &c);
    return c.chars[0] != 0 && c.chars[0] != ' ' && c.chars[0] != (uint32_t)-1 ? 1
           : c.chars[0] == (uint32_t)-1;
}

static char *sel_text(void)
{
    int l0, c0, l1, c1;
    sel_range(&l0, &c0, &l1, &c1);
    size_t cap = 256, len = 0;
    char *text = xmalloc(cap);
    for (int line = l0; line <= l1; line++) {
        int from = line == l0 ? c0 : 0, to = line == l1 ? c1 : cols - 1;
        size_t line_start = len, last_ink = len;
        for (int col = from; col <= to && col < cols; col++) {
            VTermScreenCell c;
            cell_at(line, col, &c);
            if (c.chars[0] == (uint32_t)-1)
                continue; /* the second half of a wide character */
            if (len + 4 * VTERM_MAX_CHARS_PER_CELL + 2 > cap) {
                cap *= 2;
                text = xrealloc(text, cap);
            }
            if (c.chars[0] == 0) {
                text[len++] = ' ';
                continue;
            }
            for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && c.chars[i]; i++)
                len += utf8_put(c.chars[i], text + len);
            if (c.chars[0] != ' ')
                last_ink = len;
        }
        (void)line_start;
        len = last_ink; /* drop the blanks at the end of the line */
        if (line < l1)
            text[len++] = '\n';
    }
    text[len] = 0;
    return text;
}

/* ---- drawing ---- */

static uint32_t colour(VTermColor *c, int is_fg)
{
    if (is_fg ? VTERM_COLOR_IS_DEFAULT_FG(c) : VTERM_COLOR_IS_DEFAULT_BG(c))
        return is_fg ? default_fg : default_bg;
    vterm_screen_convert_color_to_rgb(vts, c);
    return (uint32_t)c->rgb.red << 16 | (uint32_t)c->rgb.green << 8 | c->rgb.blue;
}

static inline uint32_t mix(uint32_t bg, uint32_t fg, unsigned a)
{
    unsigned na = 255 - a;
    unsigned r = (((fg >> 16) & 255) * a + ((bg >> 16) & 255) * na + 127) / 255;
    unsigned g = (((fg >> 8) & 255) * a + ((bg >> 8) & 255) * na + 127) / 255;
    unsigned b = ((fg & 255) * a + (bg & 255) * na + 127) / 255;
    return 0xff000000u | r << 16 | g << 8 | b;
}

static void fill(int x, int y, int w, int h, uint32_t rgb)
{
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > win_w)
        w = win_w - x;
    if (y + h > win_h)
        h = win_h - y;
    rgb |= 0xff000000u;
    for (int j = 0; j < h; j++) {
        uint32_t *p = fb + (size_t)(y + j) * win_w + x;
        for (int i = 0; i < w; i++)
            p[i] = rgb;
    }
}

static void blend(const uint8_t *mask, int x, int y, int w, int h, uint32_t bg, uint32_t fg)
{
    for (int j = 0; j < h; j++) {
        if (y + j < 0 || y + j >= win_h)
            continue;
        uint32_t *p = fb + (size_t)(y + j) * win_w;
        const uint8_t *m = mask + (size_t)j * w;
        for (int i = 0; i < w; i++) {
            if (!m[i] || x + i < 0 || x + i >= win_w)
                continue;
            p[x + i] = m[i] == 255 ? (fg | 0xff000000u) : mix(bg, fg, m[i]);
        }
    }
}

/* Is this scalar drawn at all? Joiners and variation selectors are not. */
static int invisible(uint32_t c)
{
    return c == 0x200d || c == 0x200c || (c >= 0xfe00 && c <= 0xfe0f) ||
           (c >= 0xe0100 && c <= 0xe01ef) || c == 0x2060 || c == 0xfeff;
}

static void draw_row(int row)
{
    int line = top_line() + row;
    int y = PAD + row * cell_h;
    int line_px = cell_h >= 28 ? 2 : 1;
    int baseline = (3 * cell_h) / 4;
    int cursor_here = scroll_off == 0 && cursor_visible && row == cursor.row;

    for (int col = 0; col < cols;) {
        VTermScreenCell c;
        cell_at(line, col, &c);
        int span = c.width == 2 && col + 1 < cols ? 2 : 1;
        int x = PAD + col * cell_w, w = span * cell_w;
        uint32_t fg = colour(&c.fg, 1), bg = colour(&c.bg, 0);
        if (c.attrs.reverse) {
            uint32_t t = fg;
            fg = bg;
            bg = t;
        }
        if (selected(line, col)) {
            uint32_t t = fg;
            fg = bg;
            bg = t;
            if (fg == bg)
                fg = ~bg & 0xffffff;
        }
        int is_cursor = cursor_here && cursor.col >= col && cursor.col < col + span;
        if (is_cursor && focused && cursor_shape == VTERM_PROP_CURSORSHAPE_BLOCK) {
            fg = default_bg;
            bg = default_fg;
        }
        if (c.attrs.conceal)
            fg = bg;
        fill(x, y, w, cell_h, bg);

        uint32_t ch = c.chars[0];
        if (ch != 0 && ch != ' ' && ch != 0xa0 && ch != (uint32_t)-1) {
            unsigned style = (c.attrs.bold ? GLYPH_BOLD : 0u) | (c.attrs.italic ? GLYPH_ITALIC : 0u);
            const uint8_t *mask = glyphs_mask(ch, style, span == 2, cell_w, cell_h);
            if (mask)
                blend(mask, x, y, w, cell_h, bg, fg);
            for (int i = 1; i < VTERM_MAX_CHARS_PER_CELL && c.chars[i]; i++) {
                if (invisible(c.chars[i]))
                    continue;
                mask = glyphs_mask(c.chars[i], style, span == 2, cell_w, cell_h);
                if (mask)
                    blend(mask, x, y, w, cell_h, bg, fg);
            }
        }
        if (c.attrs.underline) {
            int uy = baseline + line_px + 1;
            if (uy + line_px > cell_h)
                uy = cell_h - line_px;
            fill(x, y + uy, w, line_px, fg);
            if (c.attrs.underline == VTERM_UNDERLINE_DOUBLE && uy - 2 * line_px > 0)
                fill(x, y + uy - 2 * line_px, w, line_px, fg);
        }
        if (c.attrs.strike)
            fill(x, y + (11 * cell_h) / 20, w, line_px, fg);

        if (is_cursor) {
            uint32_t cc = default_fg;
            if (!focused) { /* a hollow box */
                fill(x, y, w, 1, cc);
                fill(x, y + cell_h - 1, w, 1, cc);
                fill(x, y, 1, cell_h, cc);
                fill(x + w - 1, y, 1, cell_h, cc);
            } else if (cursor_shape == VTERM_PROP_CURSORSHAPE_UNDERLINE) {
                fill(x, y + cell_h - 2, w, 2, cc);
            } else if (cursor_shape == VTERM_PROP_CURSORSHAPE_BAR_LEFT) {
                fill(x, y, 2, cell_h, cc);
            }
        }
        col += span;
    }
}

static void put(int y, int h)
{
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (y + h > win_h)
        h = win_h - y;
    if (h > 0)
        XPutImage(dpy, win, gc, image, 0, y, 0, y, (unsigned)win_w, (unsigned)h);
}

static void draw(void)
{
    if (!fb)
        return;
    int first = -1, last = -1;
    for (int r = 0; r < rows; r++) {
        if (!all_dirty && !dirty[r])
            continue;
        draw_row(r);
        dirty[r] = 0;
        if (first < 0)
            first = r;
        last = r;
    }
    if (all_dirty || exposed)
        put(0, win_h);
    else if (first >= 0)
        put(PAD + first * cell_h, (last - first + 1) * cell_h);
    all_dirty = exposed = 0;
    XFlush(dpy);
}

/* ---- the screen's callbacks ---- */

static int on_damage(VTermRect r, void *user)
{
    (void)user;
    if (scroll_off) { /* the view shows older lines too; redraw it all */
        all_dirty = 1;
        return 1;
    }
    for (int row = r.start_row; row < r.end_row; row++)
        mark_row(row);
    return 1;
}

static int on_movecursor(VTermPos pos, VTermPos old, int visible, void *user)
{
    (void)user;
    (void)visible;
    mark_row(old.row);
    mark_row(pos.row);
    cursor = pos;
    return 1;
}

static char title_buf[512];
static size_t title_len;

static void set_title(const char *title)
{
    size_t n = strlen(title);
    XChangeProperty(dpy, win, a_net_wm_name, a_utf8, 8, PropModeReplace,
                    (const unsigned char *)title, (int)n);
    /* WM_NAME is Latin-1 text: send the ASCII of it. */
    char plain[512];
    size_t m = 0;
    for (size_t i = 0; i < n && m + 1 < sizeof plain; i++)
        plain[m++] = (unsigned char)title[i] >= 0x20 && (unsigned char)title[i] < 0x7f ? title[i]
                                                                                    : '?';
    XChangeProperty(dpy, win, XA_WM_NAME, XA_STRING, 8, PropModeReplace,
                    (const unsigned char *)plain, (int)m);
}

static int on_settermprop(VTermProp prop, VTermValue *val, void *user)
{
    (void)user;
    switch (prop) {
    case VTERM_PROP_CURSORVISIBLE:
        cursor_visible = val->boolean;
        mark_row(cursor.row);
        break;
    case VTERM_PROP_CURSORSHAPE:
        cursor_shape = val->number;
        mark_row(cursor.row);
        break;
    case VTERM_PROP_ALTSCREEN:
        altscreen = val->boolean;
        scroll_off = 0;
        sel_clear();
        all_dirty = 1;
        break;
    case VTERM_PROP_MOUSE:
        mouse_mode = val->number;
        break;
    case VTERM_PROP_FOCUSREPORT:
        focus_report = val->boolean;
        break;
    case VTERM_PROP_REVERSE:
        all_dirty = 1;
        break;
    case VTERM_PROP_TITLE:
        if (val->string.initial)
            title_len = 0;
        for (size_t i = 0; i < val->string.len && title_len + 1 < sizeof title_buf; i++)
            title_buf[title_len++] = val->string.str[i];
        if (val->string.final) {
            title_buf[title_len] = 0;
            set_title(title_buf);
        }
        break;
    default:
        break;
    }
    return 1;
}

static int on_bell(void *user)
{
    (void)user;
    XBell(dpy, 0);
    return 1;
}

static const VTermScreenCallbacks screen_callbacks = {
    .damage = on_damage,
    .movecursor = on_movecursor,
    .settermprop = on_settermprop,
    .bell = on_bell,
    .sb_pushline = sb_push,
    .sb_popline = sb_pop,
    .sb_clear = sb_clear,
};

/* ---- window size ---- */

static void set_hints(int with_position, int px, int py)
{
    XSizeHints h = {0};
    h.flags = PResizeInc | PBaseSize | PMinSize | PSize;
    h.width_inc = cell_w;
    h.height_inc = cell_h;
    h.base_width = 2 * PAD;
    h.base_height = 2 * PAD;
    h.min_width = 2 * PAD + 2 * cell_w;
    h.min_height = 2 * PAD + cell_h;
    h.width = 2 * PAD + cols * cell_w;
    h.height = 2 * PAD + rows * cell_h;
    if (with_position) {
        h.flags |= USPosition;
        h.x = px;
        h.y = py;
    }
    XSetWMNormalHints(dpy, win, &h);
}

static void tell_child_size(void)
{
    if (pty < 0)
        return;
    struct winsize ws = {(unsigned short)rows, (unsigned short)cols,
                         (unsigned short)(cols * cell_w), (unsigned short)(rows * cell_h)};
    ioctl(pty, TIOCSWINSZ, &ws);
}

/* The window is w by h pixels now: fit the grid to it. */
static void fit(int w, int h)
{
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    if (w != win_w || h != win_h || !fb) {
        if (image) {
            image->data = NULL;
            XDestroyImage(image);
        }
        free(fb);
        win_w = w;
        win_h = h;
        fb = xmalloc((size_t)w * (size_t)h * sizeof fb[0]);
        image = XCreateImage(dpy, visual, (unsigned)depth, ZPixmap, 0, (char *)fb, (unsigned)w,
                             (unsigned)h, 32, 0);
        if (!image)
            die("the X server's image format cannot be used");
        const uint16_t probe = 1;
        image->byte_order = *(const uint8_t *)&probe ? LSBFirst : MSBFirst;
    }
    fill(0, 0, win_w, win_h, default_bg);
    int nc = (w - 2 * PAD) / cell_w, nr = (h - 2 * PAD) / cell_h;
    if (nc < 1)
        nc = 1;
    if (nr < 1)
        nr = 1;
    if (nr > dirty_len) {
        dirty = xrealloc(dirty, (size_t)nr);
        dirty_len = nr;
    }
    if (nc != cols || nr != rows || !vt) {
        cols = nc;
        rows = nr;
        sel_clear();
        if (vt) {
            vterm_set_size(vt, rows, cols);
            vterm_screen_flush_damage(vts);
        }
        tell_child_size();
        if (scroll_off > sb_len)
            scroll_off = sb_len;
    }
    all_dirty = 1;
}

static void set_cell_width(int w)
{
    w = clampi(w, MIN_CELL_W, MAX_CELL_W);
    if (w == cell_w)
        return;
    int keep_cols = cols, keep_rows = rows;
    cell_w = w;
    cell_h = 2 * w;
    set_hints(0, 0, 0);
    /* Ask for the same grid at the new size; fit to what there is until
     * the answer comes. */
    XResizeWindow(dpy, win, (unsigned)(2 * PAD + keep_cols * cell_w),
                  (unsigned)(2 * PAD + keep_rows * cell_h));
    fit(win_w, win_h);
    tell_child_size();
}

/* ---- paste and copy ---- */

static void paste_bytes(const unsigned char *data, size_t len, int first, int last)
{
    if (first)
        vterm_keyboard_start_paste(vt);
    char chunk[4096];
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char b = data[i];
        if (b == '\n')
            b = '\r';
        else if ((b < 0x20 && b != '\t' && b != '\r') || b == 0x7f)
            continue; /* no control characters from a paste */
        chunk[n++] = (char)b;
        if (n == sizeof chunk) {
            to_child(chunk, n);
            n = 0;
        }
    }
    to_child(chunk, n);
    if (last)
        vterm_keyboard_end_paste(vt);
}

static int incr_active;

static void request_paste(Atom selection, Time when)
{
    if (selection == XA_PRIMARY && XGetSelectionOwner(dpy, XA_PRIMARY) == win && primary_text) {
        paste_bytes((const unsigned char *)primary_text, strlen(primary_text), 1, 1);
        return;
    }
    if (selection == a_clipboard && XGetSelectionOwner(dpy, a_clipboard) == win &&
        clipboard_text) {
        paste_bytes((const unsigned char *)clipboard_text, strlen(clipboard_text), 1, 1);
        return;
    }
    XConvertSelection(dpy, selection, a_utf8, a_paste_prop, win, when);
}

/* Read what a selection owner left in our property. */
static void take_property(int from_incr)
{
    Atom type;
    int format;
    unsigned long items, left;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, win, a_paste_prop, 0, 0x1fffffff, True, AnyPropertyType, &type,
                           &format, &items, &left, &data) != Success)
        return;
    if (type == a_incr) {
        incr_active = 1; /* the text follows piece by piece */
        vterm_keyboard_start_paste(vt);
    } else if (format == 8) {
        if (from_incr) {
            if (items == 0) {
                incr_active = 0;
                vterm_keyboard_end_paste(vt);
            } else {
                paste_bytes(data, items, 0, 0);
            }
        } else {
            paste_bytes(data, items, 1, 1);
        }
    }
    if (data)
        XFree(data);
}

static void own(Atom selection, char **store, Time when)
{
    if (!sel.active)
        return;
    char *text = sel_text();
    if (!text[0]) {
        free(text);
        return;
    }
    free(*store);
    *store = text;
    XSetSelectionOwner(dpy, selection, win, when);
}

static void answer_request(XSelectionRequestEvent *rq)
{
    XSelectionEvent reply = {0};
    reply.type = SelectionNotify;
    reply.requestor = rq->requestor;
    reply.selection = rq->selection;
    reply.target = rq->target;
    reply.time = rq->time;
    reply.property = None;
    const char *text = rq->selection == XA_PRIMARY ? primary_text
                       : rq->selection == a_clipboard ? clipboard_text
                                                      : NULL;
    Atom prop = rq->property != None ? rq->property : rq->target;
    if (text && rq->target == a_targets) {
        Atom list[] = {a_targets, a_utf8, XA_STRING, a_text};
        XChangeProperty(dpy, rq->requestor, prop, XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)list, (int)(sizeof list / sizeof list[0]));
        reply.property = prop;
    } else if (text && (rq->target == a_utf8 || rq->target == XA_STRING || rq->target == a_text)) {
        XChangeProperty(dpy, rq->requestor, prop, rq->target == XA_STRING ? XA_STRING : a_utf8, 8,
                        PropModeReplace, (const unsigned char *)text, (int)strlen(text));
        reply.property = prop;
    }
    XSendEvent(dpy, rq->requestor, False, 0, (XEvent *)&reply);
}

/* ---- keys ---- */

static VTermModifier modifiers(unsigned state)
{
    VTermModifier m = VTERM_MOD_NONE;
    if (state & ShiftMask)
        m |= VTERM_MOD_SHIFT;
    if (state & ControlMask)
        m |= VTERM_MOD_CTRL;
    if (state & Mod1Mask)
        m |= VTERM_MOD_ALT;
    return m;
}

static VTermKey special_key(KeySym k)
{
    switch (k) {
    case XK_Return: return VTERM_KEY_ENTER;
    case XK_KP_Enter: return VTERM_KEY_KP_ENTER;
    case XK_Tab:
    case XK_ISO_Left_Tab: return VTERM_KEY_TAB;
    case XK_BackSpace: return VTERM_KEY_BACKSPACE;
    case XK_Escape: return VTERM_KEY_ESCAPE;
    case XK_Up:
    case XK_KP_Up: return VTERM_KEY_UP;
    case XK_Down:
    case XK_KP_Down: return VTERM_KEY_DOWN;
    case XK_Left:
    case XK_KP_Left: return VTERM_KEY_LEFT;
    case XK_Right:
    case XK_KP_Right: return VTERM_KEY_RIGHT;
    case XK_Insert:
    case XK_KP_Insert: return VTERM_KEY_INS;
    case XK_Delete:
    case XK_KP_Delete: return VTERM_KEY_DEL;
    case XK_Home:
    case XK_KP_Home: return VTERM_KEY_HOME;
    case XK_End:
    case XK_KP_End: return VTERM_KEY_END;
    case XK_Prior:
    case XK_KP_Prior: return VTERM_KEY_PAGEUP;
    case XK_Next:
    case XK_KP_Next: return VTERM_KEY_PAGEDOWN;
    default: break;
    }
    if (k >= XK_F1 && k <= XK_F35)
        return (VTermKey)VTERM_KEY_FUNCTION((int)(k - XK_F1) + 1);
    return VTERM_KEY_NONE;
}

/* The character a keysym stands for, or 0. Latin-1 keysyms are their own
 * code points; Unicode keysyms carry theirs. Keysyms of the older national
 * sets (Cyrillic_*, Greek_*, ...) are not translated yet. */
static uint32_t keysym_char(KeySym k)
{
    if ((k >= 0x20 && k <= 0x7e) || (k >= 0xa0 && k <= 0xff))
        return (uint32_t)k;
    if ((k & 0xff000000UL) == 0x01000000UL && (k & 0xffffffUL) <= 0x10ffffUL)
        return (uint32_t)(k & 0xffffffUL);
    switch (k) {
    case XK_KP_Space: return ' ';
    case XK_KP_Multiply: return '*';
    case XK_KP_Add: return '+';
    case XK_KP_Separator: return ',';
    case XK_KP_Subtract: return '-';
    case XK_KP_Decimal: return '.';
    case XK_KP_Divide: return '/';
    case XK_KP_Equal: return '=';
    case 0x20ac: return 0x20ac; /* EuroSign */
    default: break;
    }
    if (k >= XK_KP_0 && k <= XK_KP_9)
        return (uint32_t)('0' + (k - XK_KP_0));
    return 0;
}

static void on_key(XKeyEvent *ev)
{
    char buf[32];
    KeySym k = NoSymbol;
    int n = XLookupString(ev, buf, sizeof buf, &k, NULL);
    unsigned st = ev->state;
    int shift = !!(st & ShiftMask), ctrl = !!(st & ControlMask), alt = !!(st & Mod1Mask);

    /* keys the terminal keeps for itself */
    if (shift && !ctrl && (k == XK_Prior || k == XK_KP_Prior)) {
        scroll_view(rows > 1 ? rows - 1 : 1);
        return;
    }
    if (shift && !ctrl && (k == XK_Next || k == XK_KP_Next)) {
        scroll_view(-(rows > 1 ? rows - 1 : 1));
        return;
    }
    if (shift && !ctrl && (k == XK_Insert || k == XK_KP_Insert)) {
        request_paste(XA_PRIMARY, ev->time);
        return;
    }
    if (ctrl && shift && (k == XK_V || k == XK_v)) {
        request_paste(a_clipboard, ev->time);
        return;
    }
    if (ctrl && shift && (k == XK_C || k == XK_c)) {
        own(a_clipboard, &clipboard_text, ev->time);
        return;
    }
    if (ctrl && (k == XK_plus || k == XK_equal || k == XK_KP_Add)) {
        set_cell_width(cell_w + 1);
        return;
    }
    if (ctrl && (k == XK_minus || k == XK_KP_Subtract)) {
        set_cell_width(cell_w - 1);
        return;
    }
    if (IsModifierKey(k) || k == XK_Num_Lock || k == XK_ISO_Level3_Shift || k == NoSymbol)
        return;

    /* everything else goes to the child, with the view back at the end */
    if (scroll_off) {
        scroll_off = 0;
        all_dirty = 1;
    }
    sel_clear();

    VTermKey sk = special_key(k);
    if (sk != VTERM_KEY_NONE) {
        VTermModifier m = modifiers(st);
        if (k == XK_ISO_Left_Tab)
            m |= VTERM_MOD_SHIFT;
        if (sk == VTERM_KEY_KP_ENTER)
            sk = VTERM_KEY_ENTER;
        if (sk == VTERM_KEY_BACKSPACE && ctrl && !alt) {
            to_child("\x08", 1); /* Ctrl+Backspace, as xterm sends it */
            return;
        }
        if ((sk == VTERM_KEY_ENTER || sk == VTERM_KEY_BACKSPACE || sk == VTERM_KEY_ESCAPE) &&
            (m & ~VTERM_MOD_ALT)) /* plain bytes; shells do not read the long forms */
            m &= VTERM_MOD_ALT;
        vterm_keyboard_key(vt, sk, m);
        return;
    }
    if (ctrl) {
        /* XLookupString has made the control byte (Ctrl+C is 3, Ctrl+Space is 0). */
        if (n == 1 && ((unsigned char)buf[0] < 0x20 || buf[0] == 0x7f)) {
            if (alt)
                to_child("\x1b", 1);
            to_child(buf, 1);
            return;
        }
    }
    uint32_t c = keysym_char(k);
    if (!c && n == 1)
        c = (unsigned char)buf[0];
    if (!c)
        return;
    char utf8[5];
    size_t len = utf8_put(c, utf8);
    if (alt)
        to_child("\x1b", 1);
    to_child(utf8, len);
}

/* ---- mouse ---- */

static int last_click_button, click_count;
static Time last_click_time;

static void cell_of(int x, int y, int *row, int *col)
{
    *col = clampi((x - PAD) / cell_w, 0, cols - 1);
    *row = clampi((y - PAD) / cell_h, 0, rows - 1);
}

static void sel_start(int row, int col, int mode)
{
    int line = top_line() + row;
    sel.mode = mode;
    sel.a_line = sel.b_line = line;
    sel.a_col = sel.b_col = sel.a_end_col = col;
    if (mode == 1) {
        if (is_word_cell(line, col)) {
            while (sel.a_col > 0 && is_word_cell(line, sel.a_col - 1))
                sel.a_col--;
            while (sel.a_end_col < cols - 1 && is_word_cell(line, sel.a_end_col + 1))
                sel.a_end_col++;
        }
        sel.b_col = sel.a_end_col;
    } else if (mode == 2) {
        sel.a_col = 0;
        sel.a_end_col = sel.b_col = cols - 1;
    }
    sel.active = mode != 0; /* a plain click selects nothing until it moves */
    sel.dragging = 1;
    all_dirty = 1;
}

static void sel_extend(int row, int col)
{
    int line = top_line() + row;
    if (sel.mode == 1 && is_word_cell(line, col)) {
        int forward = line > sel.a_line || (line == sel.a_line && col >= sel.a_col);
        if (forward)
            while (col < cols - 1 && is_word_cell(line, col + 1))
                col++;
        else
            while (col > 0 && is_word_cell(line, col - 1))
                col--;
    }
    if (line == sel.b_line && col == sel.b_col && sel.active)
        return;
    if (line != sel.a_line || col != sel.a_col)
        sel.active = 1;
    sel.b_line = line;
    sel.b_col = col;
    all_dirty = 1;
}

static void on_button(XButtonEvent *ev, int pressed)
{
    int row, col;
    cell_of(ev->x, ev->y, &row, &col);
    int shift = !!(ev->state & ShiftMask);

    if (mouse_mode != VTERM_PROP_MOUSE_NONE && !shift) {
        VTermModifier m = modifiers(ev->state & ~(unsigned)ShiftMask);
        vterm_mouse_move(vt, row, col, m);
        if (ev->button >= 1 && ev->button <= 3)
            vterm_mouse_button(vt, (int)ev->button, pressed, m);
        else if ((ev->button == 4 || ev->button == 5) && pressed)
            vterm_mouse_button(vt, (int)ev->button, 1, m);
        return;
    }
    if (ev->button == 4 || ev->button == 5) {
        if (!pressed)
            return;
        if (altscreen) { /* a pager or editor: send arrow keys */
            for (int i = 0; i < 3; i++)
                vterm_keyboard_key(vt, ev->button == 4 ? VTERM_KEY_UP : VTERM_KEY_DOWN,
                                   VTERM_MOD_NONE);
        } else {
            scroll_view(ev->button == 4 ? 3 : -3);
        }
        return;
    }
    if (ev->button == 1) {
        if (pressed) {
            if (last_click_button == 1 && ev->time - last_click_time < 400)
                click_count = click_count % 3 + 1;
            else
                click_count = 1;
            last_click_button = 1;
            last_click_time = ev->time;
            sel_clear();
            sel_start(row, col, click_count - 1);
        } else if (sel.dragging) {
            sel.dragging = 0;
            if (sel.active)
                own(XA_PRIMARY, &primary_text, ev->time);
        }
        return;
    }
    if (ev->button == 2 && pressed)
        request_paste(XA_PRIMARY, ev->time);
    if (ev->button == 3 && pressed && sel.active) { /* extend, as xterm does */
        sel_extend(row, col);
        own(XA_PRIMARY, &primary_text, ev->time);
    }
}

static void on_motion(XMotionEvent *ev)
{
    int row, col;
    cell_of(ev->x, ev->y, &row, &col);
    if (sel.dragging && (ev->state & Button1Mask)) {
        sel_extend(row, col);
        return;
    }
    if (mouse_mode != VTERM_PROP_MOUSE_NONE && !(ev->state & ShiftMask))
        vterm_mouse_move(vt, row, col, modifiers(ev->state));
}

/* ---- X events ---- */

static void on_event(XEvent *ev)
{
    switch (ev->type) {
    case Expose:
        exposed = 1;
        break;
    case ConfigureNotify:
        if (ev->xconfigure.width != win_w || ev->xconfigure.height != win_h)
            fit(ev->xconfigure.width, ev->xconfigure.height);
        break;
    case KeyPress:
        on_key(&ev->xkey);
        break;
    case ButtonPress:
        on_button(&ev->xbutton, 1);
        break;
    case ButtonRelease:
        on_button(&ev->xbutton, 0);
        break;
    case MotionNotify:
        on_motion(&ev->xmotion);
        break;
    case FocusIn:
    case FocusOut:
        if (ev->xfocus.mode == NotifyGrab || ev->xfocus.mode == NotifyUngrab)
            break;
        focused = ev->type == FocusIn;
        mark_row(cursor.row);
        if (focus_report) {
            if (focused)
                vterm_state_focus_in(vtstate);
            else
                vterm_state_focus_out(vtstate);
        }
        break;
    case MappingNotify:
        XRefreshKeyboardMapping(&ev->xmapping);
        break;
    case SelectionRequest:
        answer_request(&ev->xselectionrequest);
        break;
    case SelectionClear:
        if (ev->xselectionclear.selection == XA_PRIMARY)
            sel_clear();
        break;
    case SelectionNotify:
        if (ev->xselection.property != None)
            take_property(0);
        break;
    case PropertyNotify:
        if (incr_active && ev->xproperty.atom == a_paste_prop &&
            ev->xproperty.state == PropertyNewValue)
            take_property(1);
        break;
    case ClientMessage:
        if (ev->xclient.message_type == a_wm_protocols &&
            (Atom)ev->xclient.data.l[0] == a_wm_delete)
            exit(0); /* closing the pty hangs the child up */
        break;
    default:
        break;
    }
}

/* ---- the child ---- */

static const char *dump_path; /* -dump-on-usr1: where SIGUSR1 writes the view as text */

static void on_signal(int sig)
{
    int saved = errno;
    if (wake_pipe[1] >= 0) {
        ssize_t r = write(wake_pipe[1], sig == SIGUSR1 ? "d" : "c", 1);
        (void)r;
    }
    errno = saved;
}

/* The view as text, for tests: one header line, then the rows. */
static void dump_view(void)
{
    FILE *f = dump_path ? fopen(dump_path, "w") : NULL;
    if (!f)
        return;
    fprintf(f, "cols=%d rows=%d cell=%dx%d scroll=%d scrollback=%d cursor=%d,%d selection=%d "
               "masks=%lu\n",
            cols, rows, cell_w, cell_h, scroll_off, sb_len, cursor.row, cursor.col, sel.active,
            glyphs_drawn());
    for (int row = 0; row < rows; row++) {
        char text[4 * VTERM_MAX_CHARS_PER_CELL + 1];
        int last = -1;
        VTermScreenCell c;
        for (int col = 0; col < cols; col++) {
            cell_at(top_line() + row, col, &c);
            if (c.chars[0] != 0 && c.chars[0] != ' ' && c.chars[0] != (uint32_t)-1)
                last = col;
        }
        for (int col = 0; col <= last; col++) {
            cell_at(top_line() + row, col, &c);
            if (c.chars[0] == (uint32_t)-1)
                continue;
            size_t n = 0;
            if (c.chars[0] == 0)
                text[n++] = ' ';
            for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && c.chars[i]; i++)
                n += utf8_put(c.chars[i], text + n);
            fwrite(text, 1, n, f);
        }
        fputc('\n', f);
    }
    fclose(f);
}

static void start_child(char **command, const char *term, int login)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0)
        die("cannot open a pty: %s", strerror(errno));
    char slave_name[128];
    if (ptsname_r(master, slave_name, sizeof slave_name) != 0)
        die("cannot name the pty: %s", strerror(errno));
    struct winsize ws = {(unsigned short)rows, (unsigned short)cols,
                         (unsigned short)(cols * cell_w), (unsigned short)(rows * cell_h)};
    ioctl(master, TIOCSWINSZ, &ws);

    if (pipe(wake_pipe) < 0)
        die("pipe: %s", strerror(errno));
    fcntl(wake_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(wake_pipe[1], F_SETFL, O_NONBLOCK);
    fcntl(wake_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(wake_pipe[1], F_SETFD, FD_CLOEXEC);
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
    if (dump_path)
        sigaction(SIGUSR1, &sa, NULL);

    child = fork();
    if (child < 0)
        die("fork: %s", strerror(errno));
    if (child == 0) {
        setsid();
        int slave = open(slave_name, O_RDWR);
        if (slave < 0)
            _exit(126);
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2)
            close(slave);
        close(master);
        close(ConnectionNumber(dpy));
        /* Whatever started the terminal may have ignored or blocked
         * signals (a script's background job has SIGINT ignored). The
         * child gets them all back, or Ctrl+C would do nothing. */
        static const int sigs[] = {SIGCHLD, SIGUSR1, SIGUSR2, SIGPIPE, SIGINT,  SIGQUIT, SIGTERM,
                                   SIGHUP,  SIGALRM, SIGTSTP, SIGTTIN, SIGTTOU, SIGCONT, SIGWINCH};
        for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
            signal(sigs[i], SIG_DFL);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        setenv("TERM", term, 1);
        setenv("COLORTERM", "truecolor", 1);
        unsetenv("COLUMNS");
        unsetenv("LINES");
        unsetenv("TERMCAP");
        char id[32];
        snprintf(id, sizeof id, "%lu", (unsigned long)win);
        setenv("WINDOWID", id, 1);
        if (command && command[0]) {
            execvp(command[0], command);
            fprintf(stderr, "neuroterm: cannot run %s: %s\r\n", command[0], strerror(errno));
        }
        const char *shell = getenv("SHELL");
        if (!shell || !shell[0]) {
            struct passwd *pw = getpwuid(getuid());
            shell = pw && pw->pw_shell && pw->pw_shell[0] ? pw->pw_shell : "/bin/sh";
        }
        for (int attempt = 0; attempt < 2; attempt++) {
            const char *base = strrchr(shell, '/');
            base = base ? base + 1 : shell;
            char arg0[128];
            snprintf(arg0, sizeof arg0, "%s%s", login ? "-" : "", base);
            execl(shell, arg0, (char *)NULL);
            fprintf(stderr, "neuroterm: cannot run %s: %s\r\n", shell, strerror(errno));
            shell = "/bin/sh"; /* a broken $SHELL must not stop a rescue */
        }
        _exit(127);
    }
    fcntl(master, F_SETFL, O_NONBLOCK);
    fcntl(master, F_SETFD, FD_CLOEXEC);
    pty = master;
}

/* Feed what the child wrote to the screen. Returns 0 at end of file. */
static int read_child(void)
{
    static char buf[65536];
    long start = now_ms();
    size_t total = 0;
    int alive = 1;
    for (;;) {
        ssize_t n = read(pty, buf, sizeof buf);
        if (n > 0) {
            vterm_input_write(vt, buf, (size_t)n);
            total += (size_t)n;
            if (total > (1u << 20) || now_ms() - start > 12)
                break;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            break;
        alive = 0; /* EIO: every copy of the other end is closed */
        break;
    }
    vterm_screen_flush_damage(vts);
    return alive;
}

static void write_child(void)
{
    while (out_len) {
        ssize_t n = write(pty, out_buf, out_len);
        if (n <= 0)
            break;
        memmove(out_buf, out_buf + n, out_len - (size_t)n);
        out_len -= (size_t)n;
    }
}

/* ---- start ---- */

static int parse_colour(const char *s, uint32_t *out)
{
    if (s[0] == '#' && strlen(s) == 7) {
        char *end;
        unsigned long v = strtoul(s + 1, &end, 16);
        if (!*end) {
            *out = (uint32_t)v;
            return 1;
        }
    }
    if (strcmp(s, "black") == 0) {
        *out = 0x000000;
        return 1;
    }
    if (strcmp(s, "white") == 0) {
        *out = 0xffffff;
        return 1;
    }
    return 0;
}

static void usage(FILE *out)
{
    fputs("usage: neuroterm [options] [-e command args...]\n"
          "  -e command args...   run the command instead of the shell (must be last)\n"
          "  -T, -title text      window title\n"
          "  -geometry COLSxROWS[+X+Y]\n"
          "  -fs points           text size; the cell is 0.8 x 1.6 of it in pixels (default 12)\n"
          "  -fg, -bg #rrggbb     text and background colour\n"
          "  -rv                  swap them\n"
          "  -sl lines            scrollback lines (default 10000)\n"
          "  -hold                keep the window after the command ends\n"
          "  -ls                  start the shell as a login shell\n"
          "  -tn name             TERM for the child (default xterm-256color)\n"
          "  -class, -name text   window class and instance\n"
          "  -display name        X display\n"
          "  -no-disk-weights     use only the weight files built into the program\n"
          "  -list-weights        print the weight files in use and exit\n"
          "  -which U+XXXX...     say which weight file draws each character, and exit\n"
          "  -dump-on-usr1 file   on SIGUSR1 write the visible text to the file (for tests)\n"
          "  -version, -help\n"
          "Other xterm options are accepted and ignored.\n"
          "Keys: Shift+PageUp/PageDown scroll back; Shift+Insert or the middle button pastes;\n"
          "Ctrl+Shift+C/V copy and paste the clipboard; Ctrl+plus/minus change the text size.\n",
          out);
}

/* xterm options that take a value and mean nothing here */
static int ignored_with_value(const char *o)
{
    static const char *const list[] = {"-fn", "-fa", "-fb", "-fw", "-fx", "-fd", "-fi", "-xrm",
                                       "-bd", "-bw", "-b",  "-cr", "-ms", "-hc", "-kt", "-ti",
                                       "-tm", "-lf", "-lc", "-mc", "-nb", "-en", "-n",  "-selbg",
                                       "-selfg", "-into", "-w", NULL};
    for (int i = 0; list[i]; i++)
        if (strcmp(o, list[i]) == 0)
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
    const char *display_name = NULL, *title = NULL, *term = "xterm-256color";
    const char *res_class = NULL, *res_name = NULL;
    char **command = NULL;
    int use_disk = 1, list_weights = 0, login = 0, reverse = 0, which_from = 0;
    int have_pos = 0, pos_x = 0, pos_y = 0;
    double points = 12.0;

    const char *prog = strrchr(argv[0], '/');
    prog = prog ? prog + 1 : argv[0];

    for (int i = 1; i < argc; i++) {
        const char *o = argv[i];
#define VALUE() (i + 1 < argc ? argv[++i] : (die("%s needs a value", o), ""))
        if (strcmp(o, "-e") == 0) {
            if (i + 1 >= argc)
                die("-e needs a command");
            command = &argv[i + 1];
            break;
        } else if (strcmp(o, "-T") == 0 || strcmp(o, "-title") == 0) {
            title = VALUE();
        } else if (strcmp(o, "-geometry") == 0 || strcmp(o, "-g") == 0) {
            const char *g = VALUE();
            unsigned gw = 0, gh = 0;
            int gx = 0, gy = 0;
            int mask = XParseGeometry(g, &gx, &gy, &gw, &gh);
            if ((mask & WidthValue) && gw >= 1 && gw <= 1000)
                cols = (int)gw;
            if ((mask & HeightValue) && gh >= 1 && gh <= 1000)
                rows = (int)gh;
            if ((mask & XValue) && (mask & YValue) && !(mask & (XNegative | YNegative))) {
                have_pos = 1;
                pos_x = gx;
                pos_y = gy;
            }
        } else if (strcmp(o, "-fs") == 0) {
            points = atof(VALUE());
        } else if (strcmp(o, "-fg") == 0) {
            const char *v = VALUE();
            if (!parse_colour(v, &default_fg))
                fprintf(stderr, "neuroterm: colour %s not understood (use #rrggbb)\n", v);
        } else if (strcmp(o, "-bg") == 0) {
            const char *v = VALUE();
            if (!parse_colour(v, &default_bg))
                fprintf(stderr, "neuroterm: colour %s not understood (use #rrggbb)\n", v);
        } else if (strcmp(o, "-rv") == 0) {
            reverse = 1;
        } else if (strcmp(o, "-sl") == 0) {
            sb_cap = clampi(atoi(VALUE()), 0, 1000000);
        } else if (strcmp(o, "-hold") == 0) {
            hold = 1;
        } else if (strcmp(o, "-ls") == 0) {
            login = 1;
        } else if (strcmp(o, "-tn") == 0) {
            term = VALUE();
        } else if (strcmp(o, "-class") == 0) {
            res_class = VALUE();
        } else if (strcmp(o, "-name") == 0) {
            res_name = VALUE();
        } else if (strcmp(o, "-display") == 0 || strcmp(o, "-d") == 0) {
            display_name = VALUE();
        } else if (strcmp(o, "-no-disk-weights") == 0) {
            use_disk = 0;
        } else if (strcmp(o, "-list-weights") == 0) {
            list_weights = 1;
        } else if (strcmp(o, "-which") == 0) {
            which_from = i + 1;
            break;
        } else if (strcmp(o, "-dump-on-usr1") == 0) {
            dump_path = VALUE();
        } else if (strcmp(o, "-version") == 0 || strcmp(o, "-v") == 0) {
            puts("neuroterm " VERSION);
            return 0;
        } else if (strcmp(o, "-help") == 0 || strcmp(o, "--help") == 0 || strcmp(o, "-h") == 0) {
            usage(stdout);
            return 0;
        } else if (ignored_with_value(o)) {
            (void)VALUE();
        } else if (o[0] != '-' && o[0] != '+') {
            fprintf(stderr, "neuroterm: %s ignored\n", o);
        }
        /* any other -option or +option: an xterm switch with no meaning here */
#undef VALUE
    }
    if (reverse) {
        uint32_t t = default_fg;
        default_fg = default_bg;
        default_bg = t;
    }

    char err[256];
    if (glyphs_init(use_disk, err, sizeof err) != 0)
        die("%s", err);
    if (list_weights) {
        glyphs_list(stdout);
        return 0;
    }
    if (which_from) {
        for (int i = which_from; i < argc; i++) {
            const char *v = argv[i];
            if ((v[0] == 'U' || v[0] == 'u') && v[1] == '+')
                v += 2;
            uint32_t scalar = (uint32_t)strtoul(v, NULL, 16);
            const char *file = glyphs_file(scalar);
            printf("U+%04X\t%s\n", (unsigned)scalar, file ? file : "no drawing: the mark");
        }
        return 0;
    }

    /* The cell: 0.6 em wide at 96 dots per inch, twice as high. */
    if (points < 3 || points > 96)
        points = 12;
    cell_w = clampi((int)(0.6 * points * 96.0 / 72.0 + 0.5), MIN_CELL_W, MAX_CELL_W);
    cell_h = 2 * cell_w;

    dpy = XOpenDisplay(display_name);
    if (!dpy)
        die("cannot open the X display %s", display_name ? display_name : getenv("DISPLAY")
                                                              ? getenv("DISPLAY")
                                                              : "(DISPLAY is not set)");
    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    visual = DefaultVisual(dpy, screen);
    depth = DefaultDepth(dpy, screen);
    Colormap cmap = DefaultColormap(dpy, screen);
    if (visual->class != TrueColor || visual->red_mask != 0xff0000 ||
        visual->green_mask != 0x00ff00 || visual->blue_mask != 0x0000ff) {
        XVisualInfo vi;
        if (!XMatchVisualInfo(dpy, screen, 24, TrueColor, &vi) || vi.red_mask != 0xff0000 ||
            vi.green_mask != 0x00ff00 || vi.blue_mask != 0x0000ff)
            die("the X display has no 24-bit true-colour visual");
        visual = vi.visual;
        depth = vi.depth;
        cmap = XCreateColormap(dpy, root, visual, AllocNone);
    }

    XSetWindowAttributes wa = {0};
    wa.background_pixel = 0xff000000u | default_bg;
    wa.border_pixel = 0;
    wa.bit_gravity = NorthWestGravity;
    wa.colormap = cmap;
    wa.event_mask = KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                    ExposureMask | StructureNotifyMask | FocusChangeMask | PropertyChangeMask;
    int w = 2 * PAD + cols * cell_w, h = 2 * PAD + rows * cell_h;
    win = XCreateWindow(dpy, root, pos_x, pos_y, (unsigned)w, (unsigned)h, 0, depth, InputOutput,
                        visual, CWBackPixel | CWBorderPixel | CWBitGravity | CWColormap |
                                    CWEventMask,
                        &wa);
    gc = XCreateGC(dpy, win, 0, NULL);

    a_wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
    a_wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    a_net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    a_net_wm_pid = XInternAtom(dpy, "_NET_WM_PID", False);
    a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    a_targets = XInternAtom(dpy, "TARGETS", False);
    a_clipboard = XInternAtom(dpy, "CLIPBOARD", False);
    a_incr = XInternAtom(dpy, "INCR", False);
    a_text = XInternAtom(dpy, "TEXT", False);
    a_paste_prop = XInternAtom(dpy, "_NEUROTERM_PASTE", False);
    XSetWMProtocols(dpy, win, &a_wm_delete, 1);

    /* Started under the name xterm or uxterm, look like it to window rules. */
    char name_buf[64];
    snprintf(name_buf, sizeof name_buf, "%s", res_name ? res_name : prog);
    XClassHint ch = {name_buf, (char *)(res_class                        ? res_class
                                        : strcmp(prog, "xterm") == 0   ? "XTerm"
                                        : strcmp(prog, "uxterm") == 0  ? "UXTerm"
                                                                       : "Neuroterm")};
    XSetClassHint(dpy, win, &ch);
    XWMHints wm = {0};
    wm.flags = InputHint;
    wm.input = True;
    XSetWMHints(dpy, win, &wm);
    long pid = (long)getpid();
    XChangeProperty(dpy, win, a_net_wm_pid, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&pid, 1);
    set_hints(have_pos, pos_x, pos_y);
    set_title(title ? title : command ? command[0] : prog);

    if (sb_cap > 0)
        sb = xmalloc((size_t)sb_cap * sizeof sb[0]);

    vt = vterm_new(rows, cols);
    if (!vt)
        die("out of memory");
    vterm_set_utf8(vt, 1);
    vterm_output_set_callback(vt, vt_output, NULL);
    vtstate = vterm_obtain_state(vt);
    vts = vterm_obtain_screen(vt);
    vterm_screen_set_callbacks(vts, &screen_callbacks, NULL);
    vterm_screen_enable_altscreen(vts, 1);
    vterm_screen_set_damage_merge(vts, VTERM_DAMAGE_SCROLL);
    VTermColor vfg, vbg;
    vterm_color_rgb(&vfg, (uint8_t)(default_fg >> 16), (uint8_t)(default_fg >> 8),
                    (uint8_t)default_fg);
    vterm_color_rgb(&vbg, (uint8_t)(default_bg >> 16), (uint8_t)(default_bg >> 8),
                    (uint8_t)default_bg);
    vterm_screen_set_default_colors(vts, &vfg, &vbg);
    vterm_screen_reset(vts, 1);
    /* The usual dark blue cannot be read on a dark background, and it is
     * the colour ls gives directories. */
    VTermColor blue;
    vterm_color_rgb(&blue, 0x5c, 0x8c, 0xff);
    vterm_state_set_palette_color(vtstate, 4, &blue);
    vterm_state_get_default_colors(vtstate, &blank_cell.fg, &blank_cell.bg);
    blank_cell.width = 1;

    fit(w, h);
    start_child(command, term, login);

    XMapWindow(dpy, win);
    /* With no window manager nobody hands out the keyboard: take it, so
     * typing works wherever the pointer is. */
    {
        Atom check = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False), type;
        int format;
        unsigned long items = 0, left;
        unsigned char *data = NULL;
        int has_wm = XGetWindowProperty(dpy, root, check, 0, 1, False, XA_WINDOW, &type, &format,
                                        &items, &left, &data) == Success &&
                     items == 1;
        if (data)
            XFree(data);
        if (!has_wm) {
            XEvent ev;
            XWindowEvent(dpy, win, StructureNotifyMask, &ev); /* wait until it is viewable */
            while (ev.type != MapNotify)
                XWindowEvent(dpy, win, StructureNotifyMask, &ev);
            XSetInputFocus(dpy, win, RevertToPointerRoot, CurrentTime);
        }
    }

    int xfd = ConnectionNumber(dpy);
    for (;;) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            on_event(&ev);
        }
        if (pty >= 0)
            write_child();
        draw();

        struct pollfd fds[3] = {{xfd, POLLIN, 0},
                                {wake_pipe[0], POLLIN, 0},
                                {pty, (short)(POLLIN | (out_len ? POLLOUT : 0)), 0}};
        int nfds = pty >= 0 ? 3 : 2;
        if (XPending(dpy))
            continue;
        if (poll(fds, (nfds_t)nfds, -1) < 0 && errno != EINTR)
            die("poll: %s", strerror(errno));

        int ended = 0;
        if (pty >= 0 && (fds[2].revents & (POLLIN | POLLHUP | POLLERR)))
            ended = !read_child();
        if (fds[1].revents & POLLIN) {
            char drain[16];
            ssize_t got;
            int want_dump = 0;
            while ((got = read(wake_pipe[0], drain, sizeof drain)) > 0)
                for (ssize_t i = 0; i < got; i++)
                    want_dump |= drain[i] == 'd';
            if (want_dump)
                dump_view();
            int status;
            if (child > 0 && waitpid(child, &status, WNOHANG) == child) {
                child_gone = 1;
                while (pty >= 0 && read_child()) { /* show its last words */
                    struct pollfd p = {pty, POLLIN, 0};
                    if (poll(&p, 1, 20) <= 0)
                        break;
                }
            }
        }
        if (child_gone || ended) {
            if (!hold)
                break;
            if (pty >= 0) {
                close(pty);
                pty = -1;
                out_len = 0;
            }
        }
    }
    /* xterm exits 0 whatever its child returned; callers rely on that. */
    return 0;
}
