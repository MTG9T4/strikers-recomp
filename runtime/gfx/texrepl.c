/* SPDX-License-Identifier: GPL-3.0-or-later
 * Texture replacement. <game>\textures\<name>.png replaces the game's texture whose content hash <name> starts with (16 hex
 * digits, as the dump names it); a folder <game>\textures\dump makes the game save every texture it loads there, once, as
 * <hash>_<width>x<height>.png (the player's own game; nothing is shipped). STRIKERS_TEXTURES=<folder> moves both.
 *
 * Which textures: ASTC (decoded to 8-bit RGBA on this PC anyway) and 8-bit RGBA / BGRA, 2D, one layer: most of this game's.
 * A replacement of another size is scaled to the texture's size (box filter); smaller mip levels are made from it. The
 * block-compressed formats (BC1-7) would need encoding again and are left as the game has them. Colours are as the dump
 * shows them (sRGB stays sRGB). The PNGs are read with Windows' WIC. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define COBJMACROS
#include <wincodec.h>
#include "tex_decode.h"
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

typedef struct { uint64_t key; char path[MAX_PATH]; } Repl;
static Repl *g_repl;
static int g_nrepl, g_dump, g_on = -1;
static char g_dir[MAX_PATH], g_dump_dir[MAX_PATH];
static SRWLOCK g_lock = SRWLOCK_INIT;
static uint64_t *g_dumped; static int g_ndumped, g_capdumped;
static LONG g_replaced, g_dumps;

static void texrepl_init(void) {
    const char *e = getenv("STRIKERS_TEXTURES");
    if (e && e[0]) snprintf(g_dir, sizeof g_dir, "%s", e);
    else {
        char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
        char *s = strrchr(exe, '\\'); if (s) *s = 0;
        snprintf(g_dir, sizeof g_dir, "%s\\textures", exe);
    }
    snprintf(g_dump_dir, sizeof g_dump_dir, "%s\\dump", g_dir);
    DWORD a = GetFileAttributesA(g_dump_dir);
    g_dump = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\*.png", g_dir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char hex[17]; int ok = 1;
            for (int i = 0; i < 16; i++) { char c = fd.cFileName[i]; if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) ok = 0; hex[i] = c; }
            hex[16] = 0;
            if (!ok) continue;
            Repl *n = realloc(g_repl, sizeof *g_repl * (size_t)(g_nrepl + 1)); if (!n) break; g_repl = n;
            g_repl[g_nrepl].key = _strtoui64(hex, NULL, 16);
            snprintf(g_repl[g_nrepl].path, MAX_PATH, "%s\\%s", g_dir, fd.cFileName);
            g_nrepl++;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    g_on = g_dump || g_nrepl > 0;
    if (g_on) printf("texrepl: %d replacement%s in %s%s\n", g_nrepl, g_nrepl == 1 ? "" : "s", g_dir, g_dump ? "; dumping every texture to textures\\dump" : "");
}
int texrepl_on(void) { if (g_on < 0) texrepl_init(); return g_on; }

/* which formats can take 8-bit RGBA texels: 1 RGBA order, 2 BGRA order, 3 one channel (R8: dumped grey, replaced from red), 0 none */
int texrepl_kind(int nf) {
    if (nf >= 0x79 && nf <= 0x94) return 1;                          /* ASTC: decoded to RGBA8 */
    if (nf == TEX_FMT_RGBA8 || nf == TEX_FMT_RGBA8_SRGB) return 1;
    if (nf == TEX_FMT_BGRA8 || nf == TEX_FMT_BGRA8_SRGB) return 2;
    if (nf == TEX_FMT_R8) return 3;
    if (nf == 0x49) return 4;                                        /* BC4 (one channel, font sheets): dumped grey; replaced by encoding again */
    return 0;
}
uint64_t texrepl_key(const void *data, size_t n, int nf, int w, int h) {
    uint64_t x = 0xcbf29ce484222325ULL ^ ((uint64_t)nf << 40) ^ ((uint64_t)w << 20) ^ (uint64_t)h;
    const uint64_t *q = data; size_t m = n / 8;
    for (size_t i = 0; i < m; i++) { x ^= q[i]; x *= 0x100000001b3ULL; x ^= x >> 29; }
    for (size_t i = m * 8; i < n; i++) { x ^= ((const uint8_t *)data)[i]; x *= 0x100000001b3ULL; }
    return x;
}

static int wic_load(const char *path, uint8_t **out, int *w, int *h) {
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IWICImagingFactory *f = NULL; IWICBitmapDecoder *d = NULL; IWICBitmapFrameDecode *fr = NULL; IWICFormatConverter *cv = NULL;
    wchar_t wp[MAX_PATH]; MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, MAX_PATH);
    int ok = 0;
    if (SUCCEEDED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f)) &&
        SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(f, wp, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &d)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(d, 0, &fr)) && SUCCEEDED(IWICImagingFactory_CreateFormatConverter(f, &cv)) &&
        SUCCEEDED(IWICFormatConverter_Initialize(cv, (IWICBitmapSource *)fr, &GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom))) {
        UINT W = 0, H = 0; IWICFormatConverter_GetSize(cv, &W, &H);
        uint8_t *p = W && H && W <= 16384 && H <= 16384 ? malloc((size_t)W * H * 4) : NULL;
        if (p && SUCCEEDED(IWICFormatConverter_CopyPixels(cv, NULL, W * 4, W * H * 4, p))) { *out = p; *w = (int)W; *h = (int)H; ok = 1; }
        else free(p);
    }
    if (cv) IWICFormatConverter_Release(cv); if (fr) IWICBitmapFrameDecode_Release(fr); if (d) IWICBitmapDecoder_Release(d); if (f) IWICImagingFactory_Release(f);
    if (SUCCEEDED(co)) CoUninitialize();
    return ok;
}
/* box filter from sw x sh to dw x dh (8-bit RGBA) */
static void scale_box(const uint8_t *s, int sw, int sh, uint8_t *d, int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        int y0 = y * sh / dh, y1 = (y + 1) * sh / dh; if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = x * sw / dw, x1 = (x + 1) * sw / dw; if (x1 <= x0) x1 = x0 + 1;
            unsigned acc[4] = {0, 0, 0, 0}, n = 0;
            for (int yy = y0; yy < y1 && yy < sh; yy++) for (int xx = x0; xx < x1 && xx < sw; xx++) { const uint8_t *p = s + ((size_t)yy * sw + xx) * 4; for (int c = 0; c < 4; c++) acc[c] += p[c]; n++; }
            for (int c = 0; c < 4; c++) d[((size_t)y * dw + x) * 4 + c] = (uint8_t)(n ? (acc[c] + n / 2) / n : 0);
        }
    }
}
/* ---- Button pictures (Settings > Audio / controls; strikers.ini button_pictures = auto|switch|xbox|playstation). The game's
 * prompts are 64x64 badges (white, a black border and symbol, a shadow; the game tints them), one texture each. For an Xbox or
 * PlayStation pad they are drawn here instead, in the same style, with Windows' GDI: nothing of the game's art is used and
 * nothing is shipped. Xbox: with the Positional layout Nintendo A is the east button, Xbox B, so A and B (and X and Y) trade
 * letters; with Labels the letters already agree. PlayStation, by position: A circle, B cross, X triangle, Y square. */
enum { G_LETTER, G_CROSS, G_CIRCLE, G_TRIANGLE, G_SQUARE, G_MENU };
typedef struct { uint64_t key; int round, shoulder; } GameGlyph;
static const GameGlyph k_glyphs[] = {        /* the badges' texture hashes in v1.3.2 (the same data for every player) */
    {0x728a1fbd12a7fd53ULL, 1, 0}, {0x417a72318bbffe5dULL, 1, 1}, {0x36f738bf2a27704aULL, 1, 2}, {0x9bfa9c91556b4697ULL, 1, 3},   /* A B X Y */
    {0xf318d10101b14abdULL, 0, 0}, {0xe9b13951102777edULL, 0, 1}, {0xafe62419f693da99ULL, 0, 2}, {0xee255d20bd16175aULL, 0, 3},   /* L R ZL ZR */
    {0x146b80d940c10834ULL, 1, 4},                                                                                                   /* + */
};
int nnpad_picture_kind(int *labels);                    /* input/nnpad_sdl.c */
#include "gfx_options.h"
static int g_pic = -1, g_pic_labels;
static int pictures(void) {                             /* 0 the game's own, 1 Xbox, 2 PlayStation */
    if (g_pic >= 0) return g_pic;
    const GfxOptions *o = gfx_options();
    int labels = 0, kind = nnpad_picture_kind(&labels);
    g_pic = o->button_pictures == 1 ? 0 : o->button_pictures == 2 ? 1 : o->button_pictures == 3 ? 2 : kind;
    g_pic_labels = o->pad_layout >= 0 ? o->pad_layout == 1 : labels;
    static const char *const nm[3] = {"the game's own", "Xbox", "PlayStation"};
    printf("texrepl: button pictures: %s%s\n", nm[g_pic], g_pic == 1 ? (g_pic_labels ? " (Labels: the letters as printed)" : " (Positional: A and B, X and Y trade letters)") : "");
    return g_pic;
}
static void gdi_shape(HDC dc, int what, const wchar_t *text, int round, int ink) {
    /* 256 x 256: the badge 216 across, centred a little up and left; the shadow is the same shape moved down and right */
    /* ink: 0 the badge's fill, 1 its border and symbol, 2 the symbol alone */
    HBRUSH white = (HBRUSH)GetStockObject(WHITE_BRUSH);
    HPEN pen; HGDIOBJ old; HGDIOBJ ob;
    if (ink != 2) {
        pen = CreatePen(PS_SOLID, ink ? 16 : 1, RGB(255, 255, 255)); old = SelectObject(dc, pen);
        ob = SelectObject(dc, ink ? GetStockObject(NULL_BRUSH) : white);
        if (round) Ellipse(dc, 18, 14, 230, 226); else RoundRect(dc, 22, 34, 226, 214, 54, 54);
        SelectObject(dc, ob); SelectObject(dc, old); DeleteObject(pen);
    }
    if (!ink) return;
    HPEN sym = CreatePen(PS_SOLID | PS_GEOMETRIC | PS_ENDCAP_ROUND | PS_JOIN_ROUND, 22, RGB(255, 255, 255));
    LOGBRUSH lb = {BS_SOLID, RGB(255, 255, 255), 0};
    HPEN sym2 = ExtCreatePen(PS_SOLID | PS_GEOMETRIC | PS_ENDCAP_ROUND | PS_JOIN_ROUND, 22, &lb, 0, NULL);
    old = SelectObject(dc, sym2 ? sym2 : sym); ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    switch (what) {
    case G_CROSS: MoveToEx(dc, 78, 74, NULL); LineTo(dc, 170, 166); MoveToEx(dc, 170, 74, NULL); LineTo(dc, 78, 166); break;
    case G_CIRCLE: Ellipse(dc, 72, 68, 176, 172); break;
    case G_TRIANGLE: { POINT p[4] = {{124, 66}, {178, 160}, {70, 160}, {124, 66}}; Polyline(dc, p, 4); } break;
    case G_SQUARE: Rectangle(dc, 78, 74, 170, 166); break;
    case G_MENU: for (int i = 0; i < 3; i++) { MoveToEx(dc, 82, 84 + i * 36, NULL); LineTo(dc, 166, 84 + i * 36); } break;
    default: {
        HFONT f = CreateFontW(text[1] ? -96 : -136, 0, 0, 0, FW_BLACK, 0, 0, 0, DEFAULT_CHARSET, 0, 0, NONANTIALIASED_QUALITY, 0, L"Segoe UI");
        HGDIOBJ of = SelectObject(dc, f); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
        RECT r = {18, round ? 8 : 30, 230, round ? 222 : 216}; DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of); DeleteObject(f);
    } break;
    }
    SelectObject(dc, ob); SelectObject(dc, old); DeleteObject(sym); if (sym2) DeleteObject(sym2);
}
static uint8_t *glyph_make(int what, const wchar_t *text, int round, int w, int h) {
    enum { S = 256 };
    BITMAPINFO bi; memset(&bi, 0, sizeof bi); bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = S; bi.bmiHeader.biHeight = -S; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    HDC dc = CreateCompatibleDC(NULL);
    uint8_t *mask[3] = {NULL, NULL, NULL};          /* shadow, badge, ink: white on black */
    HBITMAP bm[3];
    for (int m = 0; m < 3; m++) {
        void *bits; bm[m] = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0); mask[m] = bits;
        if (!bm[m] || !bits) { DeleteDC(dc); return NULL; }
        HGDIOBJ o = SelectObject(dc, bm[m]); memset(bits, 0, S * S * 4);
        if (m == 0) { SetViewportOrgEx(dc, 12, 14, NULL); gdi_shape(dc, what, text, round, 0); SetViewportOrgEx(dc, 0, 0, NULL); }
        else gdi_shape(dc, what, text, round, m == 2);
        GdiFlush(); SelectObject(dc, o);
    }
    uint8_t *big = malloc(S * S * 4), *out = malloc((size_t)w * h * 4);
    if (big && out) {
        for (int i = 0; i < S * S; i++) {             /* premultiplied: shadow (black, 70%), then the white badge, then the black ink */
            float a = mask[0][4 * i] / 255.0f * 0.7f, c = 0;
            float b = mask[1][4 * i] / 255.0f; c = c * (1 - b) + b; a = a * (1 - b) + b;
            float k = mask[2][4 * i] / 255.0f; c = c * (1 - k); a = a * (1 - k) + k;
            uint8_t v = (uint8_t)(c * 255.0f + 0.5f); big[4 * i] = big[4 * i + 1] = big[4 * i + 2] = v; big[4 * i + 3] = (uint8_t)(a * 255.0f + 0.5f);
        }
        scale_box(big, S, S, out, w, h);
        for (size_t i = 0; i < (size_t)w * h; i++) { unsigned a = out[4 * i + 3]; for (int c = 0; c < 3; c++) out[4 * i + c] = (uint8_t)(a ? (out[4 * i + c] * 255u + a / 2) / a > 255 ? 255 : (out[4 * i + c] * 255u + a / 2) / a : 0); }
    } else { free(out); out = NULL; }
    free(big);
    for (int m = 0; m < 3; m++) DeleteObject(bm[m]);
    DeleteDC(dc);
    return out;
}
static uint8_t *button_picture(uint64_t key, int w, int h) {
    for (size_t i = 0; i < sizeof k_glyphs / sizeof k_glyphs[0]; i++) if (k_glyphs[i].key == key) {
        int p = pictures();
        if (!p) return NULL;
        const GameGlyph *g = &k_glyphs[i];
        if (g->shoulder == 4) return glyph_make(G_MENU, L"", 1, w, h);
        if (!g->round) { static const wchar_t *const xb[4] = {L"LB", L"RB", L"LT", L"RT"}, *const ps[4] = {L"L1", L"R1", L"L2", L"R2"}; return glyph_make(G_LETTER, p == 1 ? xb[g->shoulder] : ps[g->shoulder], 0, w, h); }
        if (p == 2) { static const int ps[4] = {G_CIRCLE, G_CROSS, G_TRIANGLE, G_SQUARE}; return glyph_make(ps[g->shoulder], L"", 1, w, h); }
        if (g_pic_labels) return NULL;              /* Xbox, Labels: the printed letters already agree */
        static const wchar_t *const swap[4] = {L"B", L"A", L"Y", L"X"};
        return glyph_make(G_LETTER, swap[g->shoulder], 1, w, h);
    }
    return NULL;
}
/* The symbol alone (no badge), as coverage d x d, upright: the badge's 212 px square of gdi_shape scaled to d. */
static uint8_t *symbol_mask(int what, const wchar_t *text, int d) {
    enum { S = 256 };
    BITMAPINFO bi; memset(&bi, 0, sizeof bi); bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = S; bi.bmiHeader.biHeight = -S; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    HDC dc = CreateCompatibleDC(NULL); void *bits = NULL;
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    uint8_t *out = bm && bits ? malloc((size_t)d * d) : NULL;
    if (out) {
        HGDIOBJ o = SelectObject(dc, bm); memset(bits, 0, S * S * 4);
        HRGN clip = CreateEllipticRgn(40, 36, 208, 204); SelectClipRgn(dc, clip);     /* only what lies inside the circle */
        gdi_shape(dc, what, text, 1, 2); GdiFlush(); SelectClipRgn(dc, NULL); DeleteObject(clip); SelectObject(dc, o);
        const uint8_t *p = bits;
        for (int y = 0; y < d; y++) for (int x = 0; x < d; x++) {
            int sx0 = 18 + x * 212 / d, sx1 = 18 + (x + 1) * 212 / d, sy0 = 14 + y * 212 / d, sy1 = 14 + (y + 1) * 212 / d; unsigned acc = 0, n = 0;
            for (int yy = sy0; yy < sy1; yy++) for (int xx = sx0; xx < sx1; xx++) { acc += p[(yy * S + xx) * 4]; n++; }
            out[y * d + x] = (uint8_t)(n ? acc / n : 0);
        }
    }
    if (bm) DeleteObject(bm);
    DeleteDC(dc);
    return out;
}
/* The button badges of Nintendo's extension font (the sheet the game's prompts use: "OK (A)"), BC4 512 x 1024, stored upside
 * down: three rows of A, B, X, Y badges, 30 x 31 cells (top-left corners in the stored sheet). Filled: a white disc with the
 * letter cut out; the other two rows: a ring with the letter. Only the inside of each circle is drawn again. */
#define EXT_SHEET 0x11cdfa68d35a48ccULL
static const short k_ext_cells[3][4][2] = {{{441, 266}, {441, 298}, {441, 332}, {441, 365}},
                                          {{567, 100}, {567, 133}, {567, 166}, {567, 199}},
                                          {{987, 133}, {987, 166}, {987, 199}, {987, 232}}};
uint8_t *texrepl_patch(uint64_t key, int w, int h, const uint8_t *rgba) {
    if (key != EXT_SHEET || w != 512 || h != 1024) return NULL;
    int p = pictures();
    if (!p || (p == 1 && g_pic_labels)) return NULL;
    uint8_t *out = malloc((size_t)w * h * 4);
    if (!out) return NULL;
    memcpy(out, rgba, (size_t)w * h * 4);
    static const int ps[4] = {G_CIRCLE, G_CROSS, G_TRIANGLE, G_SQUARE};
    static const wchar_t *const swap[4] = {L"B", L"A", L"Y", L"X"};
    const int cw = 30, ch = 31, d = 30;
    for (int s = 0; s < 3; s++)
        for (int b = 0; b < 4; b++) {
            uint8_t *sym = symbol_mask(p == 2 ? ps[b] : G_LETTER, p == 2 ? L"" : swap[b], d);
            if (!sym) continue;
            int y0 = k_ext_cells[s][b][0], x0 = k_ext_cells[s][b][1], filled = s == 0;
            double cx = x0 + cw / 2.0, cy = y0 + ch / 2.0, inner = d * 0.40;
            for (int y = y0; y < y0 + ch; y++) for (int x = x0; x < x0 + cw; x++) {
                double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
                if (dx * dx + dy * dy >= inner * inner) continue;
                int sx = x - x0, sy = d - 1 - (y - y0); if (sy < 0) sy = 0; if (sy >= d) sy = d - 1;          /* stored upside down */
                int v = sym[sy * d + sx];
                uint8_t c = (uint8_t)(filled ? 255 - v : v);
                uint8_t *px = out + ((size_t)y * w + x) * 4; px[0] = px[1] = px[2] = c;
            }
            free(sym);
        }
    InterlockedIncrement(&g_replaced);
    return out;
}
int texrepl_wants(void) {                           /* nvn_gl.c: hash textures for the button pictures too */
    const GfxOptions *o = gfx_options();
    return o->button_pictures != 1;
}

/* The replacement for key at w x h (8-bit RGBA, malloc'd), or NULL. */
uint8_t *texrepl_find(uint64_t key, int w, int h) {
    if (w == 64 && h == 64) { uint8_t *b = button_picture(key, w, h); if (b) { InterlockedIncrement(&g_replaced); return b; } }
    if (texrepl_on() <= 0 || !g_nrepl) return NULL;
    for (int i = 0; i < g_nrepl; i++) if (g_repl[i].key == key) {
        uint8_t *img; int iw, ih;
        if (!wic_load(g_repl[i].path, &img, &iw, &ih)) { static int n; if (n++ < 10) printf("texrepl: %s could not be read\n", g_repl[i].path); return NULL; }
        if (iw == w && ih == h) { InterlockedIncrement(&g_replaced); return img; }
        uint8_t *o = malloc((size_t)w * h * 4);
        if (o) scale_box(img, iw, ih, o, w, h);
        free(img);
        if (o) InterlockedIncrement(&g_replaced);
        return o;
    }
    return NULL;
}
/* level l of a replacement made from level 0 */
void texrepl_mip(const uint8_t *base, int w0, int h0, uint8_t *out, int w, int h) { scale_box(base, w0, h0, out, w, h); }

void texrepl_dump(uint64_t key, const uint8_t *rgba, int w, int h, int bgra) {
    if (texrepl_on() <= 0 || !g_dump) return;
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < g_ndumped; i++) if (g_dumped[i] == key) { ReleaseSRWLockExclusive(&g_lock); return; }
    if (g_ndumped == g_capdumped) { int c = g_capdumped ? g_capdumped * 2 : 1024; uint64_t *n = realloc(g_dumped, sizeof *n * (size_t)c); if (!n) { ReleaseSRWLockExclusive(&g_lock); return; } g_dumped = n; g_capdumped = c; }
    g_dumped[g_ndumped++] = key;
    ReleaseSRWLockExclusive(&g_lock);
    char path[MAX_PATH]; snprintf(path, sizeof path, "%s\\%016llx_%dx%d.png", g_dump_dir, (unsigned long long)key, w, h);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return;
    uint8_t *tmp = NULL;
    if (bgra == 3) { tmp = malloc((size_t)w * h * 4); if (!tmp) return; for (size_t i = 0; i < (size_t)w * h; i++) { tmp[4 * i] = tmp[4 * i + 1] = tmp[4 * i + 2] = rgba[i]; tmp[4 * i + 3] = 255; } rgba = tmp; }
    else if (bgra) { tmp = malloc((size_t)w * h * 4); if (!tmp) return; for (size_t i = 0; i < (size_t)w * h; i++) { tmp[4 * i] = rgba[4 * i + 2]; tmp[4 * i + 1] = rgba[4 * i + 1]; tmp[4 * i + 2] = rgba[4 * i]; tmp[4 * i + 3] = rgba[4 * i + 3]; } rgba = tmp; }
    if (tex_write_png(path, rgba, w, h) == 0) InterlockedIncrement(&g_dumps);
    free(tmp);
}
/* BC4 (RGTC1 UNORM) from the red channel of 8-bit RGBA: per 4x4 block the lowest and highest value as the end points, eight
 * levels between them (the format's mode with red0 > red1), each texel the nearest. Returns the bytes written (8 a block). */
size_t texrepl_bc4(const uint8_t *rgba, int w, int h, uint8_t *out) {
    size_t n = 0;
    for (int by = 0; by < (h + 3) / 4; by++)
        for (int bx = 0; bx < (w + 3) / 4; bx++) {
            int v[16], lo = 255, hi = 0;
            for (int i = 0; i < 16; i++) { int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2); if (x >= w) x = w - 1; if (y >= h) y = h - 1; v[i] = rgba[((size_t)y * w + x) * 4]; if (v[i] < lo) lo = v[i]; if (v[i] > hi) hi = v[i]; }
            uint8_t *b = out + n; n += 8;
            if (hi == lo) { b[0] = (uint8_t)hi; b[1] = (uint8_t)lo; memset(b + 2, 0, 6); continue; }
            b[0] = (uint8_t)hi; b[1] = (uint8_t)lo;
            uint64_t bits = 0;
            for (int i = 0; i < 16; i++) {
                int t = (v[i] - lo) * 7 * 2 + (hi - lo); t /= 2 * (hi - lo);          /* 0..7 from lo to hi */
                int code = t == 7 ? 0 : t == 0 ? 1 : 8 - t;                            /* codes: 0 = red0 (hi), 1 = red1 (lo), 2..7 = 6/7..1/7 of hi */
                bits |= (uint64_t)code << (3 * i);
            }
            for (int k = 0; k < 6; k++) b[2 + k] = (uint8_t)(bits >> (8 * k));
        }
    return n;
}
void texrepl_report(void) { if (g_on > 0) printf("texrepl: %ld textures replaced, %ld dumped\n", (long)g_replaced, (long)g_dumps); }
