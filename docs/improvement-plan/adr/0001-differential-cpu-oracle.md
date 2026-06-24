# ADR 0001 — Differential CPU execution oracle (+ CI test gating)

> **Status:** Proposed · **Phase:** P1 (enabler) · **Owner:** TBD
> **Depends on:** none · **Depended on by:** [0002 (m68000 DRC)](0002-m68000-drcuml-port.md)
> **Spec:** [`docs/improvement-plan/specs/2026-06-24-mame-improvements-design.md`](../specs/2026-06-24-mame-improvements-design.md)
> **Date:** 2026-06-24

## Context

MAME has no machine-checkable behavioral test of any CPU core. The two facts that
establish the gap:

- **`-validate` never executes a CPU.** `src/emu/validity.cpp` (3161 lines) checks
  structure only — device types, memory maps, input ports, internal consistency.
  A grep for `execute_run|step|cycles|m_isdrc` in that file returns nothing. It
  cannot catch a wrong flag, a miscomputed cycle count, or a mis-decoded opcode.
- **The unit suite is tiny and not run in CI.** `tests/` contains exactly four
  Catch2 translation units — `tests/emu/attotime.cpp`, `tests/emu/video/rgbutil.cpp`,
  `tests/lib/util/corestr.cpp`, `tests/lib/util/options.cpp` — plus `tests/main.cpp`.
  The build target is `mametests` (`scripts/src/tests.lua:12`, Catch2 single-include
  at `scripts/src/tests.lua:49`). **CI never builds or runs it:** the three CI legs
  (`.github/workflows/ci-linux.yml:64`, `ci-macos.yml:39`, `ci-windows.yml:78`) run
  only `./mame -validate` (plus `reconcilelist` and an ORM check on Linux). There is
  no `make TESTS=1`, no `./mametests`, and no `srcclean` step anywhere in
  `.github/workflows/`.
- **No external test vectors are present.** `grep -rl "SingleStepTests|TomHarte|ProcessorTests"`
  over the tree returns nothing. The de-facto industry corpus
  (SingleStepTests/ProcessorTests, formerly "TomHarte") — per-instruction JSON with
  initial state, final state, and cycle-by-cycle bus activity — is not vendored or
  referenced.

The reusable hook that makes a differential oracle cheap already exists:
`device_state_interface` (`src/emu/distate.h`) exposes generic, per-CPU register
access — `state_int(index)` / `set_state_int(index, value)` (lines 299, 306),
`pc()` / `set_pc()` (lines 301, 307), `flags()` (line 303), plus the named-entry
registry that every core populates in `device_start()`. Every `cpu_device` already
advertises its registers through this interface for the debugger; the oracle can
read and write CPU state generically through it without per-core glue beyond a small
register-name mapping table.

This ADR is the **safety net that [0002](0002-m68000-drcuml-port.md) depends on**: a
68k DRC port is only reviewable if there is a regression-proof, cycle-checking
behavioral gate the interpreter and DRC must both pass.

## Decision

Add a differential CPU-execution test harness, fixtures fetched (not vendored) from
the SingleStepTests corpus, and CI wiring that gates the harness, the existing
`mametests`, and `srcclean`.

### 1. Harness design — drive one instruction, compare full state

A new Catch2 translation unit, `tests/emu/cpu/cpuoracle.cpp`, links into the
existing `mametests` target. The harness is a **standalone, headless `cpu_device`
driver** — it does NOT boot a driver/machine. For each test case it:

1. Instantiates the target `cpu_device` against a minimal fixture machine providing
   a flat RAM `address_space` (the corpus model: 24-bit/16-bit/32-bit flat memory,
   reads/writes logged).
2. Writes the **initial state** (registers + flags via `set_state_int()` keyed by a
   per-core register-name → `STATE_*` index map; initial RAM bytes into the space).
3. Runs **exactly one instruction**. The control knob is the instruction count
   pointer the scheduler normally drives: set the icount to a single-step budget and
   call `execute_run()` once (the same entry the scheduler uses, e.g.
   `m68000_device::execute_run()` at `src/devices/cpu/m68000/m68000.cpp:147`). Cores
   that step in bus sub-cycles (the new 68k microcode core loops on
   `m_inst_state`/`m_inst_substate`) are stepped until exactly one architectural
   instruction has retired.
4. Reads back **final state** via `state_int()` and the RAM space, and asserts:
   register equality, flag equality, final memory equality, and **cycle-count
   equality** (consumed icount vs. the corpus `cycles` array length / documented
   count). Cycle equality is the load-bearing assertion — it is exactly what a DRC
   must preserve.

A thin `cpu_test_harness` helper (header `tests/emu/cpu/cpu_test_harness.h`,
implementation `.cpp`) owns the fixture machine, the flat-RAM space, the
register-name map per core, the single-step loop, and JSON parsing (reuse the
in-tree `rapidjson` already used by `src/emu/machine.cpp`'s `/api/machine`).

### 2. Interpreter AND DRC through the same vectors

The same test body runs twice per core that has a DRC path: once with
`OPTION_DRC = "0"` (`src/emu/emuopts.h:167`, default `"1"` at `emuopts.cpp:199`) —
the **reference oracle** — and once with DRC enabled. DRC eligibility flows through
`cpu_device::allow_drc()` (`src/emu/devcpu.cpp:50`: `mconfig().options().drc() &&
!m_force_no_drc`); a core latches it into `m_isdrc` at start (pattern:
`src/devices/cpu/mips/mips3.cpp:398`). The harness therefore parameterizes the
fixture's options object on `drc=0/1` and asserts **both** paths match the corpus,
which transitively asserts interpreter ≡ DRC. For interpreter-only cores (today:
all of them for the first wave) the DRC leg is skipped via a capability query.

### 3. Fixtures: fetched, not vendored

The SingleStepTests JSON corpus is large (hundreds of MB across cores) and
separately licensed; it must not enter the MAME tree. Mirror MAME's existing
"generate/fetch at build time, don't commit the bytes" convention (cf. the
`custombuildtask`/generator pattern in `scripts/src/cpu.lua`, and generated headers
under `GEN_DIR`). Add:

- `tests/cpuoracle/fetch_vectors.py` — downloads a **pinned commit/tag** of the
  upstream corpus for the requested cores into a gitignored cache
  (`build/cpuoracle/<core>/`), verifying a checked-in SHA-256 manifest
  (`tests/cpuoracle/manifest.json`) so the fixtures are reproducible and
  tamper-evident. No network access at test-run time — only at fetch time.
- A `.gitignore` entry for the cache dir; the manifest (small) is the only
  committed artifact.

The harness discovers fixtures from the cache dir; if absent it **skips with a clear
message** (so a developer without the corpus still gets a green `mametests`), and CI
runs the fetch step explicitly before the test step so the oracle is mandatory there.

### 4. Runner: Catch2, inside `mametests`

Use Catch2 (already the project's test framework) rather than a new standalone
binary — one runner, one CI gate, consistent with `tests/`. Cores are Catch2
`TEST_CASE`s tagged `[cpu][z80]`, `[cpu][m6502]`, `[cpu][m68000]`, etc., so a
developer can run a single core locally (`./mametests "[m68000]"`).

### 5. Core rollout order

1. **z80** (`src/devices/cpu/z80/`) — simplest, huge corpus coverage, fast to land;
   proves the harness.
2. **m6502** (`src/devices/cpu/m6502/`) — second interpreter, exercises the
   register-map abstraction on a different core.
3. **m68000** (`src/devices/cpu/m68000/`) — the core [0002](0002-m68000-drcuml-port.md)
   ports; landing its oracle in P1 is the precondition for P2.
4. **i386** (`src/devices/cpu/i386/`) — *stretch*; x86 corpus is partial and the
   core already has a DRC-ish path, so it doubles as the first interpreter≡DRC check.

### 6. CI wiring

Extend each CI leg (`.github/workflows/ci-{linux,macos,windows}.yml`) with steps,
after the existing build:

- `make TESTS=1` (build `mametests`) — or fold `TESTS=1` into the existing build env
  block (`ci-linux.yml:55-62` already sets `TOOLS=1`).
- `python tests/cpuoracle/fetch_vectors.py --cores z80,m6502,m68000` (cache, verify
  manifest).
- `./mametests` — runs the four legacy tests **and** the new oracle. This is the
  first time `mametests` runs in CI at all.
- `srcclean` gate: build `srcclean` (it comes with `TOOLS=1`) and assert it produces
  no diff on `src/**` touched files — a new `.github/workflows/srcclean.yml` (or a
  step) that fails if `srcclean` would change a tracked file.

## Integration seams (file:line)

| Seam | Location | Role |
|---|---|---|
| Structural-only validate | `src/emu/validity.cpp` (3161 ll; no `execute_run`) | Why a behavioral oracle is needed |
| Generic state read/write | `src/emu/distate.h:299,301,303,306,307` (`state_int`/`pc`/`flags`/`set_state_int`/`set_pc`) | How the harness reads/writes CPU state per core |
| Single-step entry (68k) | `src/devices/cpu/m68000/m68000.cpp:147` (`execute_run`) | The run entry the harness calls once |
| DRC enable flag | `src/emu/devcpu.cpp:50` (`allow_drc`), `src/emu/emuopts.h:167` / `emuopts.cpp:199` (`OPTION_DRC` default "1") | Toggles interpreter vs DRC for the dual run |
| DRC latch pattern | `src/devices/cpu/mips/mips3.cpp:398` (`m_isdrc = allow_drc()`) | Template for the interpreter≡DRC invariant |
| Test target | `scripts/src/tests.lua:12` (`project("mametests")`), `:49` (Catch2 include) | Where new TUs link |
| Existing tests | `tests/emu/attotime.cpp`, `tests/emu/video/rgbutil.cpp`, `tests/lib/util/{corestr,options}.cpp`, `tests/main.cpp` | The suite to also gate |
| JSON parser in-tree | `src/emu/machine.cpp` (rapidjson usage for `/api/machine`) | Reuse for fixture parsing |
| CI legs (no test gate today) | `ci-linux.yml:64`, `ci-macos.yml:39`, `ci-windows.yml:78` | Where to add `mametests`/`srcclean` steps |
| New: harness | `tests/emu/cpu/cpuoracle.cpp`, `tests/emu/cpu/cpu_test_harness.{h,cpp}` | The differential driver |
| New: fetch | `tests/cpuoracle/fetch_vectors.py`, `tests/cpuoracle/manifest.json` | Reproducible fixture fetch |

## Alternatives considered

1. **Boot a real driver and run frames, diff a checksum.** Rejected — coarse (a frame
   masks per-instruction errors), slow, non-deterministic across hosts, and doesn't
   isolate the CPU from the rest of the machine.
2. **Standalone runner binary (not Catch2).** Rejected — adds a second test entry
   point and CI gate; Catch2/`mametests` already exists and is the thing we also want
   to start gating, so one runner is strictly better.
3. **Vendor the JSON corpus into the tree.** Rejected — hundreds of MB, separate
   license, bloats clones; violates MAME's generate-don't-commit convention. Fetch
   with a pinned, hashed manifest instead.
4. **Hand-write test vectors.** Rejected — can't match the corpus's breadth or its
   independently-derived cycle timings; hand vectors would encode our own
   assumptions, defeating the point of an oracle.
5. **Extend `-validate` to execute CPUs.** Rejected — `-validate` is shipped to end
   users and must stay fast and dependency-free; a heavyweight, fixture-dependent
   behavioral check belongs in the developer test suite, not the shipped binary.

## Consequences

**Good**
- MAME's first machine-checkable CPU behavioral gate; catches decode/flag/cycle
  regressions that `-validate` structurally cannot.
- Makes [0002](0002-m68000-drcuml-port.md) reviewable: interpreter and DRC are held
  to the same corpus, cycle-for-cycle.
- Starts running `mametests` and `srcclean` in CI — a standing coverage and hygiene
  win independent of the CPU work.
- The `device_state_interface` abstraction means new cores cost only a register-name
  map, not a bespoke harness.

**Bad / cost**
- New CI time + a fetch step (network at CI time; mitigated by caching a pinned tag).
- Per-core register-name mapping and single-step-loop quirks (esp. the 68k microcode
  core's sub-cycle stepping) are fiddly; first landing (z80) carries the design risk.
- Corpus cycle semantics must be reconciled with MAME's icount accounting per core
  (some cores count differently than the corpus's bus-cycle model) — expect a
  per-core "cycle adapter" note.
- `srcclean` gating may surface pre-existing whitespace debt; scope it to changed
  files initially.

## Accuracy & determinism preservation

This ADR **adds tests only** — zero changes to emulation behavior. It strengthens
the accuracy guarantee: `-drc 0` (interpreter) is asserted as the reference against
an independent corpus, and any DRC path is asserted equal to it. The harness is
single-threaded and runs one `cpu_device` against a flat RAM space; it does not touch
the scheduler timeline or save-state machinery, so determinism is untouched.

## Testing & validation

- **Self-test:** `make TESTS=1 && ./mametests "[cpu]"` runs the oracle; `./mametests`
  alone runs oracle + four legacy tests.
- **Per-core iteration:** `./mametests "[z80]"`.
- **Fixture provisioning:** `python tests/cpuoracle/fetch_vectors.py --cores z80`
  (verifies `manifest.json` SHA-256; populates `build/cpuoracle/`).
- **Regression gate (CI):** the new steps in all three `ci-*.yml` legs run
  `make TESTS=1`, fetch, `./mametests`, and `srcclean`. Green `mametests` + green
  `srcclean` become required.
- **Existing gates unaffected:** `./mame -validate` and `reconcilelist` continue to
  run as before.
- **Definition of done:** z80 + m6502 + m68000 oracle green in CI on all three OSes;
  `mametests` and `srcclean` wired and green.

## Open questions (for the owner before Planner runs)

1. **Corpus pin policy.** Pin to a specific upstream tag/commit and bump
   deliberately, or track latest? (Recommend: pinned tag in `manifest.json`.)
2. **CI network access.** Are the CI runners allowed outbound fetch of the corpus, or
   must we host a mirror? (Affects `fetch_vectors.py` source URL + caching strategy.)
3. **Cycle-equality strictness per core.** Some cores' icount model differs from the
   corpus bus-cycle count. Hard-fail on cycle mismatch from day one, or land
   state-equality first and ratchet cycle-equality in per core? (Recommend: state +
   cycle for z80/m6502 immediately; allow a documented per-core cycle adapter for
   68k.)
4. **`srcclean` gate scope.** Whole tree or changed-files-only initially? (Recommend:
   changed-files to avoid a large pre-existing-debt PR.)
5. **i386 stretch in P1 or deferred.** Include the partial x86 corpus now, or defer
   until after 68k? (Recommend: defer; it's a stretch goal.)
