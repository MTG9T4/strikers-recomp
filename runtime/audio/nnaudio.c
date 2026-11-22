/* SPDX-License-Identifier: GPL-3.0-or-later
 * Upstream: ssbu-recomp/runtime/nnaudio.c (de221d22f867406b4204dbd580404f603ee19b1a).
 * Adapted for Strikers; see docs/phase2-runtime.md. */
/* Historical [game]/[sdk] addresses below describe the upstream reference, not
 * Strikers. Guest call targets use generated symbols; SDK 13.3 object ABIs still
 * require validation against the target. */
/* nn::audio (AudioOut: raw PCM output) on Windows waveOut, and nn::codec::HardwareOpusDecoder.
 *
 * What the game does (read off its code, addresses are guest addresses):
 *   0x710373ec20  InitializeAudioOutParameter(&p); p.sampleRate (u32 at +0) = 48000; p.channelCount (u16 at +4) = n;
 *                 OpenDefaultAudioOut(&audioOut 0x7106dd48a8, &systemEvent 0x7106dd4860, p) -> Result (0 = ok);
 *                 then GetAudioOutChannelCount / SampleRate / SampleFormat. The format's value is ignored: the
 *                 mixer always writes 16-bit signed samples (0x710398b0fc: float * 32767 -> strh).
 *                 It allocates N buffers (N at 0x7106dd489c, clamped to 2..16) of 512 frames
 *                 (512 * channels * 2 bytes, capacity rounded up to 4 KB) and for each calls
 *                 SetAudioOutBufferInfo(buf, data, capacity, dataSize) and AppendAudioOutBuffer.
 *                 The AudioOutBuffer array has a stride of 0x28 bytes.
 *   0x7103987890  thread "nus3::audio update": StartAudioOut, then forever:
 *                 b = GetReleasedAudioOutBuffer(); if null: TimedWaitSystemEvent(0x7106dd4860, 32 ms) and retry;
 *                 else mix 512 frames into GetAudioOutBufferDataPointer(b) and AppendAudioOutBuffer(b).
 *                 The event is waited on at the SystemEvent's own address, so SystemEventType is its first member.
 *                 On shutdown: StopAudioOut, CloseAudioOut, GetAudioOutBufferDataPointer of every buffer to free it.
 *   0x710373f8b0  AcquireAudioDeviceNotificationForOutput(&event); a thread "AudioDeviceChange" waits on it and
 *                 calls GetActiveAudioDeviceName(char name[0x100]) (used with strcpy/strlen: a C string).
 *   0x71022bca24  SetAudioDeviceOutputVolume(name, volume): "AudioTvOutput" 1.0, "AudioStereoJackOutput" 1.4,
 *                 "AudioBuiltInSpeakerOutput" 2.0, "AudioUsbDeviceOutput" 1.0.
 *
 * AudioOutBuffer is only touched through these functions, so the field order inside its 0x28 bytes is ours
 * (the same as in the public SDK headers): +0 next, +8 data, +0x10 capacity, +0x18 data size, +0x20 data offset.
 *
 * Host side: every appended buffer is copied (6 channels are mixed down to stereo) and handed to waveOut at once.
 * A buffer is "released" when the device has finished playing its copy, in the order it was appended; the
 * guest's event is signalled then. Without an output device the buffers are released by the clock instead.
 * When the game falls behind real time, the gaps are filled from its recent sound (gap concealment, see g_conceal);
 * a buffer may then be released up to 20 ms before its copy has played.
 *
 * Environment: STRIKERS_AUDIO_WAV=<file>    stereo PCM16 capture before host device volume; off by default
 *              STRIKERS_AUDIO_WAV_SECONDS=n optional sample-exact limit (otherwise RIFF size limit)
 *              STRIKERS_WAV=<file>          legacy capture alias, first 30 seconds
 *              STRIKERS_AUDIO_CHANNELS=6    grant 6 channels when the game asks for them (default: stereo, the game
 *                                       then does its own mix-down, as on a console set to stereo output)
 *              STRIKERS_AUDIO_LOG=1         print the statistics every second instead of every 10 s
 *              STRIKERS_AUDIO_MUTE=1        no host device (clock-driven)
 *              STRIKERS_AUDIO_CONCEAL=<ms>  gap concealment's starting low-water mark (default 12; 0 = off, as before)
 *              STRIKERS_AUDIO_TONE=1        add a quiet 440 Hz test tone to the first second (checks the host path)
 *              STRIKERS_OPUS_DLL=<file>     libopus DLL for the Opus decoder (default libopus-0.dll, then MSYS2's)
 *
 * HardwareOpusDecoder: the game only imports the constructor; everything else goes through the object's
 * virtual table (0x71039937b8 and following): +0x00 destructor, +0x10 GetWorkBufferSize(sampleRate, channels),
 * +0x20 Initialize(sampleRate, channels, buffer, size) -> 0 = ok, +0x28 Finalize,
 * +0x48 DecodeInterleaved(size_t *consumed, int *sampleCount, int16_t *out, size_t outBytes, in, inBytes) -> 0 = ok.
 * Each input packet starts with an 8-byte header (big-endian u32 payload length, u32 final range) as in the
 * public descriptions of the console's Opus service. Decoding is done by libopus, loaded at run time; without
 * it initialization returns failure. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <mmsystem.h>
#include <objbase.h>
#include <propkeydef.h>
#include <intrin.h>
#include "host_cpu.h"
#include "hostrt.h"
#define COBJMACROS
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>
#include "../gfx/gfx_options.h"
#pragma comment(lib, "ole32.lib")
/* the four GUIDs used here, defined locally (no INITGUID in this program) */
static const GUID au_CLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID au_IID_IMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID au_IID_IAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const GUID au_SUBTYPE_PCM = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define A5 (c->x[5])
#define A6 (c->x[6])
#define RET(v) (c->x[0] = (uint64_t)(v))

void os_init_event(uint64_t key, int autoclear);     /* os_threads.c */
void os_signal_event(uint64_t key);

/* ---------------------------------------------------------------- AudioOut */
#define BUF_NEXT 0x00
#define BUF_DATA 0x08
#define BUF_CAPACITY 0x10
#define BUF_SIZE 0x18
#define BUF_OFFSET 0x20
#define SAMPLE_FORMAT_PCM16 2                         /* nn::audio::SampleFormat_PcmInt16 */
#define SLOTS 64                                      /* more than the game's maximum of 16 buffers */
#define WAV_SECONDS 30

typedef struct {
    WAVEHDR hdr;
    int16_t *pcm;                                     /* host copy, stereo */
    size_t capacity;                                  /* bytes allocated for pcm */
    uint64_t guest;                                   /* the guest's AudioOutBuffer */
    LONGLONG due;                                     /* clock-driven mode: when it has been "played" */
    unsigned frames;
    int prepared;
    int given;                                        /* the guest's buffer was given back (before its header finished: credit) */
    unsigned long long end_pos;                       /* device frames submitted up to the end of this slot */
} Slot;

static CRITICAL_SECTION g_lock;
static uint64_t g_audio_out, g_event, g_device_event;
static int g_rate = 48000, g_channels = 2, g_open, g_started;
/* 5.1: the game mixes FL FR FC LFE RL RR when the device it asks about has 6 channels (GetActiveAudioDeviceChannelCount
 * ForOutput); g_out_ch is what goes to Windows: 6 then (WAVEFORMATEXTENSIBLE, KSAUDIO_SPEAKER_5POINT1), else 2. */
static int g_out_ch = 2;
#define OUT_BPF ((unsigned)g_out_ch * 2u)          /* bytes of one output frame */
/* strikers.ini speakers = auto (default: 5.1 when Windows' default output has 6 or more channels), stereo, 5.1;
 * STRIKERS_AUDIO_CHANNELS=6 or 2 overrides. Decided once. */
static int surround_on(void) {
    static int on = -1;
    if (on >= 0) return on;
    const char *e = getenv("STRIKERS_AUDIO_CHANNELS");
    int mode = e && atoi(e) == 6 ? 1 : e && atoi(e) == 2 ? 0 : gfx_options()->speakers;
    if (mode >= 0) { on = mode; return on; }
    on = 0;
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = NULL; IMMDevice *dev = NULL; IAudioClient *ac = NULL; WAVEFORMATEX *mix = NULL;
    if (SUCCEEDED(CoCreateInstance(&au_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &au_IID_IMMDeviceEnumerator, (void **)&en)) &&
        SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev)) &&
        SUCCEEDED(IMMDevice_Activate(dev, &au_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac)) &&
        SUCCEEDED(IAudioClient_GetMixFormat(ac, &mix)) && mix) {
        on = mix->nChannels >= 6;
        printf("nn::audio: Windows' default output has %u channels: %s\n", (unsigned)mix->nChannels, on ? "5.1 surround" : "stereo");
    }
    if (mix) CoTaskMemFree(mix);
    if (ac) IAudioClient_Release(ac); if (dev) IMMDevice_Release(dev); if (en) IMMDeviceEnumerator_Release(en);
    if (SUCCEEDED(co)) CoUninitialize();
    return on;
}
static HWAVEOUT g_wave;
static HANDLE g_wake;
static Slot g_slots[SLOTS];
static unsigned g_head, g_tail;                       /* playing queue: slots head..tail-1 (mod SLOTS) */
static uint64_t g_released[SLOTS];
static unsigned g_rel_head, g_rel_tail;
static LARGE_INTEGER g_freq;
static LONGLONG g_clock_due;
static float g_volume = 1.0f;                         /* output volume of the active device */
static int g_host_percent = -1;                      /* U1: the host volume set at open (0..100), -1 before; nnaudio_set_volume changes it */
static const char g_device_name[] = "AudioTvOutput";
/* statistics, protected by g_lock */
static unsigned long long g_total_appended, g_total_released, g_total_dropped, g_nonsilent_seconds;
static unsigned g_sec_appended, g_sec_released, g_sec_peak, g_sec_polls, g_sec_empty;
/* Underruns (bounded round 2026-10-09): the device played every buffer it had while the output was started, i.e. the
 * game did not append the next one in time; the listener hears a gap until the next AppendAudioOutBuffer. Counted from
 * the first buffer with sound on (before it the game's mixer may legitimately idle). audio_report prints them. */
static unsigned long long g_underruns, g_underrun_ticks, g_underrun_longest;
static LONGLONG g_empty_since;
static unsigned g_queue_max;
/* A second, stricter measure: at an append, the DEVICE's own play position (waveOutGetPosition, samples) had already
 * reached the end of everything handed to it: the output really ran out of samples (waveOut marks a buffer "done" when
 * its own pipeline has taken it, which can be earlier than the moment it is heard). */
static unsigned long long g_frames_submitted, g_starved;
/* Gap concealment (2026-11-21, player report on a Ryzen Z1 Extreme: the game ran at 56-58 fps, CPU-bound, so Wwise
 * mixed only 93-96% of real time and the device ran dry 681 times, 7.4 s in all; every dry spell is a gap plus a click
 * at each end). The output now holds back the newest CONCEAL_XFADE frames of the game's stream. When the device's play
 * position comes within g_low frames of the end of what it was given (waveOut marks the last header done only once it
 * has been played, measured on Windows 11, so an empty header queue is already too late), the audio thread queues a
 * host-only "fill" header: the last L frames again (L = 5..20 ms, picked by correlation so the loop point lines up with the held-back tail, i.e. a
 * pitch-synchronous repeat), entered through a raised-cosine crossfade from the tail. The fill ends exactly where the
 * tail begins, so the game's next buffer continues it without a seam. A fill that goes on (the game stalled) keeps full
 * gain for CONCEAL_FULL frames, then fades to silence over CONCEAL_FADE; after a fade the game's audio ramps back in
 * over CONCEAL_RAMP. The low-water mark starts at 12 ms (a 10 ms Windows engine period and the 2 ms check) and rises by
 * 4 ms, to 40 at most, each time a fill came too late (the device had played everything): devices with longer periods
 * tune it themselves. Latency: the device keeps about g_low queued, and a fill (at most 20 ms) plus 2.7 ms ride on the
 * game's own queue. Never on the game's thread: the guest-side check only wakes the audio thread.
 * STRIKERS_AUDIO_CONCEAL=<ms> sets the starting low-water mark (default 12; 0 = off, the old output).
 * The WAV capture and STRIKERS_AUDIO_HASH stay the game's own stream. Tested by tools/audit/test_audio_conceal.py. */
#define CONCEAL_XFADE 128                                      /* held back and crossfaded, 2.7 ms */
#define CONCEAL_CORR 256                                       /* frames compared to choose the loop length */
#define CONCEAL_LMIN 240
#define CONCEAL_LMAX 960
#define CONCEAL_HIST 2048                                      /* >= CONCEAL_LMAX + CONCEAL_CORR */
#define CONCEAL_FULL 1920                                      /* 40 ms of fill at full gain */
#define CONCEAL_FADE 1920                                      /* then 40 ms down to silence */
#define CONCEAL_RAMP 480                                       /* 10 ms back up after a fade */
static int g_wave_seen;                                        /* a waveOut device was opened in this run */
static int g_conceal = -1;                                     /* starting low-water mark in ms, 0 off; -1 until the first open */
static unsigned g_low;                                         /* low-water mark now, frames */
static unsigned long long g_late_fills;                        /* fills that found the device already dry */
static int16_t g_hist[CONCEAL_HIST * 6];                       /* the game's stream as sent (output layout), newest last */
static float g_gain = 1.0f;                                    /* gain of the last frame sent */
static unsigned g_fill_run, g_fill_queued;                     /* fill frames since the game's last buffer; fill headers queued */
static int g_fill_dead;                                        /* faded to silence: no more fills until the game appends */
/* Fill credit: the game's audio thread mixes when a buffer comes back, and a buffer comes back when it has played. A fill
 * queued ahead of the game's next buffers would delay them, so the game would get its buffers later and mix less: more
 * fills (a spiral, seen in the simulation). Each fill adds its length to g_credit (at most CONCEAL_LMAX frames), and a
 * game buffer is given back once the play position is within g_credit frames of its end: to the game a fill then costs
 * no time, as a dry gap did not. The credit decays by 1/32 of the time that passes, so a game that is on time again gets
 * the old latency back within a second. */
static float g_credit;
static LONGLONG g_credit_t;
static unsigned long long g_fills, g_fill_frames, g_fill_run_longest, g_fill_faded;
static FILE *g_wav;
static int g_tone_left, g_heard;
static unsigned long long g_tone_phase;
static unsigned long long g_wav_bytes;
static unsigned long long g_wav_limit;
static FILE *g_timeline;
/* Audit 03: the producer (the guest thread that appends buffers) and its timing, always on (one QPC read per append; the
 * device position is read per append anyway). Latency = samples handed to the device and not yet played, right after an
 * append (what job 14 measured at presents as "submitted minus played"), in 1 ms bins. */
static DWORD g_producer_tid;
static HANDLE g_producer;
static volatile LONGLONG g_last_append;
static LONGLONG g_gap_longest;
static unsigned long long g_gaps_late, g_lat_n;                /* appends more than three buffer periods after the previous one */
static unsigned g_lat_hist[512];
static void producer_note(void);
static void stall_start(void);

static LONGLONG now_ticks(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
/* Optional shared QPC clock for presented frames and the actual waveOut play
 * cursor. WAV sample indices alone omit device queueing and starvation gaps. */
void audio_present_marker(long long frame) {
    if (!g_timeline) return;
    EnterCriticalSection(&g_lock);
    if (g_timeline) {
        MMTIME mt = {0}; mt.wType = TIME_SAMPLES;
        int valid = g_wave && waveOutGetPosition(g_wave, &mt, sizeof mt) == MMSYSERR_NOERROR && mt.wType == TIME_SAMPLES;
        fprintf(g_timeline, "P %lld %lld %lld %llu\n", frame, now_ticks(), valid ? (long long)mt.u.sample : -1, g_wav_bytes / OUT_BPF);
    }
    LeaveCriticalSection(&g_lock);
}

static void wav_header(void) {
    unsigned char h[44];
    uint32_t data = (uint32_t)g_wav_bytes, riff = data + 36, rate = (uint32_t)g_rate, bytes_per_sec = rate * OUT_BPF;
    uint16_t fmt = 1, ch = (uint16_t)g_out_ch, align = (uint16_t)OUT_BPF, bits = 16;
    uint32_t fmt_size = 16;
    memcpy(h, "RIFF", 4); memcpy(h + 4, &riff, 4); memcpy(h + 8, "WAVEfmt ", 8); memcpy(h + 16, &fmt_size, 4);
    memcpy(h + 20, &fmt, 2); memcpy(h + 22, &ch, 2); memcpy(h + 24, &rate, 4); memcpy(h + 28, &bytes_per_sec, 4);
    memcpy(h + 32, &align, 2); memcpy(h + 34, &bits, 2); memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
    fseek(g_wav, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, g_wav);
    fseek(g_wav, 0, SEEK_END);
    fflush(g_wav);
}

static int16_t sat16(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

/* Copies a guest buffer to host samples (stereo, or the game's 5.1 as it is); returns the number of frames and the peak. */
static unsigned convert(Slot *s, const int16_t *in, size_t bytes, unsigned *peak_out) {
    unsigned frames = (unsigned)(bytes / (2u * (unsigned)g_channels)), peak = 0;
    size_t need = (size_t)frames * OUT_BPF;
    if (need > s->capacity || !s->pcm) {
        free(s->pcm);
        s->capacity = need < 4096 ? 4096 : need;
        s->pcm = malloc(s->capacity);
    }
    int16_t *out = s->pcm;
    if (g_out_ch == 6) {                              /* 5.1 out: the game's own channels, at the volume */
        for (size_t i = 0; i < (size_t)frames * 6; i++) {
            out[i] = sat16((int)(in[i] * g_volume));
            unsigned a = (unsigned)abs(out[i]);
            if (a > peak) peak = a;
        }
        *peak_out = peak;
        return frames;
    }
    for (unsigned i = 0; i < frames; i++) {
        float l, r;
        if (g_channels == 6) {                        /* FL FR FC LFE RL RR */
            const int16_t *p = in + (size_t)i * 6;
            l = p[0] + 0.707f * p[2] + 0.251f * p[3] + 0.707f * p[4];
            r = p[1] + 0.707f * p[2] + 0.251f * p[3] + 0.707f * p[5];
        } else if (g_channels == 1) {
            l = r = in[i];
        } else {
            const int16_t *p = in + (size_t)i * (unsigned)g_channels;
            l = p[0];
            r = p[1];
        }
        int li = (int)(l * g_volume), ri = (int)(r * g_volume);
        out[i * 2] = sat16(li);
        out[i * 2 + 1] = sat16(ri);
        unsigned a = (unsigned)abs(out[i * 2]), b = (unsigned)abs(out[i * 2 + 1]);
        if (a > peak) peak = a;
        if (b > peak) peak = b;
    }
    *peak_out = peak;
    return frames;
}

/* STRIKERS_AUDIO_TONE: a quiet 440 Hz tone over the first second, to check the host output path by ear. */
static unsigned add_tone(int16_t *pcm, unsigned frames, unsigned peak) {
    for (unsigned i = 0; i < frames; i++, g_tone_phase++) {
        int v = (int)(3000.0 * sin(6.283185307179586 * 440.0 * (double)g_tone_phase / g_rate));
        for (int k = 0; k < 2; k++) {
            pcm[i * g_out_ch + k] = sat16(pcm[i * g_out_ch + k] + v);
            unsigned a = (unsigned)abs(pcm[i * g_out_ch + k]);
            if (a > peak) peak = a;
        }
    }
    return peak;
}

static void give_back(Slot *s) {
    s->given = 1;
    g_released[g_rel_tail++ % SLOTS] = s->guest;
    g_total_released++;
    g_sec_released++;
}

/* Moves the buffers that have finished playing to the released list, in order. With fill credit (g_credit, see
 * conceal_fill) a game buffer is given back once the play position is within g_credit frames of its end, while its
 * header still plays from our copy. Returns how many headers finished or buffers were given back. Caller holds g_lock. */
static int collect_finished(int everything) {
    int n = 0;
    LONGLONG now = now_ticks();
    while (g_head != g_tail) {
        Slot *s = &g_slots[g_head % SLOTS];
        if (!everything) {
            if (s->prepared) { if (!(s->hdr.dwFlags & WHDR_DONE)) break; }
            else if (!g_started || now < s->due) break;
        }
        if (s->prepared) { waveOutUnprepareHeader(g_wave, &s->hdr, sizeof s->hdr); s->prepared = 0; }
        g_head++;
        n++;
        if (!s->guest) { g_fill_queued--; continue; }   /* a fill: host only */
        if (!s->given) give_back(s);
    }
    if (!everything && g_credit >= 1.0f && g_wave && g_started && g_head != g_tail) {
        MMTIME mt;
        memset(&mt, 0, sizeof mt);
        mt.wType = TIME_SAMPLES;
        if (waveOutGetPosition(g_wave, &mt, sizeof mt) == MMSYSERR_NOERROR && mt.wType == TIME_SAMPLES) {
            unsigned long long until = (unsigned long long)mt.u.sample + (unsigned long long)g_credit;
            for (unsigned i = g_head; i != g_tail; i++) {
                Slot *s = &g_slots[i % SLOTS];
                if (!s->guest || s->given) continue;
                if (s->end_pos > until) break;
                give_back(s);
                n++;
            }
        }
    }
    return n;
}

/* Hands a slot's samples (s->frames) to the device, or schedules it by the clock without one. g_lock held. */
static void submit_slot(Slot *s) {
    s->prepared = 0;
    s->given = 0;
    if (g_wave) {
        g_frames_submitted += s->frames;
        s->end_pos = g_frames_submitted;
        memset(&s->hdr, 0, sizeof s->hdr);
        s->hdr.lpData = (LPSTR)s->pcm;
        s->hdr.dwBufferLength = s->frames * OUT_BPF;
        if (waveOutPrepareHeader(g_wave, &s->hdr, sizeof s->hdr) == MMSYSERR_NOERROR) {
            if (waveOutWrite(g_wave, &s->hdr, sizeof s->hdr) == MMSYSERR_NOERROR) s->prepared = 1;
            else waveOutUnprepareHeader(g_wave, &s->hdr, sizeof s->hdr);
        }
    }
    if (!s->prepared) {                               /* clock-driven: one buffer after the other in real time */
        LONGLONG now = now_ticks();
        if (g_clock_due < now) g_clock_due = now;
        g_clock_due += (LONGLONG)s->frames * g_freq.QuadPart / g_rate;
        s->due = g_clock_due;
    }
    g_tail++;
}

static int slot_room(Slot *s, unsigned frames) {
    size_t need = (size_t)frames * OUT_BPF;
    if (need > s->capacity || !s->pcm) {
        free(s->pcm);
        s->capacity = need < 4096 ? 4096 : need;
        s->pcm = malloc(s->capacity);
    }
    return s->pcm != NULL;
}

/* ---- gap concealment (see g_conceal) */
static float raised_cos(unsigned i, unsigned n) { return 0.5f - 0.5f * cosf(3.14159265f * ((float)i + 0.5f) / (float)n); }

/* The game's buffer (n frames in s->pcm) goes into the history; s->pcm becomes what is sent: the held-back tail of the
 * previous buffer and this one's first n - CONCEAL_XFADE frames, ramped back up after a faded fill. */
static void conceal_push(Slot *s, unsigned n) {
    unsigned ch = (unsigned)g_out_ch, keep = CONCEAL_HIST;
    int16_t *pcm = s->pcm;
    if (n >= keep) memcpy(g_hist, pcm + (size_t)(n - keep) * ch, (size_t)keep * ch * 2);
    else {
        memmove(g_hist, g_hist + (size_t)n * ch, (size_t)(keep - n) * ch * 2);
        memcpy(g_hist + (size_t)(keep - n) * ch, pcm, (size_t)n * ch * 2);
    }
    if (n <= keep - CONCEAL_XFADE) memcpy(pcm, g_hist + (size_t)(keep - CONCEAL_XFADE - n) * ch, (size_t)n * ch * 2);
    else {                                            /* longer than the history (never with 512-frame buffers) */
        memmove(pcm + (size_t)CONCEAL_XFADE * ch, pcm, (size_t)(n - CONCEAL_XFADE) * ch * 2);
        memcpy(pcm, g_hist + (size_t)(keep - CONCEAL_XFADE) * ch, (size_t)CONCEAL_XFADE * ch * 2);   /* stale only if n > keep */
    }
    for (unsigned i = 0; i < n && g_gain < 1.0f; i++) {
        g_gain += 1.0f / CONCEAL_RAMP;
        if (g_gain > 1.0f) g_gain = 1.0f;
        for (unsigned k = 0; k < ch; k++) pcm[(size_t)i * ch + k] = (int16_t)(pcm[(size_t)i * ch + k] * g_gain);
    }
    if (g_fill_run > g_fill_run_longest) g_fill_run_longest = g_fill_run;
    g_fill_run = 0;
    g_fill_dead = 0;
}

/* The loop length: the L whose preceding CONCEAL_CORR frames look most like the newest CONCEAL_CORR (normalized
 * correlation of the channel sum, every second lag and sample, then refined by one), so that playing h[E-L..] after h[E]
 * continues the waveform in phase. */
static unsigned conceal_length(void) {
    enum { SPAN = CONCEAL_LMAX + CONCEAL_CORR };
    static float m[SPAN];
    unsigned ch = (unsigned)g_out_ch;
    const int16_t *h = g_hist + (size_t)(CONCEAL_HIST - SPAN) * ch;
    for (unsigned i = 0; i < SPAN; i++) { float v = 0; for (unsigned k = 0; k < ch; k++) v += h[(size_t)i * ch + k]; m[i] = v; }
    const float *ref = m + SPAN - CONCEAL_CORR;
    unsigned best = 512;
    float best_score = -1e30f;
    for (int pass = 0; pass < 2; pass++) {
        unsigned lo = pass ? (best > CONCEAL_LMIN ? best - 1 : best) : CONCEAL_LMIN;
        unsigned hi = pass ? (best < CONCEAL_LMAX ? best + 1 : best) : CONCEAL_LMAX;
        unsigned step = pass ? 1 : 2, sstep = pass ? 1 : 2;
        for (unsigned L = lo; L <= hi; L += step) {
            const float *cand = ref - L;
            float dot = 0, energy = 1.0f;
            for (unsigned i = 0; i < CONCEAL_CORR; i += sstep) { dot += ref[i] * cand[i]; energy += cand[i] * cand[i]; }
            float score = dot / sqrtf(energy);
            if (score > best_score) { best_score = score; best = L; }
        }
    }
    return best;
}

static void conceal_reset(void) {                     /* g_lock held, nothing of ours queued */
    memset(g_hist, 0, sizeof g_hist);
    g_gain = 1.0f;
    g_fill_run = g_fill_queued = 0;
    g_fill_dead = 0;
    g_credit = 0;
}

static int conceal_possible(void) {
    return g_conceal > 0 && g_wave && g_open && g_started && g_heard && !g_fill_dead && g_tail - g_head < SLOTS;
}

/* Queues one fill: L frames that continue from the held-back tail and end where it begins. g_lock held. */
static void conceal_fill(void) {
    Slot *s = &g_slots[g_tail % SLOTS];
    unsigned L = conceal_length(), ch = (unsigned)g_out_ch;
    if (!slot_room(s, L)) return;
    const int16_t *tail = g_hist + (size_t)(CONCEAL_HIST - CONCEAL_XFADE) * ch;
    const int16_t *loop = g_hist + (size_t)(CONCEAL_HIST - CONCEAL_XFADE - L) * ch;   /* ends where the tail begins */
    int16_t *out = s->pcm;
    for (unsigned i = 0; i < L; i++) {
        float a = i < CONCEAL_XFADE ? raised_cos(i, CONCEAL_XFADE) : 1.0f;
        if (g_fill_run + i >= CONCEAL_FULL) { g_gain -= 1.0f / CONCEAL_FADE; if (g_gain < 0) g_gain = 0; }
        for (unsigned k = 0; k < ch; k++) {
            float v = loop[(size_t)i * ch + k] * a + (a < 1.0f ? tail[(size_t)i * ch + k] * (1.0f - a) : 0.0f);
            out[(size_t)i * ch + k] = sat16((int)(v * g_gain));
        }
    }
    s->guest = 0;
    s->frames = L;
    g_fill_run += L;
    g_fills++;
    g_fill_frames += L;
    g_fill_queued++;
    if (g_gain <= 0) { g_fill_dead = 1; g_fill_faded++; }
    g_credit += (float)L;
    if (g_credit > CONCEAL_LMAX) g_credit = CONCEAL_LMAX;
    submit_slot(s);
}

/* The device played silence although we were filling: the low-water mark was too low for it. +4 ms, up to 40 ms. */
static void conceal_late(void) {
    g_late_fills++;
    if (g_low < (unsigned)g_rate / 25) g_low += (unsigned)g_rate / 250;
}

/* Audio thread: a fill when the device is about to run dry (its play position within g_low frames of the end of what it
 * has, or no header left). Two signs that a fill came too late: it finds the device with nothing left, or the play
 * position fell more than 4 ms behind the clock at once (the engine played part of a period as silence before the fill
 * reached it; the reference follows slow clock drift of up to 0.1%). g_lock held. */
static int g_lag_valid;
static LONGLONG g_lag_t0, g_lag_prev;
static unsigned long long g_lag_pos0;
static double g_lag_ref;
static void conceal_check(void) {
    if (!conceal_possible()) { g_lag_valid = 0; return; }
    LONGLONG now = now_ticks();
    MMTIME mt;
    memset(&mt, 0, sizeof mt);
    mt.wType = TIME_SAMPLES;
    int have = waveOutGetPosition(g_wave, &mt, sizeof mt) == MMSYSERR_NOERROR && mt.wType == TIME_SAMPLES;
    unsigned long long pos = have ? (unsigned long long)mt.u.sample : 0;
    int dry = g_head == g_tail || (have && pos >= g_frames_submitted);
    if (have && !dry) {
        if (!g_lag_valid || pos < g_lag_pos0) { g_lag_valid = 1; g_lag_t0 = now; g_lag_pos0 = pos; g_lag_ref = 0; }
        else {
            double f = (double)g_rate / (double)g_freq.QuadPart;
            double lag = (double)(now - g_lag_t0) * f - (double)(pos - g_lag_pos0);
            double drift = g_lag_ref + (double)(now - g_lag_prev) * f / 1000.0;
            g_lag_ref = lag < drift ? lag : drift;
            if (lag - g_lag_ref > g_rate / 250.0) { conceal_late(); g_lag_ref = lag; }
        }
        g_lag_prev = now;
    } else g_lag_valid = 0;
    if (dry) conceal_late();
    else if (!have || g_frames_submitted - pos >= g_low) return;
    conceal_fill();
}

/* After headers finished: if the device has nothing left, fill (audio thread) or wake the audio thread to do it (the
 * game's thread), else count an underrun (device ran dry while started, after the first sound). g_lock held. */
static void queue_empty_check(int may_fill) {
    if (g_head != g_tail || !g_open || !g_started || !g_heard || g_empty_since) return;
    if (conceal_possible()) {
        if (may_fill) conceal_fill();
        else SetEvent(g_wake);
        return;
    }
    g_underruns++;
    g_empty_since = now_ticks();
}

/* ---- producer and stall diagnostics (audit 03) */
const char *guest_thread_name_of(DWORD tid);                   /* os_threads.c */
int guest_thread_info(int i, Ctx **c, const char **name, HANDLE *h);
DWORD os_mutex_owner(uint64_t key);
long long nvn_front_frame_next(void);
static char g_producer_name[64];
static Ctx *volatile g_producer_ctx;
static void producer_note(void) {                              /* g_lock held: the thread that appends changed (first append) */
    const char *name = guest_thread_name_of(GetCurrentThreadId());
    GROUP_AFFINITY ga;
    memset(&ga, 0, sizeof ga);
    GetThreadGroupAffinity(GetCurrentThread(), &ga);
    BOOL no_boost = FALSE;
    GetThreadPriorityBoost(GetCurrentThread(), &no_boost);
    if (g_producer) CloseHandle(g_producer);
    g_producer = NULL;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_producer, 0, FALSE, DUPLICATE_SAME_ACCESS);
    g_producer_tid = GetCurrentThreadId();
    snprintf(g_producer_name, sizeof g_producer_name, "%s", name ? name : "a host thread");
    g_producer_ctx = NULL;
    for (int i = 0; i < 256; i++) {
        Ctx *tc; const char *tn; HANDLE h;
        if (!guest_thread_info(i, &tc, &tn, &h)) break;
        if (h && GetThreadId(h) == g_producer_tid) { g_producer_ctx = tc; break; }
    }
    int prio = GetThreadPriority(GetCurrentThread());
    printf("nn::audio: buffers are appended by %s [%lu]: Windows priority %d (process class %lx), dynamic boost %s, processors %llx\n",
           g_producer_name, (unsigned long)g_producer_tid, prio, (unsigned long)GetPriorityClass(GetCurrentProcess()),
           no_boost ? "off" : "on", (unsigned long long)ga.Mask);
    /* Audit 03, the audio stalls: the producer is Wwise's audio thread (AK::EventManager, nn::os priority 18 on core 2), which
     * the console core model had at THREAD_PRIORITY_LOWEST (base 8 in the ABOVE_NORMAL class, the level of every normal
     * thread of every other program and of BundleDecompressCore2, 19): it lost whole time slices to them and the device ran
     * dry (docs/audit-03.md, F21). Audit 03 raised this one thread here; since audit 04 the priority mapping does it for 18
     * (os_threads.c host_priority: BELOW_NORMAL, base 9; STRIKERS_OS_PRIO18=0 is the old mapping), so nothing is changed here. */
    {   const char *e = getenv("STRIKERS_AUDIO_PRIORITY");
        if (e && !_stricmp(e, "mmcss")) {                     /* TEST ONLY: the Windows multimedia class scheduler ("Pro Audio") */
            HMODULE avrt = LoadLibraryA("avrt.dll");
            HANDLE (WINAPI *set)(LPCWSTR, LPDWORD) = avrt ? (HANDLE (WINAPI *)(LPCWSTR, LPDWORD))(void *)GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW") : NULL;
            DWORD task = 0;
            HANDLE h = set ? set(L"Pro Audio", &task) : NULL;
            printf("nn::audio: STRIKERS_AUDIO_PRIORITY=mmcss: the producer %s the multimedia class scheduler (Pro Audio)\n", h ? "joined" : "could NOT join");
        }
    }
    stall_start();
}

/* STRIKERS_AUDIO_STALL=<ms> (diagnostic, off by default): a time-critical thread samples the producer every millisecond while
 * it is late (no append for more than 1.5 buffer periods) and prints every late stretch of at least <ms>: where the producer
 * was (lifted code or which import; running or not on a CPU, by its cycle counter; the owner of a mutex it waits for), how
 * busy its own processor and the whole machine were, and whether the device ran dry. */
typedef struct { char key[176]; double ms; } StallBucket;
static int g_stall_ms;
static ULONG64 g_cyc0[256];
static double g_tsc_per_ms;                                    /* QueryThreadCycleTime counts time-stamp counter cycles */
static unsigned long long g_stalls, g_stall_dry;
typedef struct { LARGE_INTEGER idle, kernel, user, dpc, irq; ULONG irqs; } CpuPerf;     /* SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION */
typedef LONG (WINAPI *NtQsi)(int, void *, ULONG, ULONG *);
static NtQsi g_ntqsi;
static int cpu_busy_snapshot(CpuPerf *out, int max) {
    ULONG got = 0;
    if (!g_ntqsi || g_ntqsi(8, out, (ULONG)(sizeof *out * (size_t)max), &got) != 0) return 0;
    return (int)(got / sizeof *out);
}
static double cpu_busy(const CpuPerf *a, const CpuPerf *b, int i) {    /* kernel time includes idle time */
    double total = (double)(b[i].kernel.QuadPart - a[i].kernel.QuadPart + b[i].user.QuadPart - a[i].user.QuadPart);
    double idle = (double)(b[i].idle.QuadPart - a[i].idle.QuadPart);
    return total > 0 ? 100.0 * (total - idle) / total : 0;
}
static void stall_add(StallBucket *b, int *n, const char *key, double ms) {
    for (int i = 0; i < *n; i++) if (!strcmp(b[i].key, key)) { b[i].ms += ms; return; }
    if (*n < 24) { snprintf(b[*n].key, sizeof b[*n].key, "%s", key); b[(*n)++].ms = ms; }
}
/* Kernel view of the producer while it is not on a CPU (SystemProcessInformation, read at most every 4 ms during a late
 * stretch): Waiting (blocked) or Ready (runnable, the processor taken by something else), and which programs had a running
 * thread at the producer's priority or above at that moment (time-slice competitors). x64 layouts of
 * SYSTEM_PROCESS_INFORMATION (threads at +0x100, image name at +0x38, pid at +0x50) and SYSTEM_THREAD_INFORMATION (0x50
 * bytes: thread id at +0x30, Priority +0x38, ThreadState +0x44, WaitReason +0x48). */
static unsigned char *g_spi;
static ULONG g_spi_size;
typedef struct { char name[40]; double ms; } Rival;
static int producer_state(DWORD tid, int *prio, Rival *rv, int *nrv, double dt) {   /* 1 ready, 5 waiting, ... ; -1 unknown */
    ULONG got = 0;
    LONG st;
    if (!g_spi) { g_spi_size = 8u << 20; g_spi = malloc(g_spi_size); if (!g_spi) return -1; }
    while ((st = g_ntqsi(5, g_spi, g_spi_size, &got)) == (LONG)0xC0000004L) {             /* STATUS_INFO_LENGTH_MISMATCH */
        free(g_spi); g_spi_size = got + (1u << 20); g_spi = malloc(g_spi_size); if (!g_spi) return -1;
    }
    if (st != 0) return -1;
    int state = -1, my_prio = 0;
    DWORD self = GetCurrentProcessId();
    for (unsigned char *e = g_spi;; ) {                          /* the producer first: its state and priority */
        ULONG next = *(ULONG *)e, n = *(ULONG *)(e + 4);
        if ((DWORD)(uintptr_t)*(HANDLE *)(e + 0x50) == self)
            for (ULONG i = 0; i < n; i++) {
                unsigned char *t = e + 0x100 + (size_t)i * 0x50;
                if ((DWORD)(uintptr_t)*(HANDLE *)(t + 0x30) == tid) { state = (int)*(ULONG *)(t + 0x44); my_prio = *(LONG *)(t + 0x38); }
            }
        if (!next) break;
        e += next;
    }
    *prio = my_prio;
    if (state != 1) return state;
    for (unsigned char *e = g_spi;; ) {                          /* running threads of other programs at that priority or above */
        ULONG next = *(ULONG *)e, n = *(ULONG *)(e + 4);
        if ((DWORD)(uintptr_t)*(HANDLE *)(e + 0x50) != self)
            for (ULONG i = 0; i < n; i++) {
                unsigned char *t = e + 0x100 + (size_t)i * 0x50;
                if (*(ULONG *)(t + 0x44) != 2 || *(LONG *)(t + 0x38) < my_prio) continue;
                char name[40] = "System";
                struct { USHORT len, max; WCHAR *buf; } *u = (void *)(e + 0x38);    /* UNICODE_STRING ImageName */
                if (u->buf && u->len) { int w = WideCharToMultiByte(CP_UTF8, 0, u->buf, u->len / 2, name, (int)sizeof name - 8, NULL, NULL); name[w > 0 ? w : 0] = 0; }
                snprintf(name + strlen(name), 8, ":%ld", (long)*(LONG *)(t + 0x38));
                int k = 0;
                while (k < *nrv && strcmp(rv[k].name, name)) k++;
                if (k == *nrv && *nrv < 8) { snprintf(rv[k].name, sizeof rv[k].name, "%s", name); rv[k].ms = 0; (*nrv)++; }
                if (k < *nrv) rv[k].ms += dt;
            }
        if (!next) break;
        e += next;
    }
    return state;
}

static DWORD WINAPI stall_thread(LPVOID arg) {
    (void)arg;
    static CpuPerf p0[64], p1[64];
    StallBucket b[24];
    int nb = 0, ncpu = 0, open = 0, proc_cpu = -1;
    LONGLONG open_append = 0, prev = now_ticks();
    ULONG64 cyc0 = 0;
    unsigned long long under0 = 0;
    Rival rv[8];
    int nrv = 0;
    double ready_ms = 0, waiting_ms = 0, since_q = 0;

    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* high resolution */, TIMER_ALL_ACCESS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    for (;;) {
        LARGE_INTEGER due; due.QuadPart = -10000;
        if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, 100); else Sleep(1);
        LONGLONG now = now_ticks();
        double dt = (double)(now - prev) * 1000.0 / (double)g_freq.QuadPart;
        prev = now;
        LONGLONG last = g_last_append;
        HANDLE ph = g_producer;
        Ctx *pc = g_producer_ctx;
        if (!ph || !last || !g_started || !g_heard) continue;
        double late_ms = (double)(now - last) * 1000.0 / (double)g_freq.QuadPart;
        if (open && last != open_append) {                     /* the producer appended again: the stretch is over */
            double len = (double)(last - open_append) * 1000.0 / (double)g_freq.QuadPart;
            if (len >= g_stall_ms) {
                char line[2400];
                int k = 0;
                int n1 = cpu_busy_snapshot(p1, 64);
                double own = proc_cpu >= 0 && proc_cpu < ncpu && proc_cpu < n1 ? cpu_busy(p0, p1, proc_cpu) : -1, all = 0;
                for (int i = 0; i < ncpu && i < n1; i++) all += cpu_busy(p0, p1, i);
                if (ncpu) all /= ncpu;
                unsigned long long under = g_underruns - under0;
                g_stalls++;
                if (under) g_stall_dry++;
                k += snprintf(line + k, sizeof line - k, "nn::audio stall: frame %lld, %.1f ms between appends%s; producer %s: ", nvn_front_frame_next(), len,
                              under ? " (the device ran dry)" : "", g_producer_name);
                for (int i = 0; i < nb && k < (int)sizeof line - 200; i++) k += snprintf(line + k, sizeof line - k, "%s%s %.1f ms", i ? ", " : "", b[i].key, b[i].ms);
                k += snprintf(line + k, sizeof line - k, "; its processor %d busy %.0f%%, all %d processors %.0f%%", proc_cpu, own, ncpu, all);
                if (ready_ms + waiting_ms > 0) {
                    k += snprintf(line + k, sizeof line - k, "; kernel: ready (runnable, not running) %.1f ms, waiting %.1f ms", ready_ms, waiting_ms);
                    for (int i = 0; i < nrv && k < (int)sizeof line - 120; i++) k += snprintf(line + k, sizeof line - k, "%s%s %.1f ms", i ? ", " : "; running meanwhile at its priority or above: ", rv[i].name, rv[i].ms);
                }
                for (int i = 0, shown = 0; i < 256 && shown < 6 && k < (int)sizeof line - 120; i++) {    /* game threads that ran on the same processor meanwhile */
                    Ctx *tc; const char *tn; HANDLE th; ULONG64 q = 0;
                    if (!guest_thread_info(i, &tc, &tn, &th)) break;
                    if (!th || !g_cyc0[i] || !QueryThreadCycleTime(th, &q)) continue;
                    GROUP_AFFINITY tg; memset(&tg, 0, sizeof tg);
                    double ms = g_tsc_per_ms > 0 ? (double)(q - g_cyc0[i]) / g_tsc_per_ms : 0;
                    if (ms < 1.0 || GetThreadId(th) == g_producer_tid || !GetThreadGroupAffinity(th, &tg) || proc_cpu < 0 || !(tg.Mask >> proc_cpu & 1)) continue;
                    k += snprintf(line + k, sizeof line - k, "%s%s %.1f ms", shown++ ? ", " : "; on it meanwhile: ", tn ? tn : "?", ms);
                }
                snprintf(line + k, sizeof line - k, "\n");
                fputs(line, stdout);
                fflush(stdout);
            }

            open = 0;
        }
        if (!open) {
            if (late_ms * (double)g_rate < 1.5 * 512.0 * 1000.0) continue;   /* within 1.5 buffer periods of the last append */
            open = 1; open_append = last; nb = 0; nrv = 0; ready_ms = waiting_ms = since_q = 0;
            for (int i = 0; i < 256; i++) {
                Ctx *tc; const char *tn; HANDLE th; ULONG64 q = 0;
                if (!guest_thread_info(i, &tc, &tn, &th)) break;
                g_cyc0[i] = th && QueryThreadCycleTime(th, &q) ? q : 0;
            }
            ncpu = cpu_busy_snapshot(p0, 64);
            GROUP_AFFINITY ga; memset(&ga, 0, sizeof ga);
            proc_cpu = -1;
            if (GetThreadGroupAffinity(ph, &ga) && ga.Mask && !(ga.Mask & (ga.Mask - 1))) { proc_cpu = 0; while (!(ga.Mask >> proc_cpu & 1)) proc_cpu++; }
            QueryThreadCycleTime(ph, &cyc0);
            under0 = g_underruns;
            continue;
        }
        ULONG64 cyc = 0;
        QueryThreadCycleTime(ph, &cyc);
        int running = cyc - cyc0 > 20000;
        cyc0 = cyc;
        const char *imp = pc ? pc->in_import : NULL;
        char key[176];
        if (imp) {
            snprintf(key, sizeof key, "%s%s", imp, running ? "" : " (waiting)");
            if (!running && (strstr(imp, "utex") || strstr(imp, "ock"))) {
                DWORD owner = os_mutex_owner(pc->x[0]);
                const char *on = owner ? guest_thread_name_of(owner) : NULL;
                snprintf(key, sizeof key, "%s (waiting; owner %s)", imp, on ? on : owner ? "a host thread" : "none");
            }
        } else snprintf(key, sizeof key, "lifted code%s", running ? "" : " (ready or faulting, not on a CPU)");
        stall_add(b, &nb, key, dt);
        since_q += dt;
        if (!running && since_q >= 4.0) {                       /* what the kernel says the producer is doing */
            int prio = 0, stt = producer_state(g_producer_tid, &prio, rv, &nrv, since_q);
            if (stt == 1 || stt == 7) ready_ms += since_q;          /* Ready, DeferredReady */
            else if (stt == 5) waiting_ms += since_q;
            since_q = 0;
        } else if (running) since_q = 0;
    }
}
static void stall_start(void) {
    static int started;
    const char *e = getenv("STRIKERS_AUDIO_STALL");
    if (started || !e || atoi(e) <= 0) return;
    started = 1;
    g_stall_ms = atoi(e);
    g_ntqsi = (NtQsi)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySystemInformation");
    {   LONGLONG q0 = now_ticks(); unsigned long long t0 = __rdtsc(); Sleep(20);
        g_tsc_per_ms = (double)(__rdtsc() - t0) * (double)g_freq.QuadPart / (double)(now_ticks() - q0) / 1000.0; }
    CloseHandle(CreateThread(NULL, 0, stall_thread, NULL, 0, NULL));
    printf("nn::audio: STRIKERS_AUDIO_STALL=%d: appends more than %d ms apart are explained in the log\n", g_stall_ms, g_stall_ms);
}

/* One wake of the audio thread (waveOut's done event, or every 2 ms): finished headers, then a fill if the device has
 * nothing left. g_lock held. Returns the number of headers that finished. */
static int audio_service(void) {
    if (!g_open) return 0;
    LONGLONG now = now_ticks();
    if (g_credit > 0) {
        g_credit -= (float)((double)(now - g_credit_t) * g_rate / (double)g_freq.QuadPart / 32.0);
        if (g_credit < 0) g_credit = 0;
    }
    g_credit_t = now;
    int n = collect_finished(0);
    conceal_check();
    queue_empty_check(1);
    return n;
}

static DWORD WINAPI audio_thread(LPVOID arg) {
    (void)arg;
    LONGLONG next_report = now_ticks() + g_freq.QuadPart;
    unsigned seconds = 0;
    int every_second = getenv("STRIKERS_AUDIO_LOG") != NULL;
    unsigned acc_appended = 0, acc_released = 0, acc_peak = 0, acc_polls = 0, acc_empty = 0;
    for (;;) {
        WaitForSingleObject(g_wake, 2);
        EnterCriticalSection(&g_lock);
        int n = audio_service();
        uint64_t ev = g_event;
        LONGLONG now = now_ticks();
        int report = 0;
        unsigned queued = g_tail - g_head;
        if (now >= next_report) {
            next_report += g_freq.QuadPart;
            seconds++;
            if (g_sec_peak) g_nonsilent_seconds++;
            acc_appended += g_sec_appended; acc_released += g_sec_released; acc_polls += g_sec_polls; acc_empty += g_sec_empty;
            if (g_sec_peak > acc_peak) acc_peak = g_sec_peak;
            g_sec_appended = g_sec_released = g_sec_peak = g_sec_polls = g_sec_empty = 0;
            if (g_wav) wav_header();
            report = g_open && (every_second || seconds % 10 == 0);
        }
        LeaveCriticalSection(&g_lock);
        if (n && ev) os_signal_event(ev);
        if (report) {
            printf("nn::audio: t=%us appended %u released %u polls %u (empty %u) queued %u peak %u, total %llu buffers, %llu s with sound\n",
                   seconds, acc_appended, acc_released, acc_polls, acc_empty, queued, acc_peak, g_total_appended, g_nonsilent_seconds);
            fflush(stdout);
            acc_appended = acc_released = acc_peak = acc_polls = acc_empty = 0;
        }
    }
}

static void audio_InitializeAudioOutParameter(Ctx *c) {   /* (AudioOutParameter *): u32 sampleRate, u16 channelCount, u16 reserved */
    wr32(A0, 0);
    wr16(A0 + 4, 0);
    wr16(A0 + 6, 0);
}

static void audio_OpenDefaultAudioOut(Ctx *c) {       /* (AudioOut *, nn::os::SystemEvent *, const AudioOutParameter &) -> Result */
    unsigned want_rate = rd32(A2), want_channels = rd16(A2 + 4);
    EnterCriticalSection(&g_lock);
    g_audio_out = A0;
    g_event = A1;
    g_rate = want_rate ? (int)want_rate : 48000;
    const char *env = getenv("STRIKERS_AUDIO_CHANNELS");
    (void)env;
    g_channels = want_channels == 6 && surround_on() ? 6 : 2;
    g_out_ch = g_channels == 6 ? 6 : 2;
    g_head = g_tail = g_rel_head = g_rel_tail = 0;
    if (g_conceal < 0) {
        const char *ce = getenv("STRIKERS_AUDIO_CONCEAL");
        g_conceal = ce && *ce ? atoi(ce) : 12;
        if (g_conceal < 0) g_conceal = 0;
        if (g_conceal > 100) g_conceal = 100;
    }
    if (!g_low || (unsigned)g_rate * (unsigned)g_conceal / 1000u > g_low) g_low = (unsigned)g_rate * (unsigned)g_conceal / 1000u;
    conceal_reset();
    g_frames_submitted = 0;
    g_started = 0;
    g_clock_due = 0;
    if (g_event) os_init_event(g_event, 1);
    g_wave = NULL;
    if (!getenv("STRIKERS_AUDIO_MUTE")) {
        WAVEFORMATEXTENSIBLE x;
        WAVEFORMATEX f;
        memset(&f, 0, sizeof f);
        f.wFormatTag = WAVE_FORMAT_PCM;
        f.nChannels = (WORD)g_out_ch;
        f.nSamplesPerSec = (DWORD)g_rate;
        f.wBitsPerSample = 16;
        f.nBlockAlign = (WORD)OUT_BPF;
        f.nAvgBytesPerSec = (DWORD)g_rate * OUT_BPF;
        if (g_out_ch == 6) {                          /* 5.1: the speaker positions have to be named */
            memset(&x, 0, sizeof x);
            x.Format = f; x.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE; x.Format.cbSize = sizeof x - sizeof x.Format;
            x.Samples.wValidBitsPerSample = 16; x.dwChannelMask = KSAUDIO_SPEAKER_5POINT1; x.SubFormat = au_SUBTYPE_PCM;
        }
        MMRESULT r = waveOutOpen(&g_wave, WAVE_MAPPER, g_out_ch == 6 ? &x.Format : &f, (DWORD_PTR)g_wake, 0, CALLBACK_EVENT);
        if (r == MMSYSERR_NOERROR) g_wave_seen = 1;
        if (r != MMSYSERR_NOERROR) { g_wave = NULL; printf("nn::audio: waveOutOpen failed (%u): no sound, buffers are released by the clock\n", (unsigned)r); }
        else {
            waveOutPause(g_wave);                     /* nothing plays before StartAudioOut */
            /* Test copies are silent: only the copies a person plays (strikers_native.exe, strikers_user.exe) are audible,
             * like the window rule in display.c. The device stays open, so timing is the same as with sound.
             * STRIKERS_VOLUME=<0..100> overrides either way. */
            char exe[MAX_PATH] = "";
            GetModuleFileNameA(NULL, exe, sizeof exe);
            const char *base = strrchr(exe, '\\') ? strrchr(exe, '\\') + 1 : exe, *vol = getenv("STRIKERS_VOLUME");
#ifdef _WIN32
            int person = !_stricmp(base, "strikers_native.exe") || !_stricmp(base, "strikers_user.exe");
#else
            if (strrchr(base, '/')) base = strrchr(base, '/') + 1;
            int person = !strcmp(base, "strikers_native") || !strcmp(base, "strikers_user");      /* Linux build: no ".exe" */
#endif
            int percent = vol ? atoi(vol) : person ? 100 : 0;
            if (percent < 0) percent = 0;
            if (percent > 100) percent = 100;
            DWORD level = (DWORD)(0xffff * percent / 100);
            waveOutSetVolume(g_wave, level | level << 16);
            g_host_percent = percent;
            printf("nn::audio: host volume %d%%%s\n", percent, percent ? "" : " (test copy: silent; STRIKERS_VOLUME=<0..100> to change)");
        }
    }
    const char *wav = getenv("STRIKERS_AUDIO_WAV");
    const char *timeline = getenv("STRIKERS_AUDIO_TIMELINE");
    if (timeline && !g_timeline) {
        g_timeline = fopen(timeline, "w");
        if (g_timeline) fprintf(g_timeline, "# QPC %lld rate %d; P frame qpc device_sample wav_frames; A qpc wav_start frames peak\n", g_freq.QuadPart, g_rate);
    }
    int legacy_wav = !wav;
    if (!wav) wav = getenv("STRIKERS_WAV");
    if (wav && *wav && !g_wav) {
        g_wav = fopen(wav, "wb");
        g_wav_bytes = 0;
        const char *seconds = getenv("STRIKERS_AUDIO_WAV_SECONDS");
        unsigned limit = seconds ? (unsigned)strtoul(seconds, NULL, 10) : legacy_wav ? WAV_SECONDS : 0;
        g_wav_limit = limit ? (unsigned long long)limit * (unsigned)g_rate * 4 : 0xffffffd8ULL;
        if (g_wav_limit > 0xffffffd8ULL) g_wav_limit = 0xffffffd8ULL;
        if (g_wav) { wav_header(); printf("nn::audio: WAV capture %s (stereo PCM16 before host volume)\n", wav); }
        else printf("nn::audio: cannot open WAV capture %s\n", wav);
    }
    g_tone_left = getenv("STRIKERS_AUDIO_TONE") ? g_rate / 512 : 0;
    g_open = 1;
    LeaveCriticalSection(&g_lock);
    printf("nn::audio: OpenDefaultAudioOut: game asks %u Hz, %u channels -> %d Hz, %d channels, 16-bit, %s\n",
           want_rate, want_channels, g_rate, g_channels, g_wave ? "waveOut device open" : "no host device");
    RET(0);
}

static void audio_GetAudioOutChannelCount(Ctx *c) { RET(g_channels); }
static void audio_GetAudioOutSampleRate(Ctx *c) { RET(g_rate); }
static void audio_GetAudioOutSampleFormat(Ctx *c) { RET(SAMPLE_FORMAT_PCM16); }

static void audio_SetAudioOutBufferInfo(Ctx *c) {     /* (AudioOutBuffer *, void *data, size_t capacity, size_t dataSize) */
    wr64(A0 + BUF_NEXT, 0);
    wr64(A0 + BUF_DATA, A1);
    wr64(A0 + BUF_CAPACITY, A2);
    wr64(A0 + BUF_SIZE, A3);
    wr64(A0 + BUF_OFFSET, 0);
}

static void audio_GetAudioOutBufferDataPointer(Ctx *c) { RET(rd64(A0 + BUF_DATA)); }

static void audio_AppendAudioOutBuffer(Ctx *c) {      /* (AudioOut *, AudioOutBuffer *) -> bool */
    uint64_t buf = A1, data = rd64(buf + BUF_DATA), size = rd64(buf + BUF_SIZE);
    int ok = 0;
    EnterCriticalSection(&g_lock);
    if (g_open && g_tail - g_head < SLOTS && data && size) {
        Slot *s = &g_slots[g_tail % SLOTS];
        unsigned peak = 0;
        unsigned frames = convert(s, (const int16_t *)(uintptr_t)data, (size_t)size, &peak);
        s->guest = buf;
        s->frames = frames;
        s->prepared = 0;
        if (g_tone_left > 0) { g_tone_left--; peak = add_tone(s->pcm, frames, peak); }
        if (peak && !g_heard) { g_heard = 1; printf("nn::audio: first buffer with sound (buffer %llu, peak %u)\n", g_total_appended, peak); }
        {   /* TEST ONLY, STRIKERS_AUDIO_HASH=<file> (round "four players"): one line per buffer the game hands to the device:
             * index, frames, peak, FNV-1a 64 of the game's own samples (before our stereo copy). Compares the mixer's output
             * of two builds (tests\p15_pcm_compare.py). One getenv at the first buffer; nothing without the variable. */
            static FILE *hf; static int tried;
            if (!tried) { const char *hp = getenv("STRIKERS_AUDIO_HASH"); tried = 1; if (hp && *hp) hf = fopen(hp, "w"); }
            if (hf) {
                uint64_t h = 0xcbf29ce484222325ULL;
                const uint8_t *pb = (const uint8_t *)(uintptr_t)data;
                for (uint64_t i = 0; i < size; i++) { h ^= pb[i]; h *= 0x100000001b3ULL; }
                fprintf(hf, "%llu %u %u %016llx\n", g_total_appended, frames, peak, (unsigned long long)h);
                if ((g_total_appended & 63) == 0) fflush(hf);
            }
        }
        if (g_wav && g_wav_bytes < g_wav_limit) {
            if (g_timeline) fprintf(g_timeline, "A %lld %llu %u %u\n", now_ticks(), g_wav_bytes / OUT_BPF, frames, peak);
            size_t n = (size_t)((g_wav_limit - g_wav_bytes) / OUT_BPF);
            if (n > frames) n = frames;
            unsigned long long before = g_wav_bytes;
            size_t wrote = fwrite(s->pcm, OUT_BPF, n, g_wav);
            g_wav_bytes += wrote * OUT_BPF;
            /* Keep a valid header during playback and on the host's timed shutdown. */
            if (g_wav_bytes == g_wav_limit || g_wav_bytes / ((unsigned)g_rate * OUT_BPF) !=
                before / ((unsigned)g_rate * OUT_BPF) || wrote != n) wav_header();
            if (wrote != n) printf("nn::audio: WAV write failed; capture stopped at %llu bytes\n", g_wav_bytes);
            if (g_wav_bytes == g_wav_limit || wrote != n) { fclose(g_wav); g_wav = NULL; }
        }
        if (GetCurrentThreadId() != g_producer_tid) producer_note();
        {
            LONGLONG now = now_ticks();
            if (g_last_append && g_started && g_heard) {
                LONGLONG gap = now - g_last_append;
                if (gap > g_gap_longest) g_gap_longest = gap;
                if (gap * g_rate > 3LL * (LONGLONG)frames * g_freq.QuadPart) g_gaps_late++;
            }
            g_last_append = now;
        }
        if (g_wave && g_started && g_heard && g_frames_submitted) {
            MMTIME mt;
            memset(&mt, 0, sizeof mt);
            mt.wType = TIME_SAMPLES;
            if (waveOutGetPosition(g_wave, &mt, sizeof mt) == MMSYSERR_NOERROR && mt.wType == TIME_SAMPLES) {
                unsigned long long pos = (unsigned long long)mt.u.sample;
                if (pos >= g_frames_submitted) g_starved++;
                else {
                    unsigned long long ms = (g_frames_submitted + frames - pos) * 1000ULL / (unsigned)g_rate;
                    g_lat_hist[ms < 511 ? ms : 511]++; g_lat_n++;
                }
            }
        }
        if (g_wave && g_conceal > 0) conceal_push(s, frames);
        submit_slot(s);
        if (g_empty_since) {
            unsigned long long gap = (unsigned long long)(now_ticks() - g_empty_since);
            g_underrun_ticks += gap; if (gap > g_underrun_longest) g_underrun_longest = gap;
            g_empty_since = 0;
        }
        if (g_tail - g_head - g_fill_queued > g_queue_max) g_queue_max = g_tail - g_head - g_fill_queued;
        g_total_appended++;
        g_sec_appended++;
        if (peak > g_sec_peak) g_sec_peak = peak;
        ok = 1;
    } else {
        g_total_dropped++;
    }
    LeaveCriticalSection(&g_lock);
    RET(ok);
}

/* End-of-run line (hostrt.c report_and_exit): what the output path did in this run. */
void audio_report(FILE *out) {
    EnterCriticalSection(&g_lock);
    if (g_wav) { wav_header(); fclose(g_wav); g_wav = NULL; }
    if (g_timeline) { fclose(g_timeline); g_timeline = NULL; }
    LeaveCriticalSection(&g_lock);
    if (!g_total_appended) return;
    double f = g_freq.QuadPart ? (double)g_freq.QuadPart : 1.0;
    fprintf(out, "nn::audio: %d Hz, %d channels granted, 16-bit; %llu buffers appended, %llu released, %llu refused; deepest queue %u buffers; "
                 "underruns (device ran dry while started, after the first sound): %llu, %.1f ms in all, longest %.1f ms; %llu s with sound\n",
            g_rate, g_channels, g_total_appended, g_total_released, g_total_dropped, g_queue_max,
            g_underruns, (double)g_underrun_ticks * 1000.0 / f, (double)g_underrun_longest * 1000.0 / f, g_nonsilent_seconds);
    if (g_conceal > 0 && g_wave_seen)
        fprintf(out, "nn::audio: gaps filled (the game was late; recent sound repeated, crossfaded; low-water mark %d ms, now %.0f ms after %llu late fills): "
                     "%llu fills, %.1f ms in all, longest run %.1f ms, %llu faded to silence\n",
                g_conceal, g_low * 1000.0 / g_rate, g_late_fills, g_fills, (double)g_fill_frames * 1000.0 / g_rate, (double)(g_fill_run > g_fill_run_longest ? g_fill_run : g_fill_run_longest) * 1000.0 / g_rate, g_fill_faded);
    else fprintf(out, "nn::audio: gaps filled: off (%s)\n", g_conceal > 0 ? "no output device" : "STRIKERS_AUDIO_CONCEAL=0");
    fprintf(out, "nn::audio: appends that found the device's own play position at the end of everything submitted (the output had no samples left): %llu of %llu\n", g_starved, g_total_appended);
    if (g_lat_n) {                                              /* percentiles of the 1 ms latency bins */
        const double q[3] = {0.01, 0.5, 0.99};
        int r[3] = {0, 0, 0};
        for (int k = 0; k < 3; k++) {
            unsigned long long acc = 0, want = (unsigned long long)(q[k] * (double)g_lat_n);
            int i = 0;
            while (i < 511 && acc + g_lat_hist[i] <= want) acc += g_lat_hist[i++];
            r[k] = i;
        }
        fprintf(out, "nn::audio: producer %s; submitted-to-played latency after an append (samples handed to the device, not yet played) p01/p50/p99 %d/%d/%d ms over %llu appends; "
                     "appends more than three buffer periods after the previous one %llu, longest gap %.1f ms; late stretches of %d ms or more explained (STRIKERS_AUDIO_STALL) %llu, %llu of them with the device dry\n",
                g_producer_name, r[0], r[1], r[2], g_lat_n, g_gaps_late, (double)g_gap_longest * 1000.0 / f, g_stall_ms, g_stalls, g_stall_dry);
    }
}

static void audio_StartAudioOut(Ctx *c) {             /* (AudioOut *) -> Result */
    EnterCriticalSection(&g_lock);
    if (g_open && !g_started) {
        g_started = 1;
        g_clock_due = now_ticks();                    /* clock-driven buffers queued before the start begin now */
        for (unsigned i = g_head; i != g_tail; i++) {
            Slot *s = &g_slots[i % SLOTS];
            g_clock_due += (LONGLONG)s->frames * g_freq.QuadPart / g_rate;
            s->due = g_clock_due;
        }
        if (g_wave) waveOutRestart(g_wave);
    }
    LeaveCriticalSection(&g_lock);
    printf("nn::audio: StartAudioOut (%llu buffers queued)\n", g_total_appended - g_total_released);
    RET(0);
}

static void audio_GetReleasedAudioOutBuffer(Ctx *c) { /* (AudioOut *) -> AudioOutBuffer * or null */
    uint64_t r = 0;
    EnterCriticalSection(&g_lock);
    if (g_open && collect_finished(0)) queue_empty_check(0);
    if (g_rel_head != g_rel_tail) r = g_released[g_rel_head++ % SLOTS];
    g_sec_polls++;
    if (!r) g_sec_empty++;
    LeaveCriticalSection(&g_lock);
    RET(r);
}

static void audio_StopAudioOut(Ctx *c) {              /* (AudioOut *): everything queued becomes released */
    EnterCriticalSection(&g_lock);
    HWAVEOUT w = g_wave;
    LeaveCriticalSection(&g_lock);
    if (w) waveOutReset(w);                           /* marks every queued header as done */
    g_frames_submitted = 0;                           /* waveOutReset also sets the device position back to 0 */
    EnterCriticalSection(&g_lock);
    if (g_open) collect_finished(1);
    conceal_reset();
    g_started = 0;
    if (g_wave) waveOutPause(g_wave);
    uint64_t ev = g_event;
    LeaveCriticalSection(&g_lock);
    if (ev) os_signal_event(ev);
    printf("nn::audio: StopAudioOut\n");
}

static void audio_CloseAudioOut(Ctx *c) {             /* (AudioOut *) */
    EnterCriticalSection(&g_lock);
    HWAVEOUT w = g_wave;
    LeaveCriticalSection(&g_lock);
    if (w) waveOutReset(w);
    EnterCriticalSection(&g_lock);
    if (g_open) collect_finished(1);
    g_open = g_started = 0;
    g_wave = NULL;
    g_event = 0;
    if (g_wav) wav_header();
    LeaveCriticalSection(&g_lock);
    if (w) waveOutClose(w);
    printf("nn::audio: CloseAudioOut (%llu buffers appended, %llu released)\n", g_total_appended, g_total_released);
}

/* ---------------------------------------------------------------- audio device */
/* [sdk] 0x990f0 AcquireAudioDeviceNotificationForOutput(SystemEvent *): a second call returns 0x499 (module 153,
 * description 2; flag byte 0xc5efa2); otherwise it asks the audio device service for its output event
 * (slot +0x78 = QueryAudioDeviceOutputEvent, any failure aborts) and attaches the handle with
 * nn::os::AttachReadableHandleToSystemEvent(event, handle, managed, clear mode 1 = auto clear); Result 0.
 * The service side (not in the dump), after [doc] suyu audio/audio_device.cpp: the event is signalled when the
 * service object is created, i.e. it arrives signalled, and again whenever the output device changes.
 * Here: the output device is the Windows default render endpoint; IMMNotificationClient::OnDefaultDeviceChanged
 * (render, console role) signals the event. */
#ifdef _WIN32
typedef struct NotifyClient { const void *const *vtbl; } NotifyClient;
typedef struct { const void *const *vtbl; } ComObj;
static const GUID k_clsid_enumerator = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
static const GUID k_iid_enumerator = {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
static const GUID k_iid_notify = {0x7991eec9, 0x7e89, 0x4d85, {0x83, 0x90, 0x6c, 0x70, 0x3c, 0xec, 0x60, 0xc0}};
static const GUID k_iid_unknown = {0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
#endif
static volatile LONG g_device_acquired, g_device_changes;
#ifdef _WIN32
static HRESULT STDMETHODCALLTYPE nc_query(NotifyClient *self, const GUID *iid, void **out) {
    if (!memcmp(iid, &k_iid_notify, sizeof *iid) || !memcmp(iid, &k_iid_unknown, sizeof *iid)) { *out = self; return S_OK; }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE nc_ref(NotifyClient *self) { (void)self; return 1; }      /* static object */
static HRESULT STDMETHODCALLTYPE nc_state(NotifyClient *self, const WCHAR *id, DWORD state) { (void)self; (void)id; (void)state; return S_OK; }
static HRESULT STDMETHODCALLTYPE nc_id(NotifyClient *self, const WCHAR *id) { (void)self; (void)id; return S_OK; }
static HRESULT STDMETHODCALLTYPE nc_default(NotifyClient *self, int flow, int role, const WCHAR *id) {
    (void)self; (void)id;
    if (flow == 0 && role == 0) {                             /* eRender, eConsole */
        InterlockedIncrement(&g_device_changes);
        printf("nn::audio: the Windows default output device changed: device notification signalled\n");
        if (g_device_event) os_signal_event(g_device_event);
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE nc_prop(NotifyClient *self, const WCHAR *id, PROPERTYKEY key) { (void)self; (void)id; (void)key; return S_OK; }
static const void *const g_nc_vtbl[] = {nc_query, nc_ref, nc_ref, nc_state, nc_id, nc_id, nc_default, nc_prop};   /* IUnknown, OnDeviceStateChanged, OnDeviceAdded, OnDeviceRemoved, OnDefaultDeviceChanged, OnPropertyValueChanged */
static NotifyClient g_nc = {g_nc_vtbl};
static DWORD WINAPI device_watch_thread(void *arg) {
    ComObj *en = NULL;
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    HRESULT hr = CoCreateInstance(&k_clsid_enumerator, NULL, CLSCTX_ALL, &k_iid_enumerator, (void **)&en);
    if (SUCCEEDED(hr) && en) {
        /* IMMDeviceEnumerator: slot 6 = RegisterEndpointNotificationCallback(IMMNotificationClient *) */
        hr = ((HRESULT (STDMETHODCALLTYPE *)(ComObj *, NotifyClient *))en->vtbl[6])(en, &g_nc);
    }
    if (FAILED(hr)) printf("nn::audio: the Windows device notification is not available (0x%08lx): output device changes are not reported\n", (unsigned long)hr);
    Sleep(INFINITE);                                          /* keeps the apartment and the registration alive */
    return 0;
}
#else
static DWORD WINAPI device_watch_thread(void *arg) { (void)arg; (void)g_device_changes; return 0; }     /* Linux build: device changes are not reported */
#endif
#define AUDIO_DEVICE_NOTIFICATION_ALREADY_ACQUIRED 0x499u
static void audio_AcquireAudioDeviceNotificationForOutput(Ctx *c) {   /* (nn::os::SystemEvent *) -> Result */
    if (InterlockedCompareExchange(&g_device_acquired, 1, 0) != 0) { RET(AUDIO_DEVICE_NOTIFICATION_ALREADY_ACQUIRED); return; }
    os_init_event(A0, 1);
    g_device_event = A0;
    os_signal_event(A0);                                      /* the service's event arrives signalled */
    CloseHandle(CreateThread(NULL, 0, device_watch_thread, NULL, 0, NULL));
    RET(0);
}

static void audio_GetActiveAudioDeviceName(Ctx *c) {  /* (AudioDeviceName *): char[0x100] */
    memset((void *)(uintptr_t)A0, 0, 0x100);
    memcpy((void *)(uintptr_t)A0, g_device_name, sizeof g_device_name);
}

static void audio_SetAudioDeviceOutputVolume(Ctx *c) {   /* (const AudioDeviceName *, float) */
    const char *name = (const char *)(uintptr_t)A0;
    float v = c->v[0].f[0];
    trace_printf("nn::audio: SetAudioDeviceOutputVolume(%s, %g)\n", name, v);
    if (strcmp(name, g_device_name) == 0 && v >= 0.0f && v <= 4.0f) g_volume = v;
}

/* ---------------------------------------------------------------- nn::codec::HardwareOpusDecoder */
#define OPUS_SLOTS 10                                 /* [sdk] _ZTVN2nn5codec19HardwareOpusDecoderE has exactly 10 entries (0x00..0x48) */
#define OPUS_HEADER 8
#define OPUS_MAX_FRAMES 5760                          /* 120 ms at 48 kHz, the longest Opus packet */
#define OBJ_VTABLE 0x00
#define OBJ_STATE 0x08                                /* our decoder state (inside the game's work buffer) */
#define OBJ_RATE 0x10
#define OBJ_CHANNELS 0x14

static uint64_t g_opus_vtable[OPUS_SLOTS];
static char g_opus_names[OPUS_SLOTS][48];
static unsigned char g_opus_seen[OPUS_SLOTS];
static HMODULE g_opus_dll;
static int g_opus_tried;
static int (*p_opus_decoder_get_size)(int channels);
static int (*p_opus_decoder_init)(void *st, int rate, int channels);
static int (*p_opus_decode)(void *st, const unsigned char *data, int len, int16_t *pcm, int frame_size, int fec);
static volatile LONG64 g_opus_packets, g_opus_errors, g_opus_frames, g_opus_nonzero;

static void opus_load(void) {
    if (g_opus_tried) return;
    g_opus_tried = 1;
    const char *env = getenv("STRIKERS_OPUS_DLL");
#ifdef _WIN32
    const char *const tries[] = { env, "libopus-0.dll", "opus.dll" };
#else
    const char *const tries[] = { env, "libopus-0.dll" };      /* plat_posix.c maps it to libopus.so.0 */
#endif
    const char *used = NULL;
    for (unsigned i = 0; i < sizeof tries / sizeof tries[0] && !g_opus_dll; i++) {
        if (!tries[i]) continue;
        g_opus_dll = LoadLibraryExA(tries[i], NULL, strchr(tries[i], '\\') ? LOAD_WITH_ALTERED_SEARCH_PATH : 0);
        used = tries[i];
    }
    if (g_opus_dll) {
        p_opus_decoder_get_size = (int (*)(int))GetProcAddress(g_opus_dll, "opus_decoder_get_size");
        p_opus_decoder_init = (int (*)(void *, int, int))GetProcAddress(g_opus_dll, "opus_decoder_init");
        p_opus_decode = (int (*)(void *, const unsigned char *, int, int16_t *, int, int))GetProcAddress(g_opus_dll, "opus_decode");
        if (!p_opus_decoder_get_size || !p_opus_decoder_init || !p_opus_decode) g_opus_dll = NULL;
    }
    printf("nn::codec: libopus %s%s\n", g_opus_dll ? "loaded from " : "NOT FOUND: Opus streams will be silent (set STRIKERS_OPUS_DLL)", g_opus_dll ? used : "");
}

static uint32_t be32(uint64_t a) { uint32_t v = rd32(a); return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24); }

static void opus_destructor(Ctx *c) { wr64(A0 + OBJ_STATE, 0); }

static void opus_GetWorkBufferSize(Ctx *c) {          /* (this, int sampleRate, int channelCount) -> size_t */
    opus_load();
    int channels = (int)A2 == 1 ? 1 : 2;
    size_t size = g_opus_dll ? (size_t)p_opus_decoder_get_size(channels) : 0;
    RET((size + 0x1fff) & ~(size_t)0xfff);
}

static void opus_Initialize(Ctx *c) {                 /* (this, int sampleRate, int channelCount, void *buffer, size_t size) -> 0 ok */
    uint64_t self = A0, buffer = A3;
    int rate = (int)A1, channels = (int)A2, ok = 0;
    opus_load();
    wr32(self + OBJ_RATE, (uint32_t)rate);
    wr32(self + OBJ_CHANNELS, (uint32_t)channels);
    wr64(self + OBJ_STATE, 0);
    if (g_opus_dll && buffer && (channels == 1 || channels == 2) && (size_t)p_opus_decoder_get_size(channels) <= A4) {
        ok = p_opus_decoder_init((void *)(uintptr_t)buffer, rate, channels) == 0;
        if (ok) wr64(self + OBJ_STATE, buffer);
    }
    printf("nn::codec: HardwareOpusDecoder::Initialize(%d Hz, %d channels, work buffer %llu bytes): %s\n", rate, channels,
           (unsigned long long)A4, ok ? "libopus decoder ready" : "decoder unavailable (failure returned)");
    RET(ok ? 0 : 6);
}

static void opus_Finalize(Ctx *c) { wr64(A0 + OBJ_STATE, 0); wr32(A0 + OBJ_RATE, 0); wr32(A0 + OBJ_CHANNELS, 0); }

static void opus_DecodeInterleaved(Ctx *c) {          /* (this, size_t *consumed, int *sampleCount, int16_t *out, size_t outBytes, const void *in, size_t inBytes) */
    uint64_t self = A0, in = A5, in_size = A6;
    int16_t *out = (int16_t *)(uintptr_t)A3;
    int channels = (int)rd32(self + OBJ_CHANNELS), rate = (int)rd32(self + OBJ_RATE);
    if (channels != 1) channels = 2;
    int room = (int)(A4 / (2u * (unsigned)channels));  /* frames that fit in the output */
    if (in_size < OPUS_HEADER) { InterlockedIncrement64(&g_opus_errors); RET(6); return; }      /* any non-zero value: the game only tests for 0 */
    uint32_t len = be32(in);
    if ((uint64_t)len + OPUS_HEADER > in_size) { InterlockedIncrement64(&g_opus_errors); RET(6); return; }
    void *st = (void *)(uintptr_t)rd64(self + OBJ_STATE);
    int frames;
    if (st) {
        frames = p_opus_decode(st, (const unsigned char *)(uintptr_t)(in + OPUS_HEADER), (int)len, out, room > OPUS_MAX_FRAMES ? OPUS_MAX_FRAMES : room, 0);
        if (frames < 0) {
            if (InterlockedIncrement64(&g_opus_errors) <= 5) printf("nn::codec: opus_decode error %d (packet of %u bytes, input %llu bytes)\n", frames, len, (unsigned long long)in_size);
            RET(6);
            return;
        }
    } else {
        if(A1) wr64(A1,0); if(A2) wr32(A2,0); RET(6); return;
    }
    wr64(A1, (uint64_t)len + OPUS_HEADER);
    wr32(A2, (uint32_t)frames);
    InterlockedAdd64(&g_opus_frames, frames);
    for (int i = 0; i < frames * channels; i++) if (out[i]) { InterlockedIncrement64(&g_opus_nonzero); break; }
    LONG64 n = InterlockedIncrement64(&g_opus_packets);
    if (n == 1 || n % 5000 == 0)
        printf("nn::codec: %lld Opus packets decoded (%lld frames, %lld errors, %lld nonzero packets); last: %u bytes -> %d frames\n",
               (long long)n, (long long)g_opus_frames, (long long)g_opus_errors, (long long)g_opus_nonzero, len, frames);
    RET(0);
}

/* The four remaining virtual functions of the real class ([sdk] vtable: +0x18 IsInitialized, +0x30 GetSampleRate,
 * +0x38 GetChannelCount, +0x40 Reset) and the deleting destructor (+0x08). */
static void opus_IsInitialized(Ctx *c) { RET(rd32(A0 + OBJ_RATE) != 0); }       /* Initialize stores the rate, Finalize clears it */
static void opus_GetSampleRate(Ctx *c) { RET(rd32(A0 + OBJ_RATE)); }
static void opus_GetChannelCount(Ctx *c) { RET(rd32(A0 + OBJ_CHANNELS)); }
static void opus_Reset(Ctx *c) {                      /* back to the state right after Initialize: the decoder's history is dropped */
    void *st = (void *)(uintptr_t)rd64(A0 + OBJ_STATE);
    int channels = (int)rd32(A0 + OBJ_CHANNELS) == 1 ? 1 : 2;
    if (st && p_opus_decoder_init) p_opus_decoder_init(st, (int)rd32(A0 + OBJ_RATE), channels);
}
static void opus_deleting_destructor(Ctx *c) {        /* ~HardwareOpusDecoder() then operator delete(this): the game's own operator delete */
    uint64_t self = A0, del = 0;
    wr64(self + OBJ_STATE, 0);
    for (int i = 0; i < dyn_sym_count && !del; i++)
        if (dyn_syms[i].defined && strcmp(dyn_syms[i].name, "_ZdlPv") == 0) del = GUEST_BASE + dyn_syms[i].value;
    if (!del) guest_trap("HardwareOpusDecoder deleting destructor: the game's operator delete was not found", c->x[30] - 4);
    c->x[0] = self;
    call_guest(c, del);
}
static void opus_unknown(Ctx *c) {                    /* a virtual function the game was not seen to call */
    const char *name = hostfn_current_name();
    for (int i = 0; i < OPUS_SLOTS; i++)
        if (name == g_opus_names[i] && !g_opus_seen[i]) {
            g_opus_seen[i] = 1;
            printf("nn::codec: %s called (x1=%llx x2=%llx x3=%llx) -> 0\n", name, (unsigned long long)A1, (unsigned long long)A2, (unsigned long long)A3);
        }
    RET(0);
}

static void opus_constructor(Ctx *c) {                /* HardwareOpusDecoder(int option) */
    EnterCriticalSection(&g_lock);
    if (!g_opus_vtable[0]) {
        for (int i = 0; i < OPUS_SLOTS; i++) {
            ImportFn fn = opus_unknown;
            const char *what = NULL;
            switch (i * 8) {
            case 0x00: fn = opus_destructor; what = "destructor"; break;
            case 0x08: fn = opus_deleting_destructor; what = "deleting destructor"; break;
            case 0x18: fn = opus_IsInitialized; what = "IsInitialized"; break;
            case 0x30: fn = opus_GetSampleRate; what = "GetSampleRate"; break;
            case 0x38: fn = opus_GetChannelCount; what = "GetChannelCount"; break;
            case 0x40: fn = opus_Reset; what = "Reset"; break;
            case 0x10: fn = opus_GetWorkBufferSize; what = "GetWorkBufferSize"; break;
            case 0x20: fn = opus_Initialize; what = "Initialize"; break;
            case 0x28: fn = opus_Finalize; what = "Finalize"; break;
            case 0x48: fn = opus_DecodeInterleaved; what = "DecodeInterleaved"; break;
            }
            if (what) snprintf(g_opus_names[i], sizeof g_opus_names[i], "HardwareOpusDecoder::%s", what);
            else snprintf(g_opus_names[i], sizeof g_opus_names[i], "HardwareOpusDecoder vtable+0x%x", i * 8);
            g_opus_vtable[i] = hostfn_address(g_opus_names[i], fn);
        }
    }
    LeaveCriticalSection(&g_lock);
    wr64(A0 + OBJ_VTABLE, (uint64_t)(uintptr_t)g_opus_vtable);
    wr64(A0 + OBJ_STATE, 0);
    wr64(A0 + OBJ_RATE, 0);
}

/* SDK 13.3 overload without a SystemEvent: the guest polls released buffers. */
static void audio_OpenDefaultAudioOut_noevent(Ctx *c) {
    c->x[2] = c->x[1]; c->x[1] = 0; audio_OpenDefaultAudioOut(c);
}
static void audio_GetAudioOutState(Ctx *c) {
    EnterCriticalSection(&g_lock); int started = g_started; LeaveCriticalSection(&g_lock);
    RET(started ? 0 : 1); /* AudioOutState: Started=0, Stopped=1. */
}
static void audio_GetActiveAudioDeviceChannelCountForOutput(Ctx *c) { RET(g_open ? g_channels : surround_on() ? 6 : 2); }   /* 6: the game mixes 5.1 */
uint32_t audio_wav_frames(void) { return (uint32_t)(g_wav_bytes / OUT_BPF); }

/* ---------------------------------------------------------------- registration */
#define AU(n) "_ZN2nn5audio" n
static const ImportDef audio_imports[] = {
    {"_ZN2nn5codec19HardwareOpusDecoderC1Ei", opus_constructor},
    {AU("19OpenDefaultAudioOutEPNS0_8AudioOutERKNS0_17AudioOutParameterE"), audio_OpenDefaultAudioOut_noevent},
    {AU("16GetAudioOutStateEPKNS0_8AudioOutE"), audio_GetAudioOutState},
    {AU("41GetActiveAudioDeviceChannelCountForOutputEv"), audio_GetActiveAudioDeviceChannelCountForOutput},
    {AU("27InitializeAudioOutParameterEPNS0_17AudioOutParameterE"), audio_InitializeAudioOutParameter},
    {AU("19OpenDefaultAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventERKNS0_17AudioOutParameterE"), audio_OpenDefaultAudioOut},
    {AU("23GetAudioOutChannelCountEPKNS0_8AudioOutE"), audio_GetAudioOutChannelCount},
    {AU("21GetAudioOutSampleRateEPKNS0_8AudioOutE"), audio_GetAudioOutSampleRate},
    {AU("23GetAudioOutSampleFormatEPKNS0_8AudioOutE"), audio_GetAudioOutSampleFormat},
    {AU("21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm"), audio_SetAudioOutBufferInfo},
    {AU("28GetAudioOutBufferDataPointerEPKNS0_14AudioOutBufferE"), audio_GetAudioOutBufferDataPointer},
    {AU("20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE"), audio_AppendAudioOutBuffer},
    {AU("13StartAudioOutEPNS0_8AudioOutE"), audio_StartAudioOut},
    {AU("25GetReleasedAudioOutBufferEPNS0_8AudioOutE"), audio_GetReleasedAudioOutBuffer},
    {AU("12StopAudioOutEPNS0_8AudioOutE"), audio_StopAudioOut},
    {AU("13CloseAudioOutEPNS0_8AudioOutE"), audio_CloseAudioOut},
    {AU("39AcquireAudioDeviceNotificationForOutputEPNS_2os11SystemEventE"), audio_AcquireAudioDeviceNotificationForOutput},
    {AU("24GetActiveAudioDeviceNameEPNS0_15AudioDeviceNameE"), audio_GetActiveAudioDeviceName},
    {AU("26SetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameEf"), audio_SetAudioDeviceOutputVolume},
};

void register_audio_imports(void) {
    InitializeCriticalSection(&g_lock);
    QueryPerformanceFrequency(&g_freq);
    g_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
    register_imports(audio_imports, (int)(sizeof audio_imports / sizeof audio_imports[0]));
}

/* U1, the settings menu (menu.cpp): the host output volume (the game's whole mix; music and effects apart are the game's
 * own Options > Sound). -1 = no output device yet. */
int nnaudio_volume(void) { return g_wave ? g_host_percent : -1; }
void nnaudio_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (!g_wave) return;
    DWORD level = (DWORD)(0xffff * percent / 100);
    waveOutSetVolume(g_wave, level | level << 16);
    g_host_percent = percent;
    printf("nn::audio: host volume %d%% (settings menu)\n", percent);
}
