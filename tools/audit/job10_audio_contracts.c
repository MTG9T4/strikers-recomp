/* SPDX-License-Identifier: GPL-3.0-or-later. Exercise the production AudioOut and SDK Opus boundary. */
#include "../../runtime/audio/nnaudio.c"
const GfxOptions *gfx_options(void) { static GfxOptions o; return &o; }       /* speakers = 0: stereo, as the tests expect */

const DynSym dyn_syms[1] = {{0}};
const int dyn_sym_count = 0;
void guest_trap(const char *s, uint64_t a) { fprintf(stderr, "%s %llx\n", s, a); abort(); }
void call_guest(Ctx *c, uint64_t a) { (void)c; (void)a; abort(); }
const char *hostfn_current_name(void) { return "job10 fixture"; }
uint64_t hostfn_address(const char *s, ImportFn f) { (void)s; return (uint64_t)(uintptr_t)f; }
void register_imports(const ImportDef *d, int n) { (void)d; (void)n; }
void trace_printf(const char *s, ...) { (void)s; }
void os_init_event(uint64_t k, int a) { (void)k; (void)a; }
void os_signal_event(uint64_t k) { (void)k; }
const char *guest_thread_name_of(DWORD t) { (void)t; return NULL; }
int guest_thread_info(int i, Ctx **c, const char **n, HANDLE *h) { (void)i; (void)c; (void)n; (void)h; return 0; }
DWORD os_mutex_owner(uint64_t k) { (void)k; return 0; }
long long nvn_front_frame_next(void) { return 0; }

/* Audit 03: the producer's priority rule. A thread at `start` priority appends one buffer; returns its priority after. */
static DWORD WINAPI prio_worker(LPVOID arg) {
    SetThreadPriority(GetCurrentThread(), (int)(intptr_t)arg);
    int16_t pcm[1024] = {0};
    uint64_t buffer[5] = {0, (uint64_t)(uintptr_t)pcm, sizeof pcm, sizeof pcm, 0};
    Ctx c = {0}; c.x[0] = 1; c.x[1] = (uint64_t)(uintptr_t)buffer;
    audio_AppendAudioOutBuffer(&c);
    return (DWORD)(GetThreadPriority(GetCurrentThread()) + 100);
}
__declspec(dllexport) int producer_priority_test(int start, int above_normal_class, int disable) {
    static int ready;
    if (!ready) { InitializeCriticalSection(&g_lock); QueryPerformanceFrequency(&g_freq); ready = 1; }
    _putenv_s("STRIKERS_AUDIO_MUTE", "1"); _putenv_s("STRIKERS_AUDIO_WAV", ""); _putenv_s("STRIKERS_WAV", "");
    _putenv_s("STRIKERS_AUDIO_PRIORITY", disable ? "0" : "");
    DWORD cls = GetPriorityClass(GetCurrentProcess());
    SetPriorityClass(GetCurrentProcess(), above_normal_class ? ABOVE_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
    uint32_t params[2] = {48000, 2};
    Ctx c = {0}; c.x[0] = 1; c.x[2] = (uint64_t)(uintptr_t)params;
    audio_OpenDefaultAudioOut(&c);
    HANDLE t = CreateThread(NULL, 0, prio_worker, (LPVOID)(intptr_t)start, 0, NULL);
    DWORD code = 0;
    WaitForSingleObject(t, INFINITE); GetExitCodeThread(t, &code); CloseHandle(t);
    c.x[0] = 1; audio_CloseAudioOut(&c);
    SetPriorityClass(GetCurrentProcess(), cls);
    return (int)code - 100;
}

__declspec(dllexport) int sdk_decode(const char *dll, const unsigned char *packet, int bytes, int16_t *out, int outbytes,
                                   uint64_t *consumed, int *frames, int channels) {
    _putenv_s("STRIKERS_OPUS_DLL", dll);
    unsigned char self[64] = {0};
    Ctx c = {0};
    c.x[0] = (uint64_t)(uintptr_t)self; c.x[1] = 48000; c.x[2] = channels;
    opus_GetWorkBufferSize(&c);
    size_t size = (size_t)c.x[0];
    void *work = calloc(1, size);
    c.x[0] = (uint64_t)(uintptr_t)self; c.x[1] = 48000; c.x[2] = channels;
    c.x[3] = (uint64_t)(uintptr_t)work; c.x[4] = size;
    opus_Initialize(&c);
    if (c.x[0]) { free(work); return -1; }
    c.x[0] = (uint64_t)(uintptr_t)self; c.x[1] = (uint64_t)(uintptr_t)consumed;
    c.x[2] = (uint64_t)(uintptr_t)frames; c.x[3] = (uint64_t)(uintptr_t)out; c.x[4] = outbytes;
    c.x[5] = (uint64_t)(uintptr_t)packet; c.x[6] = bytes;
    opus_DecodeInterleaved(&c);
    int result = (int)c.x[0];
    free(work);
    return result;
}

__declspec(dllexport) int wav_test(const char *path, int limited, int enabled) {
    _putenv_s("STRIKERS_AUDIO_MUTE", "1");
    _putenv_s("STRIKERS_AUDIO_WAV", enabled ? path : "");
    _putenv_s("STRIKERS_WAV", "");
    _putenv_s("STRIKERS_AUDIO_WAV_SECONDS", limited ? "1" : "");
    InitializeCriticalSection(&g_lock);
    QueryPerformanceFrequency(&g_freq);
    g_volume = 0.5f;
    uint32_t params[2] = {48000, 2};
    Ctx c = {0}; c.x[0] = 1; c.x[2] = (uint64_t)(uintptr_t)params;
    audio_OpenDefaultAudioOut(&c);
    int16_t pcm[1024];
    for (int i = 0; i < 1024; i++) pcm[i] = i & 1 ? -2000 : 2000;
    uint64_t buffer[5] = {0, (uint64_t)(uintptr_t)pcm, sizeof pcm, sizeof pcm, 0};
    int n = limited ? 95 : 3;
    for (int i = 0; i < n; i++) {
        c.x[0] = 1; c.x[1] = (uint64_t)(uintptr_t)buffer;
        audio_AppendAudioOutBuffer(&c);
        if (!c.x[0]) return 1;
        collect_finished(1);
    }
    audio_report(stdout); /* timed shutdown must finalize a partial capture too */
    c.x[0] = 1; audio_CloseAudioOut(&c);
    DeleteCriticalSection(&g_lock);
    return 0;
}
