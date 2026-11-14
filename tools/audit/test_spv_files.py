"""The stage cache's SPIR-V files are read whole or not at all (audit 07, a cut module fails every pipeline with -13).

Compiles spv_whole() as it stands in runtime/gfx/nvn_vk_pipelines.c and runs it over every cut of a module, and checks that
module_from_glsl publishes its file by renaming a finished temporary file. Does not start the game.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import uuid

from build_runtime import ROOT, msvc_environment

TEST = r'''
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
%s
int main(void) {
    /* header, OpCapability Shader, OpFunction (5 words), OpLabel, OpReturn, OpFunctionEnd */
    static const uint32_t m[] = {0x07230203, 0x00010300, 0, 9, 0, 0x00020011, 1, 0x00050036, 2, 3, 0, 4, 0x000200F8, 5, 0x000100FD, 0x00010038};
    size_t n = sizeof m / sizeof m[0], bad = 0;
    if (!spv_whole(m, n)) { puts("FAIL: a whole module refused"); return 1; }
    for (size_t i = 0; i < n; i++) if (spv_whole(m, i)) { printf("FAIL: a module cut at word %%zu accepted\n", i); bad++; }
    static uint32_t z[16];
    for (size_t i = 0; i < n; i++) z[i] = m[i];
    z[7] = 0x00000036;                             /* an instruction of no words */
    if (spv_whole(z, n)) { puts("FAIL: a zero-length instruction accepted"); bad++; }
    z[7] = 0x7fff0036;                             /* an instruction past the end */
    if (spv_whole(z, n)) { puts("FAIL: an instruction past the end accepted"); bad++; }
    z[0] = 0;
    if (spv_whole(z, n)) { puts("FAIL: no magic accepted"); bad++; }
    if (!bad) puts("PASS: spv_whole");
    return bad != 0;
}
'''


def main():
    source = (ROOT / 'runtime/gfx/nvn_vk_pipelines.c').read_text()
    fn = re.search(r'^static int spv_whole\(.*?^}\n', source, re.M | re.S)
    assert fn, 'spv_whole not found'
    glsl = re.search(r'^static VkShaderModule module_from_glsl\(.*?^}\n', source, re.M | re.S)[0]
    assert 'MoveFileExA(tmp, path' in glsl and 'fopen(path, "wb")' not in glsl, 'the SPIR-V file is not published by a rename'
    env = msvc_environment()
    base = Path(tempfile.gettempdir()).resolve()
    tmp = base / ('strikers-spv-files-' + uuid.uuid4().hex)
    tmp.mkdir()
    try:
        (tmp / 't.c').write_text(TEST % fn[0])
        cl = Path(env['VCToolsInstallDir']) / 'bin/Hostx64/x64/cl.exe'
        r = subprocess.run([str(cl), '/nologo', 't.c'], cwd=tmp, env=env, capture_output=True, text=True)
        assert not r.returncode, r.stdout + r.stderr
        r = subprocess.run([str(tmp / 't.exe')], cwd=tmp, capture_output=True, text=True)
        print(r.stdout, end='')
        return r.returncode
    finally:
        if tmp.resolve().parent != base:
            raise RuntimeError('scratch cleanup escaped temp directory')
        shutil.rmtree(tmp)


if __name__ == '__main__':
    raise SystemExit(main())
