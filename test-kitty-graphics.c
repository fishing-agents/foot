#include "kitty-graphics.h"
#include <assert.h>
#include <errno.h>
#include <png.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
struct seen {
  unsigned replies, places, removes;
  char reply[512];
  struct kitty_graphics_placement p;
  uint32_t pixels[32], removed_id, removed_pid;
  bool removed_all;
  int place_errno;
};
static void on_reply(void *u, const char *s, size_t n) {
  struct seen *x = u;
  x->replies++;
  assert(n < sizeof(x->reply));
  memcpy(x->reply, s, n);
  x->reply[n] = 0;
}
static bool on_place(void *u, const struct kitty_graphics_placement *p) {
  struct seen *x = u;
  x->places++;
  assert((size_t)p->width * p->height <= 32);
  x->p = *p;
  memcpy(x->pixels, p->pixels, (size_t)p->width * p->height * 4);
  x->p.pixels = x->pixels;
  if (x->place_errno) {
    errno = x->place_errno;
    return false;
  }
  return true;
}
static void on_remove(void *u, uint32_t id, uint32_t pid, bool all) {
  struct seen *x = u;
  x->removes++;
  x->removed_id = id;
  x->removed_pid = pid;
  x->removed_all = all;
}
static struct kitty_graphics *new_graphics(struct seen *x) {
  struct kitty_graphics_callbacks cb = {on_reply, on_place, on_remove};
  return kitty_graphics_create(&cb, x);
}
static const char tbl[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static char *b64(const unsigned char *p, size_t n) {
  size_t z = 4 * ((n + 2) / 3);
  char *out = malloc(z + 1);
  assert(out);
  size_t j = 0;
  for (size_t i = 0; i < n; i += 3) {
    unsigned a = p[i], b = i + 1 < n ? p[i + 1] : 0,
             c = i + 2 < n ? p[i + 2] : 0;
    out[j++] = tbl[a >> 2];
    out[j++] = tbl[((a & 3) << 4) | (b >> 4)];
    out[j++] = i + 1 < n ? tbl[((b & 15) << 2) | (c >> 6)] : '=';
    out[j++] = i + 2 < n ? tbl[c & 63] : '=';
  }
  out[j] = 0;
  return out;
}
static int send_part(struct kitty_graphics *g, const char *ctl,
                     const char *payload) {
  char h[256];
  int n = snprintf(h, sizeof(h), "G%s;", ctl);
  assert(n > 0 && (size_t)n < sizeof(h));
  if (kitty_graphics_begin(g, h, (size_t)n))
    return -1;
  if (payload && kitty_graphics_put(g, payload, strlen(payload)))
    return -1;
  return kitty_graphics_end(g);
}
static void check_reply(struct seen *x, const char *s) {
  assert(strstr(x->reply, s));
}
static void test_rgb_crop_place_delete(void) {
  struct seen x = {0};
  struct kitty_graphics *g = new_graphics(&x);
  unsigned char rgba[] = {255, 0, 0, 128, 0, 255, 0, 255};
  char *p = b64(rgba, sizeof(rgba));
  assert(send_part(g,
                   "a=T,f=32,s=2,v=1,i=42,p=7,c=3,r=4,X=2,Y=1,z=3,C=1,x=1,w=1",
                   p) == 0);
  assert(x.places == 1 && x.p.id == 7 && x.p.image_id == 42 && x.p.width == 1 &&
         x.p.height == 1 && x.p.cols == 3 && x.p.rows == 4);
  assert(x.p.x_offset == 2 && x.p.y_offset == 1 && x.p.z_index == 3 &&
         !x.p.move_cursor);
  assert(x.pixels[0] == 0xff00ff00);
  check_reply(&x, "i=42,p=7;OK");
  assert(send_part(g, "a=p,i=42,p=9", "") == 0);
  assert(x.places == 2 && x.pixels[0] == 0x80800000 && x.p.id == 9);
  assert(send_part(g, "a=d,d=i,i=42,p=9", "") == 0);
  assert(x.removed_id == 42 && x.removed_pid == 9 && !x.removed_all);
  assert(send_part(g, "a=d,d=I,i=42", "") == 0);
  assert(x.removed_all);
  free(p);
  kitty_graphics_free(g);
}
static void test_rgb_formats_query_number(void) {
  struct seen x = {0};
  struct kitty_graphics *g = new_graphics(&x);
  unsigned char rgb[] = {1, 2, 3};
  char *p = b64(rgb, 3);
  assert(send_part(g, "a=q,f=24,s=1,v=1,i=77", p) == 0);
  check_reply(&x, "i=77;OK");
  assert(x.places == 0);
  x.replies = 0;
  assert(send_part(g, "a=t,f=24,s=1,v=1,I=13", p) == 0);
  check_reply(&x, "I=13;OK");
  assert(send_part(g, "a=p,I=13", "") == 0);
  assert(x.places == 1 && x.pixels[0] == 0xff010203 && x.p.image_id != 0);
  uint32_t ack_id = 0;
  assert(sscanf(x.reply, "\033_Gi=%u,I=13;OK", &ack_id) == 1 &&
         ack_id == x.p.image_id);
  x.replies = 0;
  assert(send_part(g, "a=p,I=999", "") == -1 && errno == ENOENT);
  assert(x.replies == 1 && strstr(x.reply, "I=999;ENOENT:") &&
         !strstr(x.reply, "i=0"));
  free(p);
  kitty_graphics_free(g);
}
static void test_chunking(void) {
  struct seen x = {0};
  struct kitty_graphics *g = new_graphics(&x);
  assert(send_part(g, "a=T,f=24,s=2,v=1,i=8,m=1", "AQID") == 0);
  assert(x.places == 0 && x.replies == 0);
  assert(send_part(g, "m=0", "BAUG") == 0);
  assert(x.places == 1 && x.pixels[1] == 0xff040506);
  check_reply(&x, "i=8;OK");
  x.replies = 0;
  assert(send_part(g, "a=T,f=24,s=1,v=1,i=9,m=1,q=2", "AQID") == 0);
  assert(send_part(g, "m=0,q=0", "") == 0);
  assert(x.replies == 1);
  check_reply(&x, "i=9;OK");
  x.replies = 0;
  assert(send_part(g, "a=T,f=24,s=1,v=1,i=10,m=1", "AQID") == 0);
  assert(send_part(g, "U=1,q=2,m=0", "") == -1 && errno == EINVAL);
  assert(x.replies == 0);
  kitty_graphics_free(g);
}
static void test_zlib_and_errors(void) {
  struct seen x = {0};
  struct kitty_graphics *g = new_graphics(&x);
  unsigned char rgb[] = {12, 34, 56};
  uLongf cap = compressBound(sizeof(rgb));
  unsigned char *z = malloc(cap);
  assert(compress2(z, &cap, rgb, sizeof(rgb), Z_BEST_SPEED) == Z_OK);
  char *p = b64(z, cap);
  assert(send_part(g, "a=T,f=24,s=1,v=1,o=z,i=5", p) == 0);
  assert(x.pixels[0] == 0xff0c2238);
  assert(send_part(g, "a=T,f=24,s=1,v=1,i=6", "***=") == -1 &&
         errno == EBADMSG);
  check_reply(&x, "i=6;EBADMSG:");
  assert(send_part(g, "a=T,f=24,s=1,v=1,o=z,i=7", "AAAA") == -1);
  check_reply(&x, "i=7;");
  assert(send_part(g, "a=T,f=100,i=9", "bm90cG5n") == -1);
  check_reply(&x, "i=9;EBADMSG:");
  assert(send_part(g, "a=T,f=24,s=1,v=1,t=f,i=10", "AQID") == -1 &&
         errno == ENOTSUP);
  assert(send_part(g, "a=f,i=10", "AQID") == -1 && errno == ENOTSUP);
  assert(send_part(g, "a=T,f=24,s=1,v=1,U=1,i=10", "AQID") == -1 &&
         errno == ENOTSUP);
  assert(send_part(g, "a=T,f=24,s=1,v=1,i=10,i=11", "AQID") == -1);
  assert(send_part(g, "a=T,f=24,s=16384,v=16384,i=12", " ") == -1);
  assert(send_part(g, "a=p,i=5,X=2147483648", "") == -1 && errno == EOVERFLOW);
  assert(send_part(g, "a=T,f=24,s=1,v=1,z=-1,i=10", "AQID") == -1 &&
         errno == ENOTSUP);
  x.replies = 0;
  assert(send_part(g, "U=1,a=T,f=24,s=1,v=1,q=2,i=88", "AQID") == -1 &&
         errno == ENOTSUP);
  assert(x.replies == 0);
  assert(send_part(g, "U=1,a=T,f=24,s=1,v=1,q=0,i=89", "AQID") == -1 &&
         errno == ENOTSUP);
  check_reply(&x, "i=89;ENOTSUP:");
  x.replies = 0;
  assert(send_part(g, "a=T,f=24,s=1,v=1,i=90,q=2", "AQID") == 0 &&
         x.replies == 0);
  x.place_errno = ENOMEM;
  assert(send_part(g, "a=T,f=24,s=1,v=1,i=91", "AQID") == -1 &&
         errno == ENOMEM);
  check_reply(&x, "i=91;ENOMEM:");
  free(p);
  free(z);
  kitty_graphics_free(g);
}
static void test_png_quiet_cancel_reset(void) {
  struct seen x = {0};
  struct kitty_graphics *g = new_graphics(&x);
  unsigned char rgba[] = {20, 40, 60, 128};
  png_image im = {0};
  im.version = PNG_IMAGE_VERSION;
  im.width = 1;
  im.height = 1;
  im.format = PNG_FORMAT_RGBA;
  size_t n = 0;
  assert(png_image_write_to_memory(&im, NULL, &n, 0, rgba, 0, NULL));
  unsigned char *png = malloc(n);
  assert(png_image_write_to_memory(&im, png, &n, 0, rgba, 0, NULL));
  char *p = b64(png, n);
  assert(send_part(g, "a=T,f=100,i=33,q=1", p) == 0);
  assert(x.places == 1 && x.replies == 0);
  assert(x.pixels[0] == 0x800a141e);
  assert(send_part(g, "a=q,f=24,s=1,v=1,i=34,q=2", "bad!") == -1);
  assert(x.replies == 0);
  assert(send_part(g, "a=q,f=24,s=1,v=1,i=35,q=2", "AQID") == 0 &&
         x.replies == 0);
  char h[] = "Gf=24,s=1,v=1,i=45,m=1;";
  assert(kitty_graphics_begin(g, h, sizeof(h) - 1) == 0);
  assert(kitty_graphics_put(g, "AQID", 4) == 0);
  assert(kitty_graphics_end(g) == 0);
  kitty_graphics_cancel(g);
  kitty_graphics_reset(g);
  assert(x.removed_id == 0 && x.removed_all);
  free(p);
  free(png);
  kitty_graphics_free(g);
}
int main(void) {
  test_rgb_crop_place_delete();
  test_rgb_formats_query_number();
  test_chunking();
  test_zlib_and_errors();
  test_png_quiet_cancel_reset();
  puts("kitty-graphics tests: ok");
  return 0;
}
