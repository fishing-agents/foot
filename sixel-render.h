#pragma once

#include <stdbool.h>
#include "image-render.h"

struct row;
struct coord;

/* Sixels render before the text workers. These callbacks restore backgrounds
 * and glyphs using the ordinary text renderer; tests can isolate pixel damage
 * without starting a compositor or loading fonts. */
struct sixel_text_render {
    void (*row)(struct terminal *, pixman_image_t *, pixman_region32_t *,
                struct row *, int row_no, int cursor_col);
    int (*cell)(struct terminal *, pixman_image_t *, pixman_region32_t *,
                struct row *, int row_no, int col, bool has_cursor);
};

/* Returns image pixels submitted to Pixman, excluding already-retained pixels. */
uint64_t sixel_render_images(
    struct terminal *term, pixman_image_t *pix, pixman_region32_t *damage,
    const struct coord *cursor, const struct sixel_text_render *text);
