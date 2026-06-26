# MAME Improvement Program

Planning artifacts for five approved improvements to this MAME checkout, plus a
cross-cutting mandate to broadly improve test coverage. **Planning only** — no MAME
source is changed by these documents; they feed the Planner, who writes the phase
plans, and then a later implementation program.

- **Design spec (source of truth):**
  [`specs/2026-06-24-mame-improvements-design.md`](specs/2026-06-24-mame-improvements-design.md)
- **Architecture overview:** [`../ARCHITECTURE.md`](../ARCHITECTURE.md)
- **Build/validate conventions:** [`../../CLAUDE.md`](../../CLAUDE.md)

## ADR index

| ADR | Title | Phase | Depends on |
|---|---|---|---|
| [0001](adr/0001-differential-cpu-oracle.md) | Differential CPU execution oracle (+ CI test gating) | **P1** | — |
| [0002](adr/0002-m68000-drcuml-port.md) | m68000 → DRCUML dynamic-recompiler port | **P2** | **0001** (hard gate) |
| [0003](adr/0003-frontend-usability-quick-wins.md) | Front-end usability quick wins | **P1** | — |
| [0004](adr/0004-web-control-surface.md) | Web control surface (REST + SPA over the HTTP server) | **P3** | — |
| [0005](adr/0005-build-and-driverlist-friction.md) | Build & driver-list friction reduction | **P1** | — |
| [0006](adr/0006-m68000-oracle-gate-definition.md) | m68000 differential-oracle gate definition (authority / pinning / acceptance) | **P1** | **0001** (refines); consumed by **0002** |

## Phase map (dependency-ordered)

```
P1 — Enablers + quick wins (mutually independent)
     ├── 0001  CPU oracle  ──────────────┐  (safety net)
     ├── 0003  Front-end quick wins      │
     └── 0005  Build / list friction     │
                                          ▼
P2 — m68000 DRC
     └── 0002  m68000 → DRCUML  ── gated on 0001's oracle, gate DEFINED by 0006
                                    (Leg B: interpreter ≡ DRC, cycle-exact, corpus-drift-immune)

P3 — Web control surface
     └── 0004  REST + SPA  ── independent of 0002; sequenced last (largest new surface)
```

- **P1** lands the three independent, low-risk items. 0001 is the behavioral safety
  net that P2 requires; 0003 and 0005 are standalone wins. The cross-cutting CI test
  gating (`mametests` + `srcclean`, today run by neither) lands with 0001.
- **P2** is the highest-effort/highest-risk pick and **cannot start until 0001's
  m68000 oracle is green** — that gate is what makes a cycle-accurate DRC reviewable.
  **[0006](adr/0006-m68000-oracle-gate-definition.md) defines exactly what "green" means**
  for m68000: the corpus is MAME-derived (not an independent oracle), so the gate is two legs —
  Leg A (interpreter vs corpus: 100% state, cycle-except-a-provenance-allowlist) and Leg B
  (interpreter ≡ DRC: full cycle equality, corpus-drift-immune — the part P2 actually needs).
- **P3** is a larger net-new surface, independent of P2, sequenced last; it reuses the
  audit/config/infoxml data feeds that 0003 also surfaces.

Each phase is independently shippable and independently testable. The Planner writes
the per-phase task plans (`plan/phase-{1,2,3}-*.md`) from these ADRs.

## Phase plans

| Plan | Picks / ADRs | Tasks | PR boundaries | Headline gate |
|---|---|---|---|---|
| [Phase 1 — Enablers & quick wins](plan/phase-1-enablers-and-quick-wins.md) ✅ **Complete** | 1, 3, 5 ([0001](adr/0001-differential-cpu-oracle.md)/[0003](adr/0003-frontend-usability-quick-wins.md)/[0005](adr/0005-build-and-driverlist-friction.md)) | 25 (incl. Task 0: `pacman -S make`) | A–J ✅ | z80+m6502+m68000 oracle; `mametests`+`srcclean` in CI |
| [Phase 2 — m68000 → DRCUML](plan/phase-2-m68000-drc.md) | 2 ([0002](adr/0002-m68000-drcuml-port.md)) | 10 | K–O | m68000 oracle green per **[0006](adr/0006-m68000-oracle-gate-definition.md)** (Leg A + Leg B) + native-coverage + throughput bar |
| [Phase 3 — Web control surface](plan/phase-3-web-control-surface.md) | 4 ([0004](adr/0004-web-control-surface.md)) | 11 | P–T | read-write `/api/v1` behind localhost+traversal+body-read+auth |

- **Phase 1 is complete** (all boundaries A–J merged). It started with installing GNU
  `make` (Task 0), produced the m68000 oracle that **Phase 2 hard-gates on**, and wired the
  first CI test gates (`mametests` + `srcclean`, boundary D — neither ran in CI before). The
  CPU oracle is **ratcheted per-CPU** (strict state+cycle equality for the independent
  z80/m6502 corpora; the MAME-derived m68000 corpus is a Leg-A probe per ADR 0006). Only
  Phases 2 and 3 remain.
- **Phase 2** is the first DRC increment: **native UML for a defined common-path opcode
  set** (not infra-only), an explicit **throughput acceptance bar**, oracle-gated
  cycle-for-cycle, plain 68000 only, rare/exception opcodes `cfunc_` to the interpreter.
  It cannot begin until Phase 1's m68000 oracle is green at strict cycle equality.
- **Phase 3** is **read-write v1**: the `http.cpp:185` body-read fix, a token auth model,
  static-handler traversal hardening, and localhost-default bind are all first-class v1
  tasks. Independent of Phase 2.

## Status

| ADR | Status | Open questions | Notes |
|---|---|---|---|
| 0001 | **Done** | 5 | Catch2 harness in `mametests`; SingleStepTests corpus fetched (not vendored) via pinned/hashed manifest. **All legs shipped** — z80 + m6502 (strict state+cycle), m68000 Leg-A probe per **[0006](adr/0006-m68000-oracle-gate-definition.md)**. **Boundary D landed:** all three CI legs build `TESTS=1`, fetch the corpus, and run `./mametests` (Linux = canonical full-corpus gate incl. full m68000; mac/win run a documented `CPUORACLE_MAX_FILES=64` subset); new `srcclean.yml` gates changed `src/**` files. First time `mametests` + `srcclean` run in CI. |
| 0002 | Proposed | 5 | Targets the **new microcode core** (`m68000.cpp`), not legacy Musashi. Increment 1 = plain 68000 common path; rest `cfunc_` to interpreter. Oracle-gated. |
| 0003 | Done | 4 | Pure UX; surfaces already-present data. All copy via the `_()` i18n macro. Golden-output CLI tests. **All three groups shipped** — A (CLI discoverability, T10–T12 / PR boundary E): `clihelp.{cpp,h}` + golden `[cli]` tests. B (actionable ROM/audit errors, T13–T14 / PR boundary F): `romload_messages.{cpp,h}`; missing-ROM error names the system and points at `-verifyroms` / the Audit Media menu, which now logs per-ROM audit detail. C (internal-UI guidance, T15–T18 / PR boundary G): full-selector empty-state box, slot-menu device descriptions + reboot notice, and an explicit set/append input hint. Internal-UI items (C1–C4, B2) require manual UI smoke per the plan. |
| 0004 | Proposed | 5 | HTTP server is **already frontend-owned** (`machine_manager`) and created before any machine — launcher endpoints just need frontend-scoped registration. Read-only first; localhost-default + traversal hardening required. |
| 0005 | **Done** | 5 | Tooling-only — **all three boundaries shipped.** H: source-staleness detector + `make check-sources` (PR #2). I: `reconcilelist --fix` autofix + byte-exact golden tests; CI runs check mode only (PR #6). J: fast `preflight` CI job (tiny build + `-validate` + reconcile check, gating the multi-hour legs) + tooling docs (`scripts/build/README-friction.md`). Additive/upstream-friendly; CI never runs `--fix`. Unit-tested. |
| 0006 | **Accepted** | 3 | Resolves the m68000 oracle-gate question. **Corpus is MAME-derived** (upstream: *"Generated using the microcoded core in MAME"*, 2024-08-01 snapshot) → **not** an independent oracle. Authority = the in-tree interpreter; corpus = a probe. Gate = **Leg A** (interpreter vs corpus: 100% state + cycle-except-allowlist {TAS, TRAPV, address-error}) + **Leg B** (interpreter ≡ DRC: full cycle equality, drift-immune — what P2 needs). Keeps the pin; adds a provenance-cited frozen allowlist. Blocker #1 (PC offset) = harness adapter (read PC from `m_au`). |

**Cross-cutting test mandate (all phases):** every ADR carries an explicit Testing &
validation section. P1 first wires `make TESTS=1 && ./mametests` and `srcclean` into
CI (neither runs today); subsequent work adds the CPU oracle, golden CLI tests, REST
handler/security tests, and reconciler/staleness tooling tests. "Done" means green
gates, not just compiling code.

## Verified build prerequisites

Per the design spec (verified 2026-06-24 on MSYS2 `MINGW64`): toolchain, Python, and
Git are present, but **GNU `make` is missing and is a hard build blocker** — install
with `pacman -S make` before any build/validate task. Iteration loop:
`make SOURCES=<file>` (or `SUBTARGET=tiny`), `make REGENIE=1` after adding/removing
source files, `./mame -validate` (primary gate), `make TESTS=1 && ./mametests` (unit
suite), `srcclean` (whitespace; built via `TOOLS=1`).
