#pragma once

#include <stdint.h>
#include <pixman.h>

struct terminal;

/* Snapshot cell damage before image rendering marks covered cells clean. */
void image_dirty_region(const struct terminal *term, pixman_region32_t *region);

/* Composite only the intersection of image bounds and freshly repainted
 * pixels. Retained pixels must not receive another translucent OVER. Returns
 * the number of destination pixels submitted to Pixman (zero for a clean frame).
 * repaint and damage may alias; damage may be NULL. Source coordinates are
 * relative to the full destination rectangle, including scaled sources. */
uint64_t image_composite_damage(
    pixman_op_t op, pixman_image_t *src, pixman_image_t *dst,
    int src_x, int src_y, int x, int y, int width, int height,
    const pixman_region32_t *repaint, pixman_region32_t *damage);
