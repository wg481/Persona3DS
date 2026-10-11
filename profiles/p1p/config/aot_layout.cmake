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
