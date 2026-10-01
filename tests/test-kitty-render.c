/* Tests the real Kitty terminal adapter and pixman compositor; only unrelated
 * terminal I/O, scheduling and cursor movement are stubbed. */
#include "../kitty.h"
#include "../grid.h"
#include "../render.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char reply[512];
bool term_to_slave(struct terminal *term, const void *data, size_t length)
{
    (void)term;
    assert(length < sizeof(reply));
    memcpy(reply, data, length);
    reply[length] = '\0';
    return true;
}
void render_refresh(struct terminal *term) { (void)term; }
int grid_row_abs_to_sb(const struct grid *grid, int screen_rows, int abs_row)
{
    return (abs_row - grid->offset - screen_rows + grid->num_rows) &
        (grid->num_rows - 1);
}
void term_cursor_to(struct terminal *term, int row, int col)
{
    term->grid->cursor.point.row = row;
    term->grid->cursor.point.col = col;
}
void term_cursor_right(struct terminal *term, int count)
{
    term->grid->cursor.point.col += count;
}
void term_cursor_down(struct terminal *term, int count)
{
    term->grid->cursor.point.row += count;
}

static void send_apc(struct terminal *term, const char *command)
{
    reply[0] = '\0';
    kitty_begin(term);
    for (const char *p = command; *p; p++)
        kitty_put(term, (uint8_t)*p);
    kitty_end(term);
}
static void init_grid(struct grid *grid)
{
    grid->num_rows = 16;
    grid->num_cols = 8;
    grid->rows = calloc(16, sizeof(*grid->rows));
    assert(grid->rows);
    for (int r = 0; r < 16; r++) {
        grid->rows[r] = calloc(1, sizeof(struct row));
        assert(grid->rows[r]);
        grid->rows[r]->cells = calloc(8, sizeof(struct cell));
        assert(grid->rows[r]->cells);
    }
}
static void free_grid(struct grid *grid)
{
    for (int r = 0; r < 16; r++) {
        free(grid->rows[r]->cells);
        free(grid->rows[r]);
    }
    free(grid->rows);
}
int main(void)
{
    struct terminal term = {.rows=8, .cols=8, .width=8, .height=8,
                            .cell_width=1, .cell_height=1};
    init_grid(&term.normal);
    init_grid(&term.alt);
    term.grid = &term.normal;
    send_apc(&term, "Ga=T,f=32,s=1,v=1,i=1,p=3,C=1;/wAAgA==");
    assert(strstr(reply, "i=1,p=3;OK"));
    assert(tll_length(term.normal.kitty_placements) == 1);
    struct kitty_placement *first = &tll_front(term.normal.kitty_placements);
    assert(first->image_id == 1 && first->placement_id == 3);
    assert(first->pixels[0] == 0x80800000);
    assert(term.normal.cursor.point.row == 0 && term.normal.cursor.point.col == 0);

    uint32_t pixels[64];
    for (int p = 0; p < 64; p++) pixels[p] = 0xff000000;
    pixman_image_t *image = pixman_image_create_bits(PIXMAN_a8r8g8b8, 8, 8, pixels, 32);
    assert(image);
    pixman_region32_t damage;
    pixman_region32_init(&damage);
    kitty_render_prepare(&term);
    assert(term.normal.rows[0]->dirty);
    assert(!term.normal.rows[0]->cells[0].attrs.clean);
    kitty_render_placements(&term, image, &damage);
    assert(pixels[0] == 0xff800000);
    assert(pixels[1] == 0xff000000);
    assert(pixman_region32_contains_point(&damage, 0, 0, NULL));

    /* Another image with the same placement ID is independent. */
    term.normal.cursor.point.col = 2;
    send_apc(&term, "Ga=T,f=24,s=1,v=1,i=2,p=3,C=1;AP8A");
    assert(strstr(reply, "i=2,p=3;OK"));
    assert(tll_length(term.normal.kitty_placements) == 2);
    send_apc(&term, "Ga=d,d=i,i=1,p=3;");
    assert(tll_length(term.normal.kitty_placements) == 1);
    assert(tll_front(term.normal.kitty_placements).image_id == 2);
    /* Lowercase deletion preserves the stored image. */
    send_apc(&term, "Ga=p,i=1,p=4,C=1");  /* Payloadless APC omits ';'. */
    assert(strstr(reply, "i=1,p=4;OK"));
    assert(tll_length(term.normal.kitty_placements) == 2);
    send_apc(&term, "Ga=d,d=n,I=999;");
    assert(tll_length(term.normal.kitty_placements) == 2);
    send_apc(&term, "Ga=d,d=I,i=1,p=4;");
    assert(strstr(reply, "ENOTSUP"));
    assert(tll_length(term.normal.kitty_placements) == 2);
    send_apc(&term, "Ga=d,d=I,i=1;");
    assert(tll_length(term.normal.kitty_placements) == 1);
    send_apc(&term, "Ga=p,i=1,C=1;");
    assert(strstr(reply, "ENOENT"));

    /* Snapshot backing storage is independently owned. */
    struct kitty_placement clone;
    first = &tll_front(term.normal.kitty_placements);
    assert(kitty_placement_clone(&clone, first));
    assert(clone.pixels != first->pixels && clone.pix != first->pix);
    assert(clone.pixels[0] == first->pixels[0]);
    kitty_placement_destroy(&clone);

    kitty_reset(&term);
    assert(tll_length(term.normal.kitty_placements) == 0);

    /* Unnamed placements are independent rather than replacing ID zero. */
    term_cursor_to(&term, 0, 0);
    send_apc(&term, "Ga=T,f=24,s=1,v=1,C=1;/wAA");
    term_cursor_to(&term, 0, 2);
    send_apc(&term, "Ga=T,f=24,s=1,v=1,C=1;AP8A");
    assert(tll_length(term.normal.kitty_placements) == 2);
    send_apc(&term, "Ga=T,f=24,s=1,v=1,i=22,C=1;AP8A");
    assert(tll_length(term.normal.kitty_placements) == 3);
    send_apc(&term, "Ga=d,d=A");
    assert(tll_length(term.normal.kitty_placements) == 0);
    send_apc(&term, "Ga=p,i=22,C=1");
    assert(strstr(reply, "ENOENT"));
    kitty_reset(&term);

    /* Both requested cell dimensions preserve image aspect ratio, while
     * cursor movement uses the requested placement rectangle. */
    term_cursor_to(&term, 0, 0);
    send_apc(&term, "Ga=T,f=24,s=1,v=1,i=10,c=2,r=3;/wAA");
    assert(strstr(reply, "i=10;OK"));
    assert(term.normal.cursor.point.row == 3);
    assert(term.normal.cursor.point.col == 2);
    for (int p = 0; p < 64; p++) pixels[p] = 0xff000000;
    kitty_render_placements(&term, image, &damage);
    unsigned red_pixels = 0;
    for (int p = 0; p < 64; p++)
        red_pixels += pixels[p] == 0xffff0000;
    assert(red_pixels == 4);  /* 2x2 image, not a distorted 2x3. */

    /* Clone surfaces never alias their source grid's owned pixels. */
    struct kitty_placement independent;
    const struct kitty_placement *original = &tll_front(term.normal.kitty_placements);
    assert(kitty_placement_clone(&independent, original));
    assert(independent.pixels != original->pixels);
    independent.pixels[0] = 0xff00ff00;
    assert(original->pixels[0] == 0xffff0000);
    pixman_image_unref(independent.pix);
    free(independent.pixels);

    /* Ordinary full-screen scrolling keeps absolute scrollback anchors. */
    term.scroll_region.start = 0;
    term.scroll_region.end = term.rows;
    kitty_scroll_up(&term, 1);
    assert(tll_length(term.normal.kitty_placements) == 1);
    term_cursor_to(&term, 7, 0);
    send_apc(&term, "Ga=T,f=24,s=1,v=1,i=11,C=1;AP8A");
    assert(tll_length(term.normal.kitty_placements) == 2);
    kitty_scroll_down(&term, 1);
    assert(tll_length(term.normal.kitty_placements) == 1);
    assert(tll_front(term.normal.kitty_placements).image_id == 10);

    /* Advancing the viewport clips the placement, never wraps it back to
     * the screen using shortest-distance modular arithmetic. */
    term.normal.view = 4;
    for (int p = 0; p < 64; p++) pixels[p] = 0xff000000;
    kitty_render_placements(&term, image, &damage);
    for (int p = 0; p < 64; p++) assert(pixels[p] == 0xff000000);
    term.normal.view = 0;
    term.normal.offset = 8;
    kitty_scroll_up(&term, 1);  /* Row zero has reached the oldest history row. */
    assert(tll_length(term.normal.kitty_placements) == 0);
    term.normal.offset = 0;

    kitty_reset(&term);
    kitty_fini(&term);
    pixman_region32_fini(&damage);
    pixman_image_unref(image);
    free_grid(&term.normal);
    free_grid(&term.alt);
    puts("kitty-render tests: ok");
    return 0;
}
