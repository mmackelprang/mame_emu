# ADR 0006 — m68000 differential-oracle gate definition (authority, pinning, acceptance criteria)

> **Status:** Accepted (Leg A reframed to a ~99% probe, owner-ratified 2026-06-25) · **Phase:** P1
> (resolves the Phase-1 → Phase-2 hard gate) · **Owner:** TBD
> **Depends on:** [0001 (CPU oracle)](0001-differential-cpu-oracle.md) — this ADR refines 0001's
> m68000 leg and its "cycle-equality strictness per core" open question (0001 OQ #3).
> **Depended on by:** [0002 (m68000 DRC)](0002-m68000-drcuml-port.md) — this ADR is what 0002's
> hard gate now means.
> **Spec:** [`docs/improvement-plan/specs/2026-06-24-mame-improvements-design.md`](../specs/2026-06-24-mame-improvements-design.md)
> **Date:** 2026-06-25
>
> **Update (close-out, 2026-06-25):** Leg A landed at **~99.6% state / ~99.3% cycle** and is
> **reframed as a high-coverage conformance PROBE** (not a strict 100%-state gate) — the owner chose
> to accept the ~99% bar over chasing the MAME-self-generated corpus's deferred-trace inconsistency.
> The probe is wired so it **cannot hide a regression** (strict outside a corpus-data-keyed
> deferred-exception residual; out-of-residual divergences asserted `== 0`). **Leg B (interpreter ≡
> DRC) is the load-bearing Phase-2 gate and is unchanged / corpus-immune.** See *Named provenance
> limitations* and the updated *Acceptance criteria*.

## TL;DR

The m68000 SingleStepTests corpus is **not an independent behavioral reference** — its own
upstream repository states it was *"Generated using the microcoded core in MAME"* and *"Any
bugs that exist in MAME's microcoded M68000 emulator will exist here too."* It is a **2024-08-01
snapshot of the very core the oracle is validating** (`m68000.cpp`), not a hardware- or
third-party-emulator-derived oracle like the z80 and 6502 corpora. Naive *"strict cycle equality
vs the full m68000 corpus"* therefore measures **drift from a stale MAME snapshot**, not
correctness — it is the wrong gate.

**Decision (three parts):**

1. **Authority.** For m68000, **the in-tree interpreter (`m68000.cpp` with `-drc 0`) is the
   behavioral authority** — exactly as it already is for the DRC in ADR 0002. The corpus is
   **demoted to a high-volume conformance/regression probe**, not the source of truth. This is
   a *deliberate asymmetry* from z80/m6502, justified by provenance.

2. **Pinning.** **Keep the corpus pinned** to the current commit in `manifest.json`
   (`64b253116a3de04aaac4346c43680960dc9b67e5`, 2024-08-01) — do **not** chase upstream HEAD.
   Record the provenance (MAME-derived, generator MAME revision unknown/2024-vintage) and the
   documented-divergence set **in the manifest and `tests/cpuoracle/README.md`** so the
   asymmetry is legible.

3. **Gate.** Redefine the Phase-2 hard gate as **two separable legs**:
   - **Leg A — corpus conformance (interpreter vs corpus):** *strict architectural-state
     equality (registers / flags / RAM) across 100% of replayed cases*, **plus** *strict
     cycle equality across all cases except a documented, frozen allowlist* of
     known-divergent opcodes (TAS, TRAPV, address-error vectors), each entry carrying a written
     rationale tying the divergence to a **corpus-provenance / generator-snapshot** cause rather
     than a live-core bug.
   - **Leg B — DRC self-consistency (interpreter ≡ DRC):** *strict register / flag / RAM **and**
     cycle equality between `-drc 0` and `-drc 1` across the full replayed corpus,
     allowlist included* (the allowlist exempts Leg A from the corpus, **not** Leg B from the
     interpreter). **Leg B is the cycle-exactness guarantee Phase 2's DRC actually needs**; Leg A
     is a breadth probe that the interpreter has not regressed.

Both legs must be green for "the m68000 oracle is green." Leg B is the load-bearing one for ADR
0002; Leg A protects the interpreter itself.

## Context

### What surfaced this

The boundary-C Builder completed the m68000 oracle infrastructure (decoder, register map,
sub-cycle stepper) and got a green smoke over 127 fixtures / 317,500 cases — but only **~30% exact
cycle agreement** with the pinned corpus. Two blockers were diagnosed:

1. **Prefetch-pointer / first-instruction priming** (an implementation detail in the harness).
2. **Corpus provenance / version drift** (the architectural question this ADR resolves).

### Provenance investigation (the crux)

The upstream repo (`SingleStepTests/m68000`, GitHub description and `README.md`, verified
2026-06-25) establishes:

- **The corpus is MAME-derived.** Repo description: *"TomHarte-style JSON tests for the Motorola
  68000. **Generated using the microcoded core in MAME.**"* README: *"Any bugs that exist in
  MAME's microcoded M68000 emulator will exist here too."* This is the **same core lineage**
  (`m68000.cpp`, the microcode/state-machine core — *not* legacy Musashi) that ADR 0002 ports
  and that this oracle validates.
- **It is a stale snapshot.** The pinned commit `64b2531…` is dated **2024-08-01** (author
  `raddad772`, message *"fixed the issue"*). The in-tree `m68000.cpp` is HEAD of this 2026 tree.
  ~2 years of upstream MAME microcode-core changes separate the generator snapshot from the core
  under test. **This drift — not core bugs — is the dominant cause of the ~70% cycle mismatch.**
- **Documented known-divergent set.** README STATUS: *"all of the tests except **TAS** and
  **TRAPV** are verified as good."* TAS: *"doesn't properly handle the special 5-cycle TAS
  read-modify-write timing."* TRAPV: *"some strange issue … appears to trigger incorrectly based
  on the S bit."* Plus **address-error** cases carry new cycle types `re` (read address error)
  / `we` (write address error) for cases where *"AS just isn't asserted, so the results aren't
  committed."*
- **PC semantics — this pins blocker #1.** README: *"PC is now set using `m_au` from MAME … it's
  'next prefetch address' so it's **+4** from where the test starts executing."* The in-tree core
  confirms it exactly: `state_import` for `STATE_GENPC` sets `m_pc = m_ipc + 2` and
  `m_au = m_ipc + 4` (`src/devices/cpu/m68000/m68000.cpp:366-367`). MAME's `STATE_GENPC` exports
  `m_pc` (= start + 2); the corpus encodes `m_au` (= start + 4). **The +2 PC offset is a pure
  harness read-back adapter, not a core question** — read the retired PC from `m_au`, not from
  `STATE_GENPC`. The same import path seeds the full prefetch pipeline
  (`m_ir/m_ird/m_irdi/m_irc/m_dbin`) from `m_ipc` (`:369-370`), so first-instruction priming works
  provided **RAM is applied before PC** — the identical ordering rule the 6502 leg already proved
  (`cpuoracle.cpp` apply-RAM-then-registers).

### Why this is different from z80 and m6502

The z80 corpus (`SingleStepTests/z80`) and 6502 corpus (`SingleStepTests/65x02`) are
**independently derived** — not generated from MAME. That independence is *why* they functioned
as genuine oracles and **caught real MAME bugs**: two z80 WZ/MEMPTR inaccuracies (now fixed) and
three 6502 unstable/illegal-opcode disagreements (surfaced for the maintainer; the `0xBB` LAS stub
is a real bug). Strict cycle equality against those corpora is **meaningful and achievable**
because they encode an *external* truth.

For m68000 the corpus is a **mirror of (a past version of) the thing under test**. Holding the
live core to *strict* equality with a stale self-snapshot would either (a) be unachievable wherever
the core legitimately improved since 2024, or (b) — worse — pressure us to *revert* the live core
to match a stale snapshot, which directly violates the program's "accuracy is sacred, the
interpreter is the oracle" invariant (ADR 0002 §2, §Accuracy). **The corpus cannot be the
authority for the core that generated it.**

### What Phase 2 actually needs from this gate

ADR 0002's DRC must be **cycle-for-cycle identical to the interpreter** (§5: *"no 68k DRC change
merges unless … green with `-drc 0` AND … with DRC enabled … register-, flag-, memory-, and
cycle-exact"*). That guarantee is **interpreter ≡ DRC** — it does *not* require the interpreter to
match the corpus. The interpreter is the shipping reference; the DRC must mirror *it*, not the
corpus. So the cycle-exactness Phase 2 needs lives entirely in **Leg B** (interpreter ≡ DRC) and is
**independent of corpus drift**. Leg A (interpreter vs corpus) is a valuable but *secondary*
breadth check on the interpreter, and must be defined so corpus provenance can't make it
unachievable.

## Decision

### 1. Authoritative reference: the in-tree interpreter, not the corpus

For m68000, **`m68000.cpp` under `-drc 0` is the behavioral authority.** The corpus is a
**conformance probe** (high-volume, independently-formatted, useful for catching gross
interpreter regressions and decode mistakes), **not** the definition of correct. This mirrors the
authority model ADR 0002 already mandates for the DRC and is consistent with "we never modify the
interpreter to match anything downstream."

No better third-party m68000 reference is adopted. (Surveyed: hardware-derived 68000 single-step
corpora are not publicly available at this granularity; other emulators — Musashi, the legacy MAME
core, third-party cores — are *less* authoritative than the maintained microcode core, not more.
Generating a fresh corpus from the *current* in-tree core was considered and rejected — see
Alternatives — because a self-generated corpus can only confirm the core agrees with itself.)

### 2. Corpus pinning strategy: keep the current pin; record provenance

- **Keep** `manifest.json`'s m68000 pin at `64b253116a3de04aaac4346c43680960dc9b67e5`
  (2024-08-01). Do **not** track upstream HEAD (chasing a moving MAME-derived snapshot adds churn
  with no authority gain).
- **Do not** re-pin to "a specific generator MAME revision" — the generator revision is
  undocumented upstream and, more fundamentally, pinning the generator wouldn't help: the gate's
  authority is the *live* core, not whatever core produced the corpus.
- **Annotate provenance in the manifest.** Add to the m68000 entry a machine-readable
  `provenance` block recording: `derived_from: "MAME microcoded m68000 core"`, a
  `not_independent: true` flag, the upstream STATUS note, and a `cycle_divergence_allowlist`
  pointing at the opcode list (single-sourced; see §3). The z80/m6502 entries get
  `derived_from: "independent"` / `not_independent: false` so the asymmetry is explicit in the
  one committed artifact.
- **Bump policy:** the m68000 pin is bumped only deliberately and only when a newer corpus
  *reduces* the documented divergence set (re-fetch, recompute sha256, re-derive the allowlist) —
  never automatically.

### 3. Gate definition — two legs, one frozen allowlist

#### Leg A — corpus conformance (interpreter vs corpus) — a HIGH-COVERAGE PROBE

> **Reframed (owner-ratified, 2026-06-25).** Leg A is a **high-coverage conformance probe at
> ~99.6% state / ~99.3% cycle**, **not** a strict 100%-state gate. The original "100% state, no
> exemptions" bar proved unreachable because the pinned corpus is **internally inconsistent on
> deferred-trace/exception capture** (a named provenance limitation — see *Named provenance
> limitations* below), and the owner chose to **accept the ~99% probe** rather than chase a
> MAME-self-generated corpus the interpreter (the authority) need not match cell-for-cell. **The
> load-bearing Phase-2 guarantee is Leg B (interpreter ≡ DRC), which is corpus-immune; a ~99% Leg A
> does not weaken it.** The gate is implemented so the probe **cannot hide a regression** (below).

For every replayed case (the full pinned corpus, subject only to `CPUORACLE_MAX_FILES` for
fast local runs):

- **State equality — strict OUTSIDE a principled deferred-exception residual; reported inside it.**
  Every mapped architectural register, every flag bit (the full CCR/SR), and every fixture final-RAM
  cell is **hard-REQUIRE'd to match** for every case **outside** the residual predicate
  (`m68000_residual_expected`: initial SR.T set, OR TAS/TRAPV, OR an address-error case, OR a
  branch-self-loop). A state divergence **outside** the residual is always a finding and blocks the
  gate. The **count of out-of-residual divergences is asserted `== 0`**, so the residual cannot mask
  a real bug. Divergences **inside** the residual are reported (the probe stays green); they are the
  documented corpus-provenance / harness-single-step limitations, not interpreter regressions.
  `CPUORACLE_M68_STRICT=1` hard-REQUIREs the residual too (for investigating a corpus re-pin) and
  fails on it by design.
- **Cycle equality — strict except a frozen allowlist (and the same residual).** Consumed bus
  cycles must equal the corpus cycle count for every opcode **except** those on the
  **cycle-divergence allowlist**, and cycle mismatches inside the deferred-exception residual are
  reported, not failed:
  - **TAS** (`0x4Axx` byte form) — corpus omits the special 5-cycle RMW timing (upstream STATUS).
  - **TRAPV** (`0x4E76`) — corpus generation flagged an S-bit-dependent triggering issue
    (upstream STATUS).
  - **Address-error vectors** — any case whose transaction log contains an `re` / `we`
    (read/write address-error) cycle type, where AS isn't asserted and results aren't committed
    upstream.
  - Each allowlist entry carries a **one-line written rationale** citing the upstream
    provenance/STATUS note (not a hand-wave), in the single-sourced allowlist (below). For
    allowlisted opcodes, **state equality is still asserted** where the corpus committed a final
    state; only the *cycle* comparison is skipped.
- The allowlist is **frozen and minimal** — modeled on the m6502 leg's evidence-based skip list
  (12 JAM + 3 unstable, each with a rationale). Adding to it requires a provenance citation in
  review; it is not a place to bury unexplained mismatches. Every non-allowlisted cycle mismatch
  **must** be driven to zero before the gate is green (the prefetch/`m_au` adapter and RAM-first
  ordering are expected to close the bulk of the current ~70%; any residue is a finding, not a
  silent skip).

#### Leg B — DRC self-consistency (interpreter ≡ DRC)

For every replayed case **including the Leg-A allowlist**:

- Run the same fixture through `-drc 0` and through `-drc 1`, and assert **strict register, flag,
  RAM, and cycle equality between the two MAME runs.** The corpus's expected values are *not*
  consulted here — only interpreter-output vs DRC-output. (For allowlisted opcodes the DRC routes
  to a `cfunc_` interpreter fallback per ADR 0002, so interpreter ≡ DRC holds there *by
  construction*, including cycles — which is exactly why the allowlist exempts Leg A but never
  Leg B.)
- **Leg B is the cycle-for-cycle guarantee ADR 0002 §5 requires.** It is corpus-drift-immune: it
  asks only whether two MAME execution paths agree, which is the actual safety property a DRC port
  needs.

#### How the two legs relate (the clarification ADR 0002 needs)

```
   corpus (2024 MAME snapshot)          interpreter (live m68000.cpp, -drc 0)        DRC (-drc 1)
            │                                      │                                     │
            │  Leg A: state == 100%                │   Leg B: state/flags/RAM/cycle      │
            └───── cycle == except allowlist ──────┤◄────────── EXACT, full corpus ──────┘
                   (breadth probe on the           │            (the Phase-2 guarantee)
                    interpreter; provenance-        │
                    bounded)                        │
                                          AUTHORITY: this column
```

- **Leg A** answers *"has the interpreter grossly regressed vs a known broad sample, and does it
  decode/flag every opcode correctly?"* — bounded by corpus provenance via the allowlist.
- **Leg B** answers *"is the DRC cycle-for-cycle identical to the shipping interpreter?"* — the
  real merge gate for ADR 0002, **independent of how stale the corpus is.**
- The interpreter is the authority for both. We never edit the interpreter to satisfy Leg A; an
  unexplained Leg-A *cycle* mismatch is investigated and either (a) traced to corpus drift and —
  if and only if provenance-justified — allowlisted with a rationale, or (b) found to be a real
  interpreter regression and fixed. State mismatches (Leg A) are never allowlisted.

### 4. Single-sourcing the allowlist

The cycle-divergence allowlist lives in **one** place — a small committed table (e.g.
`tests/cpuoracle/m68000_divergence.inc` or a `static const` table in `cpuoracle.cpp`, mirroring
the m6502 `k_jam_files` / `k_unstable_files` pattern) — keyed by opcode/file with a rationale
string, and **referenced** (not duplicated) from `manifest.json`'s `provenance` block and
`tests/cpuoracle/README.md`. One edit point; review sees the rationale next to the skip.

### Named provenance limitations (the documented Leg-A residual)

These are the **characterised, owner-accepted** reasons Leg A is a ~99% probe rather than a strict
100%-state gate. Each is keyed on a corpus-data signature (so the residual cannot mask an unrelated
bug), and **none is a core defect** — the interpreter is correct; the limitation is the
MAME-self-generated corpus or the harness's single-step model:

1. **Inconsistent deferred-trace/exception capture (the dominant class).** ~50% of the corpus has
   `SR.T` set; for most opcodes the corpus snapshots state **before** the trace exception, but for
   the exception-taking subset (taken branches; ILLEGAL/TRAP/CHK/RTE/MOVEtoSR; address-error pops)
   it snapshots **after** the exception ran (`SR.S|T` flipped, a frame pushed, PC vectored, extra
   cycles). No uniform single-step model matches both. Signature: initial `SR.T` set, OR TAS/TRAPV,
   OR an address-error case.
2. **Branch-self-loop (a branch whose target re-enters the branching instruction).** A `BSR -2` /
   `Bcc -2` branches onto itself; the harness single-step retires on the `m_ipc` change, which never
   occurs for a self-branch, so the guard loop exhausts without retiring. BSR/Bcc are correct (the
   corpus, from the same core, runs them once); this is a harness single-step limitation.
   **Signature: the harness's own `did_not_retire()` flag** (the step exhausted the guard with
   `m_ipc == entry_ipc`) — a precise harness-observable signal, *not* a corpus PC-delta heuristic
   (which would also exempt ordinary not-taken short branches, e.g. the 565 not-taken `Bcc +2`
   cases, and create a blind spot). (~20 cases.)

Two earlier suspected limitations were **fixed** (not accepted) during close-out, both harness-side,
zero core change: MOVEP byte-lane (a cross-case stale-RAM gap, fixed by a per-case RAM scrub) and
the `(A7)`/auto-inc-dec/ABCD/ADDX false-divergence class (a single-step **over-run** — the grant that
advances `m_ipc` also ran the next instruction's first memory write — fixed by snapshotting the
watched final-RAM cells at the same pre-grant retirement point as the registers).

## Acceptance criteria (what Phase 2 consumes — unambiguous, reviewable)

**"The m68000 oracle is green"** ⇔ all of the following, on the host backend, with the pinned
corpus fetched:

1. **Leg A state (probe):** every replayed case **outside** the deferred-exception residual
   (`m68000_residual_expected`) passes strict register + flag + RAM equality, **and the count of
   out-of-residual state divergences is `0`**. Divergences inside the residual are reported, not
   failed. (Achieved: ~99.6% — 316 230 / 317 500.)
2. **Leg A cycles (probe):** every replayed case not on the frozen allowlist **and** outside the
   residual passes strict cycle equality, **and the count of out-of-residual cycle divergences is
   `0`**. The allowlist is exactly {TAS, TRAPV file-level; address-error case-level} unless a
   review-approved provenance-cited addition is made. (Achieved: ~99.3% of cycle-checked cases.)
3. **Leg B (the Phase-2 guarantee, UNCHANGED):** 100% of replayed cases — allowlist included — pass
   strict register + flag + RAM + **cycle** equality between `-drc 0` and `-drc 1`. This is
   corpus-immune (it never consults the corpus) and is **not weakened by the ~99% Leg-A bar**.
4. The run reports its counts (state %, cycle %, allowlisted, residual, unexplained=0) the way the
   z80/m6502 legs `WARN(...)` their totals, so "green" is auditable, not silent.

For **Phase 1, Task 5** the bar is criteria **1 + 2** (the interpreter probe; there is no DRC yet, so
Leg B is vacuously satisfied / skipped via the capability query in ADR 0001 §2). For **Phase 2**
every DRC-touching PR additionally requires criterion **3** — *the load-bearing gate* — and the
ADR-0002 backend matrix (Task 8) requires criterion 3 on each available UML backend (`drcbex64`,
`drcbec` via `-drc_use_c 1`, `drcbearm64` where available).

> **Note on `-drc 0` ≡ DRC for allowlisted opcodes:** because allowlisted opcodes `cfunc_` to the
> interpreter in the DRC (ADR 0002 §3), Leg B's cycle equality there is structural, so Phase 2 still
> gets a *complete* cycle-exact interpreter≡DRC guarantee across the whole corpus — the allowlist
> never punches a hole in the DRC safety net, only in the (secondary) corpus-conformance probe.

## Close-out path (Builder, ordered)

1. **Fix the PC read-back adapter (blocker #1).** Read the retired PC from **`m_au`** (start + 4,
   what the corpus encodes), not from `STATE_GENPC`/`m_pc` (start + 2). Concretely: the m68000
   register-map / stepper compares the corpus `pc` against `m_au`, or applies a documented `+2`
   adapter to the exported PC — mirror the m6502 leg's documented per-core adapter comment.
   Confirm RAM-is-applied-before-PC ordering (the import path at `m68000.cpp:366-370` seeds the
   prefetch pipeline from `m_ipc`), as the 6502 leg already requires.
2. **Land the m68000 sub-cycle stepper + register map.** Drive `m68000_device::execute_run()`
   (`m68000.cpp:147`) one bus-cycle group at a time, retiring exactly one architectural
   instruction (step until `m_inst_state >= S_first_instruction` re-reached at substate 0 — the
   analogue of the z80 `m_ref == 0xffff00` / 6502 `m_inst_substate == 0` boundary). Add the
   m68000 `oracle_stepper` device + `[cpu][m68000]` case. Map D0–D7, A0–A7/USP/SP, SR/CCR, PC
   (via `m_au`).
3. **Bring Leg A state to 100%.** With (1)+(2), assert register/flag/RAM equality across the full
   corpus; drive any state mismatch to zero (these are harness-adapter bugs or genuine findings —
   not allowlist material). State has no allowlist.
4. **Build the frozen cycle allowlist with rationales.** Add the single-sourced
   `m68000_divergence` table: TAS, TRAPV, and the `re`/`we` address-error cases, each with a
   one-line provenance citation (upstream STATUS / cycle-type note). Reference it from the manifest
   `provenance` block and `tests/cpuoracle/README.md`.
5. **Bring Leg A cycles to 100%-minus-allowlist.** With the `m_au` adapter and correct boundary
   detection, the non-allowlisted cycle agreement must reach 100%. Investigate every residual
   mismatch: trace to corpus drift (allowlist with citation, only if provenance-justified) **or**
   to a real interpreter issue (fix, or surface as an oracle finding — never silently skip).
6. **Wire the strict REQUIREs.** Convert the green diagnostic run into hard `REQUIRE`s for
   criteria 1 + 2, matching the z80/m6502 leg structure (strict per-field, per-cell, per-cycle).
   Add the `WARN(...)` totals line.
7. **Update the manifest provenance + `tests/cpuoracle/README.md`** (Phase-1 Task 8): document the
   MAME-derived provenance, the authority asymmetry vs z80/m6502, the `m_au` PC adapter, and the
   allowlist with rationales.
8. **(Phase 2 only) Leg B.** Once the DRC exists, run the dual `-drc 0` / `-drc 1` parameterization
   (ADR 0001 §2) for criterion 3 across the full corpus including the allowlist, on the backend
   matrix. This is the gate ADR 0002 PRs K–O consume.

> **Close-out outcome (2026-06-25).** Steps 1–7 landed (PC via `m_au`; the sub-cycle stepper +
> register map; the per-case RAM scrub + retirement RAM snapshot that fixed the MOVEP byte-lane and
> the `(A7)`/over-run classes; the frozen cycle allowlist; the manifest provenance + README). The
> strict-100%-state bar from step 6 was **reframed by the owner** to the ~99.6% **probe** above,
> because the remaining residual is the corpus's own deferred-trace inconsistency (a provenance
> limitation, not a core bug). The probe hard-REQUIREs everything outside the corpus-data-keyed
> residual and asserts out-of-residual divergences `== 0`, so it stays a real gate. Step 8 (Leg B)
> remains Phase 2.

If a state/cycle divergence appears **outside** the documented residual, that is an **oracle finding
for the maintainer** (like the z80 WZ fixes) — investigate and fix it (in the harness if it is a
single-step artifact, in the core only if the corpus — from the same core — disagrees and the core
is genuinely wrong); do **not** expand the residual predicate to hide it. The probe's
`unexplained == 0` assertion enforces this. Phase 1, Task 5's risk R1 is retired: the gate is
*achievable* because Leg B (the part Phase 2 truly needs) is corpus-drift-immune, and Leg A is a
provenance-bounded probe.

## Alternatives considered

1. **Strict cycle equality vs the full corpus, no allowlist (the original Task-5 framing).**
   Rejected — measures drift from a 2024 MAME self-snapshot, not correctness; either unachievable
   where the core legitimately improved, or pressures reverting the live core to a stale snapshot
   (violates "interpreter is the oracle"). The corpus cannot be authority for the core that
   generated it.
2. **Drop the m68000 corpus entirely; gate only on interpreter ≡ DRC (Leg B alone).** Rejected as
   the *sole* gate — Leg B can't run until the DRC exists, leaving Phase-1 Task 5 with no
   machine-checkable deliverable, and we'd lose a cheap, high-volume regression probe on the
   interpreter's decode/flag/state correctness. Kept as Leg A (demoted to a probe), added Leg B as
   the Phase-2 guarantee.
3. **Re-pin to upstream HEAD / chase the latest corpus.** Rejected — a moving MAME-derived snapshot
   adds churn with zero authority gain; it's still not independent of the core under test.
4. **Regenerate a fresh corpus from the *current* in-tree core.** Rejected — a self-generated
   corpus can only prove the core agrees with itself (circular); it adds a generator to maintain and
   gives Leg A no independent signal. The pinned 2024 corpus, *because* it predates current changes,
   is at least a slightly-independent broad sample.
5. **Treat TAS/TRAPV/address-error as core bugs and "fix the interpreter to match the corpus."**
   Rejected outright — upstream documents these as *corpus-generation* issues, and the interpreter
   is the authority; we never edit the live core to match a stale snapshot. They are
   allowlisted with provenance citations, not core-changed.
6. **Use Musashi or a third-party 68000 core as the reference instead of the corpus.** Rejected —
   the maintained microcode core is *more* authoritative than the legacy/third-party cores, not
   less; ADR 0002 already targets the microcode core as canonical.

## Consequences

**Good**
- The Phase-2 hard gate becomes **unambiguous and reviewable**: two enumerated legs, explicit
  acceptance criteria, a frozen provenance-cited allowlist.
- **Phase-2's cycle-exactness guarantee is preserved and made drift-immune** — it lives in Leg B
  (interpreter ≡ DRC), which never consults the corpus, so corpus staleness can't weaken or block
  the DRC gate.
- Retires Phase-1 risk **R1** ("the gate may not be reachable"): the unreachable part (strict
  equality vs a self-snapshot) was never the part Phase 2 needed.
- Blocker #1 is correctly classified as a **harness adapter** (read PC from `m_au`), not a core or
  authority question — with the exact file:line mechanism documented for the Builder.
- The provenance asymmetry (m68000 derived vs z80/m6502 independent) is recorded in the one
  committed artifact (`manifest.json`) and the oracle README, so future cores inherit a clear
  rule: *check whether your corpus is independent before trusting strict cycle equality against
  it.*

**Bad / cost**
- m68000's corpus conformance (Leg A) is a **weaker** signal than z80/m6502's — it can't catch a
  bug the 2024 generator also had. Mitigated: Leg A is explicitly a *probe*, and real correctness
  is anchored by the interpreter-as-authority + Leg B.
- Introduces a **per-core allowlist** with a maintenance discipline (every entry needs a
  provenance citation). Mitigated by single-sourcing it and modeling the review bar on the m6502
  leg's evidence-based skip list.
- A future maintainer could **abuse the allowlist** to hide a real cycle regression. Mitigated by
  the rule that allowlist additions require an upstream-provenance citation in review, state is
  never allowlisted, and Leg B (DRC ≡ interpreter) is unaffected by the allowlist.

## Accuracy & determinism preservation

This ADR is a **test-gate definition only** — zero changes to emulation behavior. It *strengthens*
the accuracy invariant by making explicit that the **live interpreter, not any corpus, is the
m68000 authority**, and by forbidding "fix the interpreter to match the corpus." Determinism is
untouched (the oracle harness is single-threaded over a flat-RAM space, per ADR 0001).

## Open questions — RESOLVED (owner-confirmed, 2026-06-25)

All three open questions were confirmed by the owner during the Leg-A close-out and are now baked
into the implementation (`tests/emu/cpu/cpuoracle.cpp`, `tests/cpuoracle/fetch_vectors.py`,
`tests/cpuoracle/manifest.json`):

1. **Allowlist granularity — CONFIRMED: case-level for address errors, file-level for TAS/TRAPV.**
   The fetcher (`fetch_vectors.py` decoder v2) surfaces a per-case `addr_error` marker for any case
   whose transaction log carries a `re`/`we` (read/write address-error) cycle type; the gate skips
   the cycle assert **only on those marked cases**, keeping the rest of each file strict. (The
   address-error cases are spread thinly across 63 opcode files — ~22 k of 55.6 k marked cases per
   the corpus — so file-level would needlessly exempt hundreds of thousands of valid cases.) `TAS`
   and `TRAPV` are exempted at **file level** (whole opcode upstream-flagged). State equality is
   asserted for all of these; only the *cycle* comparison is exempt.
2. **Leg A assertion depth — CONFIRMED: cycle COUNT + final state only.** Leg A asserts cycle
   `count` (consumed icount vs the corpus `length`) plus register/flag/RAM final state, matching the
   z80/m6502 legs. The per-cycle bus transaction log is **not** asserted (the verbose log is dropped
   at decode time; only `length` + the `addr_error` bit are kept). Full transaction-log matching is
   deferred as a possible later ratchet.
3. **CI corpus — CONFIRMED: the gate runs the FULL corpus.** The gate path replays all 127 files /
   317.5 k cases with no default cap; `CPUORACLE_MAX_FILES` remains only as a local fast-run knob
   (and is not set in the CI gate). Wiring the full m68000 corpus into the CI `mametests` step is
   part of boundary D (ADR 0001's last item).
