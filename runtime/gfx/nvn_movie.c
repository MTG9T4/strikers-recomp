/* SPDX-License-Identifier: GPL-3.0-or-later
 * movie format 3: blit decoded NV12 to the client's NVNtexture (Buffer.Base).
 * subsdk0+11cc70 / 11c9cc / 124540 / 2fdba8. The capacity is a handle-size
 * marker, not pixel storage. Use the existing synchronous render-queue upload. */
#include "hostrt.h"
#include <stdlib.h>
#include <string.h>

ImportFn nvn_gl_pick(const char *name);

static unsigned char clip(int v) { return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* BT.601/709, full/limited range. All inputs are positive-stride NV12. */
void nvn_movie_rgba(unsigned char *out, const unsigned char *y, const unsigned char *uv,
                    int pitch, int w, int h, int bt709, int full) {
    int cy = full ? 256 : 298;
    int rv = bt709 ? (full ? 403 : 459) : (full ? 359 : 409);
    int gu = bt709 ? (full ? 48 : 55) : (full ? 88 : 100);
    int gv = bt709 ? (full ? 120 : 136) : (full ? 183 : 208);
    int bu = bt709 ? (full ? 475 : 541) : (full ? 454 : 516);
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
        int yy = y[(size_t)j * pitch + i] - (full ? 0 : 16);
        int u = uv[(size_t)(j / 2) * pitch + (i & ~1)] - 128;
        int v = uv[(size_t)(j / 2) * pitch + (i & ~1) + 1] - 128;
        unsigned char *p = out + ((size_t)j * w + i) * 4;
        p[0] = clip((cy * yy + rv * v + 128) >> 8);
        p[1] = clip((cy * yy - gu * u - gv * v + 128) >> 8);
        p[2] = clip((cy * yy + bu * u + 128) >> 8);
        p[3] = 255;
    }
}

int nvn_movie_upload(uint64_t texture, const unsigned char *y, const unsigned char *uv,
                     int pitch, int w, int h, int bt709, int full) {
    Ctx c = {0};
    if (!texture || !y || !uv || w <= 0 || h <= 0 || (w & 1) || pitch < w) return 0;
    c.x[0] = texture; nvn_gl_pick("nvnTextureGetWidth")(&c);
    int tw = (int)c.x[0];
    c.x[0] = texture; nvn_gl_pick("nvnTextureGetHeight")(&c);
    int th = (int)c.x[0];
    c.x[0] = texture; nvn_gl_pick("nvnTextureGetFormat")(&c);
    int fmt = (int)c.x[0];
    /* Native RGBA8 UNORM/SRGB surfaces. Never reinterpret another storage format. */
    if (tw < w || th < h || (fmt != 0x25 && fmt != 0x37 && fmt != 0x38)) {
        static int warned;
        if (!warned++) printf("movie: unsupported native texture %llx: %dx%d format 0x%x for %dx%d\n",
                              (unsigned long long)texture, tw, th, fmt, w, h);
        return 0;
    }
    unsigned char *rgba = malloc((size_t)w * h * 4);
    if (!rgba) return 0;
    nvn_movie_rgba(rgba, y, uv, pitch, w, h, bt709, full);
    int region[6] = {0, 0, 0, w, h, 1};
    memset(&c, 0, sizeof c);
    c.x[0] = texture; c.x[2] = (uint64_t)(uintptr_t)region; c.x[3] = (uint64_t)(uintptr_t)rgba;
    nvn_gl_pick("nvnTextureWriteTexels")(&c);
    free(rgba);
    return 1;
}
