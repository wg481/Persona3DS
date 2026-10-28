# Persona3DS

Run ***Shin Megami Tensei: Persona*** (PSP, North American release `ULUS-10432`) natively on the **New Nintendo 3DS / New 3DS XL / New 2DS XL**. The game's MIPS code is statically recompiled to C++ ahead of time, and the PSP's operating system, graphics, audio and movie playback are replaced by a native runtime. Old 3DS models are not supported.

Persona3DS is a fork of [P3P3DS](https://github.com/endercodezz/P3P3DS) by enderlit aka endercodezz, retargeted from *Persona 3 Portable* to Persona.

## Playability

Tested on a New 3DS, from New Game through the first dungeon:

- The title screen, menus, events and gameplay run at full speed, with music and sound effects.
- Movies play from files you convert yourself (see [Music and movies](#music-and-movies)) on the New 3DS's hardware video decoder.
- Both the `.3dsx` (Homebrew Launcher) and the installable `.cia` build work, and closing the `.cia` from the HOME Menu works.
- Known issue: a few dungeon spots with corners or stairs drop slightly below full speed (about 96–100 %).

Play beyond the first dungeon has not been tested yet. Open issues and measurements are in [`docs/CURRENT_STATE.md`](docs/CURRENT_STATE.md), newest first.

## What you need

- Your own *Shin Megami Tensei: Persona* `ULUS-10432` ISO (MD5 `c75f75b0f1e92d7cca77d75585bebd82`).
- A decrypted copy of its `EBOOT.BIN`. The built-in decryptor does not support this game's encryption, so dump it with [PPSSPP](https://www.ppsspp.org/): turn on its option to dump the decrypted EBOOT.BIN on game boot (in the developer tools), start the game once, and take the file from PPSSPP's `PSP/SYSTEM/DUMP/` folder.
- On Windows: [MSYS2](https://www.msys2.org/) for the PC tools, and [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the 3DS packages for the 3DS build. On Linux or macOS the same tools work from a normal shell.
  - MSYS2 UCRT64 shell: `pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-python`
- For music and movies: your own ffmpeg with ffprobe and libx264 (MSYS2 UCRT64: `pacman -S mingw-w64-ucrt-x86_64-ffmpeg`).
- For sound on the 3DS: the DSP firmware dump `sdmc:/3ds/dspfirm.cdc` (made once with the DSP1 homebrew, as for any homebrew with sound).

## Building

Two shells are used on Windows: the **MSYS2 UCRT64** shell for steps 1–4 and the **devkitPro** shell for step 5. The PC build fails in the devkitPro shell.

1. Check and unpack the game (writes `profiles/p1p/game/`, which is never committed):

   ```bash
   python tools/prepare_game.py "<your Persona ISO>" --decrypted-eboot "<PPSSPP's dumped EBOOT>"
   ```

2. Convert the music and movies once (writes `profiles/p1p/game/media/`; all 44 movies take a while; `--skip-movies` converts music only):

   ```bash
   python tools/convert_media.py
   ```

3. Build the PC tools, including the recompiler, and run the tests:

   ```bash
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build build
   ctest --test-dir build --output-on-failure
   ```

4. After pulling new changes, rerun `cmake --build build` before the 3DS build, so the 3DS build uses the current recompiler.

5. Build the 3DS app (devkitPro shell):

   ```bash
   cmake -S platform/3ds -B build/3ds -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake -DCMAKE_BUILD_TYPE=Release
   make -C build/3ds
   ```

   The first build recompiles the whole game and takes a while.

Copy to the SD card:

| SD card path | What |
|---|---|
| `/3ds/p3p3ds.3dsx` | the app, from `build/3ds/` |
| `/persona3ds/<your ISO>.iso` | the game |
| `/persona3ds/media/` | the contents of `profiles/p1p/game/media/` (optional: without it music is silent and movies are black) |
| `/3ds/dspfirm.cdc` | DSP firmware dump, needed for sound |

Quit with START+SELECT. The built app contains code generated from your copy of the game: it is for your own use, never for distribution.

## Building a CIA

The `.cia` installs to the HOME Menu with FBI or a similar title manager. It runs like the `.3dsx` and uses the same SD card files.

1. Make a folder `banner/` at the repository root (it is ignored by git) containing:
   - `bannertool` and `makerom` (or `bannertool.exe` and `makerom.exe`)
   - `banner.png` (256×128) and `audio.wav`, the HOME Menu banner and its sound
   - optionally `icon.png` (48×48); without it the `.3dsx` icon and title are used
2. After the normal 3DS build, in the devkitPro shell:

   ```bash
   make -C build/3ds cia
   ```

3. Install `build/3ds/p3p3ds.cia`.

The title, product code and unique id are in `profiles/p1p/config/profile.cmake`. The process settings (New 3DS 124 MB mode, 804 MHz, L2 cache, core 2) are in `platform/3ds/cia/p3p3ds.rsf.in`. `-DP3P_BANNER_DIR=<folder>` uses a different banner folder.

## Debugging

**Bottom screen.** It shows the title, build, speed and controls. Touch it to switch to the full performance report and back.

**Reports.** On quit (START+SELECT) the app writes `/persona3ds/report.txt`: speed, the split of the main core's time (game code, system calls, idle), memory, sound, music, movie and renderer counters. Include it in any bug report. `exit_log.txt` records the shutdown steps.

**Switch files.** Create an empty file with one of these names in `/persona3ds/` to turn an option on:

| File | Effect |
|---|---|
| `debug_screen.txt` | start with the full report on the bottom screen |
| `profile_seconds.txt` | also write `profile_seconds.csv`: one row per second with the time split, draws and the 5 most expensive system calls |
| `dump_every.txt` | containing a number N: save every Nth frame as a BMP to `/persona3ds/frames/` (slow) |
| `gpu_trace.txt` | log the first 1,200 frames' draws to `gpu_trace.log` |
| `draw_trace.txt` | log the draws of a few movie frames to `draw_trace_out.txt` (slows movies) |
| `play_movies.txt` | don't skip unconverted movies automatically |
| `no_gpu_thread.txt` | run graphics on the main core instead of core 2 |
| `no_frame_split.txt` | send each GPU frame whole at its end instead of in parts |
| `no_inline_imports.txt` | route system calls through the dispatcher, as older builds did |

The last three switch off optimizations, to check whether one of them causes a problem.

**Profile build.** It shows which game code uses the most time. It is a little slower than the normal build and writes `/persona3ds/aot_profile.txt` on quit (devkitPro shell, after `cmake --build build`):

```bash
cmake -S platform/3ds -B build/3ds-prof -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake -DCMAKE_BUILD_TYPE=Release -DP3P_AOT_PROFILE=ON
make -C build/3ds-prof
```

To look up a hot address from that file, from the UCRT64 shell:

```bash
./build/recomp/PSPRecomp/dump_function profiles/p1p/game/eboot.elf 0x<address> 200
```

`-DP3P_AOT_GPR_CACHE=OFF` builds the 3DS app without the register optimization in the generated code. Use it to check whether a problem comes from that optimization.

**PC runner.** The game also runs on PC, which is easier to debug:

```bash
./build/p3p_pc_bootstrap --run-until-blocker --max-dispatches 1000000 --dump-events .tmp/p1/run.json
```

It prints why it stopped (a missing system service, an unsupported instruction, a fault or the step budget). The options are listed in `platform/pc/main.cpp`.

## Credits

- **[P3P3DS](https://github.com/endercodezz/P3P3DS)** by enderlit aka endercodezz: the recompiler integration, PSP runtime, renderer and tools this fork is built on.
- **[PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp)** by Jessica Natalia: the static recompiler, included in `recomp/PSPRecomp` with small patches.
- **[libctru](https://github.com/devkitPro/libctru)** and **[citro3d](https://github.com/devkitPro/citro3d)** by devkitPro: the 3DS SDK and GPU library.
- **[PPSSPP](https://github.com/hrydgard/ppsspp)** by Henrik Rydgård and contributors, and **[pspautotests](https://github.com/hrydgard/pspautotests)**: the reference for PSP behaviour and the tests the runtime is checked against. No emulator code is copied.

The full list of projects used as references, with their licences, is in [`docs/REPOSITORIES.md`](docs/REPOSITORIES.md).

## License

MIT, see [LICENSE](LICENSE) (copyright endercodezz and contributors; this fork's changes are under the same license). `recomp/PSPRecomp` is third-party MIT code.

This is an independent project, not affiliated with or endorsed by Atlus, SEGA, Sony Interactive Entertainment or Nintendo. No game files, copyrighted assets or console keys are distributed; you need your own legally owned copy of the game.
