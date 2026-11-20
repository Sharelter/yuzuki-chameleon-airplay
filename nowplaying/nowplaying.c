#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <math.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "../third_party/stb_image.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "../third_party/stb_truetype.h"

#define FB_PATH "/dev/fb0"
#define FIFO_PATH "/tmp/shairport-sync-metadata"
#define FONT_PATH "/usr/share/nowplaying/nowplaying.ttf"
#define MAX_XML (12 * 1024 * 1024)
#define UI_WIDTH 1280
#define UI_HEIGHT 720

typedef struct {
  int fd;
  uint8_t *map;
  size_t map_len;
  struct fb_fix_screeninfo fix;
  struct fb_var_screeninfo var;
  int width, height, stride_px;
} framebuffer;

typedef struct {
  uint8_t *data;
  size_t size;
  stbtt_fontinfo info;
} font_face;

typedef struct {
  char title[768];
  char artist[768];
  char album[768];
  char source[256];
  char client_ip[128];
  char server_ip[128];
  char volume_text[128];
  int playing;
  int active;
  int volume_percent;
  uint64_t progress_start;
  uint64_t progress_current;
  uint64_t progress_end;
  struct timespec progress_at;
  uint8_t *art;
  int art_w, art_h;
  uint32_t art_average;
} player_state;

static volatile sig_atomic_t running = 1;
static framebuffer fb;
static font_face ui_font;
static uint32_t *canvas;
static player_state state;
static float ui_scale = 1.0f;
static int ui_origin_x;
static int ui_origin_y;
static int render_requested;

static inline int px(float v) { return ui_origin_x + (int)lrintf(v * ui_scale); }
static inline int py(float v) { return ui_origin_y + (int)lrintf(v * ui_scale); }
static inline int ps(float v) { return (int)lrintf(v * ui_scale); }

static inline uint32_t rgb(int r, int g, int b) {
  if (r < 0) r = 0;
  if (g < 0) g = 0;
  if (b < 0) b = 0;
  if (r > 255) r = 255;
  if (g > 255) g = 255;
  if (b > 255) b = 255;
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static inline int red(uint32_t c) { return (c >> 16) & 255; }
static inline int green(uint32_t c) { return (c >> 8) & 255; }
static inline int blue(uint32_t c) { return c & 255; }

static uint32_t mix(uint32_t a, uint32_t b, int amount) {
  int inv = 255 - amount;
  return rgb((red(a) * inv + red(b) * amount) / 255,
             (green(a) * inv + green(b) * amount) / 255,
             (blue(a) * inv + blue(b) * amount) / 255);
}

static void blend_pixel(int x, int y, uint32_t color, int alpha) {
  if ((unsigned)x >= (unsigned)fb.width || (unsigned)y >= (unsigned)fb.height || alpha <= 0)
    return;
  uint32_t *p = &canvas[y * fb.width + x];
  *p = mix(*p, color, alpha > 255 ? 255 : alpha);
}

static int in_round_rect(int px, int py, int x, int y, int w, int h, int radius) {
  if (px < x || py < y || px >= x + w || py >= y + h) return 0;
  int cx = px < x + radius ? x + radius : (px >= x + w - radius ? x + w - radius - 1 : px);
  int cy = py < y + radius ? y + radius : (py >= y + h - radius ? y + h - radius - 1 : py);
  int dx = px - cx, dy = py - cy;
  return dx * dx + dy * dy <= radius * radius;
}

static void round_rect(int x, int y, int w, int h, int radius, uint32_t color, int alpha) {
  x = px(x); y = py(y); w = ps(w); h = ps(h); radius = ps(radius);
  for (int yy = y; yy < y + h; yy++)
    for (int xx = x; xx < x + w; xx++)
      if (in_round_rect(xx, yy, x, y, w, h, radius))
        blend_pixel(xx, yy, color, alpha);
}

static void circle(int cx, int cy, int radius, uint32_t color, int alpha) {
  cx = px(cx); cy = py(cy); radius = ps(radius);
  int r2 = radius * radius;
  for (int y = cy - radius; y <= cy + radius; y++) {
    int dy = y - cy;
    for (int x = cx - radius; x <= cx + radius; x++) {
      int dx = x - cx;
      if (dx * dx + dy * dy <= r2) blend_pixel(x, y, color, alpha);
    }
  }
}

static uint32_t fb_pack(uint32_t c) {
  uint32_t out = 0;
  uint32_t rv = red(c), gv = green(c), bv = blue(c);
  rv >>= 8 - fb.var.red.length;
  gv >>= 8 - fb.var.green.length;
  bv >>= 8 - fb.var.blue.length;
  out |= rv << fb.var.red.offset;
  out |= gv << fb.var.green.offset;
  out |= bv << fb.var.blue.offset;
  if (fb.var.transp.length)
    out |= ((1u << fb.var.transp.length) - 1) << fb.var.transp.offset;
  return out;
}

static void present(void) {
  int page = 0;
  if (fb.var.yres_virtual >= fb.var.yres * 2)
    page = fb.var.yoffset < fb.var.yres ? 1 : 0;
  uint8_t *dst_base = fb.map + (size_t)page * fb.var.yres * fb.fix.line_length;
  for (int y = 0; y < fb.height; y++) {
    uint32_t *dst = (uint32_t *)(dst_base + (size_t)y * fb.fix.line_length);
    uint32_t *src = canvas + y * fb.width;
    for (int x = 0; x < fb.width; x++) dst[x] = fb_pack(src[x]);
  }
  if (fb.var.yres_virtual >= fb.var.yres * 2) {
    struct fb_var_screeninfo v = fb.var;
    v.yoffset = page * fb.var.yres;
    v.activate = FB_ACTIVATE_VBL;
    if (ioctl(fb.fd, FBIOPAN_DISPLAY, &v) == 0) fb.var.yoffset = v.yoffset;
  }
}

static int utf8_next(const char **s) {
  const unsigned char *p = (const unsigned char *)*s;
  if (!*p) return 0;
  int cp;
  if (p[0] < 0x80) {
    cp = p[0]; *s += 1;
  } else if ((p[0] & 0xe0) == 0xc0 && p[1]) {
    cp = ((p[0] & 0x1f) << 6) | (p[1] & 0x3f); *s += 2;
  } else if ((p[0] & 0xf0) == 0xe0 && p[1] && p[2]) {
    cp = ((p[0] & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f); *s += 3;
  } else if ((p[0] & 0xf8) == 0xf0 && p[1] && p[2] && p[3]) {
    cp = ((p[0] & 7) << 18) | ((p[1] & 0x3f) << 12) | ((p[2] & 0x3f) << 6) | (p[3] & 0x3f);
    *s += 4;
  } else {
    cp = '?'; *s += 1;
  }
  return cp;
}

static float text_width(const char *text, float size) {
  float scale = stbtt_ScaleForPixelHeight(&ui_font.info, size * ui_scale);
  float width = 0;
  int previous = 0;
  while (*text) {
    int cp = utf8_next(&text), advance, bearing;
    if (previous) width += stbtt_GetCodepointKernAdvance(&ui_font.info, previous, cp) * scale;
    stbtt_GetCodepointHMetrics(&ui_font.info, cp, &advance, &bearing);
    width += advance * scale;
    previous = cp;
  }
  return width / ui_scale;
}

static void draw_text(const char *text, float x, float baseline, float size, uint32_t color,
                      int alpha) {
  x = px(x);
  baseline = py(baseline);
  float scale = stbtt_ScaleForPixelHeight(&ui_font.info, size * ui_scale);
  int ascent, descent, linegap, previous = 0;
  stbtt_GetFontVMetrics(&ui_font.info, &ascent, &descent, &linegap);
  while (*text) {
    int cp = utf8_next(&text), advance, bearing;
    if (previous) x += stbtt_GetCodepointKernAdvance(&ui_font.info, previous, cp) * scale;
    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(&ui_font.info, cp, scale, scale, &x0, &y0, &x1, &y1);
    int gw = x1 - x0, gh = y1 - y0;
    if (gw > 0 && gh > 0) {
      uint8_t *bitmap = malloc((size_t)gw * gh);
      if (bitmap) {
        stbtt_MakeCodepointBitmap(&ui_font.info, bitmap, gw, gh, gw, scale, scale, cp);
        int ox = (int)x + x0, oy = (int)baseline + y0;
        for (int yy = 0; yy < gh; yy++)
          for (int xx = 0; xx < gw; xx++)
            blend_pixel(ox + xx, oy + yy, color, bitmap[yy * gw + xx] * alpha / 255);
        free(bitmap);
      }
    }
    stbtt_GetCodepointHMetrics(&ui_font.info, cp, &advance, &bearing);
    x += advance * scale;
    previous = cp;
  }
}

static void draw_text_fit(const char *text, float x, float baseline, float preferred,
                          float minimum, float max_width, uint32_t color, int alpha) {
  float size = preferred;
  while (size > minimum && text_width(text, size) > max_width) size -= 1.0f;
  draw_text(text, x, baseline, size, color, alpha);
}

static void draw_background(void) {
  uint32_t accent = state.art ? state.art_average : rgb(255, 48, 88);
  uint32_t top = mix(rgb(14, 13, 16), accent, state.art ? 60 : 20);
  uint32_t bottom = mix(rgb(5, 5, 8), accent, state.art ? 34 : 7);
  float cover_scale = state.art ? fmaxf((float)fb.width / state.art_w,
                                        (float)fb.height / state.art_h) : 1.0f;
  int blur_x = state.art ? state.art_w / 16 : 0;
  int blur_y = state.art ? state.art_h / 16 : 0;
  for (int y = 0; y < fb.height; y++) {
    int t = y * 255 / (fb.height - 1);
    uint32_t gradient = mix(top, bottom, t);
    uint32_t *p = canvas + y * fb.width;
    int sy = state.art ? (int)((y - (fb.height - state.art_h * cover_scale) * 0.5f) /
                               cover_scale) : 0;
    for (int x = 0; x < fb.width; x++) {
      uint32_t c = gradient;
      if (state.art) {
        int sx = (int)((x - (fb.width - state.art_w * cover_scale) * 0.5f) /
                       cover_scale);
        unsigned sum_r = 0, sum_g = 0, sum_b = 0;
        for (int by = -1; by <= 1; by++) {
          int iy = sy + by * blur_y;
          if (iy < 0) iy = 0;
          if (iy >= state.art_h) iy = state.art_h - 1;
          for (int bx = -1; bx <= 1; bx++) {
            int ix = sx + bx * blur_x;
            if (ix < 0) ix = 0;
            if (ix >= state.art_w) ix = state.art_w - 1;
            uint8_t *a = state.art + ((size_t)iy * state.art_w + ix) * 4;
            sum_r += a[0]; sum_g += a[1]; sum_b += a[2];
          }
        }
        c = mix(rgb(sum_r / 9, sum_g / 9, sum_b / 9), gradient, 176 + t / 9);
      }
      p[x] = c;
    }
  }
  circle(1160, -40, 360, accent, state.art ? 10 : 18);
  circle(80, 730, 300, rgb(48, 196, 255), state.art ? 5 : 12);
}

static void draw_note(int cx, int cy, int scale, uint32_t color, int alpha) {
  round_rect(cx + scale / 5, cy - scale, scale / 5, scale, scale / 12, color, alpha);
  round_rect(cx + scale / 5, cy - scale, scale * 3 / 5, scale / 6, scale / 14, color, alpha);
  circle(cx, cy, scale / 4, color, alpha);
  circle(cx + scale * 3 / 5, cy - scale / 8, scale / 4, color, alpha);
  round_rect(cx + scale * 3 / 5, cy - scale, scale / 5, scale * 7 / 8, scale / 12, color, alpha);
}

static int edge_sign(int px0, int py0, int ax, int ay, int bx, int by) {
  return (px0 - bx) * (ay - by) - (ax - bx) * (py0 - by);
}

static void fill_triangle(int ax, int ay, int bx, int by, int cx, int cy,
                          uint32_t color, int alpha) {
  ax = px(ax); ay = py(ay); bx = px(bx); by = py(by); cx = px(cx); cy = py(cy);
  int min_x = ax < bx ? (ax < cx ? ax : cx) : (bx < cx ? bx : cx);
  int max_x = ax > bx ? (ax > cx ? ax : cx) : (bx > cx ? bx : cx);
  int min_y = ay < by ? (ay < cy ? ay : cy) : (by < cy ? by : cy);
  int max_y = ay > by ? (ay > cy ? ay : cy) : (by > cy ? by : cy);
  for (int y = min_y; y <= max_y; y++) {
    for (int x = min_x; x <= max_x; x++) {
      int d1 = edge_sign(x, y, ax, ay, bx, by);
      int d2 = edge_sign(x, y, bx, by, cx, cy);
      int d3 = edge_sign(x, y, cx, cy, ax, ay);
      if (!((d1 < 0 || d2 < 0 || d3 < 0) && (d1 > 0 || d2 > 0 || d3 > 0)))
        blend_pixel(x, y, color, alpha);
    }
  }
}

static void draw_apple_note(int x, int y, int size, uint32_t color, int alpha) {
  int left_x = x + size * 31 / 100;
  int right_x = x + size * 68 / 100;
  int stem_w = size * 9 / 100;
  round_rect(left_x, y + size * 29 / 100, stem_w, size * 43 / 100,
             stem_w / 2, color, alpha);
  round_rect(right_x, y + size * 19 / 100, stem_w, size * 43 / 100,
             stem_w / 2, color, alpha);
  int ltop = y + size * 25 / 100, lbottom = y + size * 37 / 100;
  int rtop = y + size * 16 / 100, rbottom = y + size * 28 / 100;
  fill_triangle(left_x, ltop, right_x + stem_w, rtop, right_x + stem_w, rbottom,
                color, alpha);
  fill_triangle(left_x, ltop, right_x + stem_w, rbottom, left_x, lbottom,
                color, alpha);
  circle(x + size * 27 / 100, y + size * 72 / 100, size * 14 / 100, color, alpha);
  circle(x + size * 64 / 100, y + size * 62 / 100, size * 14 / 100, color, alpha);
}

static void draw_apple_music_icon(int x, int y, int size) {
  round_rect(x + 8, y + 12, size, size, size * 22 / 100, rgb(0, 0, 0), 80);
  int fx = px(x), fy = py(y), fs = ps(size), radius = ps(size * 22 / 100);
  for (int yy = fy; yy < fy + fs; yy++) {
    int t = (yy - fy) * 255 / (fs ? fs : 1);
    uint32_t c = mix(rgb(255, 91, 119), rgb(250, 28, 61), t);
    for (int xx = fx; xx < fx + fs; xx++)
      if (in_round_rect(xx, yy, fx, fy, fs, fs, radius))
        blend_pixel(xx, yy, c, 255);
  }
  draw_apple_note(x, y, size, rgb(255, 255, 255), 255);
}

static void draw_artwork(int x, int y, int size) {
  round_rect(x + 14, y + 20, size, size, 30, rgb(0, 0, 0), 90);
  x = px(x); y = py(y); size = ps(size);
  if (!state.art) {
    for (int yy = y; yy < y + size; yy++)
      for (int xx = x; xx < x + size; xx++)
        if (in_round_rect(xx, yy, x, y, size, size, ps(30)))
          blend_pixel(xx, yy, rgb(35, 42, 68), 255);
    int lx = (int)lrintf((x - ui_origin_x) / ui_scale);
    int ly = (int)lrintf((y - ui_origin_y) / ui_scale);
    int ls = (int)lrintf(size / ui_scale);
    circle(lx + ls / 2, ly + ls / 2, ls / 3, rgb(111, 92, 230), 95);
    draw_note(lx + ls / 2 - 18, ly + ls / 2 + 48, 125, rgb(241, 244, 255), 220);
    return;
  }
  float scale = fmaxf((float)size / state.art_w, (float)size / state.art_h);
  float sw = size / scale, sh = size / scale;
  float sx0 = (state.art_w - sw) * 0.5f, sy0 = (state.art_h - sh) * 0.5f;
  for (int dy = 0; dy < size; dy++) {
    float sy = sy0 + (dy + 0.5f) / scale;
    int iy = (int)sy;
    if (iy >= state.art_h) iy = state.art_h - 1;
    for (int dx = 0; dx < size; dx++) {
      if (!in_round_rect(x + dx, y + dy, x, y, size, size, ps(30))) continue;
      float sx = sx0 + (dx + 0.5f) / scale;
      int ix = (int)sx;
      if (ix >= state.art_w) ix = state.art_w - 1;
      uint8_t *p = state.art + ((size_t)iy * state.art_w + ix) * 4;
      canvas[(y + dy) * fb.width + x + dx] = rgb(p[0], p[1], p[2]);
    }
  }
}

static void format_time(double seconds, char out[16]) {
  if (seconds < 0 || seconds > 24 * 3600) seconds = 0;
  int s = (int)seconds;
  snprintf(out, 16, "%d:%02d", s / 60, s % 60);
}

static void get_progress(double *position, double *duration) {
  *position = *duration = 0;
  if (state.progress_end <= state.progress_start) return;
  *duration = (double)(state.progress_end - state.progress_start) / 44100.0;
  uint64_t current = state.progress_current;
  if (state.playing) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = now.tv_sec - state.progress_at.tv_sec +
                     (now.tv_nsec - state.progress_at.tv_nsec) / 1e9;
    if (elapsed > 0 && elapsed < 3600) current += (uint64_t)(elapsed * 44100.0);
  }
  if (current > state.progress_start)
    *position = (double)(current - state.progress_start) / 44100.0;
  if (*position > *duration) *position = *duration;
}

static void get_local_ip(char out[64]) {
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  strcpy(out, "Wi-Fi connecting");
  if (s < 0) return;
  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, "wlan0", IFNAMSIZ - 1);
  if (ioctl(s, SIOCGIFADDR, &ifr) == 0) {
    struct sockaddr_in *a = (struct sockaddr_in *)&ifr.ifr_addr;
    snprintf(out, 64, "%s", inet_ntoa(a->sin_addr));
  }
  close(s);
}

static void render(void) {
  draw_background();
  time_t now_t = time(NULL);
  struct tm now;
  localtime_r(&now_t, &now);
  char clock_text[32], ip[64];
  strftime(clock_text, sizeof(clock_text), "%H:%M", &now);
  get_local_ip(ip);

  draw_text("Yuzuki Chameleon", 62, 69, 18, rgb(235, 237, 243), 205);
  float cw = text_width(clock_text, 22);
  draw_text(clock_text, (UI_WIDTH - cw) / 2, 69, 22, rgb(235, 237, 243), 205);

  if (!state.active && !state.playing && !state.title[0]) {
    draw_apple_music_icon(UI_WIDTH / 2 - 108, 172, 216);
    const char *ready = "Ready for AirPlay";
    float rw = text_width(ready, 48);
    draw_text(ready, (UI_WIDTH - rw) / 2, 474, 48, rgb(246, 247, 255), 255);
    const char *name = "Select “Yuzuki Chameleon” on your Apple device";
    float nw = text_width(name, 24);
    draw_text(name, (UI_WIDTH - nw) / 2, 520, 24, rgb(171, 183, 217), 235);
    char network[128];
    snprintf(network, sizeof(network), "ONLINE  •  %s", ip);
    float iw = text_width(network, 17);
    round_rect((UI_WIDTH - (int)iw) / 2 - 18, 566, (int)iw + 36, 38, 19,
               rgb(28, 37, 62), 230);
    draw_text(network, (UI_WIDTH - iw) / 2, 592, 17, rgb(110, 224, 176), 255);
    present();
    return;
  }

  /* Screenshot-matched Apple Music composition: player at left, open field at right. */
  draw_artwork(100, 112, 420);

  const char *title = state.title[0] ? state.title : "Waiting for track information";
  const char *artist = state.artist[0] ? state.artist :
                       (state.source[0] ? state.source : "AirPlay");

  double position, duration;
  get_progress(&position, &duration);
  int bar_x = 100, bar_y = 572, bar_w = 420;
  round_rect(bar_x, bar_y, bar_w, 5, 3, rgb(242, 242, 244), 85);
  if (duration > 0) {
    int filled = (int)(bar_w * position / duration);
    if (filled < 5 && position > 0) filled = 5;
    round_rect(bar_x, bar_y, filled, 5, 3, rgb(255, 255, 255), 245);
  }
  char pos[16], remaining[16];
  format_time(position, pos);
  format_time(duration > position ? duration - position : 0, remaining);
  draw_text(pos, bar_x, bar_y + 28, 13, rgb(225, 226, 231), 205);
  char remain_label[20];
  snprintf(remain_label, sizeof(remain_label), "-%s", remaining);
  float rw = text_width(remain_label, 13);
  draw_text(remain_label, bar_x + bar_w - rw, bar_y + 28, 13,
            rgb(225, 226, 231), 205);


  int tx = 638;
  uint32_t status_color = state.playing ? rgb(111, 239, 170) : rgb(255, 202, 95);
  circle(tx + 5, 184, 5, status_color, 255);
  draw_text(state.playing ? "NOW PLAYING" : "AIRPLAY CONNECTED",
            tx + 20, 190, 15, rgb(242, 242, 246), 215);
  draw_text_fit(title, tx, 310, 52, 31, UI_WIDTH - tx - 74,
                rgb(255, 255, 255), 255);
  draw_text_fit(artist, tx, 354, 25, 19, UI_WIDTH - tx - 74,
                rgb(243, 243, 246), 235);
  if (state.album[0])
    draw_text_fit(state.album, tx, 392, 19, 16, UI_WIDTH - tx - 74,
                  rgb(215, 216, 223), 200);

  char source_line[512];
  snprintf(source_line, sizeof(source_line), "%s%s%s",
           state.source[0] ? state.source : "Apple device",
           state.client_ip[0] ? "  •  " : "",
           state.client_ip);
  draw_text("STREAMING FROM", tx, 520, 13, rgb(225, 226, 231), 160);
  draw_text_fit(source_line, tx, 551, 20, 16, UI_WIDTH - tx - 74,
                rgb(250, 250, 252), 220);

  /* Compact top-right volume treatment mirrors the macOS full-screen player. */
  int vb_x = 1012, vb_y = 61, vb_w = 132;
  draw_text("VOL", vb_x - 42, vb_y + 5, 12, rgb(239, 239, 243), 180);
  round_rect(vb_x, vb_y, vb_w, 5, 3, rgb(245, 245, 247), 75);
  round_rect(vb_x, vb_y, vb_w * state.volume_percent / 100, 5, 3,
             rgb(255, 255, 255), 235);
  char volume[24];
  snprintf(volume, sizeof(volume), "%d%%", state.volume_percent);
  draw_text(volume, vb_x + vb_w + 12, vb_y + 6, 13,
            rgb(245, 245, 248), 205);
  present();
}

static int b64_value(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

static uint8_t *base64_decode(const char *input, size_t len, size_t *out_len) {
  uint8_t *out = malloc(len * 3 / 4 + 4);
  if (!out) return NULL;
  size_t n = 0;
  int val = 0, bits = -8;
  for (size_t i = 0; i < len; i++) {
    if (input[i] == '=') break;
    int d = b64_value((unsigned char)input[i]);
    if (d < 0) continue;
    val = (val << 6) | d;
    bits += 6;
    if (bits >= 0) {
      out[n++] = (uint8_t)((val >> bits) & 255);
      bits -= 8;
    }
  }
  *out_len = n;
  return out;
}

static uint32_t fourcc(const char *s) {
  return ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16) |
         ((uint32_t)(uint8_t)s[2] << 8) | (uint8_t)s[3];
}

static void copy_payload(char *dst, size_t dst_size, const uint8_t *data, size_t len) {
  if (!dst_size) return;
  if (len >= dst_size) len = dst_size - 1;
  memcpy(dst, data, len);
  dst[len] = 0;
}

static void update_art(const uint8_t *data, size_t len) {
  int w, h, channels;
  uint8_t *image = stbi_load_from_memory(data, (int)len, &w, &h, &channels, 4);
  if (!image || w <= 0 || h <= 0) {
    if (image) stbi_image_free(image);
    return;
  }
  if (state.art) stbi_image_free(state.art);
  state.art = image;
  state.art_w = w;
  state.art_h = h;
  uint64_t r = 0, g = 0, b = 0, count = 0;
  int step = (w * h > 200000) ? 16 : 4;
  for (int i = 0; i < w * h; i += step) {
    r += image[i * 4]; g += image[i * 4 + 1]; b += image[i * 4 + 2]; count++;
  }
  state.art_average = count ? rgb(r / count, g / count, b / count) : rgb(100, 75, 220);
}

static void process_item(uint32_t type, uint32_t code, const uint8_t *data, size_t len) {
  uint32_t core = fourcc("core"), ssnc = fourcc("ssnc");
  if (type == core) {
    if (code == fourcc("minm")) copy_payload(state.title, sizeof(state.title), data, len);
    else if (code == fourcc("asar")) copy_payload(state.artist, sizeof(state.artist), data, len);
    else if (code == fourcc("asal")) copy_payload(state.album, sizeof(state.album), data, len);
  } else if (type == ssnc) {
    if (code == fourcc("PICT")) update_art(data, len);
    else if (code == fourcc("pbeg") || code == fourcc("prsm")) {
      state.playing = 1; state.active = 1;
    } else if (code == fourcc("pend")) {
      state.playing = 0;
    } else if (code == fourcc("abeg")) {
      state.active = 1;
    } else if (code == fourcc("aend")) {
      state.active = 0; state.playing = 0;
      state.title[0] = state.artist[0] = state.album[0] = 0;
      state.progress_start = state.progress_current = state.progress_end = 0;
      if (state.art) { stbi_image_free(state.art); state.art = NULL; }
    } else if (code == fourcc("snam")) {
      copy_payload(state.source, sizeof(state.source), data, len);
    } else if (code == fourcc("clip")) {
      copy_payload(state.client_ip, sizeof(state.client_ip), data, len);
    } else if (code == fourcc("svip")) {
      copy_payload(state.server_ip, sizeof(state.server_ip), data, len);
    } else if (code == fourcc("pvol")) {
      copy_payload(state.volume_text, sizeof(state.volume_text), data, len);
      double v = strtod(state.volume_text, NULL);
      state.volume_percent = v <= -144 ? 0 : (int)lrint((v + 30.0) * 100.0 / 30.0);
      if (state.volume_percent < 0) state.volume_percent = 0;
      if (state.volume_percent > 100) state.volume_percent = 100;
    } else if (code == fourcc("prgr")) {
      char temp[128];
      copy_payload(temp, sizeof(temp), data, len);
      unsigned long long a, b, c;
      if (sscanf(temp, "%llu/%llu/%llu", &a, &b, &c) == 3) {
        state.progress_start = a; state.progress_current = b; state.progress_end = c;
        clock_gettime(CLOCK_MONOTONIC, &state.progress_at);
      }
    }
  }
  render_requested = 1;
}

static char *find_between(char *start, char *end, const char *open, const char *close,
                          size_t *len) {
  char *a = strstr(start, open);
  if (!a || a >= end) return NULL;
  a += strlen(open);
  char *b = strstr(a, close);
  if (!b || b > end) return NULL;
  *len = (size_t)(b - a);
  return a;
}

static void parse_xml_item(char *item, size_t item_len) {
  char *end = item + item_len;
  size_t n;
  char *type_s = find_between(item, end, "<type>", "</type>", &n);
  if (!type_s || n >= 32) return;
  char number[32];
  memcpy(number, type_s, n); number[n] = 0;
  uint32_t type = (uint32_t)strtoul(number, NULL, 16);
  char *code_s = find_between(item, end, "<code>", "</code>", &n);
  if (!code_s || n >= 32) return;
  memcpy(number, code_s, n); number[n] = 0;
  uint32_t code = (uint32_t)strtoul(number, NULL, 16);
  char *data_s = find_between(item, end, "<data encoding=\"base64\">", "</data>", &n);
  if (data_s) {
    size_t decoded_len = 0;
    uint8_t *decoded = base64_decode(data_s, n, &decoded_len);
    if (decoded) {
      process_item(type, code, decoded, decoded_len);
      free(decoded);
    }
  } else {
    process_item(type, code, NULL, 0);
  }
}

static int init_fb(void) {
  memset(&fb, 0, sizeof(fb));
  fb.fd = open(FB_PATH, O_RDWR);
  if (fb.fd < 0) return -1;
  if (ioctl(fb.fd, FBIOGET_FSCREENINFO, &fb.fix) < 0 ||
      ioctl(fb.fd, FBIOGET_VSCREENINFO, &fb.var) < 0) return -1;
  if (fb.var.bits_per_pixel != 32) {
    fprintf(stderr, "Unsupported framebuffer depth: %u\n", fb.var.bits_per_pixel);
    return -1;
  }
  fb.width = fb.var.xres;
  fb.height = fb.var.yres;
  ui_scale = fminf((float)fb.width / UI_WIDTH, (float)fb.height / UI_HEIGHT);
  ui_origin_x = (fb.width - ps(UI_WIDTH)) / 2;
  ui_origin_y = (fb.height - ps(UI_HEIGHT)) / 2;
  fb.stride_px = fb.fix.line_length / 4;
  fb.map_len = fb.fix.smem_len;
  fb.map = mmap(NULL, fb.map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb.fd, 0);
  if (fb.map == MAP_FAILED) return -1;
  canvas = calloc((size_t)fb.width * fb.height, sizeof(uint32_t));
  return canvas ? 0 : -1;
}

static int load_font(void) {
  FILE *f = fopen(FONT_PATH, "rb");
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  rewind(f);
  ui_font.data = malloc((size_t)size);
  if (!ui_font.data || fread(ui_font.data, 1, (size_t)size, f) != (size_t)size) {
    fclose(f); return -1;
  }
  fclose(f);
  ui_font.size = (size_t)size;
  return stbtt_InitFont(&ui_font.info, ui_font.data,
                        stbtt_GetFontOffsetForIndex(ui_font.data, 0)) ? 0 : -1;
}

static void handle_signal(int sig) {
  (void)sig;
  running = 0;
}

int main(void) {
  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);
  signal(SIGPIPE, SIG_IGN);
  memset(&state, 0, sizeof(state));
  state.volume_percent = 100;
  if (init_fb() < 0) {
    perror("framebuffer");
    return 1;
  }
  if (load_font() < 0) {
    fprintf(stderr, "Unable to load %s\n", FONT_PATH);
    return 1;
  }
  render();

  struct stat st;
  if (lstat(FIFO_PATH, &st) == 0 && !S_ISFIFO(st.st_mode)) {
    fprintf(stderr, "%s exists but is not a FIFO\n", FIFO_PATH);
    return 1;
  }
  if (lstat(FIFO_PATH, &st) < 0 && mkfifo(FIFO_PATH, 0666) < 0 && errno != EEXIST) {
    perror("mkfifo");
    return 1;
  }

  char *xml = malloc(MAX_XML);
  size_t used = 0;
  if (!xml) return 1;
  int fifo = -1;
  time_t last_render = time(NULL);
  while (running) {
    if (fifo < 0) fifo = open(FIFO_PATH, O_RDONLY | O_NONBLOCK);
    struct pollfd pfd = { .fd = fifo, .events = POLLIN };
    int ready = fifo >= 0 ? poll(&pfd, 1, 250) : 0;
    if (ready > 0 && (pfd.revents & POLLIN)) {
      ssize_t n = read(fifo, xml + used, MAX_XML - used - 1);
      if (n > 0) {
        used += (size_t)n;
        xml[used] = 0;
        for (;;) {
          char *start = strstr(xml, "<item>");
          if (!start) {
            if (used > 64) used = 0;
            break;
          }
          char *end = strstr(start, "</item>");
          if (!end) {
            if (start != xml) {
              used -= (size_t)(start - xml);
              memmove(xml, start, used);
            }
            break;
          }
          end += strlen("</item>");
          parse_xml_item(start, (size_t)(end - start));
          size_t consumed = (size_t)(end - xml);
          used -= consumed;
          memmove(xml, end, used);
          xml[used] = 0;
        }
      } else if (n == 0) {
        close(fifo); fifo = -1;
      }
    }
    time_t now = time(NULL);
    int playback_screen = state.active || state.playing || state.title[0];
    if (render_requested || (playback_screen ? now != last_render
                                             : now / 60 != last_render / 60)) {
      render();
      render_requested = 0;
      last_render = now;
    }
  }
  if (fifo >= 0) close(fifo);
  free(xml);
  if (state.art) stbi_image_free(state.art);
  free(ui_font.data);
  free(canvas);
  munmap(fb.map, fb.map_len);
  close(fb.fd);
  return 0;
}
