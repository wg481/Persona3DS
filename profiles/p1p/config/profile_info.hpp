#pragma once
// Identity and host paths of the selected game profile (P3P_PROFILE_DIR),
// used by the 3DS runner, the PC runner and the Builder.
namespace p3p3ds::profile {
inline constexpr const char *game_title = "Shin Megami Tensei: Persona";
inline constexpr const char *disc_id = "ULUS10432";          // PARAM.SFO DISC_ID [UNVERIFIED]
inline constexpr const char *disc_id_display = "ULUS-10432";
inline constexpr const char *app_name = "Persona3DS";
inline constexpr const char *output_3dsx = "persona3ds.3dsx";
inline constexpr const char *sd_folder = "persona3ds";
// Guest folder served from the SD card's mods/ directory. Empty: no mod
// loader path is known for this game, so no alias is mounted.
inline constexpr const char *mods_alias = "";
// SHA-256 of the decrypted EBOOT. Empty: TODO, checks are skipped with a notice.
inline constexpr const char *elf_sha256 = "";
}
// SD card base folder on the 3DS (a macro so string literals concatenate).
#define P3P_PROFILE_SD_BASE "sdmc:/persona3ds"
