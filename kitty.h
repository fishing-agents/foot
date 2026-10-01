#pragma once

#include "terminal.h"

void kitty_begin(struct terminal *term);
void kitty_put(struct terminal *term, uint8_t byte);
void kitty_end(struct terminal *term);
void kitty_cancel(struct terminal *term);
void kitty_fini(struct terminal *term);
void kitty_reset(struct terminal *term);

/* Scrolling conservatively discards placements intersecting the terminal's
 * active scroll region; placements outside it remain anchored. */
void kitty_scroll_up(struct terminal *term, int rows);
void kitty_scroll_down(struct terminal *term, int rows);

/* Grid-owned surface lifecycle helpers. */
void kitty_placement_destroy(struct kitty_placement *placement);
void kitty_placements_clear(struct grid *grid);
/* Discard displayed images and dirty only their old cell bounds. */
void kitty_placements_invalidate(struct terminal *term);
bool kitty_placement_clone(
    struct kitty_placement *dst, const struct kitty_placement *src);

/* Composite positive-z images only over freshly repainted text/Sixel pixels.
 * Scroll copies and buffer-age repairs already include images and must not be
 * blended again. Returns the number of image pixels submitted to Pixman. */
uint64_t kitty_render_placements(
    struct terminal *term, pixman_image_t *pix, pixman_region32_t *damage);
