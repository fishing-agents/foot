#include "kitty-graphics.h"

#include <errno.h>
#include <limits.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define KG_QUOTA (64u * 1024u * 1024u)
#define KG_MAX_DIM 16384u
#define KG_MAX_CONTROL 4096u
#define KG_MAX_B64 ((KG_QUOTA * 4u / 3u) + 16u)
#define KG_MAX_IMAGES 4096u

struct image {
  struct image *next;
  uint32_t id, number, width, height;
  size_t bytes;
  uint32_t *pixels;
};
struct control {
  char action, transport, compression, delete_type;
  uint32_t format, width, height, id, number, placement_id;
  uint32_t crop_x, crop_y, crop_w, crop_h, cols, rows, x_offset, y_offset, size;
  int32_t z_index;
  unsigned quiet, more;
  bool has_id, has_number, has_size, has_quiet, no_cursor;
};
struct kitty_graphics {
  struct kitty_graphics_callbacks cb;
  void *user;
  struct image *images;
  size_t image_bytes;
  uint32_t next_id;
  struct control transfer;
  char *encoded;
  size_t encoded_len, encoded_cap, current_len;
  bool uploading, current, current_more;
  unsigned current_quiet;
};

static void free_images(struct kitty_graphics *g) {
  while (g->images) {
    struct image *i = g->images;
    g->images = i->next;
    free(i->pixels);
    free(i);
  }
  g->image_bytes = 0;
}
static void clear_upload(struct kitty_graphics *g) {
  free(g->encoded);
  g->encoded = NULL;
  g->encoded_len = g->encoded_cap = 0;
  g->uploading = g->current = false;
}
struct kitty_graphics *
kitty_graphics_create(const struct kitty_graphics_callbacks *cb, void *user) {
  if (!cb) {
    errno = EINVAL;
    return NULL;
  }
  struct kitty_graphics *g = calloc(1, sizeof(*g));
  if (g) {
    g->cb = *cb;
    g->user = user;
    g->next_id = 1;
  }
  return g;
}
void kitty_graphics_reset(struct kitty_graphics *g) {
  if (!g)
    return;
  clear_upload(g);
  free_images(g);
  if (g->cb.remove)
    g->cb.remove(g->user, 0, 0, true);
}
void kitty_graphics_free(struct kitty_graphics *g) {
  if (g) {
    clear_upload(g);
    free_images(g);
    free(g);
  }
}

static void reply(struct kitty_graphics *g, const char *text) {
  if (!g->cb.reply)
    return;
  char buf[512];
  int n = snprintf(buf, sizeof(buf), "\033_G%s\033\\", text);
  if (n > 0 && (size_t)n < sizeof(buf))
    g->cb.reply(g->user, buf, (size_t)n);
}
static const char *errname(int e) {
  switch (e) {
  case ENOENT:
    return "ENOENT";
  case ENOSPC:
    return "ENOSPC";
  case ENOTSUP:
    return "ENOTSUP";
  case EOVERFLOW:
    return "EOVERFLOW";
  case EBADMSG:
    return "EBADMSG";
  case EINVAL:
    return "EINVAL";
  case ENOMEM:
    return "ENOMEM";
  case EIO:
    return "EIO";
  case EACCES:
    return "EACCES";
  default:
    return "EINVAL";
  }
}
static void respond_error(struct kitty_graphics *g, const struct control *c,
                          int e, const char *detail) {
  if (!c || c->quiet == 2)
    return;
  char safe[240], msg[400];
  size_t j = 0;
  if (detail)
    for (size_t i = 0; detail[i] && j + 1 < sizeof(safe); i++) {
      unsigned char x = (unsigned char)detail[i];
      safe[j++] = (x >= 32 && x <= 126 && x != ':' ? (char)x : ' ');
    }
  safe[j] = '\0';
  int n;
  if (c->has_number) {
    if (c->placement_id && c->id)
      n = snprintf(msg, sizeof(msg), "i=%u,I=%u,p=%u;%s:%s", c->id, c->number,
                   c->placement_id, errname(e), safe);
    else if (c->placement_id)
      n = snprintf(msg, sizeof(msg), "I=%u,p=%u;%s:%s", c->number,
                   c->placement_id, errname(e), safe);
    else if (c->id)
      n = snprintf(msg, sizeof(msg), "i=%u,I=%u;%s:%s", c->id, c->number,
                   errname(e), safe);
    else
      n = snprintf(msg, sizeof(msg), "I=%u;%s:%s", c->number, errname(e), safe);
  } else if (c->has_id || c->action == 'q') {
    if (c->placement_id)
      n = snprintf(msg, sizeof(msg), "i=%u,p=%u;%s:%s", c->id, c->placement_id,
                   errname(e), safe);
    else
      n = snprintf(msg, sizeof(msg), "i=%u;%s:%s", c->id, errname(e), safe);
  } else
    return;
  if (n > 0 && (size_t)n < sizeof(msg))
    reply(g, msg);
}
static int parse_u32(const char *s, size_t n, uint32_t *out) {
  if (!n)
    return -1;
  uint64_t v = 0;
  for (size_t i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9')
      return -1;
    v = v * 10 + (unsigned)(s[i] - '0');
    if (v > UINT32_MAX)
      return -1;
  }
  *out = (uint32_t)v;
  return 0;
}
static int parse_i32(const char *s, size_t n, int32_t *out) {
  if (!n)
    return -1;
  bool neg = s[0] == '-';
  size_t i = neg ? 1 : 0;
  if (i == n)
    return -1;
  uint64_t v = 0;
  for (; i < n; i++) {
    if (s[i] < '0' || s[i] > '9')
      return -1;
    v = v * 10 + (unsigned)(s[i] - '0');
    if (v > (uint64_t)INT32_MAX + neg)
      return -1;
  }
  *out = neg ? (int32_t)(-(int64_t)v) : (int32_t)v;
  return 0;
}
static bool supported_key(char k) {
  return strchr("aqftsv iIpo mxywhXYcrCzUNSd", k) != NULL && k != ' ';
}
static int parse_control(char *s, size_t n, struct control *c,
                         bool continuation) {
  memset(c, 0, sizeof(*c));
  c->action = 't';
  c->format = 32;
  c->transport = 'd';
  if (!n)
    return EINVAL;
  bool seen[128] = {0}, has_m = false;
  size_t pos = 0;
  while (pos < n) {
    size_t end = pos;
    while (end < n && s[end] != ',')
      end++;
    if (end == pos)
      return EINVAL;
    char *eq = memchr(s + pos, '=', end - pos);
    if (!eq || eq != s + pos + 1 || eq + 1 == s + end)
      return EINVAL;
    char k = s[pos];
    if ((unsigned char)k >= 128 || !supported_key(k))
      return ENOTSUP;
    if (continuation && k != 'm' && k != 'q')
      return EINVAL;
    if (seen[(unsigned char)k])
      return EINVAL;
    seen[(unsigned char)k] = true;
    const char *v = eq + 1;
    size_t vn = (size_t)(s + end - v);
    uint32_t u;
    if (k == 'a' || k == 't' || k == 'o' || k == 'd') {
      if (vn != 1)
        return EINVAL;
      if (k == 'a')
        c->action = *v;
      else if (k == 't')
        c->transport = *v;
      else if (k == 'o')
        c->compression = *v;
      else
        c->delete_type = *v;
    } else if (k == 'z') {
      if (parse_i32(v, vn, &c->z_index))
        return EINVAL;
    } else {
      if (parse_u32(v, vn, &u))
        return EINVAL;
      switch (k) {
      case 'f':
        c->format = u;
        break;
      case 's':
        c->width = u;
        break;
      case 'v':
        c->height = u;
        break;
      case 'i':
        c->id = u;
        c->has_id = true;
        break;
      case 'I':
        c->number = u;
        c->has_number = true;
        break;
      case 'p':
        c->placement_id = u;
        break;
      case 'q':
        if (u > 2)
          return EINVAL;
        c->quiet = u;
        c->has_quiet = true;
        break;
      case 'm':
        if (u > 1)
          return EINVAL;
        c->more = u;
        has_m = true;
        break;
      case 'x':
        c->crop_x = u;
        break;
      case 'y':
        c->crop_y = u;
        break;
      case 'w':
        c->crop_w = u;
        break;
      case 'h':
        c->crop_h = u;
        break;
      case 'X':
        c->x_offset = u;
        break;
      case 'Y':
        c->y_offset = u;
        break;
      case 'c':
        c->cols = u;
        break;
      case 'r':
        c->rows = u;
        break;
      case 'C':
        if (u > 1)
          return EINVAL;
        c->no_cursor = u != 0;
        break;
      case 'U':
        if (u)
          return ENOTSUP;
        break;
      case 'N':
        break;
      case 'S':
        c->size = u;
        c->has_size = true;
        break;
      default:
        return ENOTSUP;
      }
    }
    pos = end + 1;
  }
  if (continuation && !has_m)
    return EINVAL;
  if (c->has_id && c->has_number)
    return EINVAL;
  if (c->transport != 'd')
    return ENOTSUP;
  if (c->compression && c->compression != 'z')
    return ENOTSUP;
  if (c->action != 't' && c->action != 'T' && c->action != 'p' &&
      c->action != 'q' && c->action != 'd')
    return ENOTSUP;
  if (c->action == 'p' && (c->has_id == c->has_number))
    return EINVAL;
  if (c->x_offset > INT_MAX || c->y_offset > INT_MAX || c->cols > INT_MAX ||
      c->rows > INT_MAX)
    return EOVERFLOW;
  if (c->z_index < 0)
    return ENOTSUP;
  if (c->action == 'd')
    return 0;
  if (c->action == 'p')
    return 0;
  if (continuation)
    return 0;
  if (c->format != 24 && c->format != 32 && c->format != 100)
    return ENOTSUP;
  if (c->format != 100 && (!c->width || !c->height))
    return EINVAL;
  if (c->width > KG_MAX_DIM || c->height > KG_MAX_DIM)
    return EOVERFLOW;
  if (c->format != 100 && (uint64_t)c->width * c->height * 4 > KG_QUOTA)
    return EOVERFLOW;
  if (c->format == 100 && c->compression && !c->has_size)
    return EINVAL;
  if (c->format == 100 && c->compression && c->size > KG_QUOTA)
    return EOVERFLOW;
  return 0;
}
static void recover_response_controls(const char *s, size_t n,
                                      struct control *c) {
  bool got_i = false, got_I = false, got_q = false, got_a = false;
  for (size_t pos = 0; pos < n;) {
    size_t end = pos;
    while (end < n && s[end] != ',')
      end++;
    if (end - pos >= 3 && s[pos + 1] == '=') {
      char k = s[pos];
      const char *v = s + pos + 2;
      size_t vn = end - pos - 2;
      uint32_t u;
      if (k == 'a' && !got_a && vn == 1) {
        c->action = *v;
        got_a = true;
      } else if (k == 'i' && !got_i && !parse_u32(v, vn, &u)) {
        c->id = u;
        c->has_id = true;
        got_i = true;
      } else if (k == 'I' && !got_I && !parse_u32(v, vn, &u)) {
        c->number = u;
        c->has_number = true;
        got_I = true;
      } else if (k == 'p' && !c->placement_id && !parse_u32(v, vn, &u))
        c->placement_id = u;
      else if (k == 'q' && !got_q && !parse_u32(v, vn, &u) && u <= 2) {
        c->quiet = u;
        c->has_quiet = true;
        got_q = true;
      }
    }
    pos = end + 1;
  }
}
int kitty_graphics_begin(struct kitty_graphics *g, const char *apc,
                         size_t len) {
  if (!g || !apc || g->current || len < 3 || len > KG_MAX_CONTROL + 2 ||
      apc[0] != 'G' || apc[len - 1] != ';') {
    errno = EINVAL;
    return -1;
  }
  char tmp[KG_MAX_CONTROL + 1];
  size_t n = len - 2;
  memcpy(tmp, apc + 1, n);
  tmp[n] = '\0';
  struct control c;
  int e;
  if (g->uploading) {
    e = parse_control(tmp, n, &c, true);
    if (e) {
      struct control old = g->transfer;
      recover_response_controls(tmp, n, &c);
      if (c.has_quiet)
        old.quiet = c.quiet;
      respond_error(g, &old, e, "Invalid chunk control");
      clear_upload(g);
      errno = e;
      return -1;
    }
    unsigned more = c.more;
    unsigned quiet = c.has_quiet ? c.quiet : g->current_quiet;
    g->transfer.more = more;
    g->current_quiet = quiet;
    g->current_more = more != 0;
  } else {
    e = parse_control(tmp, n, &c, false);
    if (e) {
      recover_response_controls(tmp, n, &c);
      respond_error(g, &c, e, "Invalid control data");
      errno = e;
      return -1;
    }
    if (c.action == 'd' || c.action == 'p') {
      g->transfer = c;
      g->current = true;
      g->current_more = false;
      g->current_quiet = c.quiet;
      return 0;
    }
    g->transfer = c;
    g->uploading = true;
    g->encoded_len = 0;
    g->current_quiet = c.quiet;
    g->current_more = c.more != 0;
  }
  g->current = true;
  g->current_len = 0;
  return 0;
}
int kitty_graphics_put(struct kitty_graphics *g, const char *p, size_t n) {
  if (!g || !g->current || (!p && n)) {
    errno = EINVAL;
    return -1;
  }
  if (!n)
    return 0;
  if (g->transfer.action == 'p' || g->transfer.action == 'd') {
    errno = EINVAL;
    return -1;
  }
  size_t max_encoded = KG_MAX_B64;
  if (!g->transfer.compression && g->transfer.format != 100) {
    uint64_t raw = (uint64_t)g->transfer.width * g->transfer.height *
                   (g->transfer.format == 24 ? 3u : 4u);
    max_encoded = (size_t)(4 * ((raw + 2) / 3));
  }
  if (n > 4096 - g->current_len || n > max_encoded - g->encoded_len ||
      n > KG_MAX_B64 - g->encoded_len) {
    int e = EOVERFLOW;
    struct control c = g->transfer;
    c.quiet = g->current_quiet;
    respond_error(g, &c, e, "Chunk exceeds protocol limit");
    clear_upload(g);
    errno = e;
    return -1;
  }
  size_t need = g->encoded_len + n;
  if (need > g->encoded_cap) {
    size_t cap = g->encoded_cap ? g->encoded_cap : 4096;
    while (cap < need) {
      if (cap > KG_MAX_B64 / 2) {
        cap = KG_MAX_B64;
        break;
      }
      cap *= 2;
    }
    char *q = realloc(g->encoded, cap);
    if (!q)
      return -1;
    g->encoded = q;
    g->encoded_cap = cap;
  }
  memcpy(g->encoded + g->encoded_len, p, n);
  g->encoded_len = need;
  g->current_len += n;
  return 0;
}
static int b64val(unsigned char c) {
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  if (c >= '0' && c <= '9')
    return c - '0' + 52;
  if (c == '+')
    return 62;
  if (c == '/')
    return 63;
  return -1;
}
static int decode64(const char *s, size_t n, unsigned char **out,
                    size_t *outn) {
  if (n % 4)
    return EBADMSG;
  size_t cap = (n / 4) * 3;
  if (cap > KG_QUOTA)
    return EOVERFLOW;
  unsigned char *d = malloc(cap ? cap : 1);
  if (!d)
    return ENOMEM;
  size_t j = 0;
  for (size_t i = 0; i < n; i += 4) {
    int a = b64val(s[i]), b = b64val(s[i + 1]);
    if (a < 0 || b < 0) {
      free(d);
      return EBADMSG;
    }
    bool p2 = s[i + 2] == '=', p3 = s[i + 3] == '=';
    int c = p2 ? 0 : b64val(s[i + 2]), e = p3 ? 0 : b64val(s[i + 3]);
    if (c < 0 || e < 0 || (p2 && !p3) || ((p2 || p3) && i + 4 != n) ||
        (p2 && (b & 15)) || (p3 && !p2 && (c & 3))) {
      free(d);
      return EBADMSG;
    }
    d[j++] = (unsigned char)((a << 2) | (b >> 4));
    if (!p2)
      d[j++] = (unsigned char)((b << 4) | (c >> 2));
    if (!p3)
      d[j++] = (unsigned char)((c << 6) | e);
  }
  *out = d;
  *outn = j;
  return 0;
}
static int inflate_bounded(const unsigned char *in, size_t inlen,
                           unsigned char **out, size_t *outlen,
                           size_t expected) {
  if (expected > KG_QUOTA || inlen > UINT_MAX || expected > UINT_MAX)
    return EOVERFLOW;
  unsigned char *d = malloc(expected ? expected : 1);
  if (!d)
    return ENOMEM;
  z_stream z = {0};
  if (inflateInit(&z) != Z_OK) {
    free(d);
    return EBADMSG;
  }
  z.next_in = (Bytef *)in;
  z.avail_in = (uInt)inlen;
  z.next_out = d;
  z.avail_out = (uInt)(expected ? expected : 1);
  int r = inflate(&z, Z_FINISH);
  size_t got = z.total_out;
  bool ok = r == Z_STREAM_END && z.total_in == inlen && got == expected;
  inflateEnd(&z);
  if (!ok) {
    free(d);
    return EBADMSG;
  }
  *out = d;
  *outlen = got;
  return 0;
}
static uint32_t premul(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  r = (uint8_t)(((unsigned)r * a + 127) / 255);
  g = (uint8_t)(((unsigned)g * a + 127) / 255);
  b = (uint8_t)(((unsigned)b * a + 127) / 255);
  return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
static int make_pixels(struct control *c, const unsigned char *src, size_t n,
                       uint32_t **pixels, uint32_t *w, uint32_t *h) {
  unsigned char *plain = NULL;
  int e = 0;
  if (c->compression) {
    size_t expected;
    if (c->format == 100)
      expected = c->size;
    else {
      uint64_t sz = (uint64_t)c->width * c->height * (c->format == 24 ? 3 : 4);
      if (sz > KG_QUOTA)
        return EOVERFLOW;
      expected = (size_t)sz;
    }
    e = inflate_bounded(src, n, &plain, &n, expected);
    if (e)
      return e;
    src = plain;
  }
  uint32_t sw = c->width, sh = c->height;
  unsigned char *rgba = NULL;
  if (c->format == 100) {
    if (n > KG_QUOTA) {
      free(plain);
      return EOVERFLOW;
    }
    png_image image = {0};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, src, n)) {
      free(plain);
      return EBADMSG;
    }
    sw = image.width;
    sh = image.height;
    if (!sw || !sh || sw > KG_MAX_DIM || sh > KG_MAX_DIM ||
        (uint64_t)sw * sh * 4 > KG_QUOTA) {
      png_image_free(&image);
      free(plain);
      return EOVERFLOW;
    }
    image.format = PNG_FORMAT_RGBA;
    rgba = malloc(PNG_IMAGE_SIZE(image));
    if (!rgba) {
      png_image_free(&image);
      free(plain);
      return ENOMEM;
    }
    if (!png_image_finish_read(&image, NULL, rgba, 0, NULL)) {
      free(rgba);
      png_image_free(&image);
      free(plain);
      return EBADMSG;
    }
    png_image_free(&image);
  } else {
    size_t bpp = c->format == 24 ? 3 : 4;
    uint64_t expected = (uint64_t)sw * sh * bpp;
    if (expected != n || (uint64_t)sw * sh * 4 > KG_QUOTA) {
      free(plain);
      return expected > KG_QUOTA ? EOVERFLOW : EBADMSG;
    }
    rgba = malloc((size_t)sw * sh * 4);
    if (!rgba) {
      free(plain);
      return ENOMEM;
    }
    for (size_t i = 0, j = 0; i < (size_t)sw * sh; i++) {
      rgba[j++] = src[i * bpp];
      rgba[j++] = src[i * bpp + 1];
      rgba[j++] = src[i * bpp + 2];
      rgba[j++] = bpp == 4 ? src[i * bpp + 3] : 255;
    }
  }
  free(plain);
  size_t count = (size_t)sw * sh;
  uint32_t *argb = malloc(count * 4);
  if (!argb) {
    free(rgba);
    return ENOMEM;
  }
  for (size_t i = 0; i < count; i++)
    argb[i] =
        premul(rgba[4 * i], rgba[4 * i + 1], rgba[4 * i + 2], rgba[4 * i + 3]);
  free(rgba);
  *pixels = argb;
  *w = sw;
  *h = sh;
  return 0;
}
static struct image *find_image(struct kitty_graphics *g, uint32_t id,
                                uint32_t number) {
  for (struct image *i = g->images; i; i = i->next)
    if (number ? i->number == number : i->id == id)
      return i;
  return NULL;
}
static uint32_t alloc_id(struct kitty_graphics *g) {
  for (uint64_t k = 0; k < UINT32_MAX; k++) {
    uint32_t id = g->next_id++;
    if (!g->next_id)
      g->next_id = 1;
    if (id && !find_image(g, id, 0))
      return id;
  }
  return 0;
}
static void remove_image(struct kitty_graphics *g, struct image **link,
                         bool all) {
  struct image *i = *link;
  *link = i->next;
  g->image_bytes -= i->bytes;
  if (g->cb.remove)
    g->cb.remove(g->user, i->id, 0, all);
  free(i->pixels);
  free(i);
}
static int store_image(struct kitty_graphics *g, uint32_t id, uint32_t number,
                       uint32_t w, uint32_t h, const uint32_t *px) {
  size_t bytes = (size_t)w * h * 4;
  if (bytes > KG_QUOTA)
    return EOVERFLOW;
  struct image **link = &g->images;
  while (*link) {
    if ((*link)->id == id && id) {
      remove_image(g, link, true);
      break;
    }
    link = &(*link)->next;
  }
  if (g->image_bytes > KG_QUOTA - bytes)
    return ENOSPC;
  unsigned count = 0;
  for (struct image *i = g->images; i; i = i->next)
    count++;
  if (count >= KG_MAX_IMAGES)
    return ENOSPC;
  struct image *i = calloc(1, sizeof(*i));
  if (!i)
    return ENOMEM;
  i->pixels = malloc(bytes);
  if (!i->pixels) {
    free(i);
    return ENOMEM;
  }
  memcpy(i->pixels, px, bytes);
  i->id = id;
  i->number = number;
  i->width = w;
  i->height = h;
  i->bytes = bytes;
  i->next = g->images;
  g->images = i;
  g->image_bytes += bytes;
  return 0;
}
static void send_ack(struct kitty_graphics *g, const struct control *c,
                     uint32_t id, bool ok, const char *error) {
  if (!g->cb.reply || (ok && c->quiet != 0) || (!ok && c->quiet == 2))
    return;
  char msg[400];
  int n;
  if (c->has_number) {
    if (c->placement_id && id)
      n = snprintf(msg, sizeof(msg), "i=%u,I=%u,p=%u;%s", id, c->number,
                   c->placement_id, ok ? "OK" : error);
    else if (c->placement_id)
      n = snprintf(msg, sizeof(msg), "I=%u,p=%u;%s", c->number, c->placement_id,
                   ok ? "OK" : error);
    else if (id)
      n = snprintf(msg, sizeof(msg), "i=%u,I=%u;%s", id, c->number,
                   ok ? "OK" : error);
    else
      n = snprintf(msg, sizeof(msg), "I=%u;%s", c->number, ok ? "OK" : error);
  } else if (c->placement_id)
    n = snprintf(msg, sizeof(msg), "i=%u,p=%u;%s", id, c->placement_id,
                 ok ? "OK" : error);
  else
    n = snprintf(msg, sizeof(msg), "i=%u;%s", id, ok ? "OK" : error);
  if (n > 0 && (size_t)n < sizeof(msg))
    reply(g, msg);
}
static int place_image(struct kitty_graphics *g, const struct control *c,
                       uint32_t image_id, uint32_t w, uint32_t h,
                       const uint32_t *pixels) {
  uint32_t x = c->crop_x, y = c->crop_y;
  if (x >= w || y >= h)
    return EINVAL;
  uint32_t cw = c->crop_w ? c->crop_w : w - x,
           ch = c->crop_h ? c->crop_h : h - y;
  if (cw > w - x)
    cw = w - x;
  if (ch > h - y)
    ch = h - y;
  if (!cw || !ch)
    return EINVAL;
  uint32_t *crop = malloc((size_t)cw * ch * 4);
  if (!crop)
    return ENOMEM;
  for (uint32_t row = 0; row < ch; row++)
    memcpy(crop + (size_t)row * cw, pixels + (size_t)(y + row) * w + x,
           (size_t)cw * 4);
  struct kitty_graphics_placement p = {.id = c->placement_id,
                                       .image_id = image_id,
                                       .width = cw,
                                       .height = ch,
                                       .pixels = crop,
                                       .cols = (int)c->cols,
                                       .rows = (int)c->rows,
                                       .x_offset = (int)c->x_offset,
                                       .y_offset = (int)c->y_offset,
                                       .z_index = c->z_index,
                                       .move_cursor = !c->no_cursor};
  errno = 0;
  bool ok = !g->cb.place || g->cb.place(g->user, &p);
  int e = errno;
  free(crop);
  return ok ? 0 : (e ? e : EIO);
}
static int delete_action(struct kitty_graphics *g, const struct control *c) {
  char d = c->delete_type ? c->delete_type : 'a';
  bool all = (d == 'A' || d == 'I' || d == 'N');
  if (d != 'a' && d != 'A' && d != 'i' && d != 'I' && d != 'n' && d != 'N')
    return ENOTSUP;
  /* Releasing one named placement must retain data if other placements still
   * reference it. Until the callback exposes reference tracking, reject this
   * subset rather than silently deleting the entire image. */
  if (all && c->placement_id)
    return ENOTSUP;
  if ((d == 'i' || d == 'I') && !c->has_id)
    return EINVAL;
  if ((d == 'n' || d == 'N') && !c->has_number)
    return EINVAL;
  if (c->has_number) {
    struct image **p = &g->images;
    while (*p && (*p)->number != c->number)
      p = &(*p)->next;
    if (*p && all)
      remove_image(g, p, true);
    else if (*p && g->cb.remove)
      g->cb.remove(g->user, (*p)->id, c->placement_id, all);
  } else if (c->has_id) {
    struct image **p = &g->images;
    while (*p && (*p)->id != c->id)
      p = &(*p)->next;
    if (*p && all)
      remove_image(g, p, true);
    else if (g->cb.remove)
      g->cb.remove(g->user, c->id, c->placement_id, all);
  } else if (all) {
    while (g->images)
      remove_image(g, &g->images, true);
    /* Anonymous displays have no stored image entry to visit above. */
    if (g->cb.remove)
      g->cb.remove(g->user, 0, 0, true);
  } else if (g->cb.remove)
    g->cb.remove(g->user, 0, 0, false);
  return 0;
}
int kitty_graphics_end(struct kitty_graphics *g) {
  if (!g || !g->current) {
    errno = EINVAL;
    return -1;
  }
  g->current = false;
  struct control c = g->transfer;
  c.quiet = g->current_quiet;
  if (c.action == 'd') {
    int e = delete_action(g, &c);
    if (e) {
      respond_error(g, &c, e, "Unsupported delete mode");
      errno = e;
      return -1;
    }
    return 0;
  }
  if (c.action == 'p') {
    struct image *i = find_image(g, c.id, c.number);
    if (!i) {
      send_ack(g, &c, c.id, false, "ENOENT:Image not found");
      errno = ENOENT;
      return -1;
    }
    int e = place_image(g, &c, i->id, i->width, i->height, i->pixels);
    char error[64];
    if (e)
      snprintf(error, sizeof(error), "%s:Placement failed", errname(e));
    send_ack(g, &c, i->id, e == 0, e ? error : NULL);
    if (e)
      errno = e;
    return e ? -1 : 0;
  }
  if (g->current_more && g->current_len % 4) {
    int e = EBADMSG;
    respond_error(g, &c, e, "Non-final chunk is not base64 aligned");
    clear_upload(g);
    errno = e;
    return -1;
  }
  if (g->current_more) {
    g->uploading = true;
    return 0;
  }
  unsigned char *raw = NULL;
  size_t raw_n = 0;
  int e = decode64(g->encoded, g->encoded_len, &raw, &raw_n);
  clear_upload(g);
  if (e) {
    respond_error(g, &c, e, "Invalid base64 data");
    errno = e;
    return -1;
  }
  uint32_t *pixels = NULL, w = 0, h = 0;
  e = make_pixels(&c, raw, raw_n, &pixels, &w, &h);
  free(raw);
  if (e) {
    respond_error(g, &c, e, "Invalid image data");
    errno = e;
    return -1;
  }
  if (c.action == 'q') {
    send_ack(g, &c, c.id, true, NULL);
    free(pixels);
    return 0;
  }
  uint32_t id = c.id;
  if (c.has_number) {
    id = alloc_id(g);
    if (!id)
      e = ENOSPC;
  }
  if (!e && id)
    e = store_image(g, id, c.has_number ? c.number : 0, w, h, pixels);
  if (!e && c.action == 'T')
    e = place_image(g, &c, id, w, h, pixels);
  if (e) {
    free(pixels);
    respond_error(g, &c, e, "Unable to store or place image");
    errno = e;
    return -1;
  }
  if (c.has_id || c.has_number)
    send_ack(g, &c, id, true, NULL);
  free(pixels);
  return 0;
}
void kitty_graphics_cancel(struct kitty_graphics *g) {
  if (g)
    clear_upload(g);
}
