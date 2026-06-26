# MAME Improvement Program — Executive Summary

*Living status document. Last updated: 2026-06-25 · `main` @ `4aab9a94` (+ boundary D)*

> **Headline:** Phase 1 (enablers + quick wins) is **complete** — all boundaries A–J
> merged, and boundary D now runs `mametests` (the differential CPU oracle) + `srcclean`
> in CI for the first time on all three OS legs. **No runtime emulation speedup has
> shipped yet** — that is Phase 2 (m68000 DRC), which is designed but not built. What has
> shipped is the **correctness safety-net, UX, and developer-experience** work that makes
> the performance work safe to attempt. Only Phases 2 and 3 remain.

## 1. Program at a glance

A five-pick improvement program over this MAME checkout, structured in three
dependency-ordered phases.

| Phase | Pick / ADR | Theme | Status |
|---|---|---|---|
| **P1** | Pick 1 / ADR 0001 — Differential CPU oracle | Correctness safety net | **✅ Done** — z80 ✅ · m6502 ✅ · m68000 ✅ Leg-A probe ~99.6% state / ~99.3% cycle ([ADR 0006](adr/0006-m68000-oracle-gate-definition.md); Leg B = the Phase-2 gate) · **CI-wiring (boundary D) merged** (`mametests` + `srcclean` on all 3 legs; Linux runs the full corpus) |
| **P1** | Pick 3 / ADR 0003 — Front-end usability | End-user UX | **✅ Done** (PRs #3 / #4 / #5) |
| **P1** | Pick 5 / ADR 0005 — Build/list friction | Developer experience | **✅ Done** (PRs #2 / #6 / #7) |
| **P2** | Pick 2 / ADR 0002 — m68000 → DRCUML | **Runtime performance** | Designed, **not started** (hard-gated on the m68000 oracle) |
| **P3** | Pick 4 / ADR 0004 — Web control surface | New capability | Designed, not started |

**Phase 1 fully merged** (boundaries A–J): the fetcher (#1), the z80/m6502/m68000 oracle
legs (#8/#9/#14), the executive summary (#13), the front-end UX work (#3/#4/#5), the
build/list friction tooling (#2/#6/#7), and the boundary-D CI wiring (`mametests` +
`srcclean`) plus its prerequisite rgbutil `-Werror` fix.

## 2. What shipped — grouped by delivered value

### Correctness (accuracy) — shipped
- A **differential CPU oracle** in `mametests` that single-steps the interpreter
  against the canonical SingleStepTests corpus at **strict state + cycle equality**:
  **z80 (~45M assertions, 1,604 fixtures)** and **m6502 (~33M assertions)**, zero
  exclusions.
- It already **caught and fixed 2 real z80-core WZ/MEMPTR accuracy bugs** (block-I/O
  interrupt + `IN r,(C)` ordering) affecting every z80 driver's flag edge-cases, and
  **surfaced an m6502 `LAS` bug** (incorrect stub, flagged for fix). z80 is one of
  MAME's most-used CPUs, so these are broad accuracy wins.
- Net effect: a **permanent regression net** — future CPU-core edits are now provably
  cycle-exact or they fail CI.

### End-user UX — shipped (ADR 0003)
- **CLI discoverability:** usage hints on `-listslots` / `-listmedia`, "did you mean"
  suggestions on unknown options, an Examples block in `-showusage`.
- **Actionable errors:** missing-ROM failures now name the failing system and point at
  `-verifyroms` / the Audit Media menu (which now logs per-ROM detail instead of
  discarding it).
- **Internal-UI guidance:** full-selector empty-state, slot-menu device descriptions +
  reboot notice, explicit set/append input hint. All copy via the `_()` i18n macro;
  golden-output CLI tests.

### Developer experience — shipped (ADR 0005)
- **Source-staleness detector** (`make check-sources`) — catches the classic "forgot
  `make REGENIE=1`" drift before it wastes a build.
- **`reconcilelist --fix`** — autofixes a divergent driver list (byte-exact
  golden-tested; CI only ever runs the *check* form).
- **Fast CI pre-flight job** — tiny build + `-validate` + reconcile check (minutes)
  that fail-fast **gates the multi-hour build legs**.

## 3. Expected improvements

**Already realized (Phase 1):** more accurate z80 / m6502 emulation; clearer CLI +
error + menu UX; a faster, safer contributor loop. **None of these change runtime
emulation speed.**

**Prospective — the performance lever (Phase 2, ADR 0002):** the 680x0 family is
**interpreter-only and is "the single largest throughput lever in the tree that lacks
a DRC"** (used by a huge swath of systems — Genesis / Mega Drive, Sega System 16/18,
CPS, Neo-Geo-era boards, Amiga, classic Mac, and more). Adding a **DRCUML dynamic
recompiler** for the common-path opcode set — reusing MAME's mature JIT backends
(`drcbex64`, `drcbearm64`, `drcbec`) on the proven mips3 / ppc dual-path template — is
expected to yield a **multiplicative `-drc 1` vs `-drc 0` speedup on 68k-bound
workloads**. Deliberately, **no specific × is claimed yet** — ADR 0002's acceptance
bar is set empirically from a first baseline run (see §4).

## 4. Plan to demonstrate performance

Because the performance win is Phase 2, the demonstration is a **before/after,
correctness-locked benchmark** (methodology specified in Phase-2 Task 7):

1. **Baseline (runnable now, pre-DRC):** pick a representative plain-68000 driver (a
   **Sega System 16** title), `make SOURCES=<sys16>.cpp`, then measure throughput
   under **`-drc 0` (interpreter)** — host-MHz / wall-clock, fixed seed, warm cache,
   averaged over N runs. This is the "before" number **and** sets the acceptance bar X.
2. **After DRC lands:** same driver, same harness, **`-drc 1`** → report the speedup ×,
   per-driver.
3. **Prove the speedup is free:** the oracle's **Leg B (interpreter ≡ DRC)** replays
   the whole corpus through both paths and asserts identical register / flag / RAM /
   **cycle** results — so the speedup carries a **cycle-exact, zero-accuracy-regression
   guarantee**. (This is why [ADR 0006](adr/0006-m68000-oracle-gate-definition.md)
   matters: Leg B is corpus-drift-immune.)
4. **Gate "done" on it:** the increment ships only if measured speedup ≥ bar, with
   monotonic non-regression as native opcode coverage widens. Artifact:
   `bench_m68000.py` + `bench-methodology-m68000.md` (host, flags, seed, variance
   tolerance).

### Demonstrable *today* (Phase 1 value)
- **Correctness net:** run `mametests "[z80]"` / `"[m6502]"` green, and show the 2 z80
  fixes + the m6502 finding the oracle produced.
- **UX:** a full-build UAT pass — bring up MAME and walk the new CLI hints, ROM / audit
  errors, and menus (before / after).
- **DX:** time the fast pre-flight vs the full legs; show `make check-sources` flagging
  a stale set and `reconcilelist --fix` repairing a divergent list.

## 5. Open threads

- **m68000 oracle gate (Leg A) — closed.** [ADR 0006](adr/0006-m68000-oracle-gate-definition.md)
  accepted; the Builder close-out landed (the `m_au` PC adapter, the deferred-exception
  residual, the frozen divergence allowlist, strict `REQUIRE`s). Leg B (interpreter ≡ DRC)
  is the remaining **Phase-2** gate, not a Phase-1 thread.
- **Boundary D — closed.** `mametests` + `srcclean` are wired into all three CI legs (Linux
  runs the full corpus, the canonical gate). The prerequisite
  `tests/emu/video/rgbutil.cpp` `-Werror=volatile` failure under C++20/GCC 14.2 was fixed.
  **This closes Pick 1 and completes Phase 1.**
- **m6502 `LAS` fix** — oracle-surfaced core bug, queued (a Phase-2-adjacent core fix, not a
  Phase-1 blocker).
- **ADR 0003 UAT** — internal-UI items (C1–C4, B2) await a manual full-build smoke.
