# CMake-side settings of the Persona (ULUS-10432) profile. Included by the root
# CMakeLists.txt and platform/3ds/CMakeLists.txt after P3P_PROFILE_DIR is set.

# Function manifest merged into MANUAL-mode generation (tools/chase_frontier.py).
set(P3P_PROFILE_FUNCTIONS_CSV "${P3P_PROFILE_DIR}/config/p1p_functions.csv")
# Community CWCheat .ini applied to the EBOOT before generation. Empty: no
# patches (an empty patch list is generated). No P1 code list is tracked yet.
set(P3P_PROFILE_CWCHEAT_INI "")
# bootstrap_expectations.hpp still holds placeholders: the PC runner's
# --verify-bootstrap smoke test is not registered until they are measured.
set(P3P_PROFILE_BOOTSTRAP_VERIFIED OFF)
# .3dsx metadata.
set(P3P_PROFILE_SMDH_NAME "Persona3DS")
set(P3P_PROFILE_SMDH_DESCRIPTION "Persona (PSP) recompilation")
set(P3P_PROFILE_SMDH_AUTHOR "wg481, based on P3P3DS by enderlit aka endercodezz")
