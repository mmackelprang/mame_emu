# Phase 1 — Enablers & quick wins (Picks 1, 3, 5)

> **Status:** Planned · **Consumes:** ADRs [0001](../adr/0001-differential-cpu-oracle.md),
> [0003](../adr/0003-frontend-usability-quick-wins.md), [0005](../adr/0005-build-and-driverlist-friction.md)
> **Spec:** [`../specs/2026-06-24-mame-improvements-design.md`](../specs/2026-06-24-mame-improvements-design.md)
> **Date:** 2026-06-24

## Goal

Land the three mutually-independent, low-risk improvements that unblock everything
downstream and put MAME's first machine-checkable behavioral gates into CI:

- **Pick 1 (ADR 0001):** a differential CPU-execution oracle (z80 → m6502 → m68000),
  fixtures fetched-not-vendored, plus wiring `mametests` + `srcclean` into CI for the
  first time. **This is the safety net Phase 2 hard-gates on.**
- **Pick 3 (ADR 0003):** front-end usability quick wins — surface already-present data
  (slot/media usage syntax, actionable ROM/audit errors, selector empty state, slot
  descriptions, input help). English-source strings via `_()`; golden CLI tests pinned
  to the C/English locale.
- **Pick 5 (ADR 0005):** build & driver-list friction — staleness detection (**warn-only
  default**, opt-in auto-REGENIE), `reconcilelist --fix` autofix, fast CI pre-flight.
  Changes are additive/upstream-friendly.

## ADRs implemented

| ADR | Picks | Tasks |
|---|---|---|
| [0001](../adr/0001-differential-cpu-oracle.md) | 1 + CI test gating | T1–T9 |
| [0003](../adr/0003-frontend-usability-quick-wins.md) | 3 | T10–T18 |
| [0005](../adr/0005-build-and-driverlist-friction.md) | 5 | T19–T24 |

## Prerequisites (must hold before any build/validate task)

1. **Install GNU make — this is Task 0.** Verified host (MSYS2 `MINGW64`) has g++ 14.2,
   clang 19.1.5, Python 3.12/3.13, Git 2.42, but **GNU `make` is missing and is a hard
   build blocker**. See **Task 0** below.
2. Toolchain otherwise verified present. SDL2/pkg-config are NOT required (native Windows
   OSD); only needed for `OSD=sdl`.
3. Iteration loop the tasks rely on: `make SOURCES=<file>` / `SUBTARGET=tiny` (fast),
   `make REGENIE=1` (after adding/removing source files), `./mame -validate` (primary
   structural gate), `make TESTS=1 && ./mametests` (unit suite), `srcclean` (via
   `TOOLS=1`).

## Cross-phase gate produced here

Phase 1 **produces** the gate Phase 2 consumes: the m68000 oracle (`./mametests
"[m68000]"`) must be green with state + cycle equality before any m68000 DRC work begins
(ADR 0001 §5, ADR 0002 §5). Per the resolved CPU-oracle decision, the oracle is
**ratcheted per-CPU** — strict state+cycle equality enforced CPU-by-CPU starting with z80
and m6502 (cleanest), then folding in m68000.

## Conventions (apply to every task)

- New source files carry the MAME license header (two comment lines):
  `// license:BSD-3-Clause` / `// copyright-holders:<name>`.
- **New `.cpp`/`.h`/`.py` test files require `make REGENIE=1`** so GENie picks them up
  (they are added to `scripts/src/tests.lua`).
- Match the brace/whitespace style of any edited file (some UI files are K&R; new test
  TUs follow the `tests/` Allman-ish style already present). Run `srcclean` on touched
  files before committing.
- No new driver/device is added in Phase 1, so no `mame.lst`/`tiny.lst` edits are needed
  here. (Pick 5's tooling *operates on* those lists but does not add systems.)

---

## Task 0 — Install GNU make (prerequisite)

- **Files:** none (environment).
- **Change:** In the MSYS2 `MINGW64` shell, `pacman -S make`. Optional, only if `OSD=sdl`
  is ever used: `pacman -S mingw-w64-x86_64-SDL2 mingw-w64-x86_64-pkgconf`.
- **Test/Validation:** `make --version` prints GNU Make ≥ 4.x. Smoke-build a single
  driver to confirm the toolchain end-to-end:
  `make SOURCES=src/mame/atari/asteroid.cpp -j3` produces a custom `mame` exe and exits 0.
  **Green:** both commands exit 0.

---

# Pick 1 — Differential CPU oracle + CI test gating (ADR 0001)

### Task 1 — Fixture fetcher + pinned, hashed manifest (no vectors vendored)

- **Files (new):** `tests/cpuoracle/fetch_vectors.py`, `tests/cpuoracle/manifest.json`,
  `.gitignore` (add `build/cpuoracle/`).
- **Change:** `fetch_vectors.py --cores z80,m6502,m68000` downloads a **pinned
  commit/tag** of the SingleStepTests/ProcessorTests corpus for the named cores into a
  gitignored cache `build/cpuoracle/<core>/`. Per the resolved decision, the corpus is
  **fetched, not vendored, with a pinned hash + mirror fallback**: `manifest.json` records
  the pinned upstream ref, a list of expected files, and a SHA-256 per fetched archive,
  plus a `mirrors: [...]` array (primary upstream URL + at least one fallback mirror).
  The script verifies every SHA-256 against the manifest and fails on mismatch. It is
  idempotent (skips files already present and verified). Add a Python license header
  comment.
- **Test/Validation:** `python tests/cpuoracle/fetch_vectors.py --cores z80` populates
  `build/cpuoracle/z80/` and prints "verified N files". Re-run is a no-op. Corrupt one
  cached byte → re-run **fails** with a clear hash-mismatch message. Point the primary URL
  at an unreachable host → it falls through to a mirror and still verifies.
  **Green:** fetch+verify exits 0 on a clean run, exits non-zero on tamper, mirror
  fallback works.

> **PR boundary A** (Task 1 standalone): the fetcher + manifest land first; no C++ yet.

### Task 2 — `cpu_test_harness` skeleton: fixture machine + flat-RAM space

- **Files (new):** `tests/emu/cpu/cpu_test_harness.h`, `tests/emu/cpu/cpu_test_harness.cpp`.
- **Files (edit):** `scripts/src/tests.lua` — add the new TUs to the `files {}` block
  (after `tests/emu/video/rgbutil.cpp`, line ~69) and add `MAME_DIR .. "src/devices"` /
  any CPU include dirs needed to the `includedirs` block (~line 48). Link the device
  libraries the harness instantiates.
- **Change:** A headless `cpu_device` driver helper that owns: (a) a minimal fixture
  machine exposing a single flat-RAM `address_space` sized per the corpus model
  (24-bit/16-bit/32-bit), logging reads/writes; (b) a per-core **register-name →
  `STATE_*`/`state_int` index map**; (c) a single-step loop; (d) JSON parsing via the
  in-tree `rapidjson` already used by `src/emu/machine.cpp`. State is read/written through
  `device_state_interface` (`src/emu/distate.h`: `state_int`/`set_state_int`/`pc`/`set_pc`/
  `flags`). No core wired yet — just the scaffolding and one trivial smoke `TEST_CASE` tagged
  `[cpu][harness]` that instantiates the fixture and asserts RAM read-back works.
- **Test/Validation:** `make REGENIE=1` (new TUs), then `make TESTS=1 && ./mametests
  "[harness]"`. **Green:** the smoke case passes; the four legacy tests still pass
  (`./mametests`).

### Task 3 — z80 oracle (strict state + cycle equality)

- **Files (new):** `tests/emu/cpu/cpuoracle.cpp` (z80 `TEST_CASE` tagged `[cpu][z80]`).
- **Files (edit):** `cpu_test_harness.{h,cpp}` — add the z80 register-name map;
  `scripts/src/tests.lua` — add `cpuoracle.cpp` to `files {}`; ensure the z80 device
  library is linked.
- **Change:** Instantiate `z80_device`, for each corpus case: write initial registers +
  flags + RAM, run **exactly one instruction** via the scheduler's `execute_run()` entry
  with a single-step icount budget, read back final state, and assert register equality,
  flag equality, final-memory equality, and **cycle-count equality** (consumed icount vs.
  corpus cycle count). Per the resolved decision, z80 enforces **strict state + cycle
  equality from day one** (the cleanest core). Fixtures discovered from
  `build/cpuoracle/z80/`; if absent, **skip with a clear message**.
- **Test/Validation:** `python tests/cpuoracle/fetch_vectors.py --cores z80` then
  `make TESTS=1 && ./mametests "[z80]"`. **Green:** all z80 cases pass with cycle
  equality; absent-fixture run skips cleanly (still green).

> **PR boundary B** (Tasks 2–3): harness + first oracle (z80). This is the design-risk
> landing — review the single-step loop and register-map abstraction here.

### Task 4 — m6502 oracle (strict state + cycle equality)

- **Files (edit):** `tests/emu/cpu/cpuoracle.cpp` (add `[cpu][m6502]` case);
  `cpu_test_harness.{h,cpp}` (m6502 register-name map); `scripts/src/tests.lua` (link the
  m6502 device library if not already linked).
- **Change:** Reuse the harness for `m6502`/`m6502_device`, exercising the register-map
  abstraction on a second core. Same four assertions; **strict state + cycle equality**
  (resolved decision: z80 + m6502 are the strict ratchet starters).
- **Test/Validation:** `python tests/cpuoracle/fetch_vectors.py --cores m6502` then
  `./mametests "[m6502]"`. **Green:** all m6502 cases pass with cycle equality.

### Task 5 — m68000 oracle (state-equality first; documented cycle adapter)

- **Files (edit):** `tests/emu/cpu/cpuoracle.cpp` (add `[cpu][m68000]`);
  `cpu_test_harness.{h,cpp}` (m68000 register-name map + the **sub-cycle single-step
  loop**: the new microcode core steps `m_inst_state`/`m_inst_substate`, so step until
  exactly one architectural instruction has retired); `scripts/src/tests.lua` (link the
  m68000 device library).
- **Change:** Drive the **new microcode core** (`m68000_device::execute_run()`,
  `src/devices/cpu/m68000/m68000.cpp:147`), not Musashi. Assert register/flag/memory
  equality immediately. For cycles: reconcile MAME's icount (bus-cycle) model with the
  corpus's bus-cycle model and **document the per-core cycle adapter** in a comment block
  + `tests/cpuoracle/README.md`. Land **state-equality green first**; turn on
  cycle-equality once the adapter is proven, ratcheting it in (this is the per-CPU ratchet
  that Phase 2 will rely on — m68000 must reach strict cycle equality before Pick 2's DRC
  work, per ADR 0002 §5).
- **Test/Validation:** `python tests/cpuoracle/fetch_vectors.py --cores m68000` then
  `./mametests "[m68000]"`. **Green (step 1):** register/flag/memory equality passes.
  **Green (step 2, ratchet):** cycle equality passes with the documented adapter.

> **PR boundary C** (Tasks 4–5): m6502 + m68000 oracles. The m68000 cycle-adapter is the
> hand-off artifact Phase 2 depends on — flag it explicitly in the PR description.

### Task 6 — Wire `mametests` into all three CI legs

- **Files (edit):** `.github/workflows/ci-linux.yml`, `ci-macos.yml`, `ci-windows.yml`.
- **Change:** After the existing build step, add: `make TESTS=1` (or fold `TESTS=1` into
  the existing build-env block — `ci-linux.yml` already sets `TOOLS=1`), a fetch step
  (`python tests/cpuoracle/fetch_vectors.py --cores z80,m6502,m68000`), then `./mametests`.
  This is the **first time `mametests` runs in CI** — it runs the four legacy tests **and**
  the oracle. (Network policy: the fetcher's mirror fallback from Task 1 covers a restricted
  runner; document which mirror CI uses.)
- **Test/Validation:** CI dry-run (or local emulation of the steps) is green on all three
  OS legs; an intentionally-failing oracle vector turns the leg red.
  **Green:** all three legs run `mametests` and pass.

### Task 7 — Wire `srcclean` into CI (changed-files scope)

- **Files (new):** `.github/workflows/srcclean.yml` (or a step in the existing legs).
- **Change:** Build `srcclean` (comes with `TOOLS=1`), run it over the PR's **changed
  `src/**` files only** (resolved scope: changed-files, to avoid a large pre-existing
  whitespace-debt PR), and fail if it would modify a tracked file (assert empty diff).
- **Test/Validation:** On a branch with a deliberately mis-indented touched file, the gate
  fails; on a clean branch it passes. **Green:** clean diff → pass, dirty diff → fail.

> **PR boundary D** (Tasks 6–7): CI wiring. Keep separate from the C++ so a CI-config
> revert is trivial if a runner misbehaves.

### Task 8 — Oracle docs + per-core onboarding note

- **Files (new):** `tests/cpuoracle/README.md`.
- **Change:** Document running the oracle (`./mametests "[m68000]"`), provisioning
  fixtures, the manifest/pin/mirror policy, the per-core register-map recipe, and the
  cycle-adapter note for m68000. No code.
- **Test/Validation:** Markdown only; `srcclean`/link check. **Green:** doc renders, links
  resolve.

### Task 9 — (Stretch, optional) i386 oracle / first interpreter≡DRC check

- **Files (edit):** `tests/emu/cpu/cpuoracle.cpp` (`[cpu][i386]`), harness register map,
  `fetch_vectors.py` (i386 partial corpus).
- **Change:** Add i386 using the **partial** x86 corpus; because i386 already has a DRC-ish
  path, run the dual `drc=0`/`drc=1` parameterization to prove the interpreter≡DRC
  invariant end-to-end before Phase 2 needs it on m68000. Resolved guidance: **defer** —
  include only if Phase 1 has slack.
- **Test/Validation:** `./mametests "[i386]"` green on both DRC legs where the corpus
  covers the opcode. **Green:** both legs match the corpus; skips uncovered opcodes
  cleanly.

---

# Pick 3 — Front-end usability quick wins (ADR 0003)

> **Copy policy (resolved):** add **English-source strings only**, all wrapped in the `_()`
> macro (`src/lib/util/language.h:24`); do **not** block on translations. **Pin the test
> locale to C/English** for golden-output CLI tests so they are stable.

### Task 10 — Golden-CLI test fixture pinned to C/English

- **Files (new):** `tests/frontend/clitext.cpp` (Catch2, tagged `[cli]`).
- **Files (edit):** `scripts/src/tests.lua` (`files {}` + any frontend include/link the
  fixture needs).
- **Change:** A fixture that forces the MAME UI language / `LANG` to C/English and provides
  helpers to capture the text emitted by the CLI command handlers. Assert on **structural
  substrings**, not full prose. Land this first so Tasks 11–14 have a test target.
- **Test/Validation:** `make REGENIE=1` then `make TESTS=1 && ./mametests "[cli]"`.
  **Green:** fixture compiles and a trivial substring assertion passes; locale is pinned.

### Task 11 — A1: `-listslots`/`-listmedia` usage-syntax hint

- **Files (edit):** `src/frontend/mame/clifront.cpp` (`listslots` ~:836, headers ~:845-846;
  `listmedia` ~:909, ~:919-920).
- **Change:** Append a translated "to use" line/column showing the literal invocation
  (`-<slot> <opt>`, `-flop1 <image>`), keeping the existing table layout and
  `osd_printf_info` style. All new copy via `_()`.
- **Test/Validation:** Add `[cli]` golden cases asserting the listslots/listmedia output
  contains the syntax-hint substring. `./mame -validate` stays green. `./mametests "[cli]"`
  green. Run `srcclean` on the file.

### Task 12 — A2: unknown-option "did you mean" + A3: `-showusage` examples

- **Files (edit):** `src/frontend/mame/clifront.cpp` (option rethrow ~:241; reuse the
  approximate-match approach at ~:304-331; `CLICOMMAND_SHOWUSAGE` ~:1735-1738).
- **Change:** A2 — before/around the unknown-option rethrow, compute approximate matches
  against known option names (factor the existing Levenshtein-style system-name matcher
  into a shared helper if not already shared) and print a translated "did you mean: …".
  A3 — append a translated **Examples** block to `-showusage` (run a game, mount a floppy,
  list a system's slots). All copy via `_()`.
- **Test/Validation:** `[cli]` golden cases: an unknown option yields a "did you mean" line;
  `-showusage` contains an Examples section. `./mametests "[cli]"` + `./mame -validate`
  green. `srcclean`.

> **PR boundary E** (Tasks 10–12): Group A (CLI discoverability) + the golden-CLI fixture.

### Task 13 — B1: actionable launch-time missing-ROM error

- **Files (edit):** `src/emu/romload.cpp` (~:671, `emu_fatalerror(EMU_ERR_MISSING_FILES,…)`).
- **Change:** Replace the bare message with one that names the failing system and points at
  the audit menu / `-verifyroms`, and — where the missing-file list is reliably available at
  that point — names the missing files. **Keep the `EMU_ERR_MISSING_FILES` code.** Copy via
  `_()`. (Resolved open question: if the file list is not reliably present here, name the
  system + point to `-verifyroms`; do not over-reach.)
- **Test/Validation:** `[cli]` golden case (or a targeted unit assertion) that the error
  text contains the system name and the audit/`-verifyroms` pointer. `./mame -validate`
  green. `srcclean`.

### Task 14 — B2: surface per-ROM audit detail in the in-UI audit menu

- **Files (edit):** `src/frontend/mame/ui/auditmenu.cpp` (~:226-227, currently discards
  detail). Detail source `src/frontend/mame/audit.cpp:498-579` (`media_auditor::summarize()`)
  is unchanged.
- **Change:** Pass an output stream into `audit_media()`/`summarize()` and surface the
  per-ROM reason strings (`NEEDS REDUMP`, `INCORRECT CHECKSUM: EXPECTED %s / FOUND %s`,
  etc.) in the audit menu instead of keeping only the pass/fail summary. Match the file's
  brace style. Copy via `_()` where new strings are introduced.
- **Test/Validation:** **Manual UI smoke** (this menu is not easily golden-tested): run the
  audit menu against a deliberately incomplete ROM set and confirm per-ROM detail appears.
  `./mame -validate` green. `srcclean`. Document the manual steps in the PR.

> **PR boundary F** (Tasks 13–14): Group B (actionable ROM/audit errors).

### Task 15 — C1: full-selector empty state

- **Files (edit):** the full system-selection menu (the full-selector counterpart to
  `src/frontend/mame/ui/simpleselgame.cpp:305-319`).
- **Change:** Add a "No system ROMs found. Please check the rompath setting…" empty-state
  box to the full selector's empty case, reusing the simple selector's copy/pattern for
  consistency. Copy via `_()`.
- **Test/Validation:** Manual UI smoke with an empty rompath shows the box. `./mame
  -validate` green. `srcclean`.

### Task 16 — C2: slot-menu device descriptions

- **Files (edit):** `src/frontend/mame/ui/slotopt.cpp` (~:191-193, currently shows
  `option->name()`).
- **Change:** Show the human device description (`devtype().fullname()`, already available)
  alongside/instead of the bare code in the list label. Match brace style.
- **Test/Validation:** Manual UI smoke: slot menu shows descriptions. `./mame -validate`
  green. `srcclean`.

### Task 17 — C3: slot-change reboot notice

- **Files (edit):** `src/frontend/mame/ui/slotopt.cpp` (~:250-253,
  `schedule_hard_reset()`).
- **Change:** Add a brief **non-blocking** translated notice that changing slots reboots
  the machine (resolved decision: notice, not a confirmation prompt — minimal friction,
  matches current one-step behavior). No change to the existing reset behavior. Copy via
  `_()`.
- **Test/Validation:** Manual UI smoke: changing a slot shows the notice and still reboots.
  `./mame -validate` green. `srcclean`.

### Task 18 — C4: input set/append discoverability

- **Files (edit):** `src/frontend/mame/ui/inputmap.cpp` (~:556-558, "not very
  discoverable").
- **Change:** Make the set-vs-append affordance explicit in the prompt; add a translated
  hint line. Copy via `_()`.
- **Test/Validation:** Manual UI smoke: the input remap prompt shows the hint. `./mame
  -validate` green. `srcclean`.

> **PR boundary G** (Tasks 15–18): Group C (internal-UI guidance). A Polisher pass is
> warranted before merge (per-file brace/whitespace + nav consistency).

---

# Pick 5 — Build & driver-list friction (ADR 0005)

> **Resolved defaults:** staleness detection is **warn-only by default** (opt-in
> `AUTO_REGENIE=1`); `reconcilelist --fix` is a **local developer tool only** (never
> auto-commits); keep changes **additive/opt-in and upstream-friendly**. New Python helpers
> live under `scripts/build/` with sibling unit tests.

### Task 19 — Source-set fingerprint + staleness detector (library + tests)

- **Files (new):** `scripts/build/sourcestale.py`, `scripts/build/tests/test_sourcestale.py`.
- **Change:** A small helper that computes a fingerprint of the source-file set GENie would
  glob — the union of `os.matchfiles` results over `src/<target>/*` plus the device
  `scripts/src/*.lua` selectors — **reusing** `collect_sources`/glob logic already in
  `scripts/build/makedep.py` so the path-walking is shared and testable. It compares the
  fingerprint to the set baked into the last generated project files and reports
  `up-to-date` / `stale` (listing the added/removed files).
- **Test/Validation:** `python -m pytest scripts/build/tests/test_sourcestale.py` against a
  fixture source tree: added `.cpp` → detected stale; removed `.cpp` → detected stale;
  unchanged tree → no false positive. **Green:** all pytest cases pass.

### Task 20 — `make check-sources` target (warn-only; opt-in auto-REGENIE)

- **Files (edit):** `makefile` (add a `check-sources` target invoking `sourcestale.py`).
- **Change:** `make check-sources` runs the detector and **warns** on staleness with a
  clear "new sources detected — run `make REGENIE=1`" message (default). With
  `AUTO_REGENIE=1` it instead triggers `REGENIE=1`. CI uses warn/fail-only. Keep purely
  additive — does not alter the default build path's behavior unless invoked.
- **Test/Validation:** On a tree with an added source file, `make check-sources` prints the
  warning and exits 0 (warn) / non-zero in fail mode; `AUTO_REGENIE=1 make check-sources`
  regenerates. On a clean tree it reports up-to-date. **Green:** correct message + exit
  code per mode.

> **PR boundary H** (Tasks 19–20): staleness detection.

### Task 21 — `reconcilelist --fix` autofix mode

- **Files (edit):** `scripts/build/makedep.py` (`reconcilelist` subparser ~:758; dispatch
  ~:1070-1082; `DriverReconciler` ~:597; `parse_list` ~:461; the `self.bad = True` sites
  ~:627,630,678,682,692,698).
- **Change:** Add a `--fix` flag (or a `fixlist` mode) that, on mismatch, **rewrites the
  `.lst`** to match the binary's `-listxml`: insert missing systems under the right
  `@source:` header in sort order, remove stale entries, **preserving the file's
  grouping/formatting/ordering conventions** that `parse_list`/`DriverReconciler` already
  understand. CI keeps running **check mode** (fail on mismatch); `--fix` is a local
  convenience the contributor reviews and commits. Never auto-commit.
- **Test/Validation:** Golden test (Task 22) is the gate. Manual: on a deliberately
  divergent `mame.lst`, `python scripts/build/makedep.py reconcilelist --fix -l
  src/mame/mame.lst <xml>` produces a corrected file; re-running **check** mode reports
  clean (`self.bad == False`).

### Task 22 — Reconciler autofix golden tests

- **Files (new):** `scripts/build/tests/test_reconcile_fix.py` + fixture `.lst` /
  `-listxml` samples.
- **Change:** Given a known-divergent `.lst` + a fixture `-listxml`, assert `--fix`
  produces the **exact** expected `.lst` (formatting, ordering, `@source:` grouping
  preserved byte-for-byte), and that check mode on the fixed file reports clean. Covers the
  "must not churn the file" risk from the ADR.
- **Test/Validation:** `python -m pytest scripts/build/tests/test_reconcile_fix.py`.
  **Green:** exact-match assertions pass; re-check reports clean.

> **PR boundary I** (Tasks 21–22): reconciler autofix + its golden tests (ship together —
> the golden test is the safety net for the file-churn risk). **Status: ✅ Done** —
> `reconcilelist --fix` landed in `scripts/build/makedep.py` with 11 byte-exact golden
> tests (`scripts/build/tests/test_reconcile_fix.py`); CI still runs check mode only.

### Task 23 — Fast CI pre-flight job

- **Files (edit):** `.github/workflows/ci-linux.yml` (add a fast pre-flight job ahead of
  the multi-hour `make -j3` leg ~:62; reconcile step ~:65-66). Drive a per-PR slice from
  the changed file set via the existing `sourcesfilter` machinery
  (`scripts/build/makedep.py:1044-1057,1066`); fall back to `SUBTARGET=tiny` for a stable
  baseline.
- **Change:** A pre-flight job that builds the `tiny`/`SOURCES=` slice and runs
  `./mame -validate` + `reconcilelist` (check) — fast (minutes). This dovetails with the
  ADR 0001 `mametests`/`srcclean` steps, which are likewise fast and belong in the
  pre-flight. The full legs still run.
- **Test/Validation:** Pre-flight passes on a clean tree; fails in minutes on an
  intentionally-broken slice (bad driver or `.lst` drift). **Green:** fast feedback works;
  full legs unaffected.

### Task 24 — Pick 5 tooling docs

- **Files (new):** `scripts/build/README-friction.md` (or extend an existing build doc).
- **Change:** Document `make check-sources` (+ `AUTO_REGENIE=1`), `reconcilelist --fix`,
  and the fast pre-flight, with upstreamability notes (additive/opt-in). No code.
- **Test/Validation:** Markdown only; `srcclean`/link check. **Green:** doc renders.

> **PR boundary J** (Tasks 23–24): fast pre-flight + docs. **Status: 🚀 in-flight**
> (`feat/ci-preflight-docs`).

---

## Suggested PR sequence (Phase 1)

| PR | Tasks | Theme | Gate |
|---|---|---|---|
| A | 1 | Fixture fetcher + manifest | pytest/CLI fetch+verify |
| B | 2–3 | Harness + z80 oracle | `mametests "[z80]"` |
| C | 4–5 | m6502 + m68000 oracle (+ cycle adapter) | `mametests "[m6502]" "[m68000]"` |
| D | 6–7 | CI: `mametests` + `srcclean` | all 3 legs green |
| E | 10–12 | Group A CLI discoverability + golden fixture | `mametests "[cli]"` |
| F | 13–14 | Group B ROM/audit errors | `[cli]` + manual UI |
| G | 15–18 | Group C internal-UI guidance | manual UI + Polisher |
| H | 19–20 | Source staleness detection | pytest |
| I | 21–22 | `reconcilelist --fix` + golden tests | pytest |
| J | 23–24 | Fast CI pre-flight + docs | CI green |

PRs A–D (Pick 1) and PRs E–G (Pick 3) and PRs H–J (Pick 5) are mutually independent and
may proceed in parallel. **Within Pick 1, B depends on A; D depends on B–C** (CI runs the
oracle). Task 0 (install `make`) precedes everything.

## Test deliverables (Phase 1)

- **CPU oracle** (`tests/emu/cpu/cpuoracle.cpp`, `cpu_test_harness.{h,cpp}`): z80 + m6502
  strict state+cycle equality; m68000 state equality then ratcheted to cycle equality.
- **Fixture fetcher + manifest** (`tests/cpuoracle/`): reproducible, hash-verified,
  mirror-fallback.
- **`mametests` + `srcclean` gated in CI** for the first time (all three OS legs).
- **Golden-CLI tests** (`tests/frontend/clitext.cpp`): listslots/listmedia syntax hint,
  unknown-option "did you mean", `-showusage` examples, missing-ROM error content — locale
  pinned to C/English, structural substrings only.
- **Build-tooling unit tests** (`scripts/build/tests/`): source-staleness detector;
  `reconcilelist --fix` golden tests.
- **Manual UI smoke** documented for the internal-UI items (C1–C4, B2).

## Definition of Done (Phase 1)

1. GNU `make` installed; iteration loop verified (Task 0).
2. z80 + m6502 + m68000 oracle green in CI on all three OSes; m68000 reaches **strict
   state + cycle equality** (the Phase-2 gate). `mametests` and `srcclean` wired and green.
3. Pick 3 golden CLI tests green; internal-UI items (C1–C4, B2) manually verified;
   English-source strings added via `_()`; `srcclean` clean.
4. Pick 5 staleness detector + `reconcilelist --fix` land with green pytest; fast
   pre-flight job green; full CI legs unaffected; changes additive/upstream-friendly.
5. Every touched file passes `srcclean`; `./mame -validate` stays green throughout.

## Risks & assumptions

- **R1 — m68000 cycle-adapter (highest).** The microcode core's icount/bus-cycle model may
  not map 1:1 to the corpus. Mitigation: land state-equality first, ratchet cycle-equality
  behind a documented adapter (Task 5). **This adapter is the artifact Phase 2 inherits** —
  if cycle-equality cannot be reached, Phase 2's hard gate is at risk; surface early.
- **R2 — CI network/corpus access.** Runners may block outbound fetch. Mitigated by the
  manifest's mirror fallback (Task 1) and caching the pinned tag; CI fetch source must be
  pinned to a reachable mirror.
- **R3 — single-step loop quirks** (esp. 68k sub-cycle stepping). Concentrated in Tasks 3
  and 5; review carefully at PR boundary B/C.
- **R4 — golden-CLI brittleness.** Mitigated by pinning locale to C/English and asserting
  structural substrings, not prose (Task 10).
- **R5 — `--fix` file churn.** A formatting/ordering mismatch would churn `mame.lst`.
  Mitigated by byte-exact golden tests shipping with the feature (Task 22).
- **A1 — assumption:** the missing-file list at `romload.cpp:671` may not be reliably
  present; B1 falls back to naming the system + `-verifyroms` pointer if so (Task 13).
- **A2 — assumption:** `srcclean` CI scope is changed-files-only initially (resolved), to
  avoid surfacing pre-existing whitespace debt as a blocking diff.
