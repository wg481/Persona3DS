#pragma once
// Kernel imports added by the Persona3DS fork for games other than P3P.
// Kept in their own files (plus one registration call in hle_modules.cpp) so
// that upstream P3P3DS changes keep merging; drop an entry here if upstream
// implements the same NID.
#include <cstdint>

namespace psprecomp {
class Runtime;
}

namespace p3p3ds {
class KernelState;
}

namespace p3p3ds::hle {

// SysMemUserForUser::0x91DE343C
// int sceKernelSetCompiledSdkVersion500_550(int sdkVersion);
// uOFW src/kd/sysmem/misc.c (sceKernelSetCompiledSdkVersion500_550): accepts
// a version whose upper 16 bits are 0x0500 or 0x0505, sets the "SDK version
// set" game-info flag (0x1000) and returns 0. Any other version resets the
// hardware and never returns. Here: the accepted case records the version
// (as the 600_620 handler does) and returns true; otherwise nothing is
// recorded and false is returned so the caller can stop the run.
[[nodiscard]] bool sceKernelSetCompiledSdkVersion500_550(KernelState &kernel, std::uint32_t version) noexcept;

void register_fork_kernel_imports(psprecomp::Runtime &runtime, KernelState &kernel);

} // namespace p3p3ds::hle
