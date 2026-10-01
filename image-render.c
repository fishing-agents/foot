#include "image-render.h"
#include "grid.h"

void
image_dirty_region(const struct terminal *term, pixman_region32_t *region)
{
    pixman_region32_clear(region);
    for (int r = 0; r < term->rows; r++) {
        const struct row *row = grid_row_in_view(term->grid, r);
        if (!row->dirty)
            continue;
        for (int c = 0; c < term->cols;) {
            if (row->cells[c].attrs.clean) {
                c++;
                continue;
            }
            const int start = c++;
            while (c < term->cols && !row->cells[c].attrs.clean)
                c++;
            pixman_region32_union_rect(
                region, region,
                term->margins.left + start * term->cell_width,
                term->margins.top + r * term->cell_height,
                (c - start) * term->cell_width, term->cell_height);
        }
    }
}

uint64_t
image_composite_damage(
    pixman_op_t op, pixman_image_t *src, pixman_image_t *dst,
    int src_x, int src_y, int x, int y, int width, int height,
    const pixman_region32_t *repaint, pixman_region32_t *damage)
{
    if (width <= 0 || height <= 0 || repaint == NULL)
        return 0;

    pixman_region32_t clip;
    pixman_region32_init(&clip);
    pixman_region32_intersect_rect(
        &clip, repaint, x, y, width, height);
    int count;
    const pixman_box32_t *boxes = pixman_region32_rectangles(&clip, &count);
    uint64_t pixels = 0;
    for (int i = 0; i < count; i++) {
        const pixman_box32_t *b = &boxes[i];
        const int w = b->x2 - b->x1;
        const int h = b->y2 - b->y1;
        pixman_image_composite32(
            op, src, NULL, dst,
            src_x + b->x1 - x, src_y + b->y1 - y,
            0, 0, b->x1, b->y1, w, h);
        pixels += (uint64_t)w * h;
    }
    if (damage != NULL)
        pixman_region32_union(damage, damage, &clip);
    pixman_region32_fini(&clip);
    return pixels;
}
