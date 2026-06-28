# Phase 2 — m68000 → DRCUML port (Pick 2)

> **Status:** In progress — **PR boundaries K + L + M shipped** (Tasks 1–5) · **Consumes:** ADR [0002](../adr/0002-m68000-drcuml-port.md)
> **Hard gate:** ADR [0001](../adr/0001-differential-cpu-oracle.md) — the m68000 oracle
> **Spec:** [`../specs/2026-06-24-mame-improvements-design.md`](../specs/2026-06-24-mame-improvements-design.md)
> **Date:** 2026-06-24 · **Last updated:** 2026-06-27 (boundary M shipped — single resident-block DISPATCH + the first native opcode (moveq) via hybrid handoff; dual-leg cycle-exact on x64 + C backends. **The memory-EA half — the hot path — is now designed in [ADR 0007](../adr/0007-m68000-native-memory-ea-suspend-mechanism.md) (all 5 open questions resolved), and boundary O / Task 10's first batch (O-mem-1) is fully planned in [`phase-2-o-mem-1-btst-absolute.md`](phase-2-o-mem-1-btst-absolute.md) — ready for Builder.**)

## Goal

Add a third execution path to the **plain 68000** — a DRCUML dynamic recompiler — using
MAME's proven mips3/ppc dual-path template, with the **interpreter as the non-negotiable
oracle**. Per the resolved decision, **increment 1 is NATIVE-COVERAGE + THROUGHPUT-BAR**,
not infra-only:

- Increment 1 **emits native UML for a defined common-path opcode set** (ALU / MOVE /
  branch / Bcc / Scc / addq / subq with register and simple-EA addressing modes) — not
  merely the infrastructure with everything falling back.
- It includes a **benchmark methodology and a measurable throughput acceptance bar** (a
  named-driver speedup target) that gates "done," in addition to correctness.
- It remains **oracle-gated cycle-for-cycle**, targets **plain 68000 first**, and routes
  **rare/exception opcodes to the interpreter via `cfunc_`**.

> **Honesty note (from ADR 0002):** a fully native, cycle-accurate 68k DRC is a
> multi-month effort. This plan scopes the *first increment* (plain 68000, user-mode
> common path, native fast-path for the defined opcode set) and the extensible
> architecture, not full-family parity.

## ADR implemented

[0002](../adr/0002-m68000-drcuml-port.md) — the entire phase. Targets the **new microcode
core** (`src/devices/cpu/m68000/m68000.cpp`), not the legacy Musashi core.
[0007](../adr/0007-m68000-native-memory-ea-suspend-mechanism.md) — an **addendum to 0002**
that designs the native memory-EA + suspend/cycle/address-error mechanism (0002 §3 deferred all
memory-touching opcodes to `cfunc_`); **boundary O / Task 10 consumes it**.

## Prerequisites

1. **Phase 1 complete** — GNU `make` installed; the iteration loop verified.
2. **HARD GATE (0001 → 0002), defined by [ADR 0006](../adr/0006-m68000-oracle-gate-definition.md):**
   the m68000 oracle (`./mametests "[m68000]"`) must be green **before any DRC code is written**.
   Because the m68000 corpus is **MAME-derived** (not an independent oracle), ADR 0006 splits the
   gate into two legs: **Leg A** (interpreter vs corpus — 100% state + cycle-except-a-frozen-
   allowlist) lands in Phase 1, Task 5; **Leg B** (interpreter ≡ DRC — full register/flag/RAM/
   **cycle** equality, **corpus-drift-immune**) is the part *this* phase produces and that every
   DRC PR below is gated on. This gate is what makes a cycle-accurate port reviewable; without it
   the port is unreviewable and Phase 2 does not start. ADR 0002 §5 / ADR 0006 acceptance criterion
   3: *no 68k DRC change merges unless the oracle is green with `-drc 0` AND the same vectors are
   green with DRC enabled, register/flag/memory/cycle-exact between the two.*
3. The DRCUML core + all three backends are present and mature (`drcuml.cpp`,
   `drcbex64.cpp`, `drcbearm64.cpp`, `drcbec.cpp`) — no new backend is built.

## The hard cross-phase gate (restated, because it governs every task)

Every DRC-touching task below has the **same merge gate** — **[ADR 0006](../adr/0006-m68000-oracle-gate-definition.md)
acceptance criterion 3 (Leg B)**: `make TESTS=1 && ./mametests "[m68000]"` must be green **both**
with `-drc 0` (oracle reference) **and** with DRC enabled (interpreter ≡ DRC), asserting register,
flag, memory, **and cycle** equality between the two MAME runs **across the full corpus, allowlist
included** (allowlisted opcodes `cfunc_` to the interpreter, so they pass by construction). This leg
is **corpus-drift-immune** — it compares interpreter-output to DRC-output, never the stale corpus.
We **never** modify the interpreter to match the DRC — divergence is always a DRC bug, routed to
`cfunc_`. (Leg A — interpreter vs corpus — is the Phase-1 deliverable and is not re-gated per DRC
PR, but `./mame -validate` and the interpreter leg must remain green.)

## Conventions

- New files (`m68000fe.{cpp,h}`, `m68000drc.cpp`, benchmark harness) carry
  `// license:BSD-3-Clause` / `// copyright-holders:<name>`.
- **New `.cpp` files require `make REGENIE=1`** and registration in
  `scripts/src/cpu.lua` (the M680X0 `files {}` block, ~:2116-2143).
- Match the existing m68000 brace/whitespace style; run `srcclean` on touched files.
- Iteration build: **build the m68000 core as part of the `mametests` (`TESTS=1`) target**
  and run the oracle — `make REGENIE=1 && make TESTS=1 && ./mametests "[m68000]"`. Do **not**
  use `make SOURCES=src/devices/cpu/m68000/m68000.cpp`: `SOURCES` filters by *driver*, and a
  CPU device has no drivers, so GENie errors. (On Windows/MSYS2 the verified recipe is
  `MSYSTEM=MINGW64 bash -lc 'export OS=Windows_NT; cd <worktree>; mingw32-make REGENIE=1 &&
  mingw32-make TESTS=1 -j32'` — the makefile needs both `MSYSTEM=MINGW64` and `OS=Windows_NT`.)
  No `mame.lst` change (no new device/driver — the existing `m68000_device` gains a path).

---

## Task 1 — Verify the gate and pin the increment-1 opcode cut line ✅ DONE (PR boundary K)

- **Files:** none (gate verification) + `src/devices/cpu/m68000/README-drc.md` (new doc
  capturing the cut line and the cycle-adapter inherited from Phase 1).
- **Change:** Confirm `./mametests "[m68000]"` is green with strict state+cycle equality
  (the Phase-1 gate). **Record the increment-1 native opcode set** (resolved cut line:
  ALU / MOVE / branch / Bcc / Scc / addq / subq, register + simple-EA modes; everything
  else `cfunc_` to the interpreter) and the inherited cycle-adapter notes. No production
  code yet.
- **Test/Validation:** `make TESTS=1 && ./mametests "[m68000]"` is green on the host
  backend with cycle equality. **Green:** gate confirmed; if not green, **stop — Phase 2
  is blocked** (escalate to Phase 1, Task 5).

### Task 2 — Single-source the DRC decode descriptors (generator extension) ✅ DONE (PR boundary K)

- **Files (edit):** `src/devices/cpu/m68000/m68000gen.py` (124 KB), inputs `m68000.lst` /
  `m68k_in.lst`; **regenerate** the committed outputs (`m68000-decode.cpp`,
  `m68000-head.h`, `m68000-s{d,i}{f,p}.cpp`) plus a **new** generated descriptor table
  (e.g. `m68000-drcdesc.ipp`/`.h`).
- **Change:** Extend the generator to **also emit a DRC-facing descriptor table** (operand
  sizes, EA modes, register read/write sets, branch/flow flags, can-fault flag) so the DRC
  frontend reuses the **interpreter's decode truth** rather than re-deriving it (resolved
  recommendation: extend the generator — single source of truth). Document the
  regeneration workflow so the generated files stay single-sourced. The descriptor table
  is **committed** alongside the other generated files (the maintainer runs the generator;
  it is not a `custombuildtask`).
- **Test/Validation:** Re-run the generator; `git diff` on the existing generated files is
  empty except the **new** descriptor table (proves the extension is additive — no decode
  drift). Build the core: `make REGENIE=1 && make
  SOURCES=src/devices/cpu/m68000/m68000.cpp`. **The oracle must still be green** (decode
  unchanged): `./mametests "[m68000]"`. `./mame -validate` green. `srcclean`.

> **PR boundary K** (Tasks 1–2): generator extension + cut-line doc. **No behavior change
> yet** — the oracle proves decode is untouched. Reviewable in isolation.
> **✅ Shipped.** `m68000gen.py` now emits `m68000-drcdesc.ipp` (1527 descriptor rows);
> regenerating the existing committed decode files (`m68000-decode.cpp`, `m68000-head.h`,
> `m68000-s{d,i}{f,p}.cpp`) leaves them **byte-identical** (empty `git diff`, additive-only).
> An `enum_str()` shim keeps that byte-identity on Python ≥ 3.11. The m68000 oracle
> (`mametests "[m68000]"`) is **green** (317,885 assertions). Cut line captured in
> `src/devices/cpu/m68000/README-drc.md`. **Boundary L is next.**

### Task 3 — DRC frontend skeleton (`m68000fe.{cpp,h}`) — block walk, no UML emit ✅ DONE (PR boundary L)

- **Files (new):** `src/devices/cpu/m68000/m68000fe.cpp`, `m68000fe.h`.
- **Files (edit):** `scripts/src/cpu.lua` (M680X0 `files {}` block ~:2116-2143).
- **Change:** Implement the **decode frontend** mirroring `mips3fe.cpp`/`ppcfe.cpp`: walk a
  basic block from a start PC, decode each opcode into an `opcode_desc` (operand sizes, EA
  modes, register read/write sets, branch/flow info, can-fault) **using the generated
  descriptor table from Task 2**. No UML emission yet — this task produces the descriptor
  stream the translator (Task 5) consumes, and a unit/debug dump path.
- **Test/Validation:** `make REGENIE=1 && make
  SOURCES=src/devices/cpu/m68000/m68000.cpp`. Add a frontend descriptor-dump assertion to
  the oracle harness (compile-only path is fine for this task) so the walked block matches
  the expected opcode boundaries on a small fixture. **The interpreter oracle is untouched
  and stays green** (`./mametests "[m68000]"`). `./mame -validate` green. `srcclean`.

### Task 4 — DRC device state + `execute_run` branch (still interpreting) ✅ DONE (PR boundary L)

- **Files (edit):** `src/devices/cpu/m68000/m68000.cpp` (add the `m_isdrc` top-level branch
  in `execute_run()` at :147; the existing microcode loop becomes the interpreter arm),
  `m68kcommon.h`/`m68000.h` (add DRC state: `m_drcuml`, `m_drcfe`, `m_drc_cache`,
  `m_drcoptions`, `m_cache_dirty`), `device_start()` (allocate UML state, register debugger
  symbols — pattern: `src/devices/cpu/mips/mips3.cpp:179-183,447-460`; latch `m_isdrc =
  allow_drc()` per `:398`).
- **Change:** Wire the DRC scaffolding so that with DRC enabled, `execute_run()` enters a
  DRC dispatcher that — for this task — **immediately `cfunc_`s every opcode back to the
  interpreter** (a 100%-fallback dispatcher). This proves the dual-path plumbing,
  block-cache lifecycle, and `m_isdrc` latch **without yet emitting any native UML**, so
  the oracle's interpreter≡DRC leg can light up against a known-safe baseline.
- **Test/Validation:** Build the core. Run the oracle **both legs**: `./mametests
  "[m68000]"` with `-drc 0` and with DRC enabled must both be green (DRC = full fallback =
  identical to interpreter, including cycles). `./mame -validate` green. `srcclean`.
  **Green:** interpreter ≡ DRC with the full-fallback dispatcher — the dual-path
  invariant is now machine-checked.

> **PR boundary L** (Tasks 3–4): frontend skeleton + dual-path plumbing with 100% `cfunc_`
> fallback. **This is the riskiest plumbing PR** — it activates the interpreter≡DRC oracle
> leg with zero native code, so any future native opcode regresses against a green
> baseline. Validate on all three backends here (Task 8 matrix) before merging.
> **✅ Shipped.** New `m68000fe.{cpp,h}` decode frontend consumes the boundary-K descriptor
> table (`s_drc_desc_table[]`) — no UML emit. `m68000.cpp` gains the dual-path
> `execute_run()` (`if (m_isdrc) execute_run_drc(); else execute_run_interpreter();`); the
> interpreter arm is the original microcode loop extracted **byte-for-byte verbatim**. The
> DRC arm is a **100% `cfunc_` dispatcher**: the compiled entry block does nothing but
> `UML_CALLC` a C function that runs the interpreter for the granted quantum, then
> `UML_EXIT(EXECUTE_OUT_OF_CYCLES)` — no native opcode emission, no register/EA mapping
> (`code_compile_block` is dormant; it exercises the frontend on a real-but-unreached path).
> `m_isdrc` is latched `allow_drc() && type()==M68000` (scoped to plain 68000; the oracle
> device opts in). The DRC cache/UML state is allocated **only** for DRC-capable types
> (m68008/MCU variants do not allocate). **Oracle Leg B is wired and green:** a new
> `[cpu][m68000][drc]` case runs the full corpus through interpreter **and** DRC and asserts
> register/flag/RAM/**cycle** equality across **317,500 cases** on **drcbex64 (x64)** and
> **drcbec (C, via `CPUORACLE_M68_DRC_C=1` → `OPTION_DRC_USE_C`)** — non-vacuously (it
> `REQUIRE`s the DRC actually engaged and that cycles are compared). Leg A unchanged
> (317,885 assertions). **arm64 (drcbearm64) deferred to the appserver/CI matrix.**
> Invocation: `./mametests "[m68000]"` (Leg A) · `./mametests "[m68000][drc]"` (Leg B x64) ·
> `CPUORACLE_M68_DRC_C=1 ./mametests "[m68000][drc]"` (Leg B C backend). **Boundary M is next.**

### Task 5 — `m68000drc.cpp`: emit native UML for the common-path opcode set ✅ DONE — first native opcode (PR boundary M)

- **Files (new):** `src/devices/cpu/m68000/m68000drc.cpp`.
- **Files (edit):** `scripts/src/cpu.lua` (register the new file); `m68000.cpp` /
  `m68000.h` (DRC compile hooks: `code_compile_block`, `cfunc_*` fallbacks).
- **Change:** Implement the **UML translator + dispatcher** (pattern:
  `mips3drc.cpp`/`ppcdrc.cpp`). For each opcode **in the resolved increment-1 set** —
  ALU / MOVE / branch / Bcc / Scc / addq / subq, register + simple-EA modes — emit a
  native `drcuml` instruction sequence with per-instruction **cycle cost taken from the
  same microcode-list source the interpreter uses** (never re-estimated), decremented into
  the UML block. **Everything outside the set, and any faulting / privileged / prefetch-
  visible-timing / FPU / MMU / mid-instruction-suspend case, routes to `cfunc_`** that
  calls the interpreter handler. Add the exception/trap entry points and block-cache
  management. This is the increment that makes the DRC **natively cover** the common path
  (the resolved native-coverage bar), not just plumb fallbacks.
- **Test/Validation:** **The hard gate** — `make TESTS=1 && ./mametests "[m68000]"` green
  with `-drc 0` AND with DRC enabled, asserting register/flag/memory/**cycle** equality on
  the native opcodes. `./mame -validate` green. `make SOURCES=...` builds clean. `srcclean`.
  **Green:** every native-emitted opcode matches the interpreter cycle-for-cycle; all
  others still `cfunc_` and match by construction.

> **PR boundary M** (Task 5): first native UML emission for the common-path set. **The
> headline increment.** Merge only on a green dual-leg oracle across the backend matrix
> (Task 8).
> **✅ Shipped.** Boundary M lands two things: (1) a **native DISPATCH via a single resident
> UML block** — `m68000drc.cpp`'s entry block decodes the current opword (`m_ird`) in-line
> and runs a native fast-path or delegates the granted quantum to the interpreter, replacing
> boundary L's single 100%-`cfunc_` block. It deliberately does **not** compile a block per
> PC / HASHJMP on `m_ipc`: the corpus scatters ~310 k distinct PCs, so per-PC compilation
> overran the 8 MiB code cache and forced flushes that were fragile across UML backends (it
> failed the appserver Linux/SysV `oracle` job — a flush-time `emu_fatalerror` escaped and
> was swallowed by `running_machine::run()` — while passing on Windows, which had cache
> headroom), and per-PC blocks went stale when the harness rewrote program RAM per case. A
> single resident block that decodes `m_ird` at runtime has neither problem. The native
> fast-path runs ONLY at a genuine instruction-fetch boundary, gated by three guards
> (`m_inst_substate==0`; `m_ipc==m_pc-2` — the guard against a prior multi-state opcode's
> retirement grant, where the next opword is already prefetched into `m_ird`;
> `m_inst_state==m_decode_table[m_ird]` — not mid multi-state). And (2) the **first native
> opcode — `moveq`** (decoded from `m_ird` at runtime) — emitted as native UML.
> `moveq`'s native path uses a **hybrid handoff**: it emits the interpreter microcode's
> CASE 0 natively (the architectural artifact — the `Dn` write, the CCR `N/Z` with `V=C=0`
> and `X/I/S/T` preserved, and the prefetch-pipe pointer advance), then sets
> `m_inst_substate=1` and hands the timing tail
> (CASE 1+2: the interruptible prefetch, the `m_icount-=4` **cycle charge taken from the
> interpreter, never re-estimated**, the suspend/payback bookkeeping, and the decode-table
> dispatch) to the **unchanged interpreter** via the quantum `cfunc_`. This keeps the
> 68000's prefetch/bus-timing model in the one place that owns it while making `moveq`'s
> architectural effect native, and is **cycle-exact by construction** (the interpreter
> resumes at substate 1 without re-running CASE 0). Everything except `moveq` still
> `cfunc_`s, so it stays exact by construction. **Two dispatch bugs** the boundary-L
> dormant path never exercised were found and fixed under the full corpus: the per-PC hash
> must be registered at *exactly* the requested PC (the 68000 prefetch PC adapter means the
> frontend's sequence-head PC differs — a wrong hash spun MISSING_CODE forever), and the
> static handlers must be **transient** (`begin_block`), not invariant — regenerating
> invariant blocks per flush leaked the cache's small permanent area until `reset()` threw
> "Out of cache space" the first time the 8 MiB cache filled (~52 k cases in). **Gate (all
> green):** Leg A unchanged (317,885 assertions, interpreter byte-for-byte unchanged); Leg B
> (interpreter ≡ DRC, register/flag/RAM/**cycle** exact) **15,722,462 assertions** across the
> full 128-fixture corpus on **drcbex64 (x64)** *and* **drcbec (C backend)**; `mametiny
> -validate` clean (m68000drc links in the tiny build); z80/m6502 oracles green. **arm64
> deferred to the appserver/CI matrix.** The **rest of the increment-1 in-set**
> (ALU/MOVE/branch/Bcc/Scc/addq/subq) widens opcode-by-opcode in **boundary O** (Task 10),
> each step gated by Leg B, on top of this dispatch. **Boundary N (Tasks 6–7) is next** —
> the native-coverage assertion + the throughput bar.

### Task 6 — Native-coverage acceptance: measure & assert the fallback ratio

- **Files (new):** `tests/emu/cpu/m68000_drc_coverage.cpp` (Catch2, tagged
  `[cpu][m68000][drc]`).
- **Files (edit):** `scripts/src/tests.lua` (`files {}`).
- **Change:** A test that runs the increment-1 corpus subset through the DRC path and
  asserts that the **defined common-path opcode set is emitted natively** (not `cfunc_`),
  e.g. by instrumenting the dispatcher to count native-vs-fallback dispatches and asserting
  the native set is covered. This operationalizes the resolved **native-coverage bar** —
  "increment 1 emits native UML for a defined common-path opcode set," machine-checked
  rather than asserted in prose.
- **Test/Validation:** `./mametests "[m68000][drc]"` green: every opcode in the increment-1
  set dispatches native; the fallback set is exactly the documented remainder. **Green:**
  coverage assertion passes.

### Task 7 — Throughput benchmark harness + acceptance bar

- **Files (new):** `tests/cpuoracle/bench_m68000.py` (or a `mametests` benchmark case) +
  `docs/improvement-plan/plan/bench-methodology-m68000.md`.
- **Change:** Define and implement the **benchmark methodology** (resolved requirement):
  measure throughput of a named, representative plain-68000 workload — a Sega System 16
  driver (or the oracle corpus replayed at scale) — under `-drc 0` vs `-drc 1`, on the host
  backend, reporting host-MHz / wall-clock with a fixed seed and warm cache, averaged over
  N runs. Document the host, build flags, and the **measurable throughput acceptance bar**
  (a concrete speedup target on the named driver for the native opcode mix — e.g. "≥ X×
  interpreter on driver D for the common-path workload"; the owner sets X from a first
  baseline run). The bar gates "done" for increment 1 **in addition to** correctness.
- **Test/Validation:** Run the benchmark on a clean build; it emits a stable speedup number
  on repeated runs (variance within a documented tolerance). **Green:** the measured
  speedup meets or exceeds the acceptance bar on the named driver; if it does not, the
  increment is not "done" (widen native coverage / investigate UML quality before
  shipping).

> **PR boundary N** (Tasks 6–7): native-coverage assertion + throughput harness/bar. These
> codify the resolved acceptance criteria; land them with or immediately after Task 5 so
> "done" is measurable.

### Task 8 — Backend matrix validation (x64 / C / arm64)

- **Files:** none new (CI/local matrix) — optionally `.github/workflows/` if a DRC matrix
  leg is added.
- **Change:** Run the full oracle dual-leg against **`drcbex64`** (host x64), **`drcbec`**
  (forced via `-drc_use_c 1` / `OPTION_DRC_USE_C`), and — where an arm64 host/runner is
  available — **`drcbearm64`**, so the port is correct on all three UML backends, not just
  the host's native one.
- **Test/Validation:** `./mametests "[m68000]"` green with DRC enabled on each backend
  (`-drc_use_c 1` for the C backend; native where available). **Green:** all available
  backends pass the dual-leg oracle.

### Task 9 — Spot integration smoke (belt-and-suspenders)

- **Files:** none (runtime check) — document in the PR.
- **Change:** Boot a handful of high-value plain-68000 games (e.g. a Sega System 16
  driver) under `-drc 1` and `-drc 0` and confirm identical behavior, as an integration
  check in addition to the oracle. Resolved: correctness gates merge; this is a
  belt-and-suspenders step before each coverage widening.
- **Test/Validation:** `make SOURCES=<sega system16 driver>.cpp` builds; the game boots
  identically under both `-drc` settings (no visual/audio/timing divergence in a short
  scripted run). `./mame -validate` green. **Green:** identical behavior both ways.

### Task 10 — Widen native coverage (follow-up increments, oracle-gated each step)

> **DEPENDS ON [ADR 0007](../adr/0007-m68000-native-memory-ea-suspend-mechanism.md).** The
> register-only opcodes (boundary M's hybrid-handoff family) cover < 44% of dynamic cycles and
> **cannot** reach the throughput bar; the real hot path is memory-addressing (aurail's
> `btst #n,(xxx).W` = 43% of cycles). ADR 0007 designs the load-bearing mechanism that boundary O
> needs: a native bus-access primitive (`generate_bus_step()`) that issues `UML_READ`/`UML_WRITE`
> against `SPACE_PROGRAM` and reproduces the interpreter's **per-bus-cycle** cycle charge, two-way
> suspend checkpoint (`m_icount<=0 && access_to_be_redone()` → refund-and-replay vs.
> keep-and-advance, setting `m_inst_substate`), and `m_aob&1 → S_ADDRESS_ERROR` fault branch — with
> cycle constants and substate numbers **single-sourced from the generator**, never re-estimated.
> The boundary-M hybrid handoff does **not** generalize to memory-EA opcodes (their hot work *is* the
> bus read, which the handoff would hand back to the interpreter — leaving it `cfunc_`).

- **Files (edit):** `m68000drc.cpp`, `m68000fe.cpp` (incrementally move opcodes from
  `cfunc_` to native emission, reusing `generate_bus_step()` from ADR 0007); `m68000gen.py` +
  `m68000-drcdesc.ipp` (extend the descriptor table with the per-opcode bus-step list — access
  kind/size/byte-lane, `−N` charge, redo/completed substate pair — additive-only, empty diff on the
  existing generated files per Task 2 / R3).
- **Change:** Each follow-up shrinks the `cfunc_` fallback set by emitting native UML for
  more opcodes, **re-validated by the oracle every step**. **ADR 0007's ordered batch plan:**
  - **O-mem-1** (the mechanism PR — lands first and alone): `generate_bus_step()` infra +
    **`btst #n,(xxx).W/.L`** (the profiled hot opcode). Reviewed in isolation against Leg B on the
    full backend matrix before any reuse. **PLANNED — full task arc in
    [`phase-2-o-mem-1-btst-absolute.md`](phase-2-o-mem-1-btst-absolute.md)** (5 tasks: generator
    bus-step descriptor → redo-flag cfunc → `generate_bus_step()` emitter → wire btst-absolute →
    coverage assertion + Linux `oracle` gate). ADR 0007's 5 open questions are resolved/locked.
    **Ready for Builder.**
  - **O-mem-2:** `btst`/`bchg`/`bclr`/`bset` with `(An)`/`(An)+`/`-(An)`, **both `#imm8` and `Dn`
    source (24 forms)** — adds the write checkpoint / RMW path. **FULLY DESIGNED & OWNER-DECIDED** in
    the [ADR 0007 O-mem-2 addendum](../adr/0007-m68000-native-memory-ea-suspend-mechanism.md#addendum--resolution-2026-06-28--o-mem-2-write-side-mechanism)
    (write-side `generate_bus_step()` via `UML_WRITEM`; auto-inc/dec EA arithmetic incl. the A7-byte-by-2
    rule; `Dn`/`#imm8` source forms; OQ-6…OQ-9 all resolved). **Ready for Planner — no open questions.**
    Key decided items the plan must encode: **Task 0a** — a mandatory fully-granted Leg-B oracle pass
    (grant `length` then drain at 1; the 1-cycle pass never reaches the native write, and this also
    retroactively gates O-mem-1's native tail — divergences there are in-scope); **Task 0** — the
    `AS_OPCODES` differential-oracle config; and the `set_current_mmu`/`enable_mmu → m_cache_dirty`
    safeguard + regen test.
  - **O-mem-3:** memory-EA `MOVE`/`MOVEA` (`.b`/`.w`/`.l`) — the broadest coverage jump.
  - **O-mem-4:** memory-EA `ALU` (`add`/`sub`/`and`/`or`/`eor`/`cmp` + immediate forms).
  - **O-mem-5+:** `(d16,PC)` source, `addq`/`subq`/`Scc`/`clr`/`tst`/`neg`/`not` memory-EA forms.

  O-mem-2…5 are independent given O-mem-1 and ordered by profiled cycle weight. Still out of scope:
  m68010 (minor delta — base 68000 only for P2), m68020/030/040 + FPU + MMU (separate, much larger
  ADR addenda), ColdFire / scc68070 / fscpu32 / MCU variants, indexed `(d8,An,Xn)` / `movem` /
  `movep` / `tas` RMW (timing-subtle; later or permanent `cfunc_`).
- **Test/Validation:** Per step, the dual-leg oracle (`./mametests "[m68000]"` +
  `"[m68000][drc]"`, register/flag/memory/cycle) stays green; the Task 6 coverage assertion is
  updated to the new native set; the Task 7 benchmark is re-run (perf reported — expect the first
  real speedup at O-mem-1, since `btst`-absolute is now native). **Green:** monotonic
  native-coverage growth with the oracle green at every step.

> **PR boundary O** (Task 10, repeated): one PR per coverage-widening batch, each gated by
> the oracle and built on the ADR-0007 mechanism. These are post-increment-1 and may continue
> indefinitely.

---

## Suggested PR sequence (Phase 2)

| PR | Tasks | Theme | Gate |
|---|---|---|---|
| K ✅ | 1–2 | Gate check + generator descriptor extension | oracle green (decode unchanged) — **shipped (PR #22, boundary K)** |
| L ✅ | 3–4 | Frontend skeleton + dual-path plumbing (100% `cfunc_`) | dual-leg oracle (full fallback) — **shipped (boundary L): Leg B lit up, interpreter ≡ DRC cycle-exact on x64 (drcbex64) + C (drcbec); arm64 deferred to CI** |
| M ✅ | 5 | Native UML for the common-path opcode set | dual-leg oracle, cycle-exact — **shipped (boundary M): real per-PC native DISPATCH + first native opcode (moveq, hybrid handoff); Leg B 15.7M assertions cycle-exact on x64 (drcbex64) + C (drcbec); rest of the in-set widens in boundary O** |
| N | 6–7 | Native-coverage assertion + throughput bar | coverage + speedup ≥ bar |
| (matrix) | 8 | x64 / C / arm64 backends | dual-leg oracle per backend |
| (smoke) | 9 | Game-boot belt-and-suspenders | identical under `-drc 0/1` |
| O… | 10 | Coverage-widening follow-ups (memory-EA — [ADR 0007](../adr/0007-m68000-native-memory-ea-suspend-mechanism.md) mechanism; `btst`-absolute first, then memory-EA MOVE/ALU) | dual-leg oracle per batch |

Strictly sequential through M (each depends on the prior plumbing). N/8/9 gate the
increment-1 "done." **O depends on [ADR 0007](../adr/0007-m68000-native-memory-ea-suspend-mechanism.md)** (the native bus-access + suspend/cycle/address-error mechanism); O-mem-1 (the mechanism + `btst`-absolute) lands first and alone, then O-mem-2…5 widen. O repeats post-increment-1.

## Test deliverables (Phase 2)

- **Dual-leg m68000 oracle vectors** exercised on every DRC PR (interpreter ≡ DRC,
  register/flag/memory/**cycle**), across the **x64 / C / arm64 backend matrix** (Task 8).
- **Native-coverage assertion** (`tests/emu/cpu/m68000_drc_coverage.cpp`): the increment-1
  opcode set is emitted natively, not `cfunc_` (Task 6).
- **Throughput benchmark + acceptance bar** (`tests/cpuoracle/bench_m68000.py` +
  `bench-methodology-m68000.md`): measurable `-drc 1` vs `-drc 0` speedup on a named
  driver, gating "done" (Task 7).
- **Generator regeneration check:** additive descriptor emission with no decode drift
  (Task 2).
- **Spot integration smoke** on real 68000 drivers (Task 9).
- **`./mame -validate`** stays green throughout (the device still validates).

## Definition of Done (Phase 2, increment 1)

1. The 0001 → 0002 hard gate held at every step: m68000 oracle green with `-drc 0` AND DRC
   enabled, register/flag/memory/**cycle** exact.
2. The DRC **emits native UML for the defined common-path opcode set** (ALU/MOVE/branch/
   Bcc/Scc/addq/subq, register + simple-EA), machine-verified by the native-coverage
   assertion (Task 6) — not infra-only.
3. The **throughput acceptance bar is met** on the named 68000 driver under the documented
   benchmark methodology (Task 7).
4. Correct on the **x64, C, and (where available) arm64** UML backends (Task 8).
5. `./mame -validate` clean; no regression in a sampled set of 68000 drivers (Task 9);
   `srcclean` clean; decode truth single-sourced via the generator (Task 2).
6. The interpreter is **untouched** in behavior; all unmatched cases `cfunc_` to it.

## Risks & assumptions

- **R1 — the gate may not be reachable (retired by [ADR 0006](../adr/0006-m68000-oracle-gate-definition.md)).**
  Originally: if Phase 1, Task 5 cannot reach strict m68000 cycle equality vs the corpus, Phase 2
  cannot start. ADR 0006 established that strict equality vs the **MAME-derived** corpus was never
  the right gate — the cycle-exactness Phase 2 needs is **Leg B (interpreter ≡ DRC)**, which is
  **corpus-drift-immune** and does not depend on the corpus at all. Leg A is provenance-bounded by
  a frozen allowlist. Mitigation: Task 1 still verifies the (now well-defined) gate first and
  **stops** if red; but the prior "unreachable cycle equality" failure mode is removed.
- **R2 — 68k prefetch/bus-timing edge cases.** Genuinely hard; expect a long `cfunc_` tail
  that shrinks slowly (ADR 0002). Mitigation: increment 1 deliberately limits native scope
  to the non-faulting user-mode common path; everything timing-subtle stays `cfunc_`. The
  *memory-EA* bus timing (the dominant remaining cycle share) is addressed by
  [ADR 0007](../adr/0007-m68000-native-memory-ea-suspend-mechanism.md), which charges cycles
  **per bus step from the same generated microcode truth** the interpreter uses (never
  re-estimated) and reproduces the interpreter's exact suspend/fault checkpoints in UML —
  cycle-exactness enforced by construction-plus-Leg-B. The genuinely subtle remainder (indexed EA,
  `movem`/`movep`/`tas` RMW) stays `cfunc_`.
- **R3 — generator extension churns committed files.** A mis-scoped generator change could
  rewrite the existing generated decode files. Mitigation: Task 2 asserts an **empty diff**
  on existing generated outputs (additive-only), gated by the oracle.
- **R4 — throughput bar set too high/low.** The acceptance bar is owner-set from a first
  baseline (Task 7). Mitigation: report the baseline before fixing X; the bar is a target
  for the native opcode mix, not the full (still-`cfunc_`) program.
- **R5 — backend divergence (x64 vs C vs arm64).** A UML sequence may be correct on one
  backend and wrong on another. Mitigation: the Task 8 matrix runs the dual-leg oracle on
  each backend before increment-1 "done."
- **A1 — assumption:** the new microcode core's per-instruction cycle counts are the
  authoritative source the DRC mirrors; the DRC matches **the interpreter's** count (what
  ships), reconciled to the corpus via the Phase-1 cycle adapter.
- **A2 — assumption (resolved):** base 68000 only for P2; m68010 and 020+/FPU/MMU are
  out of scope (separate increments / ADR addenda).
