"""Compile and exercise the production frame-script parser in a temp directory."""
from pathlib import Path
import subprocess
import tempfile
import shutil
import uuid
from build_runtime import ROOT, msvc_environment


def main():
    env = msvc_environment()
    cl = Path(env['VCToolsInstallDir']) / 'bin/Hostx64/x64/cl.exe'
    tmp = Path(tempfile.gettempdir()) / ('strikers-input-' + uuid.uuid4().hex)
    tmp.mkdir()
    try:
        src = Path(tmp) / 'test.c'
        src.write_text(r'''
#include "input_script.h"
#include <assert.h>
static long long observed = -1;
static long long movie = -1;
static int event(const char *name, long long since) { return (!strcmp(name, "open:Result") && observed >= since) || (!strcmp(name, "movie") && movie >= since); }
int main(void) {
    InputStep steps[INPUT_SCRIPT_MAX];
    FILE *f = tmpfile();
    fputs("# navigation\n100+8:Z\n110+90:ru # diagonal stick hold\n115+2:QZ\n", f);
    rewind(f);
    int n = input_script_parse(f, steps);
    assert(n == 3);
    assert(!input_script_down(steps, n, 99, 'Z'));
    assert(input_script_down(steps, n, 100, 'Z'));
    assert(input_script_down(steps, n, 107, 'Z'));
    assert(!input_script_down(steps, n, 108, 'Z'));
    assert(input_script_down(steps, n, 115, 'r'));
    assert(input_script_down(steps, n, 115, 'Q'));
    assert(input_script_down(steps, n, 199, 'u'));
    assert(!input_script_down(steps, n, 200, 'u'));
    fclose(f);
    const char *bad[] = {"-1+3:Z", "1+0:Z", "1+-1:Z", "nonsense", "1+3:Z extra", "wait open:", "wait nope", "p0 1+3:Z", "p9 1+3:Z", "9223372036854775807+3:Z"};
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
        f = tmpfile(); fputs(bad[i], f); rewind(f);
        assert(input_script_parse(f, steps) == -1); fclose(f);
    }
    /* The end-of-match event can land at any presented frame. Neither a
     * stale boot event nor an unsatisfied fence may send Plus mid-match. */
    for (int hz = 30; hz <= 60; hz += 30) {
        InputScriptState st = {0};
        movie = -1;
        f = tmpfile(); fputs("10+8:Z\nwait open:Result\n0+8:!\np2 20+8:rS\nwait movie\n5+8:Z\n", f); rewind(f);
        n = input_script_parse(f, steps); fclose(f); assert(n == 6);
        observed = 3;
        input_script_tick(steps, n, &st, 18, event);
        assert(!input_script_player_down(steps, &st, 18, 0, '!'));
        long long fulltime = hz * 240;
        input_script_tick(steps, n, &st, fulltime - 1, event);
        assert(st.begin == 0);
        observed = fulltime;
        assert(input_script_tick(steps, n, &st, fulltime, event) == 1);
        assert(input_script_player_down(steps, &st, fulltime, 0, '!'));
        assert(!input_script_player_down(steps, &st, fulltime + 8, 0, '!'));
        input_script_tick(steps, n, &st, fulltime + 20, event);
        assert(input_script_player_down(steps, &st, fulltime + 20, 1, 'r'));
        assert(!input_script_player_down(steps, &st, fulltime + 20, 0, 'r'));
        movie = fulltime + 40;
        assert(input_script_tick(steps, n, &st, movie, event) == 1);
        assert(!input_script_player_down(steps, &st, movie + 4, 0, 'Z'));
        assert(input_script_player_down(steps, &st, movie + 5, 0, 'Z'));
    }
    puts("input script contracts: PASS");
}
''')
        exe = Path(tmp) / 'test.exe'
        subprocess.run([str(cl), '/nologo', '/MD', '/std:c11', '/D_CRT_SECURE_NO_WARNINGS',
                        '/I' + str(ROOT / 'runtime/input'), str(src), '/Fe' + str(exe)],
                       cwd=tmp, env=env, check=True)
        subprocess.run([str(exe)], cwd=tmp, env=env, check=True)
    finally:
        if tmp.resolve().parent != Path(tempfile.gettempdir()).resolve():
            raise RuntimeError('scratch cleanup escaped temp directory')
        shutil.rmtree(tmp)


if __name__ == '__main__':
    main()
