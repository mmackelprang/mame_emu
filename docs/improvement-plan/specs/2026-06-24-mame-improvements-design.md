# MAME Improvement Program — Design Spec

> **Status:** Approved design (brainstorm phase). Feeds the Architect (ADRs) → Planner (phase plans).
> **Date:** 2026-06-24
> **Scope of this pass:** **planning artifacts only** — no MAME source changes. The output is specs +
> ADRs + a phased implementation plan, checked into a private `mame_emu` fork of this tree.
> **Source of the picks:** the architecture + UX analysis performed on this checkout (`d:\prj\mame`),
> summarized in `docs/ARCHITECTURE.md` and the front-end / performance investigations.

---

## 1. Problem & motivation

A read of this MAME checkout surfaced five high-leverage improvements — three engineering, two
usability — plus a pervasive testing gap. The unifying themes the owner asked to optimize for are
**performance** and **"easier to manage/control."** This program turns the five picks into an
actionable, dependency-ordered plan while honoring MAME's accuracy-first, deterministic philosophy
(planning only — nothing here changes emulation behavior without the interpreter remaining the oracle).

## 2. Goals

1. A differential CPU-execution **test oracle** in CI — MAME's first machine-checkable behavioral test.
2. A **m68000 → DRCUML** dynamic-recompiler port — the single largest throughput win (~549 drivers).
3. **Front-end usability** quick wins — make the new-user experience (attach media, fix ROMs, first run)
   self-explanatory.
4. A **web control surface** for launch / audit / config / input over the existing HTTP server.
5. Remove **build & driver-list friction** (GENie new-file invisibility; the `reconcilelist` gate).

**Cross-cutting goal (owner-mandated): broadly improve test coverage.** Not just the CPU oracle —
also gate the existing Catch2 suite (`mametests`) and `srcclean` in CI (today neither runs), and grow
unit coverage for the core utilities and any code these changes touch. Every phase below carries an
explicit "tests added / gated" deliverable; "done" requires green tests, not just compiling code.

## 3. Non-goals

- No emulation-accuracy changes. DRC must reproduce the interpreter cycle-for-cycle; `-drc 0` keeps the
  interpreter as the reference oracle.
- No live-video streaming in the web surface (bandwidth/latency; that's the Emscripten build's job).
- No parallelizing coupled CPUs — the single-threaded scheduler is the substrate for save-states/rewind.
- This pass writes **no MAME source** — only the plan. Implementation is a later program.

## 4. The five improvements (intent)

| # | Pick | One-line intent | Primary code area |
|---|------|-----------------|-------------------|
| 1 | Differential CPU oracle | Run cores against SingleStepTests/TomHarte JSON in CI; interpreter+DRC must match | new `tests/`, `.github/workflows/` |
| 2 | m68000 DRCUML port | Port the 68k family to the existing DRCUML backends, interpreter as oracle | `src/devices/cpu/m68000/`, `drcuml`/`drcbe*` |
| 3 | Front-end usability | `-listslots`/`-listmedia` examples; actionable ROM-missing errors; selector empty-state; input help | `src/frontend/mame/clifront.cpp`, `ui/`, `src/emu/romload.cpp` |
| 4 | Web control surface | REST + SPA for launcher/audit/config/input over `http.cpp`; not the pixel stream | `src/emu/http.cpp`, `web/`, `infoxml`/`audit`/`emuopts`/`config` |
| 5 | Build / list friction | Auto-detect new sources; make `reconcilelist` a fixup tool; CI feedback speed | `scripts/target/mame/mame.lua`, `scripts/build/makedep.py` |

## 5. Phasing (dependency-ordered)

- **Phase 1 — Enablers + quick wins.** Picks **1, 3, 5** (+ the CI test-gating cross-cutting work). These
  are mutually independent and unblock everything. The oracle (Pick 1) is the safety net Phase 2 needs;
  the front-end (Pick 3) and build-friction (Pick 5) items are low-risk and high-leverage on their own.
- **Phase 2 — m68000 DRC.** Pick **2**, gated on Phase 1's oracle (it makes the port reviewable and
  regression-proof). Highest-effort, highest-throughput.
- **Phase 3 — Web control surface.** Pick **4**. Independent of 2; sequenced last because it's a larger
  net-new surface and benefits from the Phase-1 front-end/data work.

Each phase is independently shippable and independently testable.

## 6. Build environment & prerequisites (verified 2026-06-24)

Verified on this host (MSYS2 `MINGW64`):

| Component | Status |
|---|---|
| `g++` / `gcc` 14.2.0 | ✅ present, compile+link smoke test passes |
| `clang` 19.1.5 | ✅ present |
| Python 3.12 / 3.13 | ✅ present (GENie/build scripts need Python) |
| Git 2.42 | ✅ present |
| CPU / disk | ✅ 32 cores, 2.2 TB free |
| **GNU `make`** | ❌ **MISSING — build blocker** |
| `pkg-config`, SDL2 dev | ❌ missing (only needed for `OSD=sdl`; native Windows OSD does not need them) |

**Prerequisite action (required before any build/validate task in Phase 1+):** install GNU make —
`pacman -S make` in MSYS2. (MAME upstream recommends the mamedev.org MinGW-w64 build environment, or the
UCRT64/CLANG64 MSYS2 subsystems per `CLAUDE.md`; the present `MINGW64` + GNU make is sufficient to build
the native Windows target.) Optional, only for the SDL OSD: `pacman -S mingw-w64-x86_64-SDL2
mingw-w64-x86_64-pkgconf`.

**Iteration commands the plan relies on** (fast, not the multi-hour full build):
- Build one driver/core for iteration: `make SOURCES=src/mame/<dir>/<driver>.cpp` (or `SUBTARGET=tiny`).
- Regenerate project files after adding/removing source files: `make REGENIE=1`.
- Primary correctness gate: `./mame -validate`.
- Unit tests: `make TESTS=1 && ./mametests` (Phase 1 wires this into CI).
- Whitespace gate: `srcclean` (built via `TOOLS=1`; Phase 1 wires it into CI).

## 7. Testing strategy (cross-cutting — required in every phase)

1. **Differential oracle (Pick 1).** Import SingleStepTests/TomHarte JSON for at least z80, m6502, m68000,
   and (stretch) i386; execute each core per-instruction and assert state + cycle equality; run the same
   vectors through the interpreter **and** any DRC path. CI-gated.
2. **Gate the existing suite.** Add `make TESTS=1 && ./mametests` to CI (currently never run); add
   `srcclean` as a CI check (currently honor-system).
3. **Grow unit coverage.** Expand the Catch2 suite around any utility/seam these changes touch (e.g. the
   web REST handlers, the front-end formatting helpers, the build-list tooling).
4. **Definition of done per task:** code compiles **and** the relevant gate(s) are green — `-validate`
   clean for driver/device-touching work; oracle green for CPU work; `mametests` green for utility work.

## 8. Success criteria

1. Five ADRs (one per pick) with concrete MAME integration seams (file:line), risks, and accuracy-
   preservation arguments.
2. Three phase plans with bite-sized, file-level tasks, each carrying explicit test deliverables.
3. The plan is buildable against the verified environment (prerequisites documented; `make` gap called out).
4. The whole package checked into a private `mame_emu` fork.

## 9. Constraints & philosophy

- **Accuracy is sacred.** Interpreter stays the oracle; DRC is acceleration only.
- **Determinism is sacred.** Nothing moves off the emulation thread that feeds the save-state timeline.
- **Planning-only this pass.** No source edits to MAME; the deliverable is the plan.

## 10. Next steps (pipeline)

1. **Architect** → `docs/improvement-plan/adr/0001..0005-*.md` — the decision records (consumes this spec).
2. **Planner** → `docs/improvement-plan/plan/phase-{1,2,3}-*.md` — the task-level plans (consumes the ADRs).
3. **Repo** → create private `mame_emu`, check in the fork + this `docs/improvement-plan/` tree.
