# Persona3DS

**Persona3DS** is a fork of [P3P3DS](https://github.com/endercodezz/P3P3DS) by enderlit aka endercodezz, retargeted from *Persona 3 Portable* to ***Shin Megami Tensei: Persona*** (the 2009 PSP remake of the first Persona, North American release `ULUS-10432`). The goal is the same as upstream's: run the game natively on the **New Nintendo 3DS / New 3DS XL / New 2DS XL** by statically recompiling the PSP's MIPS code to C++ ahead of time and replacing the PSP operating system, graphics and audio with a high-level runtime.

The engineering base (the static recompiler, the PSP HLE runtime, the GE-to-PICA200 renderer, the PC runner and the Builder) comes from P3P3DS. This fork adds a selectable game profile and the Persona profile, the PSP services Persona needs beyond P3P (`core/src/hle/fork_*.cpp`), music and movies converted ahead of time with the user's ffmpeg, sound output on the 3DS (NDSP), movie decoding on the New 3DS hardware H.264 decoder (MVD), and a few marked renderer patches. Fork code lives in its own files where possible, so upstream fixes can still be merged with `git merge`.

## Status: playable from New Game, movies being finished

- [VERIFIED] PC: boots through the logos and title screen into New Game and gameplay; music and movies (pictures and sound) play from the converted media.
- [VERIFIED] New Nintendo 3DS (maintainer's hardware): title screen, menus and gameplay at about 60 fps with sound. Movies decode on the hardware H.264 decoder but still ran at about 50-60 % of real time, and a fix for their flicker (every other frame black) is not yet confirmed on hardware.
- Known issues: noise boxes during the ATLUS logo and disclaimer fades on the 3DS (possibly fixed by the same renderer change as the movie flicker); New Game starts on Load Game's cursor position, so input scripts press UP first.
- [VERIFIED] Persona's EBOOT uses encryption tag `0xD9160BF0`, which the built-in decryptor does not support (it knows P3P's `0xD91613F0`). Decrypt it with PPSSPP's decrypted EBOOT dump and pass it with `--decrypted-eboot`. The Builder cannot build this game yet.
- Measured values and remaining placeholders (`TODO`): [`profiles/p1p/README.md`](profiles/p1p/README.md).

The verified frontier, the open problems and the latest handoff notes are in [`docs/CURRENT_STATE.md`](docs/CURRENT_STATE.md), newest first (the older entries describe upstream's P3P work).

## Game profiles

Everything game-specific lives in a profile directory, selected by one CMake cache variable:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release            # default: -DP3P_PROFILE_DIR=profiles/p1p
cmake -S platform/3ds -B build/3ds ... -DP3P_PROFILE_DIR=profiles/p1p   # same variable for the 3DS build
```

| File (in `profiles/p1p/`) | Used by |
|---|---|
| `config/p1p_ulus10432.toml` | `tools/prepare_game.py`: disc ID, checksums |
| `config/profile.cmake` | both CMake projects: functions CSV, optional CWCheat `.ini`, `.3dsx` metadata, smoke-test gate |
| `config/aot_layout.cmake` | `psp_recomp --auto` load base, unit span and unit count |
| `config/profile_info.hpp` | 3DS runner, PC runner, Builder: title, disc ID, SD folder, mods folder, ELF SHA-256 |
| `config/bootstrap_expectations.hpp` | runners: root stack and the `--verify-bootstrap` checkpoint |
| `config/p1p_functions.csv`, `config/frontier_seeds.csv` | MANUAL generation and `tools/chase_frontier.py` (`P3P_PROFILE_DIR` environment variable, same default) |
| `game/` (local, gitignored) | `eboot.elf` and `USRDIR/` written by `tools/prepare_game.py` |

## Prerequisites

- CMake 3.20+, Ninja, a C++20 compiler (built here with GCC 13; upstream CI uses GCC 14), Python 3.11+ (standard library only; `tomllib`).
- For the New 3DS build: [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the 3DS packages.
- Your own *Shin Megami Tensei: Persona* `ULUS-10432` ISO. ISO MD5 `c75f75b0f1e92d7cca77d75585bebd82` ([VERIFIED] on the maintainer's copy).
- Optional, for music and movies: your own ffmpeg (with ffprobe and libx264); for movies on the PC runner also the libavcodec development files found through pkg-config. In MSYS2 UCRT64: `pacman -S mingw-w64-ucrt-x86_64-ffmpeg mingw-w64-ucrt-x86_64-pkgconf`, then reconfigure (`cmake -S . -B build` prints `Converted movies on PC: libavcodec ...`).
- For sound on the 3DS: the DSP firmware dump `sdmc:/3ds/dspfirm.cdc` (made once with the DSP1 homebrew, as for any homebrew with sound).

## Game setup

```bash
python tools/prepare_game.py "<your Persona ISO>" --decrypted-eboot <PPSSPP dump> --check-only
python tools/prepare_game.py "<your Persona ISO>" --decrypted-eboot <PPSSPP dump>
```

It checks `PARAM.SFO` (`ULUS10432`) and the ISO MD5, verifies the decrypted EBOOT against the profile's SHA-256 (of PPSSPP's dump), writes it to `profiles/p1p/game/eboot.elf` and extracts `USRDIR/`. Game files never enter the repository.

## Music and movies

The game's ATRAC3plus music and PSMF movies are converted once on your PC, with your own ffmpeg; nothing is downloaded:

```bash
python tools/convert_media.py               # after prepare_game.py; --skip-movies converts music only
```

It writes `profiles/p1p/game/media/` (gitignored): music as IMA-ADPCM WAV, movies as H.264 Baseline streams with a frame index plus IMA-ADPCM audio, and `index.txt`, which matches each file to the game's data by a hash of its header. Already converted files are skipped; converting all 44 movies takes a while. The runtime then plays the converted music and movies where the game would decode its own (`core/include/p3p3ds/hle/fork_media.hpp`, `fork_movie.hpp`). Without converted media the game runs with silent music and black movies.

## Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Without `profiles/p1p/game/eboot.elf` both CMake projects build everything that does not come from the game (runtime, tools, Builder, tests). With the EBOOT present the AOT units are generated as well; `P3P_AOT_UNIT_COUNT` in `aot_layout.cmake` (153 for Persona) must match what `psp_recomp --auto` produces, and configuring stops with the measuring command when a profile has none.

New 3DS (devkitPro msys2 shell, after the host build):

```bash
cmake -S platform/3ds -B build/3ds -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake -DCMAKE_BUILD_TYPE=Release
make -C build/3ds
```

Optional installable `.cia` (`build/3ds/p3p3ds.cia`, installed with FBI or a similar title manager; it runs like the `.3dsx` and uses the same SD card files): make a folder `banner/` at the repository root (not tracked by git) with `bannertool` and `makerom` (or `bannertool.exe`, `makerom.exe`), `banner.png` (256x128), `audio.wav` (the banner's sound) and optionally `icon.png` (48x48; without it the `.3dsx` metadata is used), then:

```bash
make -C build/3ds cia
```

The title, product code and unique id are in `profiles/p1p/config/profile.cmake`; the process settings (124 MB New 3DS memory mode, 804 MHz, L2 cache, core 2) in `platform/3ds/cia/p3p3ds.rsf.in`. `-DP3P_BANNER_DIR=<folder>` uses another folder.

Measurement build of which game code runs most (`sdmc:/persona3ds/aot_profile.txt`, written when the game is quit with START+SELECT; a little slower than the normal build; rebuild the host tools first so the generator is current):

```bash
cmake --build build
cmake -S platform/3ds -B build/3ds-prof -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake -DCMAKE_BUILD_TYPE=Release -DP3P_AOT_PROFILE=ON
make -C build/3ds-prof
```

On the SD card:

| Path | What |
|---|---|
| `/3ds/p3p3ds.3dsx` | the app (CMake target name kept from upstream; the Builder names it `persona3ds.3dsx`) |
| `/persona3ds/<your ISO>.iso` | the game |
| `/persona3ds/media/` | a copy of `profiles/p1p/game/media/` (music and movies; optional) |
| `/3ds/dspfirm.cdc` | DSP firmware dump, needed for sound |
| `/persona3ds/report.txt` | written by the app on exit (START+SELECT): speed, memory, sound, music, movie and renderer counters |

The bottom screen shows the title, build, speed and controls; touch it to switch to the full report (performance counters) and back. Optional switch files in `/persona3ds/` (create an empty file to turn one on): `debug_screen.txt` (start with the full report), `no_inline_imports.txt` (game calls into the system go through the dispatcher as in older builds), `no_gpu_thread.txt` (graphics on the main core instead of core 2), `dump_every.txt` (contains N: saves every Nth vblank as a BMP to `/persona3ds/frames/`, slow), `draw_trace.txt` (logs the renderer's draws of a few movie frames to `draw_trace_out.txt`; may list movie picture counts, e.g. `30 90 150`, to trace 8 presents at each), `no_frame_split.txt` (submit each GPU frame whole at its end instead of in parts), `gpu_trace.txt` (upstream's renderer trace: the first 1,200 presents and their screen-space draws to `gpu_trace.log`), `play_movies.txt` (no automatic START presses through unconverted movies).

The `.3dsx` embeds code generated from your game executable: it is for your own use, never for distribution.

## Run on PC

```bash
./build/p3p_pc_bootstrap --run-until-blocker --max-dispatches 1000000 --dump-events .tmp/p1/run.json
```

The runner prints why it stopped (missing HLE service, unsupported instruction, missing function, fault or budget). Options are listed in `platform/pc/main.cpp` and in upstream's README; `--verify-bootstrap` needs measured `bootstrap_expectations.hpp` values.

A full report run (pull, build, tests, run with an input script, event summary and PNGs of the distinct screens):

```bash
bash tools/p1_report.sh <run number> 30000000 "<your ISO>" profiles/p1p/input/new_game.txt 6000
```

It stops by itself at the given vblank (6000 = about 100 s of game time) and writes `.tmp/p1/report<N>.txt` and `.tmp/p1/frames<N>/`. Input scripts in `profiles/p1p/input/`: `new_game.txt` (title, New Game, then confirms through the opening), `to_title.txt` (stops pressing at the title; the attract movie plays), `boot_presses.txt`. `--wav <file>` records the sound, `--draw-log <file> --draw-log-from <frame>` logs the GE draws of a frame.

## Merging upstream

```bash
git remote add upstream https://github.com/endercodezz/P3P3DS
git fetch upstream
git merge upstream/master
```

[INFERRED] Upstream changes to `core/`, `recomp/PSPRecomp/`, the renderer and the PC runner should mostly merge without conflicts: the fork's code is in its own files (`core/src/hle/fork_*.cpp`, `core/src/fork_vfs.cpp`, `platform/pc/fork_avcodec.cpp`, `platform/3ds/fork_mvd.hpp`, `platform/3ds/fork_ndsp.hpp`, `tools/convert_media.py`) plus wiring lines in `platform/pc/main.cpp`, `platform/pc/telemetry.hpp`, `platform/3ds/main.cpp`, `core/src/hle/hle_modules.cpp`, the Builder and both `CMakeLists.txt`. `platform/3ds/gpu_renderer.*` carries a few patches marked `P3P3DS:` and listed in `docs/CURRENT_STATE.md`; expect conflicts there if upstream changes the same lines. Upstream changes to the deleted P3P files (`profiles/p3p/`, `p3p/p3p-patches/`, the `p3p-modding-vfs` skill) show up as modify/delete conflicts: keep them deleted (`git rm <path>`), and port anything generic into `profiles/p1p/` by hand.

## Documentation

- [`docs/CURRENT_STATE.md`](docs/CURRENT_STATE.md): the verified frontier, newest first.
- [`docs/VERIFICATION.md`](docs/VERIFICATION.md), [`docs/NEXT_STEPS.md`](docs/NEXT_STEPS.md), [`docs/3DS_PLATFORM.md`](docs/3DS_PLATFORM.md): upstream P3P3DS documents, kept as reference.
- [`AGENTS.md`](AGENTS.md), [`CLAUDE.md`](CLAUDE.md): engineering rules; project skills in `.claude/skills/`.

## License

MIT, see [LICENSE](LICENSE) (copyright endercodezz and contributors; this fork's changes are under the same license). `recomp/PSPRecomp` is vendored third-party code (MIT). Research sources fetched by `python tools/fetch_references.py` and all game data are never part of the repository.

## Legal notice

This is an independent research project, not affiliated with, endorsed by or connected to Atlus, SEGA, Sony Interactive Entertainment or Nintendo. No proprietary game binaries, copyrighted assets or encryption keys of the consoles' firmware are distributed here; you must supply files from your own legally owned copy of the game.

---

## Upstream References & Credits

**Persona3DS is based on [P3P3DS](https://github.com/endercodezz/P3P3DS) by enderlit aka endercodezz.** P3P3DS stands on many open projects. Full inventory with licences and how each is used: [`docs/REPOSITORIES.md`](docs/REPOSITORIES.md). Code is never copied from GPL emulators; they are read as behaviour references.

**Static recompilation**

- [PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp) by Jessica Natalia — C++20 static recompilation framework (vendored in `recomp/PSPRecomp`, with small P3P3DS patches).
- [Yakumo](https://github.com/TeamGDB/Yakumo) by TeamGDB — Static recompilation port of *Monster Hunter Portable 3rd HD Ver.*
- [PSP-recompilation-project](https://github.com/sal063/PSP-recompilation-project) by sal063, [psprecomp](https://github.com/sp00nznet/psprecomp) by sp00nznet, [psprecomp](https://github.com/wizardengineer/psprecomp) by wizardengineer — other PSP static recompilers, design comparison.

**PSP behaviour and hardware**

- [PPSSPP](https://github.com/hrydgard/ppsspp) by Henrik Rydgård & contributors — PSP emulation ground truth and HLE reference.
- [pspautotests](https://github.com/hrydgard/pspautotests) — Hardware behavioral test suite (the HLE tests replay its expected outputs).
- [uOFW](https://github.com/uofw/uofw) — Reverse-engineered PSP firmware modules (kernel, ThreadMan, IoFileMgr, audio).
- [PSPSDK](https://github.com/pspdev/pspsdk), [vfpu-docs](https://github.com/pspdev/vfpu-docs), [prxtool](https://github.com/pspdev/prxtool), [psp-ghidra-scripts](https://github.com/pspdev/psp-ghidra-scripts) by pspdev — SDK headers and GE command list, VFPU documentation, PRX and NID tooling.
- [ghidra-allegrex](https://github.com/kotcrab/ghidra-allegrex) by kotcrab — Allegrex/VFPU processor module for Ghidra.
- [JPCSP](https://github.com/jpcsp/jpcsp) — Independent PSP emulator, second behaviour reference.

**Nintendo 3DS**

- [libctru](https://github.com/devkitPro/libctru), [citro3d](https://github.com/devkitPro/citro3d), [citro2d](https://github.com/devkitPro/citro2d), [3ds-examples](https://github.com/devkitPro/3ds-examples) and [picasso](https://github.com/devkitPro/picasso) by devkitPro — 3DS homebrew SDK, GPU library, samples and the PICA200 shader assembler.
- [nihstro](https://github.com/neobrain/nihstro) by neobrain — PICA200 shader assembler/disassembler, encoding cross-check.
- [Azahar](https://github.com/azahar-emu/azahar) — 3DS emulator used for testing builds.
- [DaedalusX64-3DS](https://github.com/MasterFeizz/DaedalusX64-3DS) by MasterFeizz — Battle-tested Citro3D rendering and NDSP audio pipeline for MIPS on 3DS.
- [GLASS](https://github.com/kynex7510/GLASS) by kynex7510 — OpenGL ES 2 layer over the PICA200, platform research.

**Persona 3 Portable and modding**

- [zarroboogs](https://github.com/zarroboogs/p3p-patches) & [DniweTamp](https://github.com/DniweTamp/Persona-3-Portable-Mod-Menu) — P3P community patches and reverse engineering research.
- [Atlus-Script-Tools](https://github.com/tge-was-taken/Atlus-Script-Tools), [Amicitia](https://github.com/tge-was-taken/Amicitia) and [AtlusFileSystemLibrary](https://github.com/tge-was-taken/AtlusFileSystemLibrary) by TGE, [AemulusModManager](https://github.com/TekkaGB/AemulusModManager) by TekkaGB, [CriFsV2Lib](https://github.com/Sewer56/CriFsV2Lib) by Sewer56, [CriPakTools](https://github.com/esperknight/CriPakTools) by esperknight — Atlus script, archive and CPK tools.
- [Persona 3 Dual](https://github.com/p3d-project/persona-3-dual) by the p3d-project team — Nintendo dual-screen Persona UI/presentation and constrained handheld-rendering reference.

**Other static recompilation and runtime references**

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) by hedge-dev — Xbox 360 PPC-to-C++ static recompilation architecture/analysis reference.
- [ReXGlue](https://github.com/rexglue/rexglue-sdk) by Tom Clay & contributors — Xbox 360 AOT runtime and Xenia-derived platform architecture reference.
- [Xenia](https://github.com/xenia-project/xenia) by Ben Vanik & contributors — Kernel, memory, and GPU architecture reference for Xbox recomp ecosystem.
- [N64Recomp](https://github.com/N64Recomp/N64Recomp) by Mr-Wiseguy & contributors — MIPS static recompilation, indirect calls, relocations and jump-table reference.
- [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime), [Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp) and [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) — runtimes and complete recompiled games.
