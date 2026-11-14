/* SPDX-License-Identifier: GPL-3.0-or-later. Real MF samples exercise copy_sample and format 3. */
#include "../../runtime/services/nnmovie_mf.c"

uint64_t hostfn_address(const char *s, ImportFn f) { (void)s; return (uint64_t)(uintptr_t)f; }
void register_imports(const ImportDef *d, int n) { (void)d; (void)n; }
void call_guest(Ctx *c, uint64_t a) { (void)c; (void)a; abort(); }
void input_mark(const char *s) { (void)s; }
void os_signal_event(uint64_t k) { (void)k; }
int fs_host_path(const char *g, char *o, size_t n) { (void)g; (void)o; (void)n; return 0; }
int fs_pkg_read_whole(const char *g, unsigned char **d, size_t *n) { (void)g; (void)d; (void)n; return 0; }   /* no packaged RomFS here */

static int uploads;
static unsigned char native_texture[192];
static unsigned char last_rgba[16];

static void width_(Ctx *c) { c->x[0] = c->x[0] == (uint64_t)(uintptr_t)native_texture ? 2 : 0; }
static void height_(Ctx *c) { width_(c); }
static void format_(Ctx *c) { c->x[0] = 0x25; }
static void write_(Ctx *c) {
    if (c->x[0] != (uint64_t)(uintptr_t)native_texture) abort();
    memcpy(last_rgba, (void *)(uintptr_t)c->x[3], 16);
    uploads++;
}
ImportFn nvn_gl_pick(const char *name) {
    if (!strcmp(name, "nvnTextureGetWidth")) return width_;
    if (!strcmp(name, "nvnTextureGetHeight")) return height_;
    if (!strcmp(name, "nvnTextureGetFormat")) return format_;
    if (!strcmp(name, "nvnTextureWriteTexels")) return write_;
    abort();
}

__declspec(dllexport) int movie_test(void) {
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return 1;
    Dec d = {0}; d.is_video = 1; d.fmt = 3; d.w = d.h = 2;
    unsigned char nv12[6] = {16, 235, 81, 145, 90, 240};
    OutBuf sample = {0};
    IMFMediaBuffer *b = NULL;
    IMFSample *s = NULL;
    BYTE *data = NULL;
    if (FAILED(MFCreateSample(&s)) || FAILED(MFCreateMemoryBuffer(sizeof nv12, &b))) return 2;
    IMFMediaBuffer_Lock(b, &data, NULL, NULL); memcpy(data, nv12, sizeof nv12);
    IMFMediaBuffer_Unlock(b); IMFMediaBuffer_SetCurrentLength(b, sizeof nv12);
    IMFSample_AddBuffer(s, b); sample.s = s;
    memset(native_texture, 0xa5, sizeof native_texture);
    int result = 0;
    if (copy_sample(&d, &sample, native_texture, 8) != 8 || uploads != 1) result |= 4;
    for (int i = 0; i < sizeof native_texture; i++) if (native_texture[i] != 0xa5) result |= 8;
    if (last_rgba[8] < 250 || last_rgba[9] > 4 || last_rgba[10] > 4 || last_rgba[11] != 255) result |= 16;
    if (copy_sample(&d, &sample, native_texture, 7) || uploads != 1) result |= 32;
    d.fmt = 4;
    unsigned char packed[8]; memset(packed, 0xa5, sizeof packed);
    if (copy_sample(&d, &sample, packed, 5) || packed[0] != 0xa5) result |= 64;
    if (copy_sample(&d, &sample, packed, 6) != 6 || memcmp(packed, nv12, 6) || packed[6] != 0xa5) result |= 128;
    /* Short source must be refused even with a large destination. */
    IMFMediaBuffer_SetCurrentLength(b, 4);
    if (copy_sample(&d, &sample, packed, sizeof packed)) result |= 256;
    /* Exercise the public decoder boundary, including native range metadata. */
    IMFMediaBuffer_SetCurrentLength(b, sizeof nv12);
    uint64_t guest[2] = {0, (uint64_t)(uintptr_t)&d};
    unsigned char buffer[0x48]; memset(buffer, 0xa5, sizeof buffer);
    buf_zero((uint64_t)(uintptr_t)buffer);
    wr64((uint64_t)(uintptr_t)buffer + B_BASE, (uint64_t)(uintptr_t)native_texture);
    wr64((uint64_t)(uintptr_t)buffer + B_CAP, 8);
    d.fmt = 3; d.guest = (uint64_t)(uintptr_t)guest; d.nout = 1;
    d.out[0] = sample; d.out[0].state = OUT_CLIENT;
    InitializeCriticalSection(&d.cs); dec_listed(&d, 1);
    Ctx c = {0}; c.x[0] = d.guest; c.x[2] = (uint64_t)(uintptr_t)buffer;
    dec_GetOutputBuffer(&c);
    if (c.x[0] || rd32((uint64_t)(uintptr_t)buffer + B_SIZE) || rd32((uint64_t)(uintptr_t)buffer + B_OFFSET)) result |= 512;
    for (int i = 0x40; i < sizeof buffer; i++) if (buffer[i] != 0xa5) result |= 1024;
    int hint = 0, count = 0;
    c.x[0] = d.guest; c.x[1] = (uint64_t)(uintptr_t)&hint; c.x[2] = (uint64_t)(uintptr_t)&count;
    dec_GetOutputBufferSize(&c);
    if (c.x[0] || hint != 0x2000 || count != 1) result |= 2048;
    wr64((uint64_t)(uintptr_t)buffer + B_CAP, 7);
    c.x[0] = d.guest; c.x[1] = 0; c.x[2] = (uint64_t)(uintptr_t)buffer;
    dec_GetOutputBuffer(&c);
    if (c.x[0] != ST_UNSUPPORTED) result |= 4096;
    dec_listed(&d, 0); DeleteCriticalSection(&d.cs);
    IMFMediaBuffer_Release(b); IMFSample_Release(s); MFShutdown();
    return result;
}
