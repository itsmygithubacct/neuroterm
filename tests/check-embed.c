#include "neuroterm_terminal.h"

#include <assert.h>
#include <stdlib.h>
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

static bool draw_all(void *user, uint32_t scalar)
{
    (void)user; (void)scalar;
    return true;
}

static bool draw_none(void *user, uint32_t scalar)
{
    (void)user; (void)scalar;
    return false;
}

/* Feed text on a fresh line and paint its first cell; the trace of it. */
static trace paint_text(neuroterm *term, const char *text, bool (*can_draw)(void *, uint32_t))
{
    trace out = {0};
    VTermScreenCell cell;
    neuroterm_cell_style style = {
        .foreground = NEUROTERM_FG, .background = NEUROTERM_BG, .focused = true,
    };
    neuroterm_painter painter = {&out, fill, glyph, can_draw};
    neuroterm_feed(term, "\033[H\033[2J", 7);
    neuroterm_feed(term, text, strlen(text));
    assert(neuroterm_cell(term, 0, 0, &cell));
    neuroterm_paint_cell(term->screen, &cell, 0, 0, 8, 16, cell.width, &style, &painter);
    return out;
}

static void check_marks(void)
{
    neuroterm term;
    trace t;
    if (!neuroterm_init(&term, 12, 4)) exit(1);
    /* e + acute: the precomposed letter where the painter draws it */
    t = paint_text(&term, "e\xcc\x81", draw_all);
    assert(t.glyphs == 1 && t.first == 0xe9);
    t = paint_text(&term, "e\xcc\x81", draw_none);
    assert(t.glyphs == 2 && t.first == 'e' && t.last == 0x301);
    t = paint_text(&term, "e\xcc\x81", NULL);
    assert(t.glyphs == 2 && t.first == 'e' && t.last == 0x301);
    /* two marks in canonical order: e, dot below, circumflex is U+1EC7 */
    t = paint_text(&term, "e\xcc\xa3\xcc\x82", draw_all);
    assert(t.glyphs == 1 && t.first == 0x1ec7);
    /* a second acute has no composite with a-acute and is drawn over it */
    t = paint_text(&term, "a\xcc\x81\xcc\x81", draw_all);
    assert(t.glyphs == 2 && t.first == 0xe1 && t.last == 0x301);
    /* Hangul jamo compose arithmetically */
    t = paint_text(&term, "\xe1\x84\x80\xe1\x85\xa1", draw_all);
    assert(t.glyphs == 1 && t.first == 0xac00);
    /* a mark over a space or no-break space is drawn alone */
    t = paint_text(&term, " \xcc\x81", draw_all);
    assert(t.glyphs == 1 && t.first == 0x301);
    t = paint_text(&term, "\xc2\xa0\xcc\x88", draw_none);
    assert(t.glyphs == 1 && t.first == 0x308);
    /* invisible characters draw nothing */
    t = paint_text(&term, "a\xef\xbf\xb9", draw_all);
    assert(t.glyphs == 1 && t.first == 'a');
    t = paint_text(&term, "a\xcd\x8f\xe2\x80\x8b", draw_all);
    assert(t.glyphs == 1 && t.first == 'a');
    /* the cell keeps the text as received */
    {
        char text[16];
        const char *clear_e_acute = "\033[H\033[2Je\xcc\x81";
        neuroterm_feed(&term, clear_e_acute, strlen(clear_e_acute));
        size_t n = neuroterm_row_text(&term, 0, text, sizeof text);
        assert(n == 3 && memcmp(text, "e\xcc\x81", 3) == 0);
    }
    neuroterm_free(&term);
}

int main(void)
{
    neuroterm term;
    VTermScreenCell cell;
    trace painted = {0};
    neuroterm_cell_style style = {
        .foreground = NEUROTERM_FG, .background = NEUROTERM_BG, .focused = true,
    };
    neuroterm_painter painter = {&painted, fill, glyph, NULL};
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
    check_marks();
    return 0;
}
