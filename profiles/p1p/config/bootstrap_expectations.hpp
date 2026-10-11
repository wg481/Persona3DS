#pragma once
#include <cstdint>
namespace p3p3ds::profile {
// Persona (ULUS-10432). TODO: every 0 below is a placeholder until measured
// on the PC runner (docs/CURRENT_STATE.md); the --verify-bootstrap smoke test
// stays unregistered until then (P3P_PROFILE_BOOTSTRAP_VERIFIED in profile.cmake).
// [VERIFIED] entry: PC runner "entry=0x08804114"; user_main: PC runner
// "[THREAD CREATE] uid=2 name=user_main entry=0x8804228" (maintainer, 2026-10-11).
inline constexpr std::uint32_t entry=0x08804114, user_main=0x08804228;
// [VERIFIED] $a0 of module start's first two imports (dump_function at
// 0x08804114): lui v0,0x0505 / ori a0,v0,0x10 before the jal at 0x08804140
// (sceKernelSetCompiledSdkVersion500_550), lui v0,3 / ori a0,v0,0x306 in the
// delay slot of the jal at 0x0880414C ([INFERRED] sceKernelSetCompilerVersion).
inline constexpr std::uint32_t sdk=0x05050010, compiler=0x00030306;
// A completed guest return used by the bootstrap checkpoint. TODO.
inline constexpr std::uint32_t return_site=0 /*TODO*/, return_target=0 /*TODO*/;
// Framebuffer addresses chosen by the game. TODO.
inline constexpr std::uint32_t color=0 /*TODO*/, display=0 /*TODO*/, depth=0 /*TODO*/;
// Initial root-thread stack, used by both runners to start the game.
// [INFERRED] top of 32 MiB PSP user RAM, the value measured for P3P; check
// against Persona's first stack accesses.
inline constexpr std::uint32_t stack_top=0x0A000000, stack_size=0x10000;
}
