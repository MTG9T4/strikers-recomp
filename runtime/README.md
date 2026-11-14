# Strikers host runtime

GPL-3.0-or-later. Files taken from ssbu-recomp and smo-recomp name their upstream file and commit in their
header; [provenance.json](provenance.json) lists them. The development notes the headers mention are not part of
this copy.

Compile with MSVC 2022 x64, without linking:

```powershell
.venv\Scripts\python tools/audit/build_runtime.py
.venv\Scripts\python tools/audit/check_runtime_symbols.py
.venv\Scripts\python tools/audit/test_runtime_contracts.py
.venv\Scripts\python tools/audit/import_coverage.py --module main --summary --check
.venv\Scripts\python tools/audit/sdklift_inventory.py
```

The build pipeline's source lists, libraries, generated inputs and products are
specified in [runtime_sources.json](runtime_sources.json). Compile Vulkan fragments
through `nvn_vk.c`; compile `game_shim_eh.c` into the game DLL. It already includes
`game_shim.c`. Build either the glslang C++ shim or the legacy shaderc shim into
`strikers_glslang.dll`. Neither shader shim belongs in the host executable.

The MSVC host uses `core/host_cpu.h`, an ABI-only view checked against the protected
`core/runtime.h` declarations. Generated CPU code continues to use the protected
header and clang. No changes to those three job 03 headers are required by this port.

Network services are offline by default. `STRIKERS_NETWORK=offline` or `0` chooses
that policy explicitly. A later host module must install a callback with
`strikers_set_network_module()` before requesting another mode. Requesting online
without that callback traps. The callback runs after the offline registrations and
replaces the services it implements. No inherited public servers or traffic exist.

Audio uses waveOut AudioOut. `STRIKERS_AUDIO_MUTE=1` selects its clock-driven sink.
The imported HardwareOpusDecoder loads `STRIKERS_OPUS_DLL`, `libopus-0.dll` or
`opus.dll`; initialization fails if decoding is unavailable. Movies use Media
Foundation. The optional FFmpeg hooks require `STRIKERS_ENABLE_FFMPEG=1` and matching
headers/DLLs; that enabled build has not been validated and retains a GNU `typeof`
portability issue. The default MSVC build requires no FFmpeg.

Configuration defaults are `strikers_controllers.ini` and `strikers_graphics.ini`
under `config/`. Environment overrides use `STRIKERS_`. Internal resolution scaling
stays at 1.0 unless `STRIKERS_ENABLE_RESOLUTION_SCALING` enables the inherited,
unvalidated target classifier.

Main has 457 implemented, 205 deliberately offline and zero missing imports.
The audit verifies SDK roots against the rules, ELF addresses and generated function
table. Regenerate with `python -I tools/lifter/sdklift.py gen`, then run
`python tools/audit/sdklift_inventory.py --write` to annotate every verified root.
Boot requires the target SDK image: `sdk_prepare_data` maps it before main's
relocations, preserving exact imported constants and shared writable SDK objects.
The four linker boundaries refer to each image's actual dynamic relocation tables.
The profanity dictionary is unavailable (uninitialized Result, no clean verdict).
Web exits as closed; VI has no compositor history; guest musl DSO registration is
handled by the host CRT. The five retained SVC veneers have explicit host dispatch.

Contract tests execute host synchronization/memory boundaries and selected real
lifted SDK functions in temporary DLLs. Static import coverage and these tests are
distinct from a final executable link, game startup and device validation.
