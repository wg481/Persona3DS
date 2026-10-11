# ULUS-10432 whole-.text AOT layout for `psp_recomp --auto`.
#
# Load base: [INFERRED] the PSP user-module default (psprecomp
# kDefaultPspUserLoadBase, also P3P's measured base); not yet measured on the
# Persona EBOOT. Span: 16 KiB matches the runtime's shift-only unit index fast
# path (recomp/PSPRecomp/src/runtime.cpp, generated_unit_span_ == 16384u).
#
# TODO: P3P_AOT_UNIT_COUNT is unknown until the decrypted EBOOT is measured:
#   build/recomp/PSPRecomp/psp_recomp profiles/p1p/game/eboot.elf --auto .tmp/p1/auto 0x08804000 16384
# prints the number of translation units (buckets 0..N-1); set it to N here.
# tools/verify_aot_layout.py fails the build if the generator disagrees.
# Empty: both CMake projects refuse to configure while the EBOOT is present.
set(P3P_AOT_LOAD_BASE "0x08804000")
set(P3P_AOT_UNIT_SPAN "16384")
set(P3P_AOT_UNIT_COUNT "")
