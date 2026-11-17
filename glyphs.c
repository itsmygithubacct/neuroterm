/* The terminal's only glyph source: Neuronte coverage masks.
 *
 * Files, in the order they are asked:
 *   the newest neuronte-split-vN.nrt, which holds the base characters and
 *   draws the mark for anything no file was trained on;
 *   then every neuronte-term-*.nrt in name order.
 * A character goes to the first file whose neuronte_untrained() is 0.
 *
 * Every file the build was given is inside the program, so nothing has to
 * be found at run time. A weights directory may add files of other names
 * (a newer split file, more term bundles). It never replaces a built-in
 * file, and a file there that does not open is skipped.
 *
 *   NEUROTERM_WEIGHTS   a directory of .nrt files
 *   NEURONTE_HOME       a Neuronte tree; its weights/ is used
 *   otherwise           ~/.local/gpu_terminal/sources/neuronte/weights,
 *                       then /usr/share/neuronte/weights
 *
 * Coverage comes from neuronte_coverage_styled(), never from drawn pixels.
 *
 * MIT licence. See LICENSE.
 */
#define _DEFAULT_SOURCE
#include "glyphs.h"

#include "neuronte.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>

struct embedded {
    const char *name;
    const unsigned char *start, *end;
};
extern const struct embedded neuroterm_embedded[];

#define MAX_FILES 48
#define NAME_LEN 128

struct source {
    char name[NAME_LEN];
    char where[512];
    const struct embedded *builtin; /* NULL for a file on disk */
    neuronte *handle;
};

static struct source files[MAX_FILES];
static int file_count;
static struct source *opened[MAX_FILES];
static int opened_count;
static unsigned long drawn;

static int has_suffix(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a > b && strcmp(s + a - b, suffix) == 0;
}

/* The N of neuronte-split-vN.nrt, or -1. */
static long split_version(const char *name)
{
    static const char prefix[] = "neuronte-split-v";
    if (strncmp(name, prefix, sizeof prefix - 1) != 0)
        return -1;
    char *end;
    long v = strtol(name + sizeof prefix - 1, &end, 10);
    if (end == name + sizeof prefix - 1 || strcmp(end, ".nrt") != 0 || v < 0)
        return -1;
    return v;
}

static int is_term(const char *name)
{
    return strncmp(name, "neuronte-term-", 14) == 0 && has_suffix(name, ".nrt");
}

static int known(const char *name)
{
    for (int i = 0; i < file_count; i++)
        if (strcmp(files[i].name, name) == 0)
            return 1;
    return 0;
}

static void add(const char *name, const char *where, const struct embedded *builtin)
{
    if (file_count >= MAX_FILES || strlen(name) >= NAME_LEN || known(name))
        return;
    if (split_version(name) < 0 && !is_term(name))
        return;
    struct source *s = &files[file_count++];
    snprintf(s->name, sizeof s->name, "%s", name);
    snprintf(s->where, sizeof s->where, "%s", where);
    s->builtin = builtin;
    s->handle = NULL;
}

static int scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    char path[512];
    while ((e = readdir(d)) != NULL) {
        if (!has_suffix(e->d_name, ".nrt"))
            continue;
        if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, e->d_name) >= sizeof path)
            continue;
        add(e->d_name, path, NULL);
    }
    closedir(d);
    return 1;
}

static void scan_disk(void)
{
    char dir[400];
    const char *v = getenv("NEUROTERM_WEIGHTS");
    if (v && v[0]) {
        scan_dir(v);
        return;
    }
    v = getenv("NEURONTE_HOME");
    if (v && v[0] && (size_t)snprintf(dir, sizeof dir, "%s/weights", v) < sizeof dir) {
        scan_dir(dir);
        return;
    }
    v = getenv("HOME");
    if (v && v[0] &&
        (size_t)snprintf(dir, sizeof dir, "%s/.local/gpu_terminal/sources/neuronte/weights", v) <
            sizeof dir &&
        scan_dir(dir))
        return;
    scan_dir("/usr/share/neuronte/weights");
}

static int open_source(struct source *s)
{
    int rc;
    if (s->builtin)
        rc = neuronte_open(s->builtin->start, (size_t)(s->builtin->end - s->builtin->start),
                           &s->handle);
    else
        rc = neuronte_open_file(s->where, &s->handle);
    if (rc != NEURONTE_OK || neuronte_nets(s->handle) < 1) {
        if (rc == NEURONTE_OK)
            neuronte_close(s->handle);
        s->handle = NULL;
        return -1;
    }
    return 0;
}

static int by_name(const void *a, const void *b)
{
    const struct source *x = *(struct source *const *)a, *y = *(struct source *const *)b;
    return strcmp(x->name, y->name);
}

int glyphs_init(int use_disk, char *err, size_t err_len)
{
    for (const struct embedded *e = neuroterm_embedded; e->name; e++)
        add(e->name, "built in", e);
    if (use_disk)
        scan_disk();

    /* The base file: the newest split file that opens. */
    for (;;) {
        struct source *best = NULL;
        for (int i = 0; i < file_count; i++) {
            long v = split_version(files[i].name);
            if (v >= 0 && !files[i].where[0])
                continue; /* tried, did not open */
            if (v >= 0 && (!best || v > split_version(best->name)))
                best = &files[i];
        }
        if (!best) {
            snprintf(err, err_len, "no neuronte-split-vN.nrt weight file opens: the program was "
                                   "built without weights and none was found on disk");
            return -1;
        }
        if (open_source(best) == 0 && neuronte_set_untrained(best->handle, 1) == NEURONTE_OK) {
            opened[opened_count++] = best;
            break;
        }
        best->where[0] = 0;
    }

    struct source *terms[MAX_FILES];
    int n = 0;
    for (int i = 0; i < file_count; i++)
        if (is_term(files[i].name))
            terms[n++] = &files[i];
    qsort(terms, (size_t)n, sizeof terms[0], by_name);
    for (int i = 0; i < n; i++)
        if (open_source(terms[i]) == 0)
            opened[opened_count++] = terms[i];
        else
            fprintf(stderr, "neuroterm: weight file %s (%s) does not open; skipped\n",
                    terms[i]->name, terms[i]->where);
    return 0;
}

void glyphs_list(FILE *out)
{
    for (int i = 0; i < opened_count; i++)
        fprintf(out, "%s\t%s\n", opened[i]->name, opened[i]->where);
}

static const neuronte *source_for(uint32_t scalar, int *trained)
{
    for (int i = 0; i < opened_count; i++)
        if (neuronte_untrained(opened[i]->handle, scalar) == 0) {
            *trained = 1;
            return opened[i]->handle;
        }
    *trained = 0;
    return opened[0]->handle; /* draws the mark */
}

int glyphs_trained(uint32_t scalar)
{
    int trained = 0;
    if (opened_count)
        source_for(scalar, &trained);
    return trained;
}

const char *glyphs_file(uint32_t scalar)
{
    int trained = 0;
    if (!opened_count)
        return NULL;
    const neuronte *n = source_for(scalar > 0x10ffffu ? NEURONTE_REPLACEMENT : scalar, &trained);
    if (!trained)
        return NULL;
    for (int i = 0; i < opened_count; i++)
        if (opened[i]->handle == n)
            return opened[i]->name;
    return NULL;
}

unsigned long glyphs_drawn(void)
{
    return drawn;
}

/* The cache: open addressing, emptied whole when it fills or the cell size
 * changes. A session shows a few hundred different cells. */
#define SLOTS 8192
#define SLOTS_FULL 6000

static struct {
    uint32_t key; /* scalar | style << 21 | wide << 23, plus 1; 0 is empty */
    uint8_t *mask;
} cache[SLOTS];
static int cache_used, cache_w, cache_h;
static float *scratch;
static size_t scratch_len;

static void cache_empty(void)
{
    for (int i = 0; i < SLOTS; i++) {
        free(cache[i].mask);
        cache[i].mask = NULL;
        cache[i].key = 0;
    }
    cache_used = 0;
}

static uint8_t to_byte(float v)
{
    return v <= 0.f ? 0 : v >= 1.f ? 255 : (uint8_t)(v * 255.f + 0.5f);
}

const uint8_t *glyphs_mask(uint32_t scalar, unsigned style, int wide, int cell_w, int cell_h)
{
    if (!opened_count || cell_w < 1 || cell_h < 1 || cell_w > 512 || cell_h > 1024)
        return NULL;
    if (scalar > 0x10ffffu)
        scalar = NEURONTE_REPLACEMENT;
    if (cell_w != cache_w || cell_h != cache_h || cache_used >= SLOTS_FULL) {
        cache_empty();
        cache_w = cell_w;
        cache_h = cell_h;
    }
    style &= GLYPH_BOLD | GLYPH_ITALIC;
    uint32_t key = (scalar | (uint32_t)style << 21 | (wide ? 1u << 23 : 0u)) + 1u;
    unsigned slot = (key * 2654435761u) >> 19;
    while (cache[slot].key && cache[slot].key != key)
        slot = (slot + 1) & (SLOTS - 1);
    if (cache[slot].key)
        return cache[slot].mask;

    int span = wide ? 2 : 1, out_w = span * cell_w;
    size_t area = (size_t)out_w * (size_t)cell_h;
    if (area > scratch_len) {
        float *grown = realloc(scratch, area * sizeof scratch[0]);
        if (!grown)
            return NULL;
        scratch = grown;
        scratch_len = area;
    }
    uint8_t *mask = calloc(area, 1);
    if (!mask)
        return NULL;

    int trained = 0;
    const neuronte *n = source_for(scalar, &trained);
    unsigned nstyle = (style & GLYPH_BOLD ? NEURONTE_STYLE_BOLD : 0u) |
                      (style & GLYPH_ITALIC ? NEURONTE_STYLE_ITALIC : 0u);
    int rc;
    if (wide && trained) {
        /* A drawing is one cell wide: centre it in the two cells. */
        rc = neuronte_coverage_styled(n, scalar, cell_w, cell_h, 0, nstyle, scratch);
        if (rc == NEURONTE_OK) {
            int left = cell_w / 2;
            for (int y = 0; y < cell_h; y++)
                for (int x = 0; x < cell_w; x++)
                    mask[(size_t)y * out_w + left + x] = to_byte(scratch[(size_t)y * cell_w + x]);
        }
    } else {
        /* One cell, or one mark across both cells of a wide character. */
        rc = neuronte_coverage_styled(n, scalar, out_w, cell_h, 0, trained ? nstyle : 0u, scratch);
        if (rc == NEURONTE_OK)
            for (size_t i = 0; i < area; i++)
                mask[i] = to_byte(scratch[i]);
    }
    if (rc != NEURONTE_OK) {
        free(mask);
        return NULL;
    }
    drawn++;
    cache[slot].key = key;
    cache[slot].mask = mask;
    cache_used++;
    return mask;
}
