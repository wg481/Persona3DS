#pragma once
// Shown on the build page while the compiler runs. Only project facts that do
// not depend on measurements of the target game (P3P3DS facts measured on
// Persona 3 Portable were removed with the retarget to Persona).
namespace p3p3ds::builder {

inline constexpr const wchar_t *kFunFacts[] = {
    L"Persona3DS does not emulate the PSP processor on your 3DS: the game's MIPS code is translated to C++ and compiled to ARM code - right now, on your PC.",
    L"The translated code is grouped into C++ files of 16 KiB of game code each, compiled in parallel.",
    L"On the 3DS, the PICA200 graphics chip draws what the PSP's Graphics Engine drew. The 480x272 picture is scaled to the 400x240 top screen.",
    L"Your ISO stays on the SD card: Persona3DS reads the game's files from it while you play.",
    L"The bottom screen shows a live report: frame rate, speed, memory. It is also saved to sdmc:/persona3ds/report.txt.",
    L"Persona3DS is built on P3P3DS by enderlit aka endercodezz, a static recompilation of Persona 3 Portable for the New 3DS.",
};

} // namespace p3p3ds::builder
