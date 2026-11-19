#include "neuroterm_terminal.h"

#include <assert.h>
#include <string.h>

typedef struct trace {
    unsigned fills, glyphs, style;
    int width;
    uint32_t first, last;
} trace;

static void fill(void *user, int x, int y, int w, int h, uint32_t rgb)
{
    (void)x; (void)y; (void)w; (void)h; (void)rgb;
    ((trace *)user)->fills++;
}

static void glyph(void *user, uint32_t scalar, unsigned style, int x, int y,
                  int w, int h, uint32_t rgb)
{
    (void)x; (void)y; (void)h; (void)rgb;
    trace *out = user;
    if (!out->glyphs) out->first = scalar;
    out->last = scalar;
    out->glyphs++;
    out->style = style;
    out->width = w;
}

int main(void)
{
    neuroterm term;
    VTermScreenCell cell;
    trace painted = {0};
    neuroterm_cell_style style = {
        .foreground = NEUROTERM_FG, .background = NEUROTERM_BG, .focused = true,
    };
    neuroterm_painter painter = {&painted, fill, glyph};
    const char *input = "\033[1;3m\033[48:2::9:8:7m\xe4\xb8\xad\xcc\x81\xef\xb8\x8f";
    if (!neuroterm_init(&term, 12, 4)) return 1;
    neuroterm_feed(&term, input, strlen(input));
    assert(neuroterm_cell(&term, 0, 0, &cell));
    assert(cell.width == 2 && neuroterm_color(&term, cell.bg) == 0x090807);
    neuroterm_paint_cell(term.screen, &cell, 0, 0, 8, 16, cell.width, &style, &painter);
    assert(painted.fills == 1 && painted.glyphs == 2 && painted.width == 16);
    assert(painted.first == 0x4e2d && painted.last == 0x301);
    assert(painted.style == (NEUROTERM_BOLD | NEUROTERM_ITALIC));

    neuroterm_feed(&term, "\033[5 q", 5);
    assert(term.cursor_shape == VTERM_PROP_CURSORSHAPE_BAR_LEFT);
    neuroterm_free(&term);
    return 0;
}
