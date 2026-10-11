#pragma once
// Decryption of a retail EBOOT.BIN (~PSP container, tag 0xD91613F0: the tag
// of P3P ULUS-10512; other games may use others [UNVERIFIED]) into the ELF the
// recompiler reads. C++ port of
// tools/prepare_game.py (decrypt_eboot), checked against its output by
// tests/test_psp_eboot.cpp via the reference SHA-256.
#include "profile_info.hpp" // <P3P_PROFILE_DIR>/config
#include <cstdint>
#include <string>
#include <vector>

namespace p3p3ds::builder {

// SHA-256 of the profile's decrypted executable (tools/prepare_game.py).
// Empty while the profile is not measured: the check is skipped.
inline constexpr const char *kExpectedElfSha256 = p3p3ds::profile::elf_sha256;

// AES-128-CBC decryption with a zero IV (the only mode the container uses).
std::vector<std::uint8_t> aes128_cbc_decrypt(const std::uint8_t key[16], const std::uint8_t *data, std::size_t size);

// Returns the decrypted ELF, or throws std::runtime_error with a user-facing message.
std::vector<std::uint8_t> decrypt_eboot(const std::vector<std::uint8_t> &eboot);

std::string sha256_hex(const std::vector<std::uint8_t> &data);

} // namespace p3p3ds::builder
