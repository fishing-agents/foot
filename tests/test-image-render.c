#define _POSIX_C_SOURCE 200809L
#include "../grid.h"
#include "../kitty.h"
#include "../sixel-render.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BACKGROUND 0xff102030u

/* Only text/font drawing, PTY scheduling and Sixel cache construction are
 * stubbed. Tests exercise the same Kitty/Sixel compositors as real Foot. */
bool term_to_slave(struct terminal *term, const void *data, size_t len)
{ (void)term; (void)data; (void)len; return true; }
void render_refresh(struct terminal *term) { (void)term; }
void term_cursor_to(struct terminal *t, int r, int c)
{ t->grid->cursor.point = (struct coord){.row = r, .col = c}; }
void term_cursor_right(struct terminal *t, int n) { t->grid->cursor.point.col += n; }
void term_cursor_down(struct terminal *t, int n) { t->grid->cursor.point.row += n; }
int grid_row_abs_to_sb(const struct grid *g, int rows, int abs)
{ return (abs - g->offset - rows + g->num_rows) & (g->num_rows - 1); }
void sixel_sync_cache(const struct terminal *term, struct sixel *six)
{ (void)term; (void)six; }

struct fixture {
    struct terminal term;
    uint32_t *pixels;
    pixman_image_t *pix;
    pixman_region32_t damage;
};
static bool overflowing_glyph;

static void clean(struct fixture *f)
{
    for (int r = 0; r < f->term.grid->num_rows; r++) {
        struct row *row = f->term.grid->rows[r];
        row->dirty = false;
        for (int c = 0; c < f->term.cols; c++) row->cells[c].attrs.clean = true;
    }
    pixman_region32_clear(&f->damage);
}
static void fill(struct fixture *f)
{
    for (int i = 0; i < f->term.width * f->term.height; i++) f->pixels[i] = BACKGROUND;
}
static void init(struct fixture *f, int rows, int cols, int cell_size)
{
    memset(f, 0, sizeof(*f));
    struct terminal *t = &f->term;
    t->rows = rows; t->cols = cols;
    t->cell_width = t->cell_height = cell_size;
    t->width = cols * cell_size; t->height = rows * cell_size;
    t->grid = &t->normal;
    t->normal.num_rows = 1;
    while (t->normal.num_rows < rows * 2) t->normal.num_rows *= 2;
    t->normal.num_cols = cols;
    t->normal.rows = calloc(t->normal.num_rows, sizeof(*t->normal.rows));
    assert(t->normal.rows);
    for (int r = 0; r < t->normal.num_rows; r++) {
        t->normal.rows[r] = calloc(1, sizeof(struct row));
        assert(t->normal.rows[r]);
        t->normal.rows[r]->cells = calloc(cols, sizeof(struct cell));
        assert(t->normal.rows[r]->cells);
    }
    f->pixels = malloc((size_t)t->width * t->height * sizeof(*f->pixels));
    assert(f->pixels);
    f->pix = pixman_image_create_bits(PIXMAN_a8r8g8b8, t->width, t->height,
                                    f->pixels, t->width * sizeof(*f->pixels));
    assert(f->pix);
    pixman_region32_init(&f->damage);
    fill(f); clean(f);
}
static void fini(struct fixture *f)
{
    kitty_fini(&f->term);
    tll_foreach(f->term.grid->sixel_images, it) {
        pixman_image_unref(it->item.pix);
        free(it->item.original.data);
        tll_remove(f->term.grid->sixel_images, it);
    }
    for (int r = 0; r < f->term.grid->num_rows; r++) {
        free(f->term.grid->rows[r]->cells); free(f->term.grid->rows[r]);
    }
    free(f->term.grid->rows);
    pixman_region32_fini(&f->damage);
    pixman_image_unref(f->pix); free(f->pixels);
}
static void dirty(struct fixture *f, int r, int c)
{
    struct row *row = grid_row_in_view(f->term.grid, r);
    row->dirty = true; row->cells[c].attrs.clean = false;
}
static int text_cell(struct terminal *t, pixman_image_t *pix,
                     pixman_region32_t *damage, struct row *row,
                     int r, int c, bool cursor)
{
    (void)cursor;
    const int cols = overflowing_glyph && c + 1 < t->cols ? 2 : 1;
    const pixman_color_t bg = {0x1010, 0x2020, 0x3030, 0xffff};
    const int x = t->margins.left + c * t->cell_width;
    const int y = t->margins.top + r * t->cell_height;
    pixman_image_fill_rectangles(PIXMAN_OP_SRC, pix, &bg, 1,
        &(pixman_rectangle16_t){x, y, cols * t->cell_width, t->cell_height});
    pixman_region32_union_rect(damage, damage, x, y, cols * t->cell_width, t->cell_height);
    for (int i = 0; i < cols; i++) row->cells[c + i].attrs.clean = true;
    return cols;
}
static void text_row(struct terminal *t, pixman_image_t *pix,
                     pixman_region32_t *damage, struct row *row, int r, int cursor)
{
    for (int c = 0; c < t->cols;) {
        if (row->cells[c].attrs.clean) { c++; continue; }
        c += text_cell(t, pix, damage, row, r, c, c == cursor);
    }
}
static const struct sixel_text_render text = {.row = text_row, .cell = text_cell};
static uint64_t sixels(struct fixture *f)
{
    return sixel_render_images(&f->term, f->pix, &f->damage,
                               &(const struct coord){-1, -1}, &text);
}
static void apc(struct fixture *f, const char *cmd)
{
    kitty_begin(&f->term);
    for (const char *p = cmd; *p; p++) kitty_put(&f->term, *p);
    kitty_end(&f->term);
}
static void add_sixel(struct fixture *f, int col, int width, int height,
                      bool opaque, uint32_t color)
{
    struct sixel six = {.pos = {.row = 0, .col = col}, .width = width, .height = height,
        .cols = (width + f->term.cell_width - 1) / f->term.cell_width,
        .rows = (height + f->term.cell_height - 1) / f->term.cell_height,
        .opaque = opaque};
    uint32_t *data = malloc((size_t)width * height * sizeof(*data));
    assert(data);
    for (int i = 0; i < width * height; i++) data[i] = color;
    six.original.data = data;
    six.pix = pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height, data, width * 4);
    assert(six.pix);
    tll_push_back(f->term.grid->sixel_images, six);
    for (int r = 0; r < six.rows; r++)
        for (int c = col; c < col + six.cols; c++) dirty(f, r, c);
}
static void kitty_tests(void)
{
    struct fixture f; init(&f, 8, 8, 1);
    apc(&f, "Ga=T,f=32,s=1,v=1,i=1,c=8,r=8,C=1;/wAAgA==");
    image_dirty_region(&f.term, &f.damage);
    assert(kitty_render_placements(&f.term, f.pix, &f.damage) == 64);
    const uint32_t expected = f.pixels[0];
    assert(expected != BACKGROUND);
    clean(&f);
    for (int n = 0; n < 100; n++) {
        assert(kitty_render_placements(&f.term, f.pix, &f.damage) == 0);
        for (int i = 0; i < 64; i++) assert(f.pixels[i] == expected);
    }
    dirty(&f, 3, 2);
    image_dirty_region(&f.term, &f.damage);
    f.pixels[3 * 8 + 2] = BACKGROUND;
    assert(kitty_render_placements(&f.term, f.pix, &f.damage) == 1);
    for (int i = 0; i < 64; i++) assert(f.pixels[i] == expected);

    /* Foot's full-screen scroll moves complete composited pixels. */
    clean(&f);
    memmove(f.pixels, f.pixels + 8, 56 * sizeof(*f.pixels));
    for (int i = 56; i < 64; i++) f.pixels[i] = BACKGROUND;
    f.term.grid->offset = f.term.grid->view = 1;
    pixman_region32_union_rect(&f.damage, &f.damage, 0, 7, 8, 1);
    assert(kitty_render_placements(&f.term, f.pix, &f.damage) == 0);
    for (int i = 0; i < 56; i++) assert(f.pixels[i] == expected);
    clean(&f);
    memmove(f.pixels + 8, f.pixels, 56 * sizeof(*f.pixels));
    for (int i = 0; i < 8; i++) f.pixels[i] = BACKGROUND;
    f.term.grid->offset = f.term.grid->view = 0;
    pixman_region32_union_rect(&f.damage, &f.damage, 0, 0, 8, 1);
    assert(kitty_render_placements(&f.term, f.pix, &f.damage) == 8);
    for (int i = 0; i < 64; i++) assert(f.pixels[i] == expected);

    /* Recycled buffers already contain the complete previous image. */
    uint32_t copy[64]; memcpy(copy, f.pixels, sizeof(copy));
    pixman_image_t *other = pixman_image_create_bits(PIXMAN_a8r8g8b8, 8, 8, copy, 32);
    assert(other); clean(&f);
    assert(kitty_render_placements(&f.term, other, &f.damage) == 0);
    assert(memcmp(copy, f.pixels, sizeof(copy)) == 0);
    pixman_image_unref(other);

    /* Add/delete invalidate only the affected cells, not entire text rows. */
    apc(&f, "Ga=d,d=A"); clean(&f);
    term_cursor_to(&f.term, 2, 3);
    apc(&f, "Ga=T,f=24,s=1,v=1,i=2,C=1;AP8A");
    image_dirty_region(&f.term, &f.damage);
    assert(pixman_region32_contains_rectangle(&f.damage,
        &(pixman_box32_t){3, 2, 4, 3}) == PIXMAN_REGION_IN);
    assert(!pixman_region32_contains_point(&f.damage, 0, 2, NULL));
    clean(&f); dirty(&f, 7, 7); image_dirty_region(&f.term, &f.damage);
    assert(kitty_render_placements(&f.term, f.pix, &f.damage) == 0);
    clean(&f); kitty_placements_invalidate(&f.term);
    image_dirty_region(&f.term, &f.damage);
    assert(!pixman_region32_contains_point(&f.damage, 0, 2, NULL));
    assert(pixman_region32_contains_point(&f.damage, 3, 2, NULL));
    fini(&f);
}
static void sixel_tests(void)
{
    struct fixture f; init(&f, 8, 8, 1);
    add_sixel(&f, 0, 4, 2, true, 0xff00cc40);
    assert(sixels(&f) == 8);
    clean(&f); assert(sixels(&f) == 0);
    dirty(&f, 0, 2); f.pixels[2] = BACKGROUND;
    assert(sixels(&f) == 1 && f.pixels[2] == 0xff00cc40);
    assert(!pixman_region32_contains_point(&f.damage, 1, 0, NULL));
    clean(&f); dirty(&f, 0, 7);
    assert(sixels(&f) == 0);
    fini(&f);

    /* Two transparent Sixels share a dirty row. The first text callback
     * cleans both cells, but the second image must still composite its cell. */
    init(&f, 8, 8, 1);
    add_sixel(&f, 0, 3, 2, false, 0x80800000);
    add_sixel(&f, 5, 3, 2, false, 0x80000080);
    assert(sixels(&f) == 12);
    uint32_t reference[64]; memcpy(reference, f.pixels, sizeof(reference));
    for (int n = 0; n < 100; n++) {
        clean(&f); assert(sixels(&f) == 0);
        assert(memcmp(reference, f.pixels, sizeof(reference)) == 0);
    }
    clean(&f); dirty(&f, 0, 1); dirty(&f, 0, 6);
    assert(sixels(&f) == 2);
    assert(memcmp(reference, f.pixels, sizeof(reference)) == 0);
    clean(&f); dirty(&f, 0, 1); overflowing_glyph = true;
    assert(sixels(&f) == 2); overflowing_glyph = false;
    assert(memcmp(reference, f.pixels, sizeof(reference)) == 0);
    fini(&f);

    /* Partial-cell image edges must restore the uncovered background too. */
    init(&f, 4, 4, 2); add_sixel(&f, 0, 3, 3, true, 0xff00cc40);
    assert(sixels(&f) == 9);
    assert(f.pixels[2] == 0xff00cc40 && f.pixels[3] == BACKGROUND);
    assert(f.pixels[2 * 8] == 0xff00cc40 && f.pixels[3 * 8] == BACKGROUND);
    fini(&f);

    /* A glyph drawn to erase an opaque partial-cell edge can repaint a
     * neighbouring Sixel too, even though that neighbour was initially clean. */
    init(&f, 4, 4, 2);
    add_sixel(&f, 0, 1, 2, true, 0xff00cc40);
    add_sixel(&f, 1, 2, 2, true, 0xff0040cc);
    assert(sixels(&f) == 6);
    uint32_t edge_reference[64]; memcpy(edge_reference, f.pixels, sizeof(edge_reference));
    clean(&f); dirty(&f, 0, 0); overflowing_glyph = true;
    assert(sixels(&f) == 6); overflowing_glyph = false;
    assert(memcmp(edge_reference, f.pixels, sizeof(edge_reference)) == 0);
    fini(&f);
}
static double elapsed(const struct timespec *a, const struct timespec *b)
{ return b->tv_sec - a->tv_sec + (b->tv_nsec - a->tv_nsec) / 1e9; }
static void workload(bool benchmark)
{
    struct fixture f; init(&f, 64, 64, 16);
    apc(&f, "Ga=T,f=32,s=1,v=1,i=1,c=64,r=64,C=1;/wAAgA==");
    image_dirty_region(&f.term, &f.damage);
    uint64_t full = kitty_render_placements(&f.term, f.pix, &f.damage);
    assert(full == (uint64_t)f.term.width * f.term.height);
    clean(&f); dirty(&f, 32, 32); image_dirty_region(&f.term, &f.damage);
    text_row(&f.term, f.pix, &f.damage, grid_row_in_view(f.term.grid, 32), 32, -1);
    uint64_t cell = kitty_render_placements(&f.term, f.pix, &f.damage);
    assert(cell == 256);
    printf("Kitty 1024x1024: full=%llu pixels, one cell=%llu, clean=0\n",
           (unsigned long long)full, (unsigned long long)cell);
    if (benchmark) {
        struct timespec a, b, c;
        clock_gettime(CLOCK_MONOTONIC, &a);
        for (int n = 0; n < 100; n++) {
            fill(&f); pixman_region32_clear(&f.damage);
            pixman_region32_union_rect(&f.damage, &f.damage, 0, 0, f.term.width, f.term.height);
            assert(kitty_render_placements(&f.term, f.pix, &f.damage) == full);
        }
        clock_gettime(CLOCK_MONOTONIC, &b);
        for (int n = 0; n < 100; n++) {
            clean(&f); dirty(&f, 32, 32); image_dirty_region(&f.term, &f.damage);
            text_row(&f.term, f.pix, &f.damage, grid_row_in_view(f.term.grid, 32), 32, -1);
            assert(kitty_render_placements(&f.term, f.pix, &f.damage) == cell);
        }
        clock_gettime(CLOCK_MONOTONIC, &c);
        printf("100 background+Kitty composite passes: full %.3f ms; incremental %.3f ms\n",
               elapsed(&a, &b) * 1000, elapsed(&b, &c) * 1000);
    }
    fini(&f);
    init(&f, 64, 64, 16); add_sixel(&f, 0, 1024, 1024, true, 0xff00cc40);
    assert(sixels(&f) == 1024u * 1024u);
    clean(&f); dirty(&f, 32, 32);
    assert(sixels(&f) == 256);
    printf("Sixel 1024x1024: old dirty-row work=%d pixels, one cell=256, clean=0\n",
           f.term.width * f.term.cell_height);
    fini(&f);
}
int main(int argc, char **argv)
{
    kitty_tests(); sixel_tests();
    workload(argc > 1 && strcmp(argv[1], "--benchmark") == 0);
    puts("image-render tests: ok");
    return 0;
}
