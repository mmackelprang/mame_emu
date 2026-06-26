<!--
license:BSD-3-Clause
copyright-holders:Mark Mackelprang
-->
# Differential CPU-execution oracle

This directory holds the fetch/provenance plumbing for MAME's differential CPU
oracle (see [ADR 0001](../../docs/improvement-plan/adr/0001-differential-cpu-oracle.md)).
The oracle replays the [SingleStepTests](https://github.com/SingleStepTests)
per-instruction corpus against MAME's CPU cores and asserts strict
register / flag / RAM / cycle equality, one instruction at a time.

- **Harness:** `tests/emu/cpu/cpu_test_harness.{h,cpp}` — boots a headless
  single-CPU `running_machine`, applies a fixture, single-steps exactly one
  instruction, reads the result back.
- **Test cases:** `tests/emu/cpu/cpuoracle.cpp` — the `[cpu][z80]`,
  `[cpu][m6502]`, `[cpu][m68000]` Catch2 cases. Built into `mametests`.
- **Fetcher:** `fetch_vectors.py` — downloads + verifies the pinned corpus into
  the gitignored `build/cpuoracle/<core>/` cache.
- **Manifest:** `manifest.json` — the only committed corpus artifact: a pinned
  ref + sha256 + mirrors + **provenance block** per core.

## Provisioning the fixtures

```sh
python tests/cpuoracle/fetch_vectors.py --cores z80,m6502,m68000
```

Fixtures are **fetched, not vendored** (hundreds of MB, separately licensed): the
fetcher verifies the pinned archive sha256, extracts into the cache, and writes a
per-file checksum index. The run is idempotent — a complete, untampered cache is a
no-op. If the cache is absent the oracle tests skip cleanly (so `mametests` stays
green without the corpus). No network access at test-run time.

Per-core fast-run knob: `CPUORACLE_MAX_FILES=N` caps the number of fixture files
(local iteration only — the CI gate runs the full corpus).

## Authority asymmetry: independent (z80/m6502) vs MAME-derived (m68000)

The **z80** (`SingleStepTests/z80`) and **6502** (`SingleStepTests/65x02`) corpora
are **independently derived** — not generated from MAME. That independence is why
they function as genuine oracles and have caught real MAME bugs (two z80
WZ/MEMPTR inaccuracies, now fixed; three 6502 unstable/illegal-opcode
disagreements surfaced for the maintainer). For these cores **strict state + cycle
equality is meaningful and required**, and the `manifest.json` provenance block
records `derived_from: "independent"`.

The **m68000** corpus (`SingleStepTests/m68000`) is **MAME-generated** — its own
upstream README states it was *"Generated using the microcoded core in MAME"* and
*"Any bugs that exist in MAME's microcoded M68000 emulator will exist here too."*
It is a **2024-08-01 snapshot of the very core under test** (`m68000.cpp`), not an
independent reference. Per [ADR 0006](../../docs/improvement-plan/adr/0006-m68000-oracle-gate-definition.md),
the **in-tree interpreter (`-drc 0`) is the m68000 behavioral authority** and the
corpus is demoted to a high-volume conformance/regression *probe*. The manifest
records `derived_from: "MAME microcoded m68000 core"`, `not_independent: true`, and
the upstream STATUS note. We never edit the interpreter to match the corpus.

## m68000 PC adapter (`m_au`)

The m68000 corpus encodes the PC field from MAME's **`m_au`** ("next prefetch
address", = instruction start + 4), **not** from `STATE_GENPC`, which exports
`m_pc` (= start + 2). `m68000.cpp` confirms it: `state_import` sets `m_pc = m_ipc+2`
and `m_au = m_ipc+4` on a PC write. The harness therefore writes
`GENPC = corpus_pc - 4` going in (so `m_au == corpus_pc` on entry) and reads the
retired PC straight from `m_au` coming out (`oracle_retired_pc()`), captured at the
instruction's retirement before the next grant advances it. This is a **pure
harness-side read-back adapter** — `m_au` is a protected member reachable from the
`oracle_m68000_device` subclass; **no shared-core change**. The same subclass also
calls `update_user_super()` after applying SR (to re-sync the active-A7 banking the
state interface doesn't) and snapshots state to model away the deferred trace
exception — all harness-side, all reading protected members, zero interpreter
behavior change.

## m68000 frozen cycle-divergence allowlist

Single-sourced in `cpuoracle.cpp` (`k_m68000_cycle_allowlist_files` + the
`addr_error` case marker), mirroring the m6502 leg's evidence-based skip list.
Each entry carries an upstream-provenance rationale; the list is frozen — additions
require a citation in review. **State is never allowlisted; only the cycle
comparison is exempted.**

| Entry | Granularity | Provenance rationale |
|---|---|---|
| `TAS.json` | file | Upstream STATUS: TAS *"doesn't properly handle the special 5-cycle TAS read-modify-write timing"*; the corpus `length` omits it. |
| `TRAPV.json` | file | Upstream STATUS: TRAPV *"appears to trigger incorrectly based on the S bit"* during corpus generation. |
| address-error cases | case (`addr_error` marker) | Cases whose transaction log carried a read/write address-error cycle (`re`/`we`); AS isn't asserted and results aren't committed upstream, so the corpus cycle count isn't comparable. The fetcher (decoder v2) emits the marker. |

## Status (m68000 Leg A — a high-coverage probe)

The harness reaches **~99.6% state / ~99.3% cycle** equality, and the
`[cpu][m68000]` gate is **GREEN**. Per the owner-ratified ADR 0006 reframing, Leg A
is a **high-coverage conformance PROBE**, not a strict 100%-state gate: it
hard-`REQUIRE`s strict STATE on every case **outside** a corpus-data-keyed
deferred-exception residual, strict CYCLE on every non-allowlisted such case, and
**asserts the count of out-of-residual divergences `== 0`** — so the residual cannot
hide a regression. Divergences **inside** the residual are reported (the probe stays
green). `CPUORACLE_M68_STRICT=1` hard-`REQUIRE`s the residual too (for investigating
a corpus re-pin) and fails on it by design.

**The residual is characterised, owner-accepted, and contains no core bug:**

- **Inconsistent deferred-trace/exception capture** (the dominant class): the
  MAME-generated corpus snapshots state *before* the trace for most opcodes but
  *after* it for the exception-taking subset (taken branches; ILLEGAL/TRAP/CHK/RTE/
  MOVEtoSR; address-error pops). Signature: initial `SR.T` set, OR TAS/TRAPV, OR an
  address-error case.
- **Branch-self-loop** (~20 cases): a `BSR -2` / `Bcc -2` branches onto itself, which
  the harness single-step (retire on `m_ipc` change) re-executes. BSR/Bcc are correct
  (the corpus runs them once). Signature: BSR/Bcc with `|final.pc − initial.pc| ≤ 4`.

Two earlier suspected limitations were **fixed**, harness-side, zero core change:
**MOVEP** byte-lane (a cross-case stale-RAM gap → per-case RAM scrub) and the
**`(A7)`/auto-inc-dec/ABCD/ADDX** class (a single-step **over-run** where the grant
that advances `m_ipc` ran the next instruction's first write → snapshot the watched
final-RAM cells at the pre-grant retirement point). See the residual notes in
`cpuoracle.cpp` and ADR 0006. (Phase-2's load-bearing guarantee is **Leg B**,
interpreter ≡ DRC, corpus-immune and unaffected by the ~99% Leg-A bar.)
