# Persona3DS

**Persona3DS** is a fork of [P3P3DS](https://github.com/endercodezz/P3P3DS) by enderlit aka endercodezz, retargeted from *Persona 3 Portable* to ***Shin Megami Tensei: Persona*** (the 2009 PSP remake of the first Persona, North American release `ULUS-10432`). The goal is the same as upstream's: run the game natively on the **New Nintendo 3DS / New 3DS XL / New 2DS XL** by statically recompiling the PSP's MIPS code to C++ ahead of time and replacing the PSP operating system, graphics and audio with a high-level runtime.

All the engineering (the static recompiler, the PSP HLE runtime, the GE-to-PICA200 renderer, NDSP audio, the PC runner and the Builder) comes from P3P3DS. This fork only adds a selectable game profile and the Persona profile, so that upstream fixes can still be merged with `git merge`.

## Status: does not boot yet

- [VERIFIED] The host runtime, tools, Builder and tests build and pass without game data (Linux, GCC 13; `ctest` 24/24 with 3 skipped tests that need game data or reference clones).
- [UNVERIFIED] Nothing has been run on Persona's executable yet. Its load base, RAM size, SDK version, entry point, EBOOT checksums and AOT unit count are placeholders (`TODO`) in [`profiles/p1p/`](profiles/p1p/README.md) until the EBOOT is decrypted and measured.
- [UNVERIFIED] The built-in EBOOT decryptor only knows the encryption tag P3P uses (`0xD91613F0`). If Persona's EBOOT uses another tag, decrypt it with another tool (e.g. PPSSPP's decrypted EBOOT dump) and pass it with `--decrypted-eboot`.
- [UNVERIFIED] Which PSP services Persona imports beyond those P3P needed is unknown; the first PC run will stop at the first missing one.

The verified frontier and its history are in [`docs/CURRENT_STATE.md`](docs/CURRENT_STATE.md) (the older entries describe upstream's P3P work).

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
- Your own *Shin Megami Tensei: Persona* `ULUS-10432` ISO. Reported ISO MD5 `c75f75b0f1e92d7cca77d75585bebd82` (third-party listing, [UNVERIFIED]; `prepare_game.py` prints the measured value).

## Game setup

```bash
python tools/prepare_game.py "<your Persona ISO>" --check-only
python tools/prepare_game.py "<your Persona ISO>"
```

It checks `PARAM.SFO` (`ULUS10432`), decrypts `EBOOT.BIN` to `profiles/p1p/game/eboot.elf` and extracts `USRDIR/`. While the profile's checksums are placeholders it prints the measured hashes and continues with a warning; paste them into the profile once confirmed. Game files never enter the repository.

## Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Without `profiles/p1p/game/eboot.elf` both CMake projects build everything that does not come from the game (runtime, tools, Builder, tests). With the EBOOT present, configuring stops until `P3P_AOT_UNIT_COUNT` is set in `aot_layout.cmake`; the error message prints the `psp_recomp --auto` command that measures it.

New 3DS (devkitPro msys2 shell, after the host build):

```bash
cmake -S platform/3ds -B build/3ds -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake -DCMAKE_BUILD_TYPE=Release
make -C build/3ds
```

On the SD card: `/3ds/p3p3ds.3dsx` (CMake target name kept from upstream; the Builder names it `persona3ds.3dsx`) and your ISO in `/persona3ds/`. The `.3dsx` embeds code generated from your game executable: it is for your own use, never for distribution.

## Run on PC

```bash
./build/p3p_pc_bootstrap --run-until-blocker --max-dispatches 1000000 --dump-events .tmp/p1/run.json
```

The runner prints why it stopped (missing HLE service, unsupported instruction, missing function, fault or budget). Options are listed in `platform/pc/main.cpp` and in upstream's README; `--verify-bootstrap` needs measured `bootstrap_expectations.hpp` values.

## Merging upstream

```bash
git remote add upstream https://github.com/endercodezz/P3P3DS
git fetch upstream
git merge upstream/master
```

[INFERRED] Upstream changes to `core/`, `recomp/PSPRecomp/`, the renderer and the PC runner should merge without conflicts, because this fork only touched them for profile wiring (a few lines in `platform/pc/main.cpp`, `platform/pc/telemetry.hpp`, `platform/3ds/main.cpp`, the Builder and both `CMakeLists.txt`). Upstream changes to the deleted P3P files (`profiles/p3p/`, `p3p/p3p-patches/`, the `p3p-modding-vfs` skill) show up as modify/delete conflicts: keep them deleted (`git rm <path>`), and port anything generic into `profiles/p1p/` by hand.

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
