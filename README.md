# strikers-recomp

A static recompilation of Mario Strikers: Battle League (Switch, title 010019401051C000, update v393216 / 1.3.2) into a
native Windows x64 program, following the approach of ssbu-recomp and smo-recomp: the game's AArch64 code is translated to
C ahead of time, and a host runtime replaces the console's system services, NVN graphics (on Vulkan), audio and input.

## Features

- **Resolution:** internal resolution from 0.5x to 2x (2x renders the game's 1080p at 4K; Vulkan renderer), and FSR 1
  upscaling with sharpening, or nearest or integer scaling, to fit the window.
- **Ultrawide (experimental):** widens the game's own camera to 21:9, 32:9 or the window's shape; the interface is
  stretched to fit, or centred.
- **Image:** HDR10 with adjustable paper white and peak brightness, SMAA, anisotropic filtering up to 16x.
- **Textures:** ASTC textures decoded on the graphics card; texture dumping and replacement, for texture packs.
- **Mods:** mods and Atmosphere cheats.
- **Controls:** Xbox, PlayStation, Switch and GameCube controllers, with Xbox and PlayStation button pictures; rebindable
  keyboard; mouse clicks in menus.
- **Local Wireless:** with PCs on the same network, with nothing to set up; or over the internet through a private group
  network.
- **More:** fast-forward (hold Tab), saves imported from emulators, 5.1 surround sound.
- Runs at 60 fps like the Switch version (a 30 fps cap is available); the game's own clock stays at 60 Hz.

## Screenshots

Taken from the 0.1.0 release on Windows. The pictures are hosted on the release page, not in this repository.

![A match, 21:9 ultrawide](https://github.com/MTG9T4/strikers-recomp/releases/download/v0.1.0/screenshot-ultrawide.jpg)

![A match, 16:9](https://github.com/MTG9T4/strikers-recomp/releases/download/v0.1.0/screenshot-match.jpg)

| | |
|---|---|
| ![Main menu](https://github.com/MTG9T4/strikers-recomp/releases/download/v0.1.0/screenshot-main-menu.jpg) | ![Character select](https://github.com/MTG9T4/strikers-recomp/releases/download/v0.1.0/screenshot-character-select.jpg) |

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

Files taken from ssbu-recomp and smo-recomp name their upstream file and commit in their header. The development notes
some headers mention are not part of this copy.

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

## Support

If you'd like to support the project, you can [buy me a coffee](https://buymeacoffee.com/mtg9t4). It's entirely
optional: everything here stays free.

## License

GPL-3.0-or-later (see `LICENSE`). The runtime started from ssbu-recomp and smo-recomp (GPL-3.0-or-later); files taken
from them name their upstream file and commit in their header, and `runtime/provenance.json` lists them.
