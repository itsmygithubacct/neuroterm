/* The terminal's only glyph source: Neuronte coverage masks.
 *
 * MIT licence. See LICENSE.
 */
#ifndef NEUROTERM_GLYPHS_H
#define NEUROTERM_GLYPHS_H

#include <stdint.h>
#include <stdio.h>

enum { GLYPH_BOLD = 1u, GLYPH_ITALIC = 2u };

/* Open the weight files: the ones built into the program, then, when
 * use_disk is not zero, files of other names found in a weights directory.
 * Returns 0, or -1 with a message in err when no file with the base
 * characters opens. */
int glyphs_init(int use_disk, char *err, size_t err_len);

/* One line per opened file: its name and where it came from. */
void glyphs_list(FILE *out);

/* 1 when some file was trained on the scalar, 0 when it gets the mark. */
int glyphs_trained(uint32_t scalar);

/* The name of the file that draws the scalar, or NULL when it gets the mark. */
const char *glyphs_file(uint32_t scalar);

/* The coverage of one character, 0..255 per pixel, row-major, span * cell_w
 * wide and cell_h high, where span is 2 for a character two cells wide and 1
 * otherwise. A character no file was trained on gets Neuronte's own mark,
 * one mark across the whole span. The mask belongs to the cache; it is
 * valid until the next call. NULL when the cell cannot be drawn. */
const uint8_t *glyphs_mask(uint32_t scalar, unsigned style, int wide, int cell_w, int cell_h);

/* How many masks the nets have drawn since the start. */
unsigned long glyphs_drawn(void);

#endif
