# P3P3DS — Engineering Rules & Working Notes

Working instructions for all contributors and coding agents working in this repository. Read this file before performing any task.

---

## 1. What is P3P3DS

**Objective:** Run *Shin Megami Tensei: Persona* (PSP remake, `ULUS-10432`, North American release) on the **New Nintendo 3DS / New 3DS XL / New 2DS XL** family of consoles.

This repository (Persona3DS) is a fork of [P3P3DS](https://github.com/endercodezz/P3P3DS), which targets *Persona 3 Portable* (`ULUS-10512`). The engine is unchanged and game-neutral; everything game-specific lives in the profile selected by `P3P_PROFILE_DIR` (default `profiles/p1p`). Do not edit `core/`, `recomp/PSPRecomp/`, `platform/3ds/gpu_renderer.*` or `platform/pc/` beyond profile wiring, and do not rename existing identifiers, namespaces (`p3p3ds::`), CMake variables (`P3P_*`) or files: upstream fixes must keep merging with `git merge`. Comments and docs that mention P3P are upstream history. Exception (maintainer decision, 2026-10-11): `platform/3ds/gpu_renderer.*` may get minimal, evidence-backed patches for Persona's rendering, each marked with a `P3P3DS:` comment and listed in `docs/CURRENT_STATE.md`. Second exception (maintainer decision, 2026-10-11): `core/src/hle/ge.cpp` and `core/include/p3p3ds/profile.hpp` may get the same kind of minimal, evidence-backed, `P3P3DS:`-marked patches for the GE command path's speed and its measurement, listed in `docs/CURRENT_STATE.md`.

**Working Hypothesis (Architecture C: Hybrid Static Recompilation):**
- **Game Allegrex code:** Ahead-of-Time (AOT) offline static recompilation into C/C++ translation units, compiled to native ARM11 machine code via devkitARM GCC.
- **PSP OS & Kernel services:** Lightweight, modular High-Level Emulation (HLE) runtime (C/C++).
- **PSP Graphics Engine (GE):** Native `citro3d` / DMP PICA200 hardware renderer backend.
- **PSP Audio:** Native Nintendo 3DS DSP (`ndsp`) hardware-accelerated audio backend.
- **PSP Filesystem:** Multi-tier Virtual File System (VFS) with runtime mod and asset redirection on SDMC (`sdmc:/persona3ds/`).

*The architecture is not a dogma:* If empirical experiments or profiling demonstrate that an aspect (e.g. interpreter fallback for unresolved indirect calls, or an alternative renderer pipeline) is superior, document the proof with benchmarks and propose the revision.

---

## 2. Workspace Containment

All temporary files, caches, build output, and generated data produced by the project must reside strictly inside the root of the P3P3DS workspace:
- **Never use external directories** such as `C:\`, user home (`~`), `%TEMP%`, `%APPDATA%`, `%LOCALAPPDATA%`, Documents, Desktop, or other host paths without explicit user permission.
- **Allowed workspace directories:**
  - `.tmp/` — Temporary test files, intermediate script scratchpads.
  - `.cache/` — Build and tool caches.
  - `build/` — Intermediate build artifacts.
  - `out/` — Final binary artifacts.
- **Environment redirection:** Where possible before running tests or compilers, redirect `TEMP`, `TMP`, and `TMPDIR` to `.tmp/` within the workspace.
- **Host environment integrity:** Never install global dependencies, modify system PATH, touch the Windows registry, or edit global user configs without explicit user authorization.

---

## 3. Core Engineering Principle: MEASURE FIRST

Never make unverified technical claims. Do not write phrases such as:
- *"will run at 60 FPS"*
- *"zero cost abstraction"*
- *"native speed"*
- *"1:1 hardware mapping"*
- *"JIT is impossible"*
- *"this API is completely unused"*

without providing concrete measurements, source code citations, disassembly excerpts, or reproducible benchmarks.

**Claim Status Markings:**
Every non-trivial architectural or technical assertion in documentation and reports must be tagged:
- `[VERIFIED]`: Confirmed against real game executable, hardware test, or reference code (must cite exact repository, file, and line/symbol).
- `[INFERRED]`: Strongly suggested by architecture or patterns, but not yet directly measured on target hardware.
- `[UNVERIFIED]`: Working hypothesis or theoretical assumption requiring empirical testing.
- `[WRONG]`: Previously believed claim refuted by empirical test or source inspection (keep in log to prevent regressions).

---

## 4. Source Hierarchy

Read and follow [AGENTS.md Section 4](AGENTS.md#4-source-hierarchy) for the canonical source policy, including PSP API research order and the prohibition on copying PPSSPP implementation code.

---

## 5. Repository Boundaries & Third-Party Code

Tracked third-party code: `recomp/PSPRecomp`. Never mass-refactor or reformat them; a needed change is the **minimal viable patch**, commented with the reason (`P3P3DS:`), without unrelated cleanups.

Research sources (`references/`, `psp/`, `3ds/`, other recompilers in `recomp/`, the asset tools) are untracked clones made by `python tools/fetch_references.py`; read them, never edit them. Everything else, including `tools/*.py`, is P3P3DS code.

---

## 6. Development Order (Small Verifiable Steps)

Read and follow [AGENTS.md Section 6](AGENTS.md#6-p3p3ds-development-workflow) for the canonical micro-sprint workflow and hard STOP rule, and Section 7 for frontier verification. Keep that operational policy in one place; the sequence below is only the long-term roadmap.

Do not attempt to fix Allegrex decoding, write the HLE kernel, port the graphics renderer, and compile for 3DS simultaneously.

Follow the verified sequence:
```text
Static Analysis (EBOOT.ELF)
  ↓
Import & Function Boundary Discovery
  ↓
AOT Translation Unit Generation
  ↓
PC Host Runner Execution
  ↓
Core HLE (SysMem, ThreadMan, IoFileMgr)
  ↓
Game Boot Milestone (Titles / Disclaimers on PC)
  ↓
Graphics (GE display lists) & Audio (NDSP)
  ↓
Nintendo 3DS Platform Integration
```

---

## 7. Testing & Verification

Every functional change must include a verifiable test method:
- Unit test for isolated logic;
- Comparison against `pspautotests` for instruction or OS behavior;
- Behavioral diff against PPSSPP headless/log output;
- Deterministic trace log (e.g. instruction count, registered thread UIDs, opened file paths);
- Known P3P boot milestone (e.g. `module_start` reached, CRI initialization, first `sceIoOpen`).

*"Compiled successfully" is never proof of behavioral correctness.*

---

## 8. Persona (PSP) Target Scope

- Initial target: **ULUS-10432** (*Shin Megami Tensei: Persona*, US release, 2009-09-22).
- Do not introduce other regions (EU or JP releases) until the US build boots and functions correctly.
- All game-specific addresses, hook offsets, and structures must reside in `profiles/p1p/config/` with clear attribution and verification tags. Values not yet measured stay `TODO` placeholders, never plausible-looking guesses.
- **Never scatter hardcoded magic addresses across generic runtime code.**

---

## 9. Modding & Localization Architecture

Generic modding support and arbitrary localization packages are first-class architectural requirements:
- Asset modifications must not require recompiling the runtime executable.
- The runtime must remain language-neutral: no specific translation or language may be hardcoded into the core engine.
- Support arbitrary community mod packages and fan translations equally; community binary hooks (CWCheat/EBOOT patches) are applied cleanly through the profile.
- No mod loader path is known for Persona yet (`mods_alias` is empty in `profiles/p1p/config/profile_info.hpp`); P3P's mod chain was upstream-specific.

---

## 10. Nintendo 3DS Target Constraints

- Target **New Nintendo 3DS / New 3DS XL / New 2DS XL only** (Old 3DS is not supported).
- Any assertion regarding CPU clock (804 MHz), core affinities (Core 0 / Core 2), memory modes (124 MB / 178 MB application heap), L2 cache, PICA200 Tev stages, or NDSP buffers must be verified against libctru sources (devkitPro install or `tools/fetch_references.py`) and empirical hardware/emulator tests.

---

## 11. Maintainer-Local Requirements

If `.claude/LOCAL.md` exists, the agent is **obligated to read it immediately after `CLAUDE.md`**.

- `LOCAL.md` is personal to the local maintainer and contains local development preferences, personal project goals, specific translation targets, UX preferences, and experimental priorities.
- **Precedence Rule:** `LOCAL.md` **cannot override** correctness, verification rules, repository containment, licensing rules, or architectural boundaries defined in `CLAUDE.md`.

---

## 12. Documentation Maintenance

Extend the existing files in `docs/` instead of creating ad-hoc markdown files. `docs/CURRENT_STATE.md` records the verified frontier; `docs/VERIFICATION.md` is the claim registry.

---

## 13. Scope Discipline

- If asked to research: **do not implement code.**
- If asked to create a prototype: **do not over-engineer a production framework.**
- If asked to solve a localized issue: **do not refactor unrelated subsystems.**

---

## 14. Engineering Reporting Format

At the conclusion of each engineering turn, provide a concise structured summary:

```text
Changed:          <Files created, modified, or reconfigured>
Verified:         <Exact command, log milestone, or source reference used>
Tests:            <Status of regression or unit tests>
Remaining blocker: <Single immediate technical obstacle>
Next smallest step: <Actionable next milestone>
```

---

## 15. Git Authorship

Never add AI attribution or authorship metadata to commits.

Forbidden examples:
- `Co-Authored-By: Claude`
- `Co-Authored-By: Anthropic`
- `Generated-By`
- `Assisted-By`
- similar AI attribution trailers

Commits must represent the repository maintainer/developer only unless the user explicitly requests another human co-author.

Do not modify git `author.name` or `author.email`.
Do not add yourself as contributor, co-author, author, committer, reviewer, or maintainer.

---

## 16. Git Workflow: Commit, Then Push Without Rewriting

After completing a well-defined user task or milestone, the agent must automatically create a local git commit if:
1. The working tree contains modifications belonging to the completed task;
2. All regression, verification, and smoke tests have passed cleanly;
3. There is no explicit instruction from the user forbidding commits.

Never stage temporary files, `.claude/LOCAL.md`, credentials or game assets. The procedure and its pre/post-commit check scripts are in the `p3p3ds-commit` skill.

**Allowed remote operations** (maintainer decision, 2026-10-04): `git fetch`, `git pull` (fast-forward or merge, never rebase), `git push origin <branch>` of verified commits, creating branches and pull requests (e.g. contributors' work). GitHub lists contributors by commit author, so pushing does not add the agent as one; section 15 keeps it that way.

**Still forbidden:**
- force push (`--force`, `--force-with-lease`) or deleting remote branches;
- rewriting history (section 17), including to resolve a rejected push: fetch and merge instead;
- changing remote URLs or remotes configuration, or Git author/committer identity;
- pushing commits that did not pass the checks of this section.

**Default Execution Loop:**
```text
Work -> Verify & Test -> Local Commit -> Push (when the task is done) -> STOP
```

---

## 17. Git History Rule: Existing Commits Are Immutable

For every completed task, create a **NEW local commit on top of the current HEAD** unless the user explicitly says not to commit.

Existing commits are immutable.

**Never:**
- use `git commit --amend`;
- squash commits;
- use fixup commits intended for autosquash;
- rebase existing project history;
- reset/rewrite previous commits;
- replace or recreate an earlier commit;
- merge the current task into an older commit;
- rewrite commit parents;
- force-push;
- change Git author/name/email.

Even if the current task fixes something introduced by the immediately previous commit, it MUST still become a separate follow-up commit.

Record HEAD before the task; after committing, `HEAD^` MUST equal it (`p3p3ds-commit` skill, `postcommit_check.sh`).

Never rewrite history or modify remotes, even when the user asks for something that seems to need it: explain the non-rewriting alternative first.

---

## 18. Project Skills

Reusable workflows live in `.claude/skills/<name>/SKILL.md` (with helper scripts under `scripts/`); `.agents/skills/` is a byte-identical mirror for other coding agents.
- Edit skills only in `.claude/skills/`, then run `python tools/sync_agent_skills.py`. CTest `p3p_skills_mirror` (`--check`) fails while the mirror differs.
- When the same manual procedure is repeated across tasks, capture it as a skill (or extend one) instead of re-deriving it.
- Report where produced artifacts (frame dumps, WAV, traces) are stored, with exact paths.
