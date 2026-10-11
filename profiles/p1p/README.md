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

## Placeholders

Nothing here has been measured on the game yet. Values marked `TODO` are
unknown until the EBOOT is decrypted; they are deliberately not filled with
guesses:

- `p1p_ulus10432.toml`: `ram_mb`, `load_base`, `sdk_version`, `entry_point`,
  `encrypted_eboot_sha256`, `decrypted_elf_sha256` (`iso_md5` is a third-party
  value, `[UNVERIFIED]`).
- `aot_layout.cmake`: `P3P_AOT_UNIT_COUNT` (empty; configuring with the EBOOT
  present fails with the command that measures it). Load base `0x08804000` is
  `[INFERRED]`.
- `bootstrap_expectations.hpp`: every checkpoint value is `0`; the root stack
  (`0x0A000000`, 64 KiB) is `[INFERRED]` from the PSP memory map.
- `profile_info.hpp`: `elf_sha256` is empty.
- `p1p_functions.csv`: `module_start` / `user_main` are commented rows.

## Preparing the game

```bash
python tools/prepare_game.py "<your Persona ISO>" --check-only
python tools/prepare_game.py "<your Persona ISO>"
```

This writes `profiles/p1p/game/eboot.elf` and `profiles/p1p/game/USRDIR/`.
While the checksums are placeholders the tool prints the measured hashes and
continues with a warning. The built-in decryptor only knows the EBOOT tag
`0xD91613F0` (the tag P3P uses); if Persona's EBOOT uses another tag
`[UNVERIFIED]`, dump a decrypted EBOOT with PPSSPP and pass it with
`--decrypted-eboot <file>`.

No commercial executables, disc images or game assets are stored in this
repository.
