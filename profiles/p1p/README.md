# Persona (ULUS-10432) profile

Game-specific data for *Shin Megami Tensei: Persona* (PSP remake, North American
release `ULUS-10432`). Both CMake projects read the profile selected by the
cache variable `P3P_PROFILE_DIR` (default `profiles/p1p`); the runtime itself is
game-neutral.

## Layout

```text
profiles/p1p/
  README.md
  config/
    p1p_ulus10432.toml          identity and checksums (tools/prepare_game.py)
    profile.cmake               CMake settings: functions CSV, CWCheat .ini, SMDH strings, smoke-test gate
    profile_info.hpp            title, disc ID, SD folder, mods alias, ELF SHA-256 (runners, Builder)
    aot_layout.cmake            psp_recomp --auto load base, unit span, unit count
    bootstrap_expectations.hpp  bootstrap checkpoint values and root stack (runners)
    p1p_functions.csv           function manifest for MANUAL generation
    frontier_seeds.csv          managed seeds (tools/chase_frontier.py)
    cwcheat_patches.txt         selected CWCheat codes (none)
  input/
    title_start.txt             commented template of a controller script
  game/                         local only (gitignored): eboot.elf, USRDIR/
```

## Measured values and remaining placeholders

Measured by the maintainer on their own copy (2026-10-11, details in
`docs/CURRENT_STATE.md`):

- `[VERIFIED]` ISO MD5 `c75f75b0f1e92d7cca77d75585bebd82`, encrypted
  `EBOOT.BIN` SHA-256 `30990909...dfa629de`, EBOOT tag `0xD9160BF0`.
- `[VERIFIED]` Decrypted ELF SHA-256 `60c050e3...30121a1b`: the hash of
  PPSSPP's decrypted EBOOT dump. Another decryptor may produce different bytes.
- `[VERIFIED]` Relocatable PRX (ELF type `0xFFA0`), `e_entry` `0x114`, module
  `pspsys`; runner entry `0x08804114`; `psp_recomp --auto` gives 153 units.
- `[INFERRED]` Load base `0x08804000` (the loader's choice for a relocatable
  PRX; not measured on hardware).

Still `TODO` (never filled with guesses):

- `p1p_ulus10432.toml`: `ram_mb`, `sdk_version`.
- `bootstrap_expectations.hpp`: every checkpoint value except `entry`; the
  root stack (`0x0A000000`, 64 KiB) is `[INFERRED]` from the PSP memory map.
- `p1p_functions.csv`: `user_main` is a commented row.

## Preparing the game

This writes `profiles/p1p/game/eboot.elf` and `profiles/p1p/game/USRDIR/`.
The built-in decryptor only knows the EBOOT tag `0xD91613F0` (the tag P3P
uses); Persona's EBOOT uses `0xD9160BF0` `[VERIFIED]`, so dump a decrypted
EBOOT with PPSSPP and pass it with `--decrypted-eboot <file>`:

```bash
python tools/prepare_game.py "<your Persona ISO>" --decrypted-eboot <PPSSPP dump> --check-only
python tools/prepare_game.py "<your Persona ISO>" --decrypted-eboot <PPSSPP dump>
```

No commercial executables, disc images or game assets are stored in this
repository.
