/* Probe for runtime/gfx/texrepl.c (tools/audit/test_texrepl.py): the BC4 encoder against tex_decode.c's decoder, the content
 * key, the button badges drawn for Xbox and PlayStation, and the patch of the extension font's badges on a sheet (a stand-in
 * made here, or the player's own dumped sheet given on the command line, written back patched for a look). */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../../runtime/gfx/gfx_options.h"
static GfxOptions g_o;
const GfxOptions *gfx_options(void) { return &g_o; }
static int g_kind, g_labels;
int nnpad_picture_kind(int *labels) { *labels = g_labels; return g_kind; }
#include "../../runtime/gfx/texrepl.c"

static int fails;
#define CHECK(c, what) do { if (c) printf("ok   %s\n", what); else { printf("FAIL %s\n", what); fails++; } } while (0)

int main(int argc, char **argv) {
    /* BC4: a gradient and a hard edge, encoded and decoded again */
    enum { W = 64, H = 64 };
    static uint8_t img[W * H * 4], back[W * H * 4], bc[W * H];
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { uint8_t v = (uint8_t)(x < 32 ? x * 8 : (y * 4 + x) & 255); uint8_t *p = img + (y * W + x) * 4; p[0] = v; p[1] = p[2] = 0; p[3] = 255; }
    size_t n = texrepl_bc4(img, W, H, bc);
    CHECK(n == (size_t)(W / 4) * (H / 4) * 8, "BC4: 8 bytes a 4x4 block");
    tex_decode_rgba8(0x49, bc, W, H, back);
    int worst = 0;
    for (int by = 0; by < H / 4; by++) for (int bx = 0; bx < W / 4; bx++) {
        int lo = 255, hi = 0;
        for (int i = 0; i < 16; i++) { int v = img[((by * 4 + i / 4) * W + bx * 4 + i % 4) * 4]; if (v < lo) lo = v; if (v > hi) hi = v; }
        for (int i = 0; i < 16; i++) { int o = ((by * 4 + i / 4) * W + bx * 4 + i % 4) * 4, e = abs(img[o] - back[o]); int allowed = (hi - lo) / 14 + 1; if (e - allowed > worst) worst = e - allowed; }
    }
    CHECK(worst <= 0, "BC4: every texel within half a step of its block's eight levels");
    CHECK(texrepl_key(img, sizeof img, 0x25, W, H) == texrepl_key(img, sizeof img, 0x25, W, H) && texrepl_key(img, sizeof img, 0x25, W, H) != texrepl_key(img, sizeof img, 0x25, W, H - 1), "key: the same for the same texels, another for another shape");

    /* badges: PlayStation, then Xbox with the Positional and the Labels layout */
    g_o.button_pictures = 3; g_o.pad_layout = -1; g_pic = -1;
    uint8_t *a = texrepl_find(0x728a1fbd12a7fd53ULL, 64, 64), *b = texrepl_find(0x417a72318bbffe5dULL, 64, 64);
    CHECK(a && b && memcmp(a, b, 64 * 64 * 4), "PlayStation: A and B get badges, and different ones");
    if (a) { int opaque = 0, white = 0, black = 0; for (int i = 0; i < 64 * 64; i++) { if (a[4 * i + 3] > 200) { opaque++; if (a[4 * i] > 200) white++; if (a[4 * i] < 50) black++; } }
             CHECK(opaque > 1500 && opaque < 4096 && white > 800 && black > 200, "PlayStation A badge: a white disc, a black border and symbol, see-through corners"); }
    free(a); free(b);
    g_o.button_pictures = 2; g_o.pad_layout = 0; g_pic = -1;
    a = texrepl_find(0x728a1fbd12a7fd53ULL, 64, 64);
    CHECK(a != NULL, "Xbox, Positional: A's badge is drawn again (it shows B)");
    free(a);
    g_o.pad_layout = 1; g_pic = -1;
    a = texrepl_find(0x728a1fbd12a7fd53ULL, 64, 64); b = texrepl_find(0xf318d10101b14abdULL, 64, 64);
    CHECK(a == NULL && b != NULL, "Xbox, Labels: A stays the game's; L becomes LB");
    free(a); free(b);
    g_o.button_pictures = 1; g_pic = -1;
    CHECK(texrepl_find(0x728a1fbd12a7fd53ULL, 64, 64) == NULL && texrepl_find(0x9bfa9c91556b4697ULL, 64, 64) == NULL, "Switch: nothing is replaced");

    /* the extension font sheet: only the insides of the twelve badge circles change */
    static uint8_t sheet[512 * 1024 * 4];
    uint8_t *real = NULL; int rw = 0, rh = 0;
    if (argc > 1 && wic_load(argv[1], &real, &rw, &rh) && rw == 512 && rh == 1024) memcpy(sheet, real, sizeof sheet);
    else for (int i = 0; i < 512 * 1024; i++) { sheet[4 * i] = sheet[4 * i + 1] = sheet[4 * i + 2] = (uint8_t)((i * 37) & 255); sheet[4 * i + 3] = 255; }
    free(real);
    g_o.button_pictures = 3; g_o.pad_layout = 0; g_pic = -1;
    uint8_t *p = texrepl_patch(EXT_SHEET, 512, 1024, sheet);
    CHECK(p != NULL, "PlayStation: the font sheet is patched");
    if (p) {
        long changed = 0, outside = 0;
        for (int y = 0; y < 1024; y++) for (int x = 0; x < 512; x++) {
            int in = 0;
            for (int s = 0; s < 3; s++) for (int k = 0; k < 4; k++) if (y >= k_ext_cells[s][k][0] && y < k_ext_cells[s][k][0] + 31 && x >= k_ext_cells[s][k][1] && x < k_ext_cells[s][k][1] + 30) in = 1;
            int diff = memcmp(p + ((size_t)y * 512 + x) * 4, sheet + ((size_t)y * 512 + x) * 4, 3) != 0;
            if (diff) { if (in) changed++; else outside++; }
        }
        CHECK(changed > 12 * 200 && outside == 0, "the sheet: the twelve badges drawn again, nothing outside them touched");
        if (argc > 2) { tex_write_png(argv[2], p, 512, 1024); printf("     patched sheet: %s\n", argv[2]); }
        free(p);
    }
    CHECK(texrepl_patch(EXT_SHEET ^ 1, 512, 1024, sheet) == NULL, "another sheet is left alone");
    printf("%s: %d failed\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
