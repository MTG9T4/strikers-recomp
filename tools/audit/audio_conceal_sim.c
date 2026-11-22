/* SPDX-License-Identifier: GPL-3.0-or-later. Gap concealment of the AudioOut output (nnaudio.c), in virtual time.
 *
 * The production nnaudio.c runs against a simulated waveOut device and a simulated game, one virtual tick per sample:
 *   device  the Windows engine takes 480 frames (10 ms) per period from the headers it was given; a header is marked done
 *           once its last frame has been played (what waveOut does on Windows 11: the probe in the 2026-11-21 commit saw
 *           the last header done at the moment its position reached the end), the play position counts played frames.
 *           A period without enough frames plays silence for the rest: the device ran dry.
 *   game    four AudioOut buffers of 512 frames, as Strikers (StartAudioOut with 4 queued). Its audio thread takes a
 *           released buffer, mixes it (3.2 ms) and appends. Two pacings:
 *           mode 0, game speed: buffer k is not mixed before k * 512 / speed frames of time (jittered by +-25% of a
 *                   buffer): a game at 56 fps whose audio follows its frames, the brief's "producer at 93%";
 *           mode 1, release-paced: mixed as soon as a buffer comes back, after a scheduling delay (exponential, mean
 *                   `delay_ms`): Wwise waiting for the AudioOut event on a busy CPU, as the player's log shows.
 *           Both get four stalls (30, 60, 120, 180 ms of one buffer at 5, 10, 15, 20 s) when `stalls` is set.
 *   audio thread  audio_service() at every done signal and every 2 ms, as audio_thread does.
 * The game's signal is a smooth chord with vibrato and note envelopes, so any discontinuity in the device's output shows
 * as a jump in its second difference ("click"). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <mmsystem.h>
#include <math.h>

static LONGLONG v_now;                                          /* virtual clock, 1 tick = 1 frame at 48 kHz */
static BOOL WINAPI fake_qpc(LARGE_INTEGER *t) { t->QuadPart = v_now; return TRUE; }

#define DEV_MAXQ 128
static unsigned g_period = 480;                                 /* engine period, frames (10 ms) */
#define PERIOD g_period
static struct {
    int open, running;
    WAVEHDR *q[DEV_MAXQ];
    unsigned qn, qoff;                                          /* headers given; frames of q[0] already taken */
    unsigned long long played;                                  /* real frames whose period has finished */
    unsigned long long taken_this_period;                       /* real frames in the period now playing */
    LONGLONG period_start;
    WAVEHDR *done_at_end[DEV_MAXQ];                             /* headers whose last frame is in the period now playing */
    unsigned done_n;
    int signal;
    short *out; size_t out_n, out_cap;                          /* everything the device played, stereo */
    unsigned long long dry;                                     /* frames of silence played while running */
} dev;

static MMRESULT WINAPI fake_open(LPHWAVEOUT h, UINT id, LPCWAVEFORMATEX f, DWORD_PTR cb, DWORD_PTR inst, DWORD fl) {
    (void)id; (void)f; (void)cb; (void)inst; (void)fl;
    dev.open = 1; dev.running = 0; *h = (HWAVEOUT)(uintptr_t)1; return MMSYSERR_NOERROR;
}
static MMRESULT WINAPI fake_pause(HWAVEOUT h) { (void)h; dev.running = 0; return 0; }
static MMRESULT WINAPI fake_restart(HWAVEOUT h) { (void)h; if (!dev.running) { dev.running = 1; dev.period_start = v_now; } return 0; }
static MMRESULT WINAPI fake_volume(HWAVEOUT h, DWORD v) { (void)h; (void)v; return 0; }
static MMRESULT WINAPI fake_prepare(HWAVEOUT h, LPWAVEHDR p, UINT n) { (void)h; (void)n; p->dwFlags |= WHDR_PREPARED; return 0; }
static MMRESULT WINAPI fake_unprepare(HWAVEOUT h, LPWAVEHDR p, UINT n) { (void)h; (void)n; p->dwFlags &= ~WHDR_PREPARED; return 0; }
static MMRESULT WINAPI fake_write(HWAVEOUT h, LPWAVEHDR p, UINT n) {
    (void)h; (void)n;
    if (dev.qn == DEV_MAXQ) return MMSYSERR_NOMEM;
    p->dwFlags &= ~WHDR_DONE; dev.q[dev.qn++] = p; return 0;
}
static MMRESULT WINAPI fake_reset(HWAVEOUT h) {
    (void)h;
    for (unsigned i = 0; i < dev.qn; i++) dev.q[i]->dwFlags |= WHDR_DONE;
    for (unsigned i = 0; i < dev.done_n; i++) dev.done_at_end[i]->dwFlags |= WHDR_DONE;
    dev.qn = dev.qoff = dev.done_n = 0; dev.played = dev.taken_this_period = 0;
    return 0;
}
static MMRESULT WINAPI fake_close(HWAVEOUT h) { (void)h; dev.open = 0; return 0; }
static MMRESULT WINAPI fake_position(HWAVEOUT h, LPMMTIME t, UINT n) {
    (void)h; (void)n;
    LONGLONG into = v_now - dev.period_start;                 /* frames of the current period played so far */
    unsigned long long part = dev.running ? (unsigned long long)(into < (LONGLONG)dev.taken_this_period ? into : (LONGLONG)dev.taken_this_period) : 0;
    t->wType = TIME_SAMPLES; t->u.sample = (DWORD)(dev.played + part); return 0;
}
static void out_push(short l, short r) {
    if (dev.out_n + 2 > dev.out_cap) { dev.out_cap = dev.out_cap ? dev.out_cap * 2 : 1 << 20; dev.out = realloc(dev.out, dev.out_cap * 2); }
    dev.out[dev.out_n++] = l; dev.out[dev.out_n++] = r;
}
/* One engine period boundary: the period that was playing is over (its headers are done), the next 480 frames are taken. */
static void dev_period(void) {
    dev.played += dev.taken_this_period;
    for (unsigned i = 0; i < dev.done_n; i++) { dev.done_at_end[i]->dwFlags |= WHDR_DONE; dev.signal = 1; }
    dev.done_n = 0;
    dev.taken_this_period = 0;
    dev.period_start = v_now;
    unsigned need = PERIOD;
    while (need && dev.qn) {
        WAVEHDR *hd = dev.q[0];
        unsigned frames = hd->dwBufferLength / 4, take = frames - dev.qoff < need ? frames - dev.qoff : need;
        const short *s = (const short *)hd->lpData + (size_t)dev.qoff * 2;
        for (unsigned i = 0; i < take; i++) out_push(s[i * 2], s[i * 2 + 1]);
        dev.qoff += take; need -= take; dev.taken_this_period += take;
        if (dev.qoff == frames) { dev.done_at_end[dev.done_n++] = hd; memmove(dev.q, dev.q + 1, --dev.qn * sizeof dev.q[0]); dev.qoff = 0; }
    }
    for (; need; need--) { out_push(0, 0); dev.dry++; }
}

#define QueryPerformanceCounter fake_qpc
#define waveOutOpen fake_open
#define waveOutPause fake_pause
#define waveOutRestart fake_restart
#define waveOutSetVolume fake_volume
#define waveOutPrepareHeader fake_prepare
#define waveOutUnprepareHeader fake_unprepare
#define waveOutWrite fake_write
#define waveOutReset fake_reset
#define waveOutClose fake_close
#define waveOutGetPosition fake_position
#include "../../runtime/audio/nnaudio.c"

const GfxOptions *gfx_options(void) { static GfxOptions o; return &o; }       /* speakers = 0: stereo */
const DynSym dyn_syms[1] = {{0}};
const int dyn_sym_count = 0;
void guest_trap(const char *s, uint64_t a) { fprintf(stderr, "%s %llx\n", s, a); abort(); }
void call_guest(Ctx *c, uint64_t a) { (void)c; (void)a; abort(); }
const char *hostfn_current_name(void) { return "conceal sim"; }
uint64_t hostfn_address(const char *s, ImportFn f) { (void)s; return (uint64_t)(uintptr_t)f; }
void register_imports(const ImportDef *d, int n) { (void)d; (void)n; }
void trace_printf(const char *s, ...) { (void)s; }
void os_init_event(uint64_t k, int a) { (void)k; (void)a; }
void os_signal_event(uint64_t k) { (void)k; }
const char *guest_thread_name_of(DWORD t) { (void)t; return "sim game audio"; }
int guest_thread_info(int i, Ctx **c, const char **n, HANDLE *h) { (void)i; (void)c; (void)n; (void)h; return 0; }
DWORD os_mutex_owner(uint64_t k) { (void)k; return 0; }
long long nvn_front_frame_next(void) { return 0; }

/* the game's signal: a chord that changes every 0.6 s (phase-continuous), 5 Hz vibrato, a 1.5 Hz swell: smooth, so
 * every jump in the device's output is ours */
static double g_phase[3];
static short source_frame(unsigned long long n) {
    static const double chords[4][3] = {{196.0, 246.9, 293.7}, {220.0, 277.2, 329.6}, {174.6, 220.0, 261.6}, {164.8, 207.7, 246.9}};
    const double *f = chords[(n / 28800) % 4];
    double t = (double)n / 48000.0, vib = 1.0 + 0.01 * sin(2 * 3.141592653589793 * 5.0 * t), v = 0;
    double env = 0.55 + 0.45 * sin(2 * 3.141592653589793 * 1.5 * t);
    for (int k = 0; k < 3; k++) { g_phase[k] += 2 * 3.141592653589793 * f[k] * vib / 48000.0; v += sin(g_phase[k]); }
    return (short)(5000.0 * env * v);
}

static unsigned long long g_rng = 0x9e3779b97f4a7c15ULL;
static double rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (double)(g_rng >> 11) / 9007199254740992.0; }

typedef struct {
    double seconds, dry_ms, fill_ms, faded, lat_p50, lat_p99, lat_max_ms, src_d2_max, out_d2_max;
    double underruns, underrun_ms, starved, appends, fills, clicks, hard_dry_runs, dry_runs;
} SimResult;

__declspec(dllexport) void conceal_sim_period(unsigned frames) { g_period = frames; }   /* a multiple of 48 */

/* mode 0: speed = the game's speed against real time; mode 1: speed unused, delay_ms = mean scheduling delay.
 * conceal_ms: STRIKERS_AUDIO_CONCEAL (0 = the old output) */
__declspec(dllexport) int conceal_sim(int mode, double speed, double delay_ms, double seconds, int conceal_ms, int stalls,
                                      const char *wav_path, SimResult *r) {
    char cv[16] = "";                                           /* < 0: unset, the default */
    if (conceal_ms >= 0) snprintf(cv, sizeof cv, "%d", conceal_ms);
    _putenv_s("STRIKERS_AUDIO_CONCEAL", cv);
    _putenv_s("STRIKERS_AUDIO_MUTE", ""); _putenv_s("STRIKERS_AUDIO_WAV", ""); _putenv_s("STRIKERS_WAV", "");
    _putenv_s("STRIKERS_AUDIO_TIMELINE", ""); _putenv_s("STRIKERS_AUDIO_STALL", ""); _putenv_s("STRIKERS_AUDIO_TONE", "");
    InitializeCriticalSection(&g_lock);
    /* every run starts from a freshly loaded module's state */
    g_conceal = -1; g_low = 0; g_late_fills = 0; g_lag_valid = 0; g_heard = 0; g_open = g_started = 0; g_wave = NULL; g_producer_tid = 0; g_last_append = 0;
    g_underruns = g_underrun_ticks = g_underrun_longest = 0; g_empty_since = 0; g_starved = 0; g_queue_max = 0;
    g_total_appended = g_total_released = g_total_dropped = 0; g_lat_n = 0; memset(g_lat_hist, 0, sizeof g_lat_hist);
    g_gap_longest = 0; g_gaps_late = 0; g_fills = g_fill_frames = g_fill_run_longest = g_fill_faded = 0;
    g_freq.QuadPart = 48000;
    v_now = 1;
    memset(&dev, 0, sizeof dev);
    g_rng = 0x9e3779b97f4a7c15ULL; memset(g_phase, 0, sizeof g_phase);

    uint32_t params[2] = {48000, 2};
    Ctx c = {0}; c.x[0] = 1; c.x[1] = 0; c.x[2] = (uint64_t)(uintptr_t)params;
    audio_OpenDefaultAudioOut(&c);
    static int16_t pcm[4][1024];
    static uint64_t bufs[4][5];
    for (int i = 0; i < 4; i++) {                               /* SetAudioOutBufferInfo + Append, silent, then Start */
        memset(pcm[i], 0, sizeof pcm[i]);
        c.x[0] = (uint64_t)(uintptr_t)bufs[i]; c.x[1] = (uint64_t)(uintptr_t)pcm[i]; c.x[2] = sizeof pcm[i]; c.x[3] = sizeof pcm[i];
        audio_SetAudioOutBufferInfo(&c);
        c.x[0] = 1; c.x[1] = (uint64_t)(uintptr_t)bufs[i]; audio_AppendAudioOutBuffer(&c);
    }
    c.x[0] = 1; audio_StartAudioOut(&c);

    unsigned long long src_n = 0, appends = 0, lat_n = 0;
    static unsigned lat_hist[1000];
    memset(lat_hist, 0, sizeof lat_hist);
    double lat_max = 0;
    uint64_t mixing = 0;
    LONGLONG mix_done = 0, end = (LONGLONG)(seconds * 48000.0), game_start = v_now;
    unsigned long long k_mixed = 0;
    double next_due = 0;
    const double stall_at[4] = {5.0, 10.0, 15.0, 20.0}, stall_ms[4] = {30, 60, 120, 180};
    int stall_next = 0;
    static short src_l[48000 * 120];
    double src_d2_max = 0;
    for (LONGLONG step = 0; v_now < end; step++) {
        v_now += 48;                                            /* 1 ms steps; the engine runs on exact 10 ms boundaries */
        if (dev.running && v_now - dev.period_start >= PERIOD) { LONGLONG keep = v_now; v_now = dev.period_start + PERIOD; dev_period(); v_now = keep; }
        if (dev.signal || step % 2 == 0) {                      /* the audio thread: woken by waveOut, else every 2 ms */
            dev.signal = 0;
            EnterCriticalSection(&g_lock); audio_service(); LeaveCriticalSection(&g_lock);
        }
        for (;;) {                                              /* the game's audio thread */
            if (mixing && v_now >= mix_done) {
                int16_t *p = (int16_t *)(uintptr_t)bufs[0][1];
                for (int i = 0; i < 4; i++) if ((uint64_t)(uintptr_t)bufs[i] == mixing) p = pcm[i];
                for (int i = 0; i < 512; i++) {
                    short v = source_frame(src_n);
                    if (src_n < sizeof src_l / sizeof src_l[0]) src_l[src_n] = v;
                    src_n++;
                    p[i * 2] = v; p[i * 2 + 1] = v;
                }
                c.x[0] = 1; c.x[1] = mixing; audio_AppendAudioOutBuffer(&c);
                appends++;
                MMTIME mt; fake_position(NULL, &mt, sizeof mt);
                double ms = ((double)g_frames_submitted - mt.u.sample) / 48.0;   /* given to the device, not yet played */
                if (ms > lat_max) lat_max = ms;
                lat_hist[ms < 999 ? (int)ms : 999]++; lat_n++;
                mixing = 0;
            }
            if (mixing) break;
            if (mode == 0 && (double)(v_now - game_start) < next_due) break;     /* the game has not got that far */
            c.x[0] = 1; audio_GetReleasedAudioOutBuffer(&c);
            if (!c.x[0]) break;
            mixing = c.x[0];
            double cost = 154.0;                                                 /* 3.2 ms of mixing */
            if (mode == 1) cost += -delay_ms * 48.0 * log(1.0 - rnd());
            if (stalls && stall_next < 4 && (double)v_now / 48000.0 >= stall_at[stall_next]) cost += stall_ms[stall_next++] * 48.0;
            mix_done = v_now + (LONGLONG)cost;
            k_mixed++;
            if (mode == 0) {
                double due = (double)k_mixed * 512.0 / speed + (rnd() - 0.5) * 256.0;
                if (due > next_due) next_due = due;
            }
        }
    }
    /* source smoothness, then the device output's: second differences of the left channel */
    for (unsigned long long i = 2; i < src_n && i < sizeof src_l / sizeof src_l[0]; i++) {
        double d2 = fabs((double)src_l[i] - 2.0 * src_l[i - 1] + src_l[i - 2]);
        if (d2 > src_d2_max) src_d2_max = d2;
    }
    double click_at = 4.0 * src_d2_max + 50.0, out_d2_max = 0;
    unsigned long long clicks = 0, dry_runs = 0, hard_dry = 0;
    size_t frames = dev.out_n / 2, start = 48000;               /* after the first second (the game's silent start) */
    for (size_t i = start; i < frames; i++) {
        double d2 = fabs((double)dev.out[i * 2] - 2.0 * dev.out[(i - 1) * 2] + dev.out[(i - 2) * 2]);
        if (d2 > out_d2_max) out_d2_max = d2;
        if (d2 > click_at) { clicks++; i += 48; }                /* one click per millisecond at most */
    }
    for (size_t i = start; i + PERIOD <= frames; i += PERIOD) {   /* periods of pure silence after sound: dry runs */
        int silent = 1;
        for (size_t k = 0; k < PERIOD && silent; k++) silent = dev.out[(i + k) * 2] == 0;
        int was = 1;
        for (size_t k = 1; k <= 96 && was; k++) was = dev.out[(i - k) * 2] == 0;
        if (silent && !was) {
            dry_runs++;
            int loud = 0;                                       /* the 2 ms before the gap still loud: a hard cut */
            for (size_t k = 1; k <= 96; k++) if (abs(dev.out[(i - k) * 2]) > 300) loud = 1;
            hard_dry += loud;
        }
    }
    r->seconds = seconds; r->dry_ms = dev.dry / 48.0; r->src_d2_max = src_d2_max; r->out_d2_max = out_d2_max;
    r->clicks = (double)clicks; r->dry_runs = (double)dry_runs; r->hard_dry_runs = (double)hard_dry;
    r->underruns = (double)g_underruns; r->underrun_ms = g_underrun_ticks / 48.0; r->starved = (double)g_starved; r->appends = (double)appends;
    r->fills = (double)g_fills; r->fill_ms = g_fill_frames / 48.0; r->faded = (double)g_fill_faded;
    unsigned long long acc = 0; r->lat_p50 = r->lat_p99 = -1;
    for (int i = 0; i < 1000; i++) {
        acc += lat_hist[i];
        if (r->lat_p50 < 0 && acc * 2 >= lat_n) r->lat_p50 = i;
        if (r->lat_p99 < 0 && acc * 100 >= lat_n * 99) r->lat_p99 = i;
    }
    r->lat_max_ms = lat_max;
    if (wav_path && *wav_path) {                                /* what the device played, to listen to */
        FILE *w = fopen(wav_path, "wb");
        if (w) {
            uint32_t data = (uint32_t)(frames * 4), riff = data + 36, rate = 48000, bps = 48000 * 4, fs = 16;
            uint16_t fmt = 1, chn = 2, align = 4, bits = 16;
            fwrite("RIFF", 1, 4, w); fwrite(&riff, 4, 1, w); fwrite("WAVEfmt ", 1, 8, w); fwrite(&fs, 4, 1, w);
            fwrite(&fmt, 2, 1, w); fwrite(&chn, 2, 1, w); fwrite(&rate, 4, 1, w); fwrite(&bps, 4, 1, w);
            fwrite(&align, 2, 1, w); fwrite(&bits, 2, 1, w); fwrite("data", 1, 4, w); fwrite(&data, 4, 1, w);
            fwrite(dev.out, 4, frames, w); fclose(w);
        }
    }
    audio_report(stdout);
    fflush(stdout);
    c.x[0] = 1; audio_StopAudioOut(&c);
    c.x[0] = 1; audio_CloseAudioOut(&c);
    free(dev.out); dev.out = NULL;
    DeleteCriticalSection(&g_lock);
    return 0;
}
