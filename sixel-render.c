#include "sixel-render.h"
#include "grid.h"
#include "sixel.h"
#include "util.h"

static uint64_t
render_sixel_chunk(struct terminal *term, pixman_image_t *pix,
                   pixman_region32_t *damage, const struct sixel *sixel,
                   int term_start_row, int img_start_row, int count,
                   const pixman_region32_t *repaint)
{
    /* Translate row/column to x/y pixel values */
    const int x = term->margins.left + sixel->pos.col * term->cell_width;
    const int y = term->margins.top + term_start_row * term->cell_height;

    /* Width/height, in pixels - and don't touch the window margins */
    const int width = max(
        0,
        min(sixel->width,
            term->width - x - term->margins.right));
    const int height = max(
        0,
        min(
            min(count * term->cell_height,                          /* 'count' number of rows */
                sixel->height - img_start_row * term->cell_height), /* What remains of the sixel */
            term->height - y - term->margins.bottom));

    /* Verify we're not stepping outside the grid */
    xassert(x >= term->margins.left);
    xassert(y >= term->margins.top);
    xassert(width == 0 || x + width <= term->width - term->margins.right);
    xassert(height == 0 || y + height <= term->height - term->margins.bottom);

    //LOG_DBG("sixel chunk: %dx%d %dx%d", x, y, width, height);

    return image_composite_damage(
        sixel->opaque ? PIXMAN_OP_SRC : PIXMAN_OP_OVER, sixel->pix, pix,
        0, img_start_row * term->cell_height, x, y, width, height,
        repaint, damage);
}

static void
render_sixel(struct terminal *term, pixman_image_t *pix,
             pixman_region32_t *damage, const struct coord *cursor,
             const struct sixel *sixel, pixman_region32_t *repaint,
             const struct sixel_text_render *text, uint64_t *pixels)
{
    xassert(sixel->pix != NULL);
    xassert(sixel->width >= 0);
    xassert(sixel->height >= 0);

    const int view_end = (term->grid->view + term->rows - 1) & (term->grid->num_rows - 1);
    const bool last_row_needs_erase = sixel->height % term->cell_height != 0;
    const bool last_col_needs_erase = sixel->width % term->cell_width != 0;

    int chunk_img_start = -1;  /* Image-relative start row of chunk */
    int chunk_term_start = -1; /* Viewport relative start row of chunk */
    int chunk_row_count = 0;   /* Number of rows to emit */

#define maybe_emit_sixel_chunk_then_reset()                             \
    if (chunk_row_count != 0) {                                         \
        *pixels += render_sixel_chunk(                                             \
            term, pix, damage, sixel,                                   \
            chunk_term_start, chunk_img_start, chunk_row_count, repaint); \
        chunk_term_start = chunk_img_start = -1;                        \
        chunk_row_count = 0;                                            \
    }

    /*
     * Iterate all sixel rows:
     *
     *  - ignore rows that aren't visible on-screen
     *  - ignore rows that aren't dirty (they have already been rendered)
     *  - chunk consecutive dirty rows into a 'chunk'
     *  - emit (render) chunk as soon as a row isn't visible, or is clean
     *  - emit final chunk after we've iterated all rows
     *
     * The purpose of this is to reduce the amount of pixels that
     * needs to be composited and marked as damaged for the
     * compositor.
     *
     * Since we do CPU based composition, rendering is a slow and
     * heavy task for foot, and thus it is important to not re-render
     * things unnecessarily.
     */

    for (int _abs_row_no = sixel->pos.row;
         _abs_row_no < sixel->pos.row + sixel->rows;
         _abs_row_no++)
    {
        const int abs_row_no = _abs_row_no & (term->grid->num_rows - 1);
        const int term_row_no =
            (abs_row_no - term->grid->view + term->grid->num_rows) &
            (term->grid->num_rows - 1);

        /* Check if row is in the visible viewport */
        if (view_end >= term->grid->view) {
            /* Not wrapped */
            if (!(abs_row_no >= term->grid->view && abs_row_no <= view_end)) {
                /* Not visible */
                maybe_emit_sixel_chunk_then_reset();
                continue;
            }
        } else {
            /* Wrapped */
            if (!(abs_row_no >= term->grid->view || abs_row_no <= view_end)) {
                /* Not visible */
                maybe_emit_sixel_chunk_then_reset();
                continue;
            }
        }

        /* Is the row dirty? */
        struct row *row = term->grid->rows[abs_row_no];
        xassert(row != NULL);  /* Should be visible */

        if (!row->dirty) {
            maybe_emit_sixel_chunk_then_reset();
            continue;
        }

        int cursor_col = cursor->row == term_row_no ? cursor->col : -1;

        /*
         * If image contains transparent parts, render all (dirty)
         * cells beneath it.
         *
         * If image is opaque, loop cells and set their 'clean' bit,
         * to prevent the grid rendered from overwriting the sixel
         *
         * If the last sixel row only partially covers the cell row,
         * 'erase' the cell by rendering them.
         *
         * In all cases, do *not* clear the 'dirty' bit on the row, to
         * ensure the regular renderer includes them in the damage
         * rect.
         */
        if (!sixel->opaque) {
            /* TODO: multithreading */
            text->row(term, pix, damage, row, term_row_no, cursor_col);
        } else {
            for (int col = sixel->pos.col;
                 col < min(sixel->pos.col + sixel->cols, term->cols);
                 col++)
            {
                struct cell *cell = &row->cells[col];

                if (!cell->attrs.clean) {
                    bool last_row = abs_row_no == sixel->pos.row + sixel->rows - 1;
                    bool last_col = col == sixel->pos.col + sixel->cols - 1;

                    if ((last_row_needs_erase && last_row) ||
                        (last_col_needs_erase && last_col))
                    {
                        text->cell(term, pix, damage, row, term_row_no, col, cursor_col == col);
                    } else {
                        cell->attrs.clean = 1;
                        cell->attrs.confined = 1;
                    }
                }
            }
        }

        /* Include actual glyph extents from transparent backgrounds AND
         * partial opaque edges. Later Sixels must see this repaint even when
         * the text callbacks have already marked the cells clean. */
        if (damage != NULL)
            pixman_region32_union(repaint, repaint, damage);

        if (chunk_term_start == -1) {
            xassert(chunk_img_start == -1);
            chunk_term_start = term_row_no;
            chunk_img_start = _abs_row_no - sixel->pos.row;
            chunk_row_count = 1;
        } else
            chunk_row_count++;
    }

    maybe_emit_sixel_chunk_then_reset();
#undef maybe_emit_sixel_chunk_then_reset
}

uint64_t
sixel_render_images(struct terminal *term, pixman_image_t *pix,
                    pixman_region32_t *damage, const struct coord *cursor,
                    const struct sixel_text_render *text)
{
    if (likely(tll_length(term->grid->sixel_images)) == 0)
        return 0;

    uint64_t pixels = 0;
    /* Take this snapshot before opaque Sixels or render_row mark cells clean.
     * Copy/scroll repairs contain complete composited pixels, so they are not
     * repaint damage and must not be included here. */
    pixman_region32_t repaint;
    pixman_region32_init(&repaint);
    image_dirty_region(term, &repaint);

    const int scrollback_end
        = (term->grid->offset + term->rows) & (term->grid->num_rows - 1);

    const int view_start
        = (term->grid->view
           - scrollback_end
           + term->grid->num_rows) & (term->grid->num_rows - 1);

    const int view_end = view_start + term->rows - 1;

    //LOG_DBG("SIXELS: %zu images, view=%d-%d",
    //        tll_length(term->grid->sixel_images), view_start, view_end);

    tll_foreach(term->grid->sixel_images, it) {
        const struct sixel *six = &it->item;
        const int start
            = (six->pos.row
               - scrollback_end
               + term->grid->num_rows) & (term->grid->num_rows - 1);
        const int end = start + six->rows - 1;

        //LOG_DBG("  sixel: %d-%d", start, end);
        if (start > view_end) {
            /* Sixel starts after view ends, no need to try to render it */
            continue;
        } else if (end < view_start) {
            /* Image ends before view starts. Since the image list is
             * sorted, we can safely stop here */
            break;
        }

        sixel_sync_cache(term, &it->item);
        render_sixel(term, pix, damage, cursor, &it->item, &repaint, text, &pixels);
    }
    pixman_region32_fini(&repaint);
    return pixels;
}

