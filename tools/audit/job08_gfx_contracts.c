/* GPL-3.0-or-later. Host shader-parser regressions, run with MSVC ASan. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvn_front.h"

static int bad;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); bad++; } } while (0)

static void flow_target(unsigned words, unsigned target, int valid) {
    uint64_t code[168] = {0};
    /* Scheduling word; SSY target; unconditional SYNC; target EXIT. */
    code[1] = 0xe290000000000000ull | ((uint64_t)((target - 2) * 8) << 20);
    code[2] = 0xf0f800000007000full;
    if (valid) code[target] = 0xe30000000007000full;
    MxIn in = {0}; MxOut out;
    in.stage = MX_COMPUTE; in.target = 1;
    in.code = (const uint8_t *)code; in.code_len = (int)(words * sizeof code[0]);
    in.local_size[0] = in.local_size[1] = in.local_size[2] = 1;
    int ok = mx_translate(&in, &out);
    CHECK(ok == valid);
    if (!valid) CHECK(strstr(out.why, "outside the code") != NULL);
    printf("flow target %u in %u words: %s\n", target, words, ok ? "translated" : out.why);
    free(out.glsl);
}

static void package(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { CHECK(fp != NULL); return; }
    unsigned h[5];
    if (fread(h, 1, sizeof h, fp) != sizeof h || h[0] != 0x3144584d || h[3] < 0x75c) {
        CHECK(0); fclose(fp); return;
    }
    uint8_t *ctl = malloc(h[3]), *data = malloc(h[4]);
    CHECK(fread(ctl, 1, h[3], fp) == h[3]);
    CHECK(fread(data, 1, h[4], fp) == h[4]);
    fclose(fp);
    Stage st = {0};
    memcpy(st.ctl, ctl + 0x6f0, sizeof st.ctl);
    unsigned len = st.ctl[2], off = st.ctl[6];
    CHECK(off <= h[4] && len <= h[4] - off);
    if (off > h[4] || len > h[4] - off) { free(ctl); free(data); return; }
    st.code = data + off;
    CHECK(nvn_stage_data(&st) == data);
    MxIn in = {0}; MxOut out;
    in.stage = MX_COMPUTE; in.target = 1;
    in.code = st.code; in.code_len = (int)len;
    memcpy(in.local_size, ctl + 0x748, sizeof in.local_size);
    memcpy(&in.smem_bytes, ctl + 0x754, 4);
    memcpy(&in.lmem_bytes, ctl + 0x758, 4);
    if (st.ctl[3]) { in.cdata = nvn_stage_data(&st) + st.ctl[4]; in.cdata_size = st.ctl[3]; }
    int ok = mx_translate(&in, &out);
    printf("package %s: code offset 0x%x, %u bytes: %s\n", path, off, len, ok ? "translated" : out.why);
    /* A real stage may use an unsupported GPU instruction, but must not start in
     * its package header or report the spurious out-of-range flow target. */
    CHECK(strcmp(out.op, "INVALID") != 0);
    CHECK(strstr(out.why, "outside the code") == NULL);
    free(out.glsl); free(ctl); free(data);
}

int main(int argc, char **argv) {
    flow_target(16, 5, 1);
    flow_target(16, 15, 1);
    flow_target(16, 17, 0);   /* inside the old allocation's eight-byte padding */
    flow_target(16, 24, 0);   /* first scheduling word beyond the old allocation */
    flow_target(16, 190, 0);
    flow_target(168, 190, 0); /* exact ASan boot 01: target 190, 176-byte leader allocation */
    for (int i = 1; i < argc; i++) package(argv[i]);
    printf("job08 graphics contracts: %d failed\n", bad);
    return bad != 0;
}
