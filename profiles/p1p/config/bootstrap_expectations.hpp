#pragma once
#include <cstdint>
namespace p3p3ds::profile {
// Persona (ULUS-10432). TODO: every 0 below is a placeholder until the
// decrypted EBOOT is measured on the PC runner (docs/CURRENT_STATE.md); the
// --verify-bootstrap smoke test stays unregistered until then
// (P3P_PROFILE_BOOTSTRAP_VERIFIED in profile.cmake).
inline constexpr std::uint32_t entry=0 /*TODO*/, user_main=0 /*TODO*/;
inline constexpr std::uint32_t sdk=0 /*TODO*/, compiler=0 /*TODO*/;
// A completed guest return used by the bootstrap checkpoint. TODO.
inline constexpr std::uint32_t return_site=0 /*TODO*/, return_target=0 /*TODO*/;
// Framebuffer addresses chosen by the game. TODO.
inline constexpr std::uint32_t color=0 /*TODO*/, display=0 /*TODO*/, depth=0 /*TODO*/;
// Initial root-thread stack, used by both runners to start the game.
// [INFERRED] top of 32 MiB PSP user RAM, the value measured for P3P; check
// against Persona's first stack accesses.
inline constexpr std::uint32_t stack_top=0x0A000000, stack_size=0x10000;
}
