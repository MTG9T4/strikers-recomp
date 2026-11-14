/* SPDX-License-Identifier: GPL-3.0-or-later. Decoder ownership/back-pressure regression. */
#include "job10_movie_contracts.c"

static int queue_case(int type) {
    uint64_t guest[2] = {0};
    Ctx c = {0};
    c.x[0] = (uint64_t)(uintptr_t)guest; c.x[1] = type; c.x[2] = type == 3 ? 3 : 0; c.x[3] = 2;
    decoder_ctor(&c);
    Dec *d = dec_of((uint64_t)(uintptr_t)guest);
    c.x[0] = d->guest; dec_Start(&c);
    IMFSample *sample = NULL;
    if (FAILED(MFCreateSample(&sample))) return 1;
    int held[64], n = 0, failed = 0, eos = 0;
    float ring[16] = {0};
    /* Like main+ab52b8, take every available output without releasing it.
     * main+ab5f64 constructs sixteen-entry output rings. The codec must stop
     * producing when its finite output pool is held, retaining pending input. */
    for (int frame = 0; frame < 31; ++frame) {
        int idx = -1, more = 0;
        c.x[0] = d->guest; c.x[1] = (uint64_t)(uintptr_t)&idx; c.x[2] = (uint64_t)(uintptr_t)&more;
        dec_AcquireInputBufferIndex(&c);
        if (c.x[0] == ST_NO_INPUT) break;
        if (c.x[0] || idx < 0) { failed = 2; break; }
        if (frame < 30) IMFSample_AddRef(sample);
        Token token = {TOKEN_MAGIC, frame < 30 ? sample : NULL, frame < 30 ? frame * 33367LL : -1, 0};
        memcpy(d->mem + idx * d->slot, &token, sizeof token);
        c.x[0] = d->guest; c.x[1] = idx; c.x[2] = 0; c.x[3] = sizeof token; c.x[4] = token.ts; c.x[5] = frame == 30 ? FLAG_EOS : 0;
        dec_SendInputBufferForDecode(&c);
        int64_t ts; unsigned flags;
        for (;;) {
            c.x[0] = d->guest; c.x[1] = (uint64_t)(uintptr_t)&idx; c.x[2] = (uint64_t)(uintptr_t)&ts;
            c.x[3] = (uint64_t)(uintptr_t)&flags; c.x[4] = (uint64_t)(uintptr_t)&more;
            dec_AcquireOutputBufferIndex(&c);
            if (c.x[0] == ST_NO_OUTPUT) break;
            if (c.x[0] || n == 64) { failed = 4; break; }
            ring[n % 16] = (float)ts / 1e6f;
            eos |= (flags & FLAG_EOS) != 0;
            held[n++] = idx;
        }
    }
    printf("job11 movie queue type %d: %d outstanding output indices (client ring capacity 16), %d pending inputs\n", type, n, d->dec_n);
    printf("client ring first PTS %.3f, EOS received %d\n", ring[0], eos);
    if (!n || n > 16 || !d->dec_n) failed |= 8;
    if (ring[0] != 0 || eos) failed |= 128;
    /* Releasing one index must resume progress, preserving the next PTS. */
    c.x[0] = d->guest; c.x[1] = held[0]; dec_ReleaseOutputBufferIndex(&c);
    if (d->out_n != 1 || d->out[d->out_list[0]].ts != n * 33367LL) failed |= 16;
    c.x[0] = d->guest; dec_Stop(&c);
    if (d->out_n || d->dec_n || d->in_n) failed |= 32;
    c.x[0] = d->guest; dec_Start(&c);
    if (d->in_n != d->nin) failed |= 64;
    c.x[0] = d->guest; dec_dtor(&c);
    IMFSample_Release(sample);
    return failed;
}

__declspec(dllexport) int movie_queue_test(void) {
    InitializeCriticalSection(&g_init_lock);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return 1;
    int result = queue_case(3);
    result |= queue_case(4);
    MFShutdown(); DeleteCriticalSection(&g_init_lock);
    return result;
}
