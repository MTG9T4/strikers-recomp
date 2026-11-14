# strikers-recomp

A static recompilation of Mario Strikers: Battle League (Switch, title 010019401051C000, update v393216 / 1.3.2) into a
native Windows x64 program, following the approach of ssbu-recomp and smo-recomp: the game's AArch64 code is translated to
C ahead of time, and a host runtime replaces the console's system services, NVN graphics (on Vulkan), audio and input.

## Bring your own game

This repository holds only the project's own tools and runtime. **It contains no game code, game data, artwork, keys or
firmware, and it never will. It reads no game package and no console key, and it decrypts nothing.** To use it you need
your own copy of the game and the 1.3.2 update, and these two folders, extracted on your own Switch with a homebrew dump
tool (nxdumptool, for example):

- the **ExeFS** of update v393216 (1.3.2): `main`, `main.npdm`, `rtld`, `sdk`, `subsdk0`;
- the game's **RomFS** with that update applied.

Setup (`StrikersSetup.exe`, or `scripts/setup.py --exefs <folder> --romfs <folder>` for developers) checks that they are
this game's 1.3.2 files (`installer/game_files.py`) and builds the game on your PC; everything derived from them stays in
the installation's `build/` folder and is never uploaded. Even the icon and the start-up picture come from your files:
put the game's `icon_AmericanEnglish.dat` (from its control data) next to the folders and setup makes them from it
(`installer/game_art.py`). Nothing in this project downloads games or keys, and nothing here is a substitute for owning
the game.

Not affiliated with, endorsed by or sponsored by Nintendo. Mario Strikers and Mario are trademarks of Nintendo; the names
appear here only to say which game this project works with.

## Players

Download the newest `StrikersRecomp-<version>-windows-x64.zip` from
[Releases](https://github.com/MTG9T4/strikers-recomp/releases), extract all of it to a folder you can write to, and open
`StrikersSetup.exe`. The ZIP holds the setup, the launcher, the runtime and the build tools; it contains nothing of the
game. Setup builds the game on your PC from your own ExeFS and RomFS (a first build takes 10 to 60 minutes). The
programs are not code-signed, so Windows SmartScreen may say it protected your PC: choose **More info**, then **Run
anyway**.

See [docs/PLAYING.md](docs/PLAYING.md): installing, setup, controls, settings, updates and troubleshooting.

## Developers

`docs/` holds the design notes and the record of each piece of work (`reference-architecture.md` for how the runtime
is put together, `phase*.md` and `audit-*.md` for the history).

| Tool | Version | Where |
|---|---|---|
| Python | 3.13 | `.venv` (`py -3.13 -m venv .venv`, then `pip install -r requirements.txt`) |
| LLVM/clang | 9.0.0 | `deps/llvm` (`python -I scripts/fetch_deps.py llvm`) |
| MSVC Build Tools | 2022, C++ workload + Windows SDK | winget `Microsoft.VisualStudio.2022.BuildTools` |
| SDL3 | 3.4.18 | `deps/SDL3` |
| Vulkan-Headers | 1.4.329 | `deps/Vulkan-Headers` |
| glslang | 16.6.0 | `deps/glslang` |
| Dear ImGui | 1.91.9b | `deps/imgui` |
| Vulkan SDK | 1.4 (validation layers for debugging) | winget `KhronosGroup.VulkanSDK` |

`python -I scripts/fetch_deps.py` fetches everything under `deps/`, each archive checked against a pinned SHA-256. The
test harnesses in `tools/audit/` run against the build in `build/` that `scripts/setup.py` made from your folders.

## License

GPL-3.0-or-later (see `LICENSE`). The runtime started from ssbu-recomp and smo-recomp (GPL-3.0-or-later); files taken
from them name their upstream file and commit in their header, and `runtime/provenance.json` lists them.
