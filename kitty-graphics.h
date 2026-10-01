#ifndef KITTY_GRAPHICS_H
#define KITTY_GRAPHICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct kitty_graphics;

/* pixels is a cropped, premultiplied ARGB32 buffer, valid only during place().
 * cols/rows are requested terminal-cell dimensions (zero means unspecified);
 * pixel scaling/letterboxing for them is the adapter's responsibility. */
struct kitty_graphics_placement {
  uint32_t id;            /* placement id */
  uint32_t image_id;      /* resolved image id */
  uint32_t width, height; /* dimensions of cropped pixel buffer */
  const uint32_t *pixels; /* premultiplied 0xAARRGGBB */
  int cols, rows, x_offset, y_offset, z_index;
  bool move_cursor;
};

struct kitty_graphics_callbacks {
  void (*reply)(void *user, const char *data, size_t len); /* complete APC */
  bool (*place)(void *user, const struct kitty_graphics_placement *placement);
  /* image_id=0 means all images; placement_id=0 means all placements.
   * all=true also removes image data, false removes display placements only. */
  void (*remove)(void *user, uint32_t image_id, uint32_t placement_id,
                 bool all);
};

/* Engine owns images up to 64 MiB total and dimensions up to 16384x16384.
 * All functions are single-threaded. begin() accepts data after ESC_ including
 * the leading G and control terminator ';'. put() accepts base64 payload bytes.
 * end() finishes one APC; m=1 leaves an upload open for the next begin().
 * Return 0 on success, -1 with errno set on failure. */
struct kitty_graphics *
kitty_graphics_create(const struct kitty_graphics_callbacks *callbacks,
                      void *user);
void kitty_graphics_free(struct kitty_graphics *graphics);
void kitty_graphics_reset(struct kitty_graphics *graphics);
int kitty_graphics_begin(struct kitty_graphics *graphics, const char *apc,
                         size_t len);
int kitty_graphics_put(struct kitty_graphics *graphics, const char *payload,
                       size_t len);
int kitty_graphics_end(struct kitty_graphics *graphics);
void kitty_graphics_cancel(struct kitty_graphics *graphics);

#endif
