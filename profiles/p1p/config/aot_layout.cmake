# ULUS-10432 whole-.text AOT layout for `psp_recomp --auto`.
#
# Load base: [INFERRED] the PSP user-module default (psprecomp
# kDefaultPspUserLoadBase, also P3P's measured base); the EBOOT is a
# relocatable PRX, so the base is the loader's choice, not measured on hardware. Span: 16 KiB matches the runtime's shift-only unit index fast
# path (recomp/PSPRecomp/src/runtime.cpp, generated_unit_span_ == 16384u).
#
# [VERIFIED] Maintainer, 2026-10-11: `psp_recomp profiles/p1p/game/eboot.elf
# --auto <dir> 0x08804000 16384` on the PPSSPP-dumped ELF (SHA-256
# 60c050e36bd874ad0b16e794002c7bcafd6ae833c12922c28520a6fa30121a1b) reports
# 6,211 function seeds, 483,724 emitted instruction PCs, 88,312 blocks and
# 153 translation units; the full build then succeeded.
# tools/verify_aot_layout.py fails the build if the generator disagrees.
set(P3P_AOT_LOAD_BASE "0x08804000")
set(P3P_AOT_UNIT_SPAN "16384")
set(P3P_AOT_UNIT_COUNT "153")

# Guest leaf functions replaced by a host routine at their fixed JAL call sites
# (`psp_recomp --native-leaf ADDR=KIND`, recomp/PSPRecomp/tools/codegen_main.cpp).
# A call still runs the guest function when the host routine declines (range
# outside main RAM, write watch).
# 0x089649E0 memset: [VERIFIED] disassembly 0x089649E0-0x08964AB4 (maintainer's
# hot_dump.txt of the ELF above, 2026-10-15): a0 destination, a1 byte value
# (`andi a1,0xFF` / `seb`), a2 length; byte loop below 16 bytes, aligned 8-byte
# `sw` loop at 0x08964A40, tail bytes; returns a0 in v0 (`jr ra; addu v0,a0`).
# [VERIFIED] AOT profile of build 9180b513: 14.6% of generated-code samples,
# 6600 of them in the 0x08964A40 loop.
# [INFERRED] callers use only v0: the routine leaves v1/a1-a3/t0 as scratch
# (MIPS o32 caller-saved), the host routine sets v0 only.
set(P3P_AOT_NATIVE_LEAVES "0x089649E0=memset")
