#include "kitty.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "grid.h"
#include "image-render.h"
#include "kitty-graphics.h"
#include "macros.h"
#include "render.h"
#include "util.h"


#define LOG_MODULE "kitty"
#define LOG_ENABLE_DBG 0
#include "log.h"

#define KITTY_PLACEMENT_MAX_BYTES (64u * 1024u * 1024u)
#define KITTY_PLACEMENT_MAX_COUNT 256u
#define KITTY_APC_HEADER_MAX 4096u
#define KITTY_DIMENSION_MAX 16384

static void kitty_image_reply(void *user, const char *data, size_t len);
static bool kitty_place_image(
    void *user, const struct kitty_graphics_placement *source);
static void kitty_remove_image(
    void *user, uint32_t image_id, uint32_t placement_id, bool all);

static const struct kitty_graphics_callbacks kitty_callbacks = {
    .reply = &kitty_image_reply,
    .place = &kitty_place_image,
    .remove = &kitty_remove_image,
};

static void
kitty_apc_clear(struct terminal *term)
{
    term->kitty_apc.header_len = 0;
    term->kitty_apc.in_payload = false;
    term->kitty_apc.failed = false;
}

static struct kitty_graphics *
kitty_engine(struct terminal *term)
{
    if (term->kitty_graphics == NULL)
        term->kitty_graphics = kitty_graphics_create(&kitty_callbacks, term);
    return term->kitty_graphics;
}

void
kitty_begin(struct terminal *term)
{
    kitty_apc_clear(term);
    if (kitty_engine(term) == NULL)
        term->kitty_apc.failed = true;
}

static bool
kitty_header_append(struct terminal *term, uint8_t byte)
{
    if (term->kitty_apc.header_len >= KITTY_APC_HEADER_MAX)
        return false;

    if (term->kitty_apc.header_len == term->kitty_apc.header_cap) {
        size_t cap = term->kitty_apc.header_cap == 0 ? 64 : term->kitty_apc.header_cap * 2;
        cap = min(cap, KITTY_APC_HEADER_MAX);
        char *header = realloc(term->kitty_apc.header, cap);
        if (header == NULL)
            return false;
        term->kitty_apc.header = header;
        term->kitty_apc.header_cap = cap;
    }

    term->kitty_apc.header[term->kitty_apc.header_len++] = byte;
    return true;
}

void
kitty_put(struct terminal *term, uint8_t byte)
{
    if (term->kitty_apc.failed)
        return;

    if (!term->kitty_apc.in_payload) {
        if (!kitty_header_append(term, byte)) {
            term->kitty_apc.failed = true;
            return;
        }

        if (byte != ';')
            return;

        if (kitty_graphics_begin(term->kitty_graphics,
                                 term->kitty_apc.header,
                                 term->kitty_apc.header_len) < 0)
        {
            term->kitty_apc.failed = true;
            return;
        }

        term->kitty_apc.in_payload = true;
        return;
    }

    if (kitty_graphics_put(term->kitty_graphics, (const char *)&byte, 1) < 0)
        term->kitty_apc.failed = true;
}

void
kitty_end(struct terminal *term)
{
    /* Commands without an image payload (notably a=p and a=d) may omit
     * the separator entirely. Normalize them for the engine at ST. */
    if (!term->kitty_apc.failed && !term->kitty_apc.in_payload &&
        term->kitty_apc.header_len > 0)
        kitty_put(term, ';');

    if (!term->kitty_apc.failed && term->kitty_apc.in_payload)
        (void)kitty_graphics_end(term->kitty_graphics);
    else if (term->kitty_graphics != NULL)
        kitty_graphics_cancel(term->kitty_graphics);

    kitty_apc_clear(term);
}

void
kitty_cancel(struct terminal *term)
{
    if (term->kitty_graphics != NULL)
        kitty_graphics_cancel(term->kitty_graphics);
    kitty_apc_clear(term);
}

void
kitty_image_reply(void *user, const char *data, size_t len)
{
    struct terminal *term = user;
    (void)term_to_slave(term, data, len);
}

static void
kitty_mark_rect_dirty(struct terminal *term, int start_row, int end_row,
                      int start_col, int end_col)
{
    if (term->grid == NULL || term->grid->num_rows <= 0)
        return;

    start_row = max(start_row, 0);
    end_row = min(end_row, term->rows - 1);
    start_col = max(start_col, 0);
    end_col = min(end_col, term->cols - 1);
    for (int r = start_row; r <= end_row; r++) {
        struct row *row = grid_row_in_view(term->grid, r);
        row->dirty = true;
        for (int c = start_col; c <= end_col; c++)
            row->cells[c].attrs.clean = false;
    }
}

static int
kitty_row_delta(const struct terminal *term, int abs_row)
{
    /* Convert both absolute rows to the same scrollback-relative origin. The
     * ring's shortest modular distance is not meaningful here: an older row
     * can be almost a full ring behind the viewport and must not reappear. */
    const int placement_sb = grid_row_abs_to_sb(term->grid, term->rows, abs_row);
    const int view_sb = grid_row_abs_to_sb(
        term->grid, term->rows, term->grid->view);
    return placement_sb - view_sb;
}

static bool
kitty_render_dimensions(const struct terminal *term,
                        const struct kitty_placement *p,
                        int *width, int *height, int *x_offset, int *y_offset)
{
    if (term->cell_width <= 0 || term->cell_height <= 0)
        return false;

    int64_t w = p->width;
    int64_t h = p->height;
    int64_t x_ofs = 0, y_ofs = 0;
    if (!p->cols_implicit && !p->rows_implicit) {
        const int64_t box_w = (int64_t)p->cols * term->cell_width;
        const int64_t box_h = (int64_t)p->rows * term->cell_height;
        if (box_w <= 0 || box_h <= 0)
            return false;

        /* Fit within the requested cell rectangle without distorting the
         * source. Center the unused area to provide letterboxing/pillarboxing. */
        if ((int64_t)p->width * box_h > (int64_t)p->height * box_w) {
            w = box_w;
            h = max(1, box_w * p->height / p->width);
        } else {
            h = box_h;
            w = max(1, box_h * p->width / p->height);
        }
        x_ofs = (box_w - w) / 2;
        y_ofs = (box_h - h) / 2;
    } else if (!p->cols_implicit) {
        w = (int64_t)p->cols * term->cell_width;
        h = (w * p->height + p->width - 1) / p->width;
    } else if (!p->rows_implicit) {
        h = (int64_t)p->rows * term->cell_height;
        w = (h * p->width + p->height - 1) / p->height;
    }

    if (w <= 0 || h <= 0 || w > INT_MAX || h > INT_MAX ||
        x_ofs > INT_MAX || y_ofs > INT_MAX)
        return false;
    *width = w;
    *height = h;
    *x_offset = x_ofs;
    *y_offset = y_ofs;
    return true;
}

static void
kitty_mark_placement_dirty(struct terminal *term,
                           const struct kitty_placement *p)
{
    int width, height, fit_x, fit_y;
    if (!kitty_render_dimensions(term, p, &width, &height, &fit_x, &fit_y))
        return;
    const int64_t x = (int64_t)term->margins.left +
        (int64_t)p->col * term->cell_width + p->x_offset + fit_x;
    const int64_t y = (int64_t)term->margins.top +
        (int64_t)kitty_row_delta(term, p->row) * term->cell_height +
        p->y_offset + fit_y;
    const int64_t left = max(x, term->margins.left);
    const int64_t top = max(y, term->margins.top);
    const int64_t right = min(x + width, term->width - term->margins.right);
    const int64_t bottom = min(y + height, term->height - term->margins.bottom);
    if (right <= left || bottom <= top)
        return;
    kitty_mark_rect_dirty(
        term, (top - term->margins.top) / term->cell_height,
        (bottom - 1 - term->margins.top) / term->cell_height,
        (left - term->margins.left) / term->cell_width,
        (right - 1 - term->margins.left) / term->cell_width);
}

static size_t
kitty_placement_usage(const struct terminal *term, size_t *count)
{
    size_t bytes = 0;
    *count = 0;
    const struct grid *grids[] = {&term->normal, &term->alt};
    for (size_t i = 0; i < sizeof(grids) / sizeof(grids[0]); i++) {
        tll_foreach(grids[i]->kitty_placements, it) {
            bytes += it->item.size;
            (*count)++;
        }
    }
    return bytes;
}

static bool
kitty_place_image(void *user,
                  const struct kitty_graphics_placement *source)
{
    struct terminal *term = user;
    if (source == NULL || source->pixels == NULL || source->width == 0 ||
        source->height == 0 || source->width > KITTY_DIMENSION_MAX ||
        source->height > KITTY_DIMENSION_MAX || source->cols < 0 ||
        source->rows < 0 || source->cols > KITTY_DIMENSION_MAX ||
        source->rows > KITTY_DIMENSION_MAX || source->z_index < 0 ||
        term->grid == NULL || term->cell_width <= 0 || term->cell_height <= 0)
    {
        errno = source != NULL && source->z_index < 0 ? ENOTSUP : EINVAL;
        return false;
    }

    const size_t width = source->width;
    const size_t height = source->height;
    if (width > SIZE_MAX / sizeof(uint32_t) / height) {
        errno = EOVERFLOW;
        return false;
    }
    const size_t size = width * height * sizeof(uint32_t);
    if (size > KITTY_PLACEMENT_MAX_BYTES) {
        errno = ENOSPC;
        return false;
    }

    struct kitty_placement p = {
        .image_id = source->image_id,
        .placement_id = source->id,
        .row = grid_row_absolute(term->grid, term->grid->cursor.point.row),
        .col = term->grid->cursor.point.col,
        .cols = source->cols,
        .rows = source->rows,
        .cols_implicit = source->cols == 0,
        .rows_implicit = source->rows == 0,
        .x_offset = source->x_offset,
        .y_offset = source->y_offset,
        .z_index = source->z_index,
        .width = source->width,
        .height = source->height,
        .size = size,
    };
    if (p.cols_implicit && p.rows_implicit) {
        p.cols = max(1, (source->width + term->cell_width - 1) / term->cell_width);
        p.rows = max(1, (source->height + term->cell_height - 1) / term->cell_height);
    } else if (p.cols_implicit) {
        int64_t target_h = (int64_t)p.rows * term->cell_height;
        p.cols = max(1, (target_h * source->width +
                         (int64_t)source->height * term->cell_width - 1) /
                        ((int64_t)source->height * term->cell_width));
    } else if (p.rows_implicit) {
        int64_t target_w = (int64_t)p.cols * term->cell_width;
        p.rows = max(1, (target_w * source->height +
                         (int64_t)source->width * term->cell_height - 1) /
                        ((int64_t)source->width * term->cell_height));
    }

    if (p.cols <= 0 || p.rows <= 0 || p.cols > KITTY_DIMENSION_MAX ||
        p.rows > KITTY_DIMENSION_MAX)
    {
        errno = EINVAL;
        return false;
    }

    size_t count;
    size_t bytes = kitty_placement_usage(term, &count);
    /* Placement id zero means an anonymous, fresh display on every place;
     * only explicit ids participate in replacement. */
    if (p.placement_id != 0) {
        const struct grid *budget_grids[] = {&term->normal, &term->alt};
        for (size_t i = 0; i < sizeof(budget_grids) / sizeof(budget_grids[0]); i++) {
            tll_foreach(budget_grids[i]->kitty_placements, it) {
                if (it->item.image_id == p.image_id &&
                    it->item.placement_id == p.placement_id)
                {
                    bytes -= it->item.size;
                    count--;
                }
            }
        }
    }
    if (count >= KITTY_PLACEMENT_MAX_COUNT ||
        bytes > KITTY_PLACEMENT_MAX_BYTES - size)
    {
        errno = ENOSPC;
        return false;
    }

    p.pixels = malloc(size);
    if (p.pixels == NULL) {
        errno = ENOMEM;
        return false;
    }
    memcpy(p.pixels, source->pixels, size);
    p.pix = pixman_image_create_bits(
        PIXMAN_a8r8g8b8, p.width, p.height, p.pixels,
        p.width * (int)sizeof(uint32_t));
    if (p.pix == NULL) {
        free(p.pixels);
        errno = ENOMEM;
        return false;
    }

    /* Replace only an explicitly named placement with the same image owner.
     * Anonymous placements (id=0) deliberately accumulate independently. */
    if (p.placement_id != 0) {
        struct grid *grids[] = {&term->normal, &term->alt};
        for (size_t i = 0; i < sizeof(grids) / sizeof(grids[0]); i++) {
            tll_foreach(grids[i]->kitty_placements, it) {
                if (it->item.image_id == p.image_id &&
                    it->item.placement_id == p.placement_id)
                {
                    kitty_mark_placement_dirty(term, &it->item);
                    kitty_placement_destroy(&it->item);
                    tll_remove(grids[i]->kitty_placements, it);
                }
            }
        }
    }

    tll_foreach(term->grid->kitty_placements, it) {
        if (it->item.z_index > p.z_index) {
            tll_insert_before(term->grid->kitty_placements, it, p);
            goto placed;
        }
    }
    tll_push_back(term->grid->kitty_placements, p);

placed:
    kitty_mark_placement_dirty(term, &p);
    if (source->move_cursor) {
        const int start_row = term->grid->cursor.point.row;
        const int start_col = term->grid->cursor.point.col;
        term_cursor_to(term, start_row, start_col);
        term_cursor_down(term, p.rows);
        term_cursor_right(term, p.cols);
    }
    render_refresh(term);
    return true;
}

static void
kitty_remove_from_grid(struct terminal *term, struct grid *grid,
                       uint32_t image_id, uint32_t placement_id)
{
    tll_foreach(grid->kitty_placements, it) {
        const bool image_matches = image_id == 0 || it->item.image_id == image_id;
        const bool placement_matches =
            placement_id == 0 || it->item.placement_id == placement_id;
        if (!image_matches || !placement_matches)
            continue;

        if (grid == term->grid)
            kitty_mark_placement_dirty(term, &it->item);
        kitty_placement_destroy(&it->item);
        tll_remove(grid->kitty_placements, it);
    }
}

void
kitty_placements_invalidate(struct terminal *term)
{
    kitty_remove_from_grid(term, term->grid, 0, 0);
}

static void
kitty_remove_image(void *user, uint32_t image_id,
                   uint32_t placement_id, bool all)
{
    struct terminal *term = user;
    (void)all;
    kitty_remove_from_grid(term, &term->normal, image_id, placement_id);
    kitty_remove_from_grid(term, &term->alt, image_id, placement_id);
    render_refresh(term);
}

void
kitty_placement_destroy(struct kitty_placement *placement)
{
    if (placement->pix != NULL)
        pixman_image_unref(placement->pix);
    free(placement->pixels);
    memset(placement, 0, sizeof(*placement));
}

void
kitty_placements_clear(struct grid *grid)
{
    tll_foreach(grid->kitty_placements, it) {
        kitty_placement_destroy(&it->item);
        tll_remove(grid->kitty_placements, it);
    }
}

bool
kitty_placement_clone(struct kitty_placement *dst,
                      const struct kitty_placement *src)
{
    *dst = *src;
    dst->pix = NULL;
    dst->pixels = malloc(src->size);
    if (dst->pixels == NULL)
        return false;
    memcpy(dst->pixels, src->pixels, src->size);
    dst->pix = pixman_image_create_bits(
        PIXMAN_a8r8g8b8, dst->width, dst->height, dst->pixels,
        dst->width * (int)sizeof(uint32_t));
    if (dst->pix == NULL) {
        free(dst->pixels);
        memset(dst, 0, sizeof(*dst));
        return false;
    }
    return true;
}

void
kitty_fini(struct terminal *term)
{
    kitty_cancel(term);
    kitty_placements_clear(&term->normal);
    kitty_placements_clear(&term->alt);
    kitty_graphics_free(term->kitty_graphics);
    term->kitty_graphics = NULL;
    free(term->kitty_apc.header);
    term->kitty_apc.header = NULL;
    term->kitty_apc.header_cap = 0;
}

void
kitty_reset(struct terminal *term)
{
    kitty_cancel(term);
    if (term->kitty_graphics != NULL)
        kitty_graphics_reset(term->kitty_graphics);
    kitty_placements_clear(&term->normal);
    kitty_placements_clear(&term->alt);
    render_refresh(term);
}

static void
kitty_discard_reused_placements(struct terminal *term, int rows, bool reverse)
{
    const int ring_rows = term->grid->num_rows;
    tll_foreach(term->grid->kitty_placements, it) {
        const int anchor = it->item.row;
        const int oldest_distance = grid_row_abs_to_sb(term->grid, term->rows, anchor);
        /* Forward scroll reuses the oldest history rows. Reverse scroll
         * inserts blank rows before the old viewport and erases its bottom. */
        const int inserted_distance =
            (anchor - term->grid->offset + rows + ring_rows) & (ring_rows - 1);
        const int bottom_distance =
            (anchor - term->grid->offset - term->rows + rows + ring_rows) & (ring_rows - 1);
        if ((!reverse && oldest_distance >= rows) ||
            (reverse && inserted_distance >= rows && bottom_distance >= rows))
            continue;
        kitty_mark_placement_dirty(term, &it->item);
        kitty_placement_destroy(&it->item);
        tll_remove(term->grid->kitty_placements, it);
    }
}

void
kitty_scroll_up(struct terminal *term, int rows)
{
    if (rows > 0 && term->grid != NULL)
        kitty_discard_reused_placements(term, rows, false);
}

void
kitty_scroll_down(struct terminal *term, int rows)
{
    if (rows > 0 && term->grid != NULL)
        kitty_discard_reused_placements(term, rows, true);
}

uint64_t
kitty_render_placements(struct terminal *term, pixman_image_t *pix,
                        pixman_region32_t *damage)
{
    if (term->grid == NULL || damage == NULL ||
        !pixman_region32_not_empty(damage) ||
        tll_length(term->grid->kitty_placements) == 0)
        return 0;

    uint64_t pixels = 0;
    const int clip_left = term->margins.left;
    const int clip_top = term->margins.top;
    const int clip_right = term->width - term->margins.right;
    const int clip_bottom = term->height - term->margins.bottom;

    tll_foreach(term->grid->kitty_placements, it) {
        struct kitty_placement *p = &it->item;
        int width, height, fit_x, fit_y;
        if (!kitty_render_dimensions(
                term, p, &width, &height, &fit_x, &fit_y))
            continue;

        int64_t x64 = (int64_t)term->margins.left +
            (int64_t)p->col * term->cell_width + p->x_offset + fit_x;
        int64_t y64 = (int64_t)term->margins.top +
            (int64_t)kitty_row_delta(term, p->row) * term->cell_height +
            p->y_offset + fit_y;
        if (x64 < INT_MIN || x64 > INT_MAX || y64 < INT_MIN || y64 > INT_MAX)
            continue;
        int x = x64;
        int y = y64;

        int left = max(x, clip_left);
        int top = max(y, clip_top);
        int right = (int)min((int64_t)x + width, clip_right);
        int bottom = (int)min((int64_t)y + height, clip_bottom);
        if (right <= left || bottom <= top)
            continue;

        pixman_transform_t transform;
        pixman_transform_init_scale(
            &transform,
            pixman_double_to_fixed((double)p->width / width),
            pixman_double_to_fixed((double)p->height / height));
        pixman_image_set_transform(p->pix, &transform);
        pixman_image_set_filter(p->pix, PIXMAN_FILTER_BILINEAR, NULL, 0);
        /* Bilinear sampling at scaled source boundaries must extend edge
         * texels, not blend against transparent black. */
        pixman_image_set_repeat(p->pix, PIXMAN_REPEAT_PAD);
        pixels += image_composite_damage(
            PIXMAN_OP_OVER, p->pix, pix, left - x, top - y,
            left, top, right - left, bottom - top, damage, NULL);
        pixman_transform_init_identity(&transform);
        pixman_image_set_transform(p->pix, &transform);

    }
    return pixels;
}
