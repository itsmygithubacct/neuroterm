/* In-canvas NeuroTerm terminal core. The host supplies input, PTY transport,
 * and the renderer. See third_party/libvterm/UPSTREAM for parser provenance. */
#ifndef NEUROTERM_TERMINAL_H
#define NEUROTERM_TERMINAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vterm.h>

#define NEUROTERM_FG 0xd0d4dcu
#define NEUROTERM_BG 0x101318u

typedef struct neuroterm {
    int cols, rows;
    VTerm *term;
    VTermScreen *screen;
    bool cursor_visible;
    int cursor_shape;
} neuroterm;

/* Configure the common terminal and palette. The returned handle is owned
 * by the caller, and the callback table must outlive it. */
VTerm *neuroterm_create(int cols, int rows, uint32_t fg, uint32_t bg,
                        const VTermScreenCallbacks *callbacks, void *user);
bool neuroterm_init(neuroterm *t, int cols, int rows);
void neuroterm_free(neuroterm *t);
bool neuroterm_resize(neuroterm *t, int cols, int rows);
void neuroterm_feed(neuroterm *t, const void *bytes, size_t len);
size_t neuroterm_read(neuroterm *t, char *buf, size_t cap);
VTermPos neuroterm_cursor(const neuroterm *t);
bool neuroterm_cell(const neuroterm *t, int col, int row, VTermScreenCell *cell);
uint32_t neuroterm_color(const neuroterm *t, VTermColor color);
size_t neuroterm_row_text(const neuroterm *t, int row, char *buf, size_t cap);

enum { NEUROTERM_BOLD = 1u, NEUROTERM_ITALIC = 2u };

/* can_draw, if set, says whether the glyph backend has a drawing of its own
 * for a scalar. A cell's base and its combining marks are then composed, as
 * Unicode canonical composition does, into the precomposed character when
 * the backend can draw it; the marks left over are drawn over it. Without
 * can_draw, or when the composite cannot be drawn, the base and each mark
 * are drawn as received. The cell's text is never changed. */
typedef struct neuroterm_painter {
    void *user;
    void (*fill)(void *user, int x, int y, int w, int h, uint32_t rgb);
    void (*glyph)(void *user, uint32_t scalar, unsigned style, int x, int y,
                  int w, int h, uint32_t rgb);
    bool (*can_draw)(void *user, uint32_t scalar);
} neuroterm_painter;

typedef struct neuroterm_cell_style {
    uint32_t foreground, background;
    bool selected, cursor, focused;
    int cursor_shape;
    bool paint_default_background;
} neuroterm_cell_style;

/* Paint one cell or a two-column lead cell. The host supplies the backing
 * canvas and its glyph backend. Draw a blank separately for an orphan tail. */
void neuroterm_paint_cell(VTermScreen *screen, const VTermScreenCell *cell,
                         int x, int y, int cell_w, int cell_h, int span,
                         const neuroterm_cell_style *style, const neuroterm_painter *painter);

#endif
