# ADR 0002 — m68000 → DRCUML dynamic-recompiler port

> **Status:** Proposed · **Phase:** P2 · **Owner:** TBD
> **Depends on:** **[0001 (CPU oracle)](0001-differential-cpu-oracle.md) — hard gate**, with the
> m68000 gate **defined by [0006](0006-m68000-oracle-gate-definition.md)** (the corpus is
> MAME-derived, so the gate's cycle-exactness lives in Leg B: interpreter ≡ DRC)
> **Spec:** [`docs/improvement-plan/specs/2026-06-24-mame-improvements-design.md`](../specs/2026-06-24-mame-improvements-design.md)
> **Date:** 2026-06-24

> **This is the highest-effort, highest-risk pick in the program.** It is sequenced
> after [0001](0001-differential-cpu-oracle.md) precisely because it is not safely
> reviewable without a cycle-checking behavioral oracle. Be honest with the owner:
> a correct, cycle-accurate 68k DRC is a multi-month effort, not a quick win. This
> ADR scopes the *first increment* (plain 68000, user-mode common path) and the
> architecture that makes the increment extensible.

## Context

The 680x0 family is interpreter-only and is one of MAME's most-used CPUs — referenced
by on the order of ~549 drivers (Sega System 16, CPS, Neo Geo, countless others). It
is the single largest throughput lever in the tree that lacks a DRC.

**Two interpreters exist — this is the central architectural fact.** The m68000
directory contains:

- A **new microcode/state-machine core** — `src/devices/cpu/m68000/m68000.cpp`
  (`m68000_device::execute_run()` at line 147). It is **bus-cycle accurate**: it
  steps through instruction sub-states via `m_inst_state` / `m_inst_substate`,
  dispatching `m_handlers_f[]` / `m_handlers_p[]` function tables generated from a
  microcode list. Its `execute_run()` loop (lines 147–181) consumes `m_icount` in
  bus cycles and can suspend mid-instruction (`m_post_run`, `do_post_run()`,
  `m_count_before_instruction_step`). The decode/handler tables are **generated** by
  `m68000gen.py` (`src/devices/cpu/m68000/m68000gen.py`, 124 KB) from `m68000.lst` /
  `m68k_in.lst`, producing the checked-in `m68000-decode.cpp`, `m68000-head.h`,
  `m68000-s{d,i}{f,p}.cpp` files. **These generated files are committed** (listed
  directly in `scripts/src/cpu.lua:2142-2143…` under `files {}`, not built by a
  `custombuildtask`) — the maintainer runs the generator by hand and commits output.
- The **legacy Musashi core** — `src/devices/cpu/m68000/m68kcpu.cpp`
  (`m68000_musashi_device::execute_run()` at line 832), with `m68kops.cpp` (generated
  by `m68kmake.py`). This is the older instruction-step interpreter.

Both derive from a common base in `m68kcommon.{h,cpp}`. **Which core a driver uses
matters for the DRC target.**

**The proven dual-path template** in MAME is robust and repeated across many cores
(`*drc.cpp` + `*fe.cpp` decode frontend + an `execute_run` that branches on
`m_isdrc`): `src/devices/cpu/mips/` (`mips3.cpp` + `mips3drc.cpp` + `mips3fe.cpp`),
plus `powerpc/ppcdrc.cpp` + `ppcfe.cpp`, `sh/sh{2,4,_}fe.cpp`, `e132xs/`, `sharc/`,
`unsp/`, `dspp/`, `mb86235/`. The mechanism:

- `execute_run()` checks `m_isdrc` and either interprets or enters the DRC
  dispatcher.
- `m_isdrc = allow_drc()` is latched at start (`src/devices/cpu/mips/mips3.cpp:398`);
  `allow_drc()` is `mconfig().options().drc() && !m_force_no_drc`
  (`src/emu/devcpu.cpp:50`). Default `drc=1` (`src/emu/emuopts.cpp:199`), and
  `-drc 0` forces the interpreter — **the reference oracle**.
- The DRC frontend (`*fe.cpp`) decodes a basic block into a `drcuml` instruction
  list; the backend (`drcuml.cpp` → `drcbex64.cpp` / `drcbearm64.cpp` / `drcbec.cpp`)
  JITs or interprets the UML. `OPTION_DRC_USE_C` (`emuopts.cpp:201`) forces the C
  backend; `OPTION_DRC_LOG_UML` / `_NATIVE` (lines 202-203) dump disassembly for
  debugging.

The DRCUML core and all three backends are present and mature
(`src/devices/cpu/drcuml.cpp`, `drcbex64.cpp` 226 KB, `drcbearm64.cpp` 186 KB,
`drcbec.cpp` 87 KB).

## Decision

Add a third execution path to the 68k family — a DRCUML recompiler — using the
mips3/ppc dual-path template, with the **interpreter as the non-negotiable oracle**
and **the [0001](0001-differential-cpu-oracle.md) corpus + interpreter≡DRC test as
the merge gate.** Target the **new microcode core** (`m68000.cpp`), not Musashi,
because it is the family's canonical, bus-accurate reference and the maintained path.

### 1. DRC frontend architecture for 68k

New files mirroring the template, under `src/devices/cpu/m68000/`:

- `m68000fe.{cpp,h}` — the **decode frontend**: walk a basic block from a start PC,
  decode each opcode into a `opcode_desc` (operand sizes, EA modes, register
  read/write sets, branch/flow info, whether it can fault), reusing the **existing
  decode tables** the interpreter already generates rather than re-deriving decode.
  Where the `m68000gen.py` microcode list already encodes operand/EA semantics, the
  generator is extended (or a sibling generator added) to **also emit a DRC-facing
  descriptor table** so decode logic is single-sourced. *Decision: reuse the
  generator where sane; do not fork the decode truth.*
- `m68000drc.cpp` — the **UML translator + dispatcher**: for each opcode, emit a
  `drcuml` instruction sequence (the per-instruction "generate" methods, as in
  `mips3drc.cpp`), plus the block-cache management, exception/trap entry points,
  and the `static` recompile hooks (`code_compile_block`, the
  `cfunc_*` C fallbacks for the hard cases — privilege violations, address errors,
  group-0 exceptions, FPU ops, MMU).
- `m68000.cpp::execute_run()` gains a top-level branch on `m_isdrc` (the existing
  microcode loop becomes the `else`/interpreter arm). DRC state (`m_drcuml`,
  `m_drcfe`, `m_drc_cache`, `m_drcoptions`, `m_cache_dirty`) is added to
  `m68000_device`, allocated in `device_start()` exactly as
  `src/devices/cpu/mips/mips3.cpp:179-183,447-460` does (UML state, symbol
  registration for debugger UML logs).

### 2. Interpreter-as-oracle relationship

The interpreter arm is untouched in behavior and remains the definition of correct.
The DRC must produce **identical architectural state and identical icount** after
each block. Where the DRC cannot match a subtle bus-cycle behavior cheaply, it
**falls back to a `cfunc_` that calls the interpreter handler** for that opcode
(standard template practice) rather than approximating. `-drc 0` always yields the
reference. We never "fix" the interpreter to match the DRC; divergence is always a
DRC bug.

### 3. Cycle-accuracy strategy (the hard part)

68k timing is intricate: prefetch pipeline, EA-calculation cycle costs, read/write
ordering, and the new core's mid-instruction suspension. The strategy:

- **Per-instruction cycle cost is taken from the same source the interpreter uses**
  (the microcode list's documented cycle counts), emitted into the UML block as
  icount decrements — never independently re-estimated.
- **First increment deliberately limits scope to the common, non-faulting,
  user-mode path** of plain 68000 instructions. Anything touching prefetch-visible
  timing edge cases, address/bus errors, privilege violations, FPU, MMU, or the
  mid-instruction suspend path **routes to the interpreter via `cfunc_`** until the
  oracle proves a DRC fast-path matches.
- **The icount/sub-cycle accounting is reconciled against the
  [0001](0001-differential-cpu-oracle.md) corpus cycle counts** as the acceptance
  test (see Testing). If the 68k icount model and the corpus bus-cycle model differ,
  that reconciliation is documented as a per-core cycle adapter in 0001 — and the
  DRC must match *the interpreter's* count, which is what ships.

### 4. Incremental rollout

1. **Plain `m68000` (the base device), common instruction subset, DRC fast-path;
   everything else → interpreter `cfunc_`.** Land behind `-drc` (already default on)
   but gated by the oracle. This is the first reviewable increment.
2. Widen the fast-path instruction coverage on `m68000`, shrinking the `cfunc_`
   fallback set, each step re-validated by the oracle.
3. Extend to `m68010` (`m68010.cpp`) — minor delta.
4. `m68020`/`m68030`/`m68040` (full EA modes, FPU, MMU, caches) — **separate, much
   larger** increments, each its own ADR addendum; not in scope here.
5. ColdFire (`mcf5206e`), `scc68070`, `fscpu32`, MCU variants — out of scope.

The base 68000 alone covers the bulk of the ~549-driver win (Sega/CPS/Neo-Geo are
predominantly plain 68000/68010).

### 5. How [0001](0001-differential-cpu-oracle.md) gates this (definition: [0006](0006-m68000-oracle-gate-definition.md))

No 68k DRC change merges unless: (a) the m68000 oracle (`./mametests "[m68000]"`) is
green with `-drc 0`, **and** (b) the same vectors are green with DRC enabled —
asserting interpreter ≡ DRC, register-, flag-, memory-, and **cycle-exact**. The
oracle's dual-run parameterization (0001 §2) is exactly this check. This converts an
otherwise unreviewable port into a regression-proof one.

**What "green" means precisely is fixed by [ADR 0006](0006-m68000-oracle-gate-definition.md),**
because the m68000 corpus turned out to be **MAME-derived** (upstream:
*"Generated using the microcoded core in MAME"*, a 2024-08-01 snapshot) rather than an independent
oracle. ADR 0006 splits the gate into **Leg A** (interpreter vs corpus — a breadth probe: 100%
state equality + cycle equality except a frozen, provenance-cited allowlist of {TAS, TRAPV,
address-error} opcodes) and **Leg B** (interpreter ≡ DRC — full register/flag/RAM/**cycle**
equality across the whole corpus, allowlist included). **Clause (b) above is exactly Leg B**, and
it is **corpus-drift-immune** — it compares two MAME execution paths, never the stale corpus — so
it gives the DRC the cycle-for-cycle guarantee it needs regardless of corpus provenance. For
allowlisted opcodes the DRC `cfunc_`s to the interpreter (§3 below), so Leg B holds there by
construction, including cycles: the allowlist never punches a hole in the DRC safety net. The
interpreter — never the corpus — is the authority for both legs.

## Integration seams (file:line)

| Seam | Location | Role |
|---|---|---|
| New microcode interpreter (oracle target) | `src/devices/cpu/m68000/m68000.cpp:147` (`execute_run`) | Gains the `m_isdrc` branch; remains the reference |
| Microcode generator | `src/devices/cpu/m68000/m68000gen.py` (124 KB) + `m68000.lst`, `m68k_in.lst` | Extend to emit DRC decode descriptors |
| Generated (committed) decode/handlers | `m68000-decode.cpp`, `m68000-head.h`, `m68000-s{d,i}{f,p}.cpp` | Decode truth to reuse, not re-derive |
| Legacy Musashi core (NOT the target) | `src/devices/cpu/m68000/m68kcpu.cpp:832` (`m68000_musashi_device::execute_run`) | Out of scope; documents the two-core fact |
| Common base | `src/devices/cpu/m68000/m68kcommon.{h,cpp}` | Shared device base |
| DRC template — frontend | `src/devices/cpu/mips/mips3fe.cpp`, `powerpc/ppcfe.cpp` | Pattern for `m68000fe.cpp` |
| DRC template — translator | `src/devices/cpu/mips/mips3drc.cpp`, `powerpc/ppcdrc.cpp` | Pattern for `m68000drc.cpp` |
| DRC start/latch pattern | `src/devices/cpu/mips/mips3.cpp:179-183,398,447-460` | UML/cache/symbol setup + `m_isdrc = allow_drc()` |
| DRC enable / oracle toggle | `src/emu/devcpu.cpp:50` (`allow_drc`), `src/emu/emuopts.cpp:199` (`drc` default "1"), `201-203` (`drc_use_c`, log options) | `-drc 0` is the reference; backend/log knobs |
| DRCUML core + backends | `src/devices/cpu/drcuml.cpp`, `drcbex64.cpp`, `drcbearm64.cpp`, `drcbec.cpp` | The UML target (all present, mature) |
| Build wiring | `scripts/src/cpu.lua` (M680X0 `files {}` block, ~2116-2143) | New `m68000fe.cpp`/`m68000drc.cpp` registered here |
| The oracle gate | [0001](0001-differential-cpu-oracle.md) §2, `tests/emu/cpu/cpuoracle.cpp` `[m68000]` | Merge gate (interpreter ≡ DRC, cycle-exact) |

## Alternatives considered

1. **Port the Musashi core (`m68kcpu.cpp`) instead of the new microcode core.**
   Rejected — Musashi is the legacy path; the microcode core is the maintained,
   bus-accurate reference and where upstream effort is going. DRC-ing the deprecated
   core would bit-rot.
2. **A hand-written native 68k JIT (no DRCUML).** Rejected — throws away the three
   working, portable, save-state-aware DRCUML backends and the decade of dual-path
   conventions; would be unmaintainable and non-portable (x64/arm64/C-fallback come
   free with DRCUML).
3. **Approximate cycle timing in the DRC fast path.** Rejected outright — violates
   "accuracy is sacred." Anything we can't match exactly routes to the interpreter
   `cfunc_`. Timing fidelity is the whole point of the oracle gate.
4. **Big-bang full-family port (000–040 + FPU + MMU) in one go.** Rejected — far too
   large to review or validate; the increment plan lands the high-value, low-risk
   plain-68000 path first, oracle-gated, then widens.
5. **Skip [0001](0001-differential-cpu-oracle.md) and validate by booting games.**
   Rejected — game-boot diffing can't prove cycle-exactness and hides
   per-instruction regressions; the port would be unreviewable. 0001 is a hard
   prerequisite.

## Consequences

**Good**
- Largest single throughput improvement available — accelerates ~549 drivers' main
  CPU on x64 and arm64 hosts, with a C fallback elsewhere.
- Reuses mature, portable, save-state-aware DRCUML infrastructure; no new backend.
- The increment + `cfunc_` fallback strategy means each merge is small, correct, and
  oracle-proven; coverage widens monotonically.

**Bad / cost**
- Highest effort and risk in the program; the first increment alone is substantial,
  and full-family parity is out of scope here.
- Adds significant new code (`m68000fe.cpp`, `m68000drc.cpp`) and DRC state to a
  heavily-used core; bugs would be widely felt — which is exactly why it is
  oracle-gated and increment-staged.
- Generator extension to emit DRC descriptors touches `m68000gen.py` and the
  committed generated files; the regeneration workflow must be documented so the
  decode truth stays single-sourced.
- 68k prefetch/bus-timing edge cases are genuinely hard; expect a long tail of
  `cfunc_` fallbacks that only shrink slowly.

## Accuracy & determinism preservation

- **Interpreter is the oracle.** `-drc 0` runs the unchanged microcode core; DRC must
  match it register-, flag-, memory-, and cycle-exact, enforced by
  [0001](0001-differential-cpu-oracle.md). Unmatched cases fall back to the
  interpreter via `cfunc_`; we never alter the interpreter to suit the DRC.
- **Determinism preserved.** DRC executes on the same emulation thread, driven by the
  same single-threaded scheduler and the same `m_icount` accounting; it feeds the
  same save-state timeline. No work moves off-thread. DRCUML's block cache is rebuilt
  deterministically and is not part of save state. This honors the spec's hard
  constraint that nothing moves off the timeline-feeding thread.

## Testing & validation

- **Merge gate:** `make TESTS=1 && ./mametests "[m68000]"` must be green **both** with
  `-drc 0` (oracle reference) and DRC enabled (interpreter ≡ DRC) — register, flag,
  memory, and cycle equality (the 0001 dual-run, §1.6).
- **Backend matrix:** validate against `drcbex64` (host x64), `drcbec` (forced via
  `-drc_use_c 1` / `OPTION_DRC_USE_C`), and — where available — `drcbearm64`, so the
  port is correct on all three UML backends, not just the host's native one.
- **Structural gate:** `./mame -validate` stays green (the device still validates).
- **Iteration build:** the m68000 core builds as part of the `mametests` (`TESTS=1`)
  target — `make REGENIE=1 && make TESTS=1 && ./mametests "[m68000]"` for fast
  core+oracle rebuilds. Do **not** use `make SOURCES=src/devices/cpu/m68000/m68000.cpp`:
  `SOURCES` filters by *driver*, and a CPU device has no drivers, so GENie errors. A
  representative driver build (e.g. a Sega System 16 driver) is used for an integration
  smoke test.
- **Spot integration:** boot a handful of high-value 68000 games under `-drc 1` and
  `-drc 0` and confirm identical behavior (in addition to the oracle), as a
  belt-and-suspenders check before each coverage widening.
- **Definition of done (first increment):** plain `m68000` common-path DRC green in
  the oracle on all three backends; `-validate` clean; no regression in a sampled set
  of 68000 drivers.

## Open questions (for the owner before Planner runs)

1. **Generator vs. hand-written frontend.** Extend `m68000gen.py` to emit DRC decode
   descriptors (single-sourced decode, more upfront generator work), or hand-write
   `m68000fe.cpp` decode against the existing tables (faster to start, risks
   decode-truth drift)? (Recommend: extend the generator — single source of truth.)
2. **First-increment instruction scope.** How aggressive is "common path"? Propose:
   ALU/MOVE/branch/Bcc/scc/addq/subq with register and simple-EA addressing modes;
   everything else `cfunc_`. Owner to confirm the cut line.
3. **Acceptance threshold to ship increment 1.** Ship as soon as it is correct
   (oracle-green) even if most opcodes still `cfunc_` to the interpreter (so the
   *infrastructure* lands), or hold until a target % of dynamic instructions are
   native? (Recommend: ship infra-first once oracle-green; widen in follow-ups.)
4. **m68010 in P2 or deferred.** Include the small 68010 delta in this phase, or keep
   P2 strictly to base 68000? (Recommend: base 68000 only for P2.)
5. **Performance target.** Is there a throughput acceptance bar (e.g. ≥N× on a named
   68000 driver) that gates "done," or is correctness-only sufficient for the first
   increment with perf tracked as a follow-up? (Recommend: correctness-gates-merge;
   perf reported, not gated, for increment 1.)
