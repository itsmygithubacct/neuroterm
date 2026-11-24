/* libvterm integration based on NeuroTerm's terminal setup, MIT.
 * Copyright (c) 2026 itsmygithubacct */
#include "neuroterm_terminal.h"

#include <string.h>

static int set_term_prop(VTermProp prop, VTermValue *value, void *user)
{
    neuroterm *t = user;
    if (prop == VTERM_PROP_CURSORVISIBLE) t->cursor_visible = value->boolean;
    if (prop == VTERM_PROP_CURSORSHAPE) t->cursor_shape = value->number;
    return 1;
}

VTerm *neuroterm_create(int cols, int rows, uint32_t fg, uint32_t bg,
                        const VTermScreenCallbacks *callbacks, void *user)
{
    VTermColor foreground, background, blue;
    VTerm *term = vterm_new(rows, cols);
    if (!term) return NULL;
    VTermScreen *screen = vterm_obtain_screen(term);
    if (!screen) {
        vterm_free(term);
        return NULL;
    }
    vterm_set_utf8(term, 1);
    vterm_screen_set_callbacks(screen, callbacks, user);
    vterm_screen_enable_altscreen(screen, 1);
    vterm_screen_set_damage_merge(screen, VTERM_DAMAGE_SCROLL);
    vterm_color_rgb(&foreground, (fg >> 16) & 255, (fg >> 8) & 255, fg & 255);
    vterm_color_rgb(&background, (bg >> 16) & 255, (bg >> 8) & 255, bg & 255);
    vterm_screen_set_default_colors(screen, &foreground, &background);
    vterm_screen_reset(screen, 1);
    vterm_color_rgb(&blue, 0x5c, 0x8c, 0xff);
    vterm_state_set_palette_color(vterm_obtain_state(term), 4, &blue);
    return term;
}

bool neuroterm_init(neuroterm *t, int cols, int rows)
{
    static const VTermScreenCallbacks callbacks = {.settermprop = set_term_prop};
    memset(t, 0, sizeof *t);
    t->cursor_visible = true;
    t->cursor_shape = VTERM_PROP_CURSORSHAPE_BLOCK;
    t->term = neuroterm_create(cols, rows, NEUROTERM_FG, NEUROTERM_BG, &callbacks, t);
    if (!t->term) return false;
    t->screen = vterm_obtain_screen(t->term);
    t->cols = cols;
    t->rows = rows;
    return true;
}

void neuroterm_free(neuroterm *t)
{
    if (t->term) vterm_free(t->term);
    memset(t, 0, sizeof *t);
}

bool neuroterm_resize(neuroterm *t, int cols, int rows)
{
    if (cols <= 0 || rows <= 0) return false;
    if (cols == t->cols && rows == t->rows) return true;
    vterm_set_size(t->term, rows, cols);
    vterm_screen_flush_damage(t->screen);
    vterm_get_size(t->term, &t->rows, &t->cols);
    return t->rows == rows && t->cols == cols;
}

void neuroterm_feed(neuroterm *t, const void *bytes, size_t len)
{
    vterm_input_write(t->term, bytes, len);
    vterm_screen_flush_damage(t->screen);
}

size_t neuroterm_read(neuroterm *t, char *buf, size_t cap)
{
    return vterm_output_read(t->term, buf, cap);
}

VTermPos neuroterm_cursor(const neuroterm *t)
{
    VTermPos cursor;
    vterm_state_get_cursorpos(vterm_obtain_state(t->term), &cursor);
    return cursor;
}

bool neuroterm_cell(const neuroterm *t, int col, int row, VTermScreenCell *cell)
{
    return vterm_screen_get_cell(t->screen, (VTermPos){.row = row, .col = col}, cell) != 0;
}

static uint32_t rgb_color(VTermScreen *screen, VTermColor color)
{
    vterm_screen_convert_color_to_rgb(screen, &color);
    return (uint32_t)color.rgb.red << 16 | (uint32_t)color.rgb.green << 8 | color.rgb.blue;
}

uint32_t neuroterm_color(const neuroterm *t, VTermColor color)
{
    return rgb_color(t->screen, color);
}

size_t neuroterm_row_text(const neuroterm *t, int row, char *buf, size_t cap)
{
    if (!cap) return 0;
    VTermRect rect = {row, row + 1, 0, t->cols};
    size_t n = vterm_screen_get_text(t->screen, buf, cap - 1, rect);
    buf[n] = 0;
    return n;
}

#include "unicode_tables.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* Default-ignorable code points and the interlinear annotation controls
 * have no glyph: joiners, selectors, bidi controls, tags and the like. */
static bool ignorable(uint32_t scalar)
{
    size_t lo = 0, hi = COUNT(ignorable_ranges);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (scalar < ignorable_ranges[mid].first) hi = mid;
        else if (scalar > ignorable_ranges[mid].last) lo = mid + 1;
        else return true;
    }
    return false;
}

static bool drawable(uint32_t scalar)
{
    return scalar > 0x20 && scalar != 0xa0 && scalar != UINT32_MAX && !ignorable(scalar);
}

static bool blank(uint32_t scalar)
{
    return scalar == 0x20 || scalar == 0xa0;
}

static int combining_class(uint32_t scalar)
{
    size_t lo = 0, hi = COUNT(combining_classes);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (scalar < combining_classes[mid].first) hi = mid;
        else if (scalar > combining_classes[mid].last) lo = mid + 1;
        else return combining_classes[mid].ccc;
    }
    return 0;
}

/* The primary composite of a and b, or 0. Hangul syllables arithmetically. */
static uint32_t compose_pair(uint32_t a, uint32_t b)
{
    if (a >= 0x1100 && a < 0x1113 && b >= 0x1161 && b < 0x1176)
        return 0xac00 + ((a - 0x1100) * 21 + (b - 0x1161)) * 28;
    if (a >= 0xac00 && a <= 0xd7a3 && (a - 0xac00) % 28 == 0 && b > 0x11a7 && b < 0x11c3)
        return a + (b - 0x11a7);
    size_t lo = 0, hi = COUNT(compose_pairs);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (a < compose_pairs[mid].first || (a == compose_pairs[mid].first && b < compose_pairs[mid].second))
            hi = mid;
        else if (a > compose_pairs[mid].first || b > compose_pairs[mid].second)
            lo = mid + 1;
        else
            return compose_pairs[mid].composite;
    }
    return 0;
}

/* The scalars to draw for a cell: its own, or its base composed with the
 * marks it can take (canonical composition: a mark is blocked by an
 * earlier mark left over with a class of 0 or one at least its own) when
 * the painter can draw the composite. Returns the count. */
static int cell_scalars(const VTermScreenCell *cell, const neuroterm_painter *painter,
                        uint32_t out[VTERM_MAX_CHARS_PER_CELL])
{
    int n = 0;
    while (n < VTERM_MAX_CHARS_PER_CELL && cell->chars[n]) {
        out[n] = cell->chars[n];
        n++;
    }
    if (n < 2 || !painter->can_draw || combining_class(out[0]) != 0)
        return n;
    uint32_t composed[VTERM_MAX_CHARS_PER_CELL];
    uint32_t starter = out[0];
    int kept = 1, last = -1;
    for (int i = 1; i < n; i++) {
        int ccc = combining_class(out[i]);
        uint32_t c = last == 0 || (last > 0 && last >= ccc) ? 0 : compose_pair(starter, out[i]);
        if (c) {
            starter = c;
        } else {
            composed[kept++] = out[i];
            last = ccc;
        }
    }
    if (starter == out[0] || !painter->can_draw(painter->user, starter))
        return n;
    composed[0] = starter;
    for (int i = 0; i < kept; i++)
        out[i] = composed[i];
    return kept;
}

void neuroterm_paint_cell(VTermScreen *screen, const VTermScreenCell *cell,
                         int x, int y, int cell_w, int cell_h, int span,
                         const neuroterm_cell_style *style, const neuroterm_painter *painter)
{
    uint32_t fg = VTERM_COLOR_IS_DEFAULT_FG(&cell->fg) ? style->foreground : rgb_color(screen, cell->fg);
    uint32_t bg = VTERM_COLOR_IS_DEFAULT_BG(&cell->bg) ? style->background : rgb_color(screen, cell->bg);
    int width = span * cell_w;
    int line = cell_h >= 28 ? 2 : 1;
    if (cell->attrs.reverse != style->selected) {
        uint32_t swap = fg;
        fg = bg;
        bg = swap;
    }
    if (style->selected && fg == bg) fg = ~bg & 0xffffffu;
    if (style->cursor && style->focused && style->cursor_shape == VTERM_PROP_CURSORSHAPE_BLOCK) {
        fg = style->background;
        bg = style->foreground;
    }
    if (cell->attrs.conceal) fg = bg;
    if (style->paint_default_background || bg != style->background)
        painter->fill(painter->user, x, y, width, cell_h, bg);

    /* Marks over a space or no-break space are drawn alone. */
    if (fg != bg && (drawable(cell->chars[0]) || blank(cell->chars[0]))) {
        unsigned flags = (cell->attrs.bold ? NEUROTERM_BOLD : 0u) |
                         (cell->attrs.italic ? NEUROTERM_ITALIC : 0u);
        uint32_t scalars[VTERM_MAX_CHARS_PER_CELL];
        int n = cell_scalars(cell, painter, scalars);
        for (int i = 0; i < n; i++)
            if (drawable(scalars[i]))
                painter->glyph(painter->user, scalars[i], flags, x, y, width, cell_h, fg);
    }
    if (cell->attrs.underline) {
        int uy = (3 * cell_h) / 4 + line + 1;
        if (uy + line > cell_h) uy = cell_h - line;
        painter->fill(painter->user, x, y + uy, width, line, fg);
        if (cell->attrs.underline == VTERM_UNDERLINE_DOUBLE && uy - 2 * line > 0)
            painter->fill(painter->user, x, y + uy - 2 * line, width, line, fg);
    }
    if (cell->attrs.strike)
        painter->fill(painter->user, x, y + (11 * cell_h) / 20, width, line, fg);

    if (!style->cursor) return;
    uint32_t cursor = style->foreground;
    if (!style->focused) {
        painter->fill(painter->user, x, y, width, 1, cursor);
        painter->fill(painter->user, x, y + cell_h - 1, width, 1, cursor);
        painter->fill(painter->user, x, y, 1, cell_h, cursor);
        painter->fill(painter->user, x + width - 1, y, 1, cell_h, cursor);
    } else if (style->cursor_shape == VTERM_PROP_CURSORSHAPE_UNDERLINE) {
        painter->fill(painter->user, x, y + cell_h - 2, width, 2, cursor);
    } else if (style->cursor_shape == VTERM_PROP_CURSORSHAPE_BAR_LEFT) {
        painter->fill(painter->user, x, y, 2, cell_h, cursor);
    }
}
