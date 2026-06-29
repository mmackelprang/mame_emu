# O-mem-3a Implementation Plan — native MOVEA `(An)`/`(An)+`/`-(An)` → An (`.w`+`.l`), the long two-word read + register write-back opener

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the gentlest opener of the memory-EA `MOVE`/`MOVEA` arc — **MOVEA `(An)`/`(An)+`/`-(An)` → An in both `.w` and `.l` (6 forms)** — by adding one parameterized native emitter (`generate_movea_mem`) that issues the source-EA read(s) through the **already-frozen** `generate_bus_step()` read path, latches the high word for the long case, and writes the destination **address register** (`ext32` sign-extend for `.w`; `set_16h`+`set_16l` 32-bit assembly for `.l`). MOVEA writes a **register, not the bus**, so there is **no data-write step, no `generate_bus_step()` edit, and no flags**. Gated cycle-exact by the three existing oracle grant modes (1-cycle, full-grant, single-offset partial-grant) on x64 + C.

**Architecture:** O-mem-3a is the first sub-batch of **O-mem-3** (phase-2 Task 10 / boundary O, increment plan §6 row 3), pinned by the **[ADR 0007 O-mem-3 addendum](../adr/0007-m68000-native-memory-ea-suspend-mechanism.md#addendum--resolution-2026-06-28--o-mem-3-memory-ea-movemovea)** (sections M1–M6, the 5-sub-batch split, OQ-10). Per the addendum's mechanism-novelty-isolation rule, **3a introduces exactly one new mechanism: the long two-word READ + high-word latch (`m_alue`) + the register write-back.** It touches `generate_bus_step()` **not at all** (M1: the read branch already commits a full word when `byte_lane == 0`, and the address-error branch is already kind-agnostic) — 3a is a pure additive emitter + generator-descriptor extension over a frozen primitive, mirroring how O-mem-1 landed the mechanism alone. The word forms are 2 bus steps (data read → final prefetch); the long forms are 3 (read-high → read-low → final prefetch), each read `byte_lane = 0` with `has_addr_error = 1`. All per-step charges, substate pairs, and the `-(An)` predecrement internal `−2` are **single-sourced from `m68000gen.py`** (ADR R-A is the top risk; do not hand-transcribe).

**Planner scope resolution (`(d16,An)` isolated in 3e):** the ADR's 3a row originally listed `(d16,An)→An` (8 forms); the Planner moved MOVEA `(d16,An)→An` (`das`→`ad`, 2 forms) into **3e** to keep 3a proving only the long-read + latch + register write-back with zero `(d16,An)` EA-arithmetic novelty (the addendum's batch table and OQ-10 are updated accordingly). **3a is therefore 6 forms: MOVEA `(An)/(An)+/-(An) → An`, `.w`+`.l`.**

**Tech Stack:** C++17 (the `m68000_device` / DRCUML emitter, drcbe_x64 + drcbec backends), Python 3 (the `m68000gen.py` generator), GENie/`mingw32-make` build, Catch2 oracle harness (`tests/emu/cpu/cpuoracle.cpp` + `cpu_test_harness.{cpp,h}`). MSYS2 UCRT64/MINGW64 toolchain on Windows; appserver Linux for the authoritative `oracle` CI job.

> ⚠ **REQUIRED before any task — read the [ADR 0007 O-mem-3 addendum](../adr/0007-m68000-native-memory-ea-suspend-mechanism.md#addendum--resolution-2026-06-28--o-mem-3-memory-ea-movemovea) in full** (M1–M6, the pinned-vs-derive list, OQ-10). Every mechanism decision is owner-decided there; this plan implements those answers, it does not re-derive them. **Four facts gate everything 3a-specific:**
> 1. **No `generate_bus_step()` edit** (M1). MOVEA reads via the frozen read branch (`byte_lane == 0` full-word read, `has_addr_error == 1`) and writes a **register**, not the bus. If you find yourself editing `generate_bus_step()`, stop — that is 3b's job (the word data WRITE).
> 2. **MOVEA decodes `rx`/`ry` from `m_irdi`, not `m_ird`** (the `_df` handlers: `int rx = map_sp(((m_irdi >> 9) & 7) | 8); int ry = map_sp((m_irdi & 7) | 8);`). MOVEA is the **first** native opcode whose handler reads `m_irdi`, and `static_generate_entry_point` does **NOT** latch it (it only checks the three guards and loads `I7 = m_ird`). The emitter **must** store `m_irdi = m_ird` before any yield, or the interpreter's partial handler will decode `rx`/`ry` from a **stale** `m_irdi` on resume → wrong An written → Leg-B failure. This is the single highest-risk 3a-specific bug (caught by the 1-cycle pass, which forces a mid-instruction yield).
> 3. **The long high-word latch `m_alue = m_dbin` is load-bearing** (M3). On the full-grant pass the native runs read-high → latch → read-low → register-write in one pass; if it suspends after read-low (substate 4) the interpreter resumes at the final-prefetch state and reads `m_alue` for `set_16h` — so the native must have latched it. Validated end-to-end by the full-grant pass; the narrow read-high→read-low *redo-resume* boundary is by-construction (OQ-10 owner-decided 2026-06-28: no partial-grant sweep).
> 4. **OQ-10 is resolved (no sweep).** All three grant modes already exist in `cpu_test_harness.cpp`; **no oracle change is needed for 3a.** The long forms gate on the single `length-4` partial-grant offset, exactly as the single-access batches.

## Global Constraints

These apply to **every** task below — copied forward from ADR 0007 (body + O-mem-1/2/3 addenda) + the O-mem-2 plan + the cut-line doc + the user's workflow rules, with the O-mem-3a deltas marked.

- **THE GATE for every DRC-touching task is the oracle GREEN on the appserver LINUX `oracle` CI job.** Local Windows green is **necessary-not-sufficient** — boundary M passed Windows but failed Linux SysV twice on `drcbe_x64 offset_from_rbp`. The `oracle` CI job is NOT preflight (preflight is a tiny-build smoke).
- **O-mem-3a gate = THREE grant modes, both backends.** A batch merges only when **all of**: (1) 1-cycle Leg B (`drcbex64` + `drcbec`), (2) full-grant Leg B (`CPUORACLE_M68_DRC_FULLGRANT=1`, both backends — the gate that runs the **whole** native instruction incl. the long latch + register write-back), and (3) single-offset partial-grant Leg B (`CPUORACLE_M68_DRC_PARTGRANT=1`, both backends — the mid-instruction resume handoff), with **Leg A unchanged**. **No new oracle mode and no parameterized sweep** (OQ-10, owner-decided 2026-06-28).
- **No `generate_bus_step()` change** (M1). 3a is additive-only over the frozen primitive: one new emitter, the generator filter widening, the dispatch arms, the coverage assertion, the doc. If the read branch needs *any* edit to make a MOVEA read work, that is a bug in this plan — re-read M1 and the as-built `generate_bus_step()` (`m68000drc.cpp:437-547`).
- **ABI-safe codegen:** zero plain `mem(&device_field)` operands. Every device-state field is accessed via `UML_LOAD`/`UML_STORE` with a pointer base (the boundary-M / O-mem-1/2 pattern in `m68000drc.cpp`). `UML_READ`'s address/result are UML registers, so the read op is inherently ABI-safe; the field plumbing around it (`m_irdi`, `m_aob`, `m_at`, `m_au`, `m_pc`, `m_da[]`, `m_sp`, `m_dbin`, `m_edb`, `m_alue`, `m_alub`, `m_irc`, `m_ir`, `m_ird`, `m_icount`, `m_inst_substate`, `m_inst_state`, `m_next_state`, `m_int_next_state`, `m_base_ssw`, `m_sr`) all goes through LOAD/STORE.
- **Generator additivity:** regenerating must leave existing generated decode files **byte-identical** (empty `git diff` on `m68000-decode.cpp`, `m68000-head.h`, `m68000-s{d,i}{f,p}.cpp`). Only `m68000-drcdesc.ipp` may grow (the new MOVEA word/long read runs). **No new `step.kind`, no new descriptor field** (M1/M6) — the existing read kinds + `byte_lane`/`has_addr_error`/`pre_charge` fields already express every MOVEA access. The `enum_str()` shim preserves byte-identity on Python ≥ 3.11 — do not remove it.
- **Single-source rule (ADR 0007 R-A, the top risk):** the bus-step list — kinds, charges, the redo/completed substate pair, the predecrement internal `−2`, the `byte_lane`/`has_addr_error` flags — is emitted from the SAME microcode walk that generates the interpreter handler (`drc_bus_steps()` parses the handler text `generate_source_from_code` emits). **Never hand-type a substate number or charge into the emitter.** A hand-written step table is *not* an acceptable fallback for O-mem-3 (it was a documented temporary fallback for O-mem-1 only; the generator path is proven through O-mem-2).
- **Build env (verified recipe):** `MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd <worktree>; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'`, then `./mametests "[m68000]"`. Do **NOT** use `make SOURCES=...m68000.cpp` (SOURCES filters by *driver*; a CPU device has no driver and GENie errors). `make REGENIE=1` is required whenever a `.cpp`/`.h` is added and after Task 1 regenerates the `.ipp`.
- **Backend matrix:** every oracle pass on **x64 (`drcbex64`)** AND the **C backend (`CPUORACLE_M68_DRC_C=1`)**. arm64 (`drcbearm64`) is deferred to the CI matrix.
- **Cycle truth:** the interpreter (`m68000.cpp` under `-drc 0`) is the authority. The DRC mirrors its cycle count; divergence is always a DRC bug, resolved by routing the offending form back to `cfunc_` (drop it from `is_native_opcode` / the dispatch table) — never by editing the interpreter or weakening an assertion.
- **Scope lock:** O-mem-3a = the **6 MOVEA forms** only (`movea.w`/`movea.l` × `(An)`/`(An)+`/`-(An)` → `An`). MOVEA `(d16,An)→An` (O-mem-3e), MOVE reg↔mem (3b), MOVE mem→mem (3c), long MOVE (3d), every `(d16,An)` form (3e), and native `AS_OPCODES` space-selection are **out of scope** (the gate keeps `AS_OPCODES`/user-space/MMU machines on `cfunc_`).
- **Style:** match the existing m68000 brace/whitespace style (tabs, the K&R-ish style already in `m68000drc.cpp`); license header `// license:BSD-3-Clause` / `// copyright-holders:Mark Mackelprang` on any new file (none expected); run `srcclean` (built via `TOOLS=1`) on touched files before committing. Work on a short-lived branch and open a PR (never commit DRC source straight to `main`).

---

## The interpreter shapes O-mem-3a must mirror (read before any task)

Every native form is a *transcription* of its `_df` handler in `m68000-sdf.cpp` — the one the plain `m68000_device` runs. **Read each handler line-by-line while emitting its form** (R-A is the top risk). The shapes below are verified against the tree.

### The 6 forms and their `{value, mask}` (confirm against each handler's `// xxxx ffff` comment)

| # | Form | Handler (`m68000-sdf.cpp`) | `{value, mask}` | bus steps | substate ladder (kind) |
|---|---|---|---|---|---|
| 1 | `movea.w (An),An` | `movea_w_ais_ad_df:57162` | `0x3050, 0xf1f8` | 2 | data 1/2 · final-prefetch 3/4 |
| 2 | `movea.w (An)+,An` | `movea_w_aips_ad_df:57224` | `0x3058, 0xf1f8` | 2 | data 1/2 · final-prefetch 3/4 |
| 3 | `movea.w -(An),An` | `movea_w_pais_ad_df:57290` | `0x3060, 0xf1f8` | 2 | data 1/2 (pre_charge 2) · final-prefetch 3/4 |
| 4 | `movea.l (An),An` | `movea_l_ais_ad_df:41263` | `0x2050, 0xf1f8` | 3 | read-hi 1/2 · read-lo 3/4 · final-prefetch 5/6 |
| 5 | `movea.l (An)+,An` | `movea_l_aips_ad_df:41351` | `0x2058, 0xf1f8` | 3 | read-hi 1/2 · read-lo 3/4 · final-prefetch 5/6 |
| 6 | `movea.l -(An),An` | `movea_l_pais_ad_df:41442` | `0x2060, 0xf1f8` | 3 | read-hi 1/2 (pre_charge 2) · read-lo 3/4 · final-prefetch 5/6 |

> **Excluded from 3a (→ 3e):** `movea.w (d16,An),An` = `0x3068` (`movea_w_das_ad_df:57358`) and `movea.l (d16,An),An` = `0x2068` (`movea_l_das_ad_df:41534`). Do **not** admit `das`. **Single-source the substate ladders from the generator; do NOT hand-transcribe them.**

### Word MOVEA skeleton (verified against `movea_w_ais_ad_df:57162`)

```cpp
// rx/ry are decoded from m_irdi (NOT m_ird):
int rx = map_sp(((m_irdi >> 9) & 7) | 8);   // dest An index
int ry = map_sp((m_irdi & 7) | 8);          // src  An index
// --- EA setup (adrw1/adrw2 for (An)) : substates feed the data read ---
m_aob = m_da[ry]; m_at = m_da[ry];          // (An): no reg writeback
m_dcr = 0;                                  // dead scratch for MOVEA (see note)
m_base_ssw = SSW_DATA | SSW_R;
m_edb = m_program.read_interruptible(m_aob & ~1);  // <generate_bus_step DATA read ; byte_lane=0 ; has_addr_error=1 ; substates 1/2>
// (commit) m_dbin = m_edb;                 // the source word
// --- final prefetch + REGISTER write-back (mrgm1/mmrw3) : substates 3/4 ; PROGRAM ; addr-error ---
m_aob = m_au; m_ir = m_irc; m_pc = m_au;
m_da[rx] = ext32(m_dbin);                   // WORD MOVEA: sign-extend the 16-bit source to the full 32-bit An
m_au = m_au + 2;
m_ird = m_ir; if(m_next_state != S_TRACE) m_next_state = m_int_next_state;
m_base_ssw = SSW_PROGRAM | SSW_R;
m_edb = m_opcodes.read_interruptible(m_aob & ~1);  // <generate_bus_step prefetch read ; byte_lane=0 ; has_addr_error=1 ; substates 3/4>
// (retire) m_irc = m_dbin = m_edb; set_ftu_const(); m_inst_state = m_next_state ? m_next_state : m_decode_table[m_ird]; trace
```

### Long MOVEA skeleton (verified against `movea_l_ais_ad_df:41263`)

```cpp
// --- read HIGH (adrl1) : substates 1/2 ; DATA ; addr-error ---
m_aob = m_da[ry]; m_au = m_da[ry] + 2; m_base_ssw = SSW_DATA | SSW_R;
m_edb = m_program.read_interruptible(m_aob & ~1);  // <generate_bus_step ; byte_lane=0 ; has_addr_error=1>
// (commit) m_dbin = m_edb;                 // HIGH word
// --- read LOW (adrl2) : substates 3/4 ; DATA ; addr-error.  THE LATCH IS HERE: ---
m_aob = m_au; m_alub = m_dbin; m_alue = m_dbin; m_at = m_au; m_au = m_pc + 2;   // m_alue := HIGH word (load-bearing)
m_base_ssw = SSW_DATA | SSW_R;
m_edb = m_program.read_interruptible(m_aob & ~1);  // <generate_bus_step>
// (commit) m_dbin = m_edb;                 // LOW word (m_alue still holds HIGH)
// --- final prefetch + REGISTER write-back (mrgl1/mrgl2) : substates 5/6 ; PROGRAM ; addr-error ---
m_aob = m_au; m_ir = m_irc; m_pc = m_au;
set_16l(m_da[rx], m_dbin);                  // low 16 of An := LOW word     (mrgl1)
m_au = m_au + 2;
m_ird = m_ir; if(m_next_state != S_TRACE) m_next_state = m_int_next_state;
set_16h(m_da[rx], m_alue);                  // high 16 of An := HIGH word    (mrgl2)
m_base_ssw = SSW_PROGRAM | SSW_R;
m_edb = m_opcodes.read_interruptible(m_aob & ~1);  // <generate_bus_step prefetch read ; substates 5/6>
// (retire) m_irc = m_dbin = m_edb; set_ftu_const(); m_inst_state = ...; trace
```

### EA arithmetic per mode (M3 — word delta 2, long delta 4; **NO A7 byte exception** — MOVEA is word/long only)

| Mode | token | word setup (verified) | long setup (verified) | reg writeback | internal charge |
|---|---|---|---|---|---|
| `(An)` | `ais` | `m_aob = m_at = m_da[ry]` | `m_aob = m_da[ry]; m_au = m_da[ry] + 2` | none | none |
| `(An)+` | `aips` | `m_aob = m_at = m_da[ry]` (old); `m_da[ry] = m_da[ry] + 2` | high read from old `m_da[ry]`; `m_da[ry] = m_da[ry] + 4` at the read-low setup (`pinl3`) | post-inc | none |
| `-(An)` | `pais` | `m_au = m_da[ry] - 2; m_aob = m_at = m_au; m_da[ry] = m_au` | `m_au = m_da[ry] - 4; m_aob = m_au; m_da[ry] = m_au` | pre-dec | **`m_icount -= 2`** at `pdc{w,l}1`, **no** suspend checkpoint |

Verified: word `(An)` `movea_w_ais_ad_df:57167`; `(An)+` `movea_w_aips_ad_df:57229-57235`; `-(An)` `movea_w_pais_ad_df:57298-57303` (the internal `−2` at `:57299`). Long `(An)` `movea_l_ais_ad_df:41268-41269`; `(An)+` `movea_l_aips_ad_df:41356-41384` (writeback `m_da[ry]=m_au` at `pinl3:41384`); `-(An)` `movea_l_pais_ad_df:41446-41453` (the internal `−2` at `:41450`, address delta `−4` at `:41447`). The predecrement `−2` (NOT the `−2`/`−4` address delta) is folded onto the **first read step's `pre_charge`** by the generator (Task 1), exactly as O-mem-2's `pais` forms — so the emitter does **not** charge it.

### Register write-back — the ONLY per-size difference (M3, pinned)

| Size | Write-back | UML idiom |
|---|---|---|
| `.w` | `m_da[rx] = ext32(m_dbin)` (`ext32(v) = s32(s16(v))`, `m68000.h:349`) | `UML_SEXT(I, m_dbin, SIZE_WORD)` then `UML_STORE(m_da[0], rx, I, SIZE_DWORD, SCALE_x4)` |
| `.l` | `set_16l(m_da[rx], m_dbin)` + `set_16h(m_da[rx], m_alue)` (`m68000.h:351-352`) | `m_da[rx] = (m_alue << 16) \| (m_dbin & 0xffff)` — both halves overwritten, no observable intermediate (both run before the final-prefetch read, no suspend between) |

`map_sp(r) = (r == 15 ? m_sp : r)` (`m68000.h:360`): for `((m_irdi & 7) | 8)`, if the low 3 bits == 7 the index is the **active A7 bank** `m_sp` (15 or 16), else `(idx) | 8` (A0–A6 = 8–14). MOVEA may target A7 (`rx == m_sp`); `m_sp` is stable across MOVEA (the write is `m_da[m_sp] = value`, it does not move `m_sp`).

### Fields that are SET by the handler but DEAD for MOVEA (skip per the O-mem-1 precedent — VERIFY not in the oracle compare set)

- `m_dcr = 0` (word handlers): written, never read for MOVEA. Skip (the O-mem-1 btst emitter skips the analogous dead `alu_*` writes that only touch uncompared scratch). VERIFY `m_dcr` is not in the oracle's compared architectural state; if uncertain, setting it is cheap and harmless.
- `m_alub = m_dbin` (long, at the read-low setup): written, never read for MOVEA (only `m_alue` feeds `set_16h`). Skip `m_alub` (set only `m_alue`, which **is** load-bearing). Same VERIFY/escape as `m_dcr`.
- `alu_and(m_dbin, 0xffff)` / `alu_and(m_alub, 0xffff)` (handler ALU calls): by-value, write only `m_aluo`/`m_isr` (uncompared scratch). **Not** emitted, exactly as `generate_btst_imm8_absolute` omits its `alu_eor8`/`alu_and8` (see `m68000drc.cpp:577-586`).

---

## File Structure

| File | Responsibility | Touched in |
|---|---|---|
| `src/devices/cpu/m68000/m68000gen.py` | Generator — widen `drc_bus_steps()` to admit the 6 MOVEA `(An)/(An)+/-(An)→An` word/long forms; additive-only | Task 1 |
| `src/devices/cpu/m68000/m68000-drcdesc.ipp` | Generated descriptor table — regenerated; existing rows byte-identical, new MOVEA runs appended | Task 1 |
| `src/devices/cpu/m68000/m68000.h` | declare `generate_movea_mem`, the `movea_form` struct + `movea_ea`/`movea_size` enums (beside `bitop_form`) | Task 2 |
| `src/devices/cpu/m68000/m68000drc.cpp` | `generate_movea_mem()` emitter; `is_native_opcode` patterns; the 6 gated dispatch arms | Tasks 2, 3 |
| `tests/emu/cpu/cpuoracle.cpp` | native-coverage assertion for the 6 forms (in the `[m68000][drc][gate]` case); the AS_OPCODES differential opcode subset if it is restricted | Tasks 3, 4 |
| `src/devices/cpu/m68000/README-drc.md` | move the 6 MOVEA forms from `cfunc_` to native (behind the gate); record the long-read + register-write-back mechanism | Task 4 |

No new `.cpp`/`.h` file is created (the emitter lives in the existing `m68000drc.cpp`); `make REGENIE=1` is run once per build because Task 1 regenerates the `.ipp`. **`generate_bus_step()` is NOT in this table — 3a does not touch it.**

---

## Task 1: Generator extension — admit the 6 MOVEA read-form runs (additive)

**Goal:** Single-source the 3a bus-step lists (OQ-2) from `m68000gen.py` so the emitter takes each form's read charges + substate pairs + the `-(An)` predecrement `−2` from the same microcode walk the interpreter uses. Purely a **filter widening** — the existing read-step parser (O-mem-1) and predecrement-`−2` recognizer (O-mem-2) already emit every row MOVEA needs (all reads, `byte_lane=0`, `has_addr_error=1`; **no** `DATA_WRITE` rows). Additive-only: existing generated files stay byte-identical; only `m68000-drcdesc.ipp` grows.

**Files:**
- Modify: `src/devices/cpu/m68000/m68000gen.py` — the `drc_bus_steps()` opcode filter (the O-mem-1/2 admit clause).
- Regenerate: `src/devices/cpu/m68000/m68000-drcdesc.ipp`.
- Validate against: `m68000-sdf.cpp` (the 6 MOVEA handlers).

**Interfaces:**
- Produces (consumed by Task 2): bus runs for the 6 `{value, mask}` patterns (`0x3050/0x3058/0x3060` word, `0x2050/0x2058/0x2060` long, all mask `0xf1f8`); word runs = 2 read rows, long runs = 3 read rows; the `-(An)` (`0x3060`/`0x2060`) first read row carries `pre_charge = 2`; **no** `DATA_WRITE` rows.

- [ ] **Step 1: Verify the round-trip is additive-clean (baseline).** Regenerate all outputs and confirm an empty diff (from `src/devices/cpu/m68000/`):
```bash
python m68000gen.py decode  m68000.lst m68000-decode.cpp
python m68000gen.py header  m68000.lst m68000-head.h
python m68000gen.py sdf     m68000.lst m68000-sdf.cpp
python m68000gen.py sif     m68000.lst m68000-sif.cpp
python m68000gen.py sdp     m68000.lst m68000-sdp.cpp
python m68000gen.py sip     m68000.lst m68000-sip.cpp
python m68000gen.py drcdesc m68000.lst m68000-drcdesc.ipp
git diff --stat
```
Expected: empty. If `m68000-drcdesc.ipp` differs, the tree is stale or Python differs — resolve before editing.

- [ ] **Step 2: Pin the 6 handlers' bus-step truth.** Read each of `movea_w_{ais,aips,pais}_ad_df` and `movea_l_{ais,aips,pais}_ad_df`. Record per step: the `m_icount -= N` charge(s) (note the `−2` at `pdc{w,l}1` for `pais`, which has **no** suspend checkpoint), the redo/completed substate pair, that **every** read (data AND prefetch) is followed by `if(m_aob & 1){ m_icount -= 4; m_inst_state = S_ADDRESS_ERROR; return; }` (so `has_addr_error = 1` on all), and that **no** read carries a byte-lane mask arg (`read_interruptible(m_aob & ~1)` with no second arg → `byte_lane = 0`). This is the table in the plan header; the generator must emit *these exact numbers*.

- [ ] **Step 3: Widen the `drc_bus_steps()` opcode filter to admit MOVEA `(An)/(An)+/-(An)→An`.** In `m68000gen.py`, in the O-mem-1/2 admit clause (the `is_o1`/`is_o2` predicate added in the O-mem-2 generator extension), add the 3a clause:
```python
    base   = drc_base_mnemonic(ii[2][0])
    src_ea = drc_ea_mode[ii[2][1]]
    dst_ea = drc_ea_mode[ii[2][2]]
    BITOPS = ('btst', 'bchg', 'bclr', 'bset')
    REGIND = (DRC_EA_AIS, DRC_EA_AIPS, DRC_EA_PAIS)   # (An), (An)+, -(An)
    is_o1  = (base == 'btst' and src_ea == DRC_EA_IMM and dst_ea in (DRC_EA_ABSW, DRC_EA_ABSL))
    is_o2  = (base in BITOPS and src_ea in (DRC_EA_IMM, DRC_EA_DD) and dst_ea in REGIND)
    # O-mem-3a: MOVEA (An)/(An)+/-(An) -> An, word AND long.  movea.b does not exist,
    # so no size filter is needed; the (d16,An) source 'das' is deliberately EXCLUDED
    # (that is O-mem-3e).  Register-source MOVEA (An->An, src 'as') and every absolute/
    # PC/immediate/indexed MOVEA source are excluded (src_ea not in REGIND).
    is_o3a = (base == 'movea' and src_ea in REGIND and dst_ea == DRC_EA_AD)
    if not (is_o1 or is_o2 or is_o3a):
        return []
```
> **Verify the EA-mode constant names** against the `drc_ea_mode` map definition in `m68000gen.py`: confirm `(An)`=`ais`→`DRC_EA_AIS`, `(An)+`=`aips`→`DRC_EA_AIPS`, `-(An)`=`pais`→`DRC_EA_PAIS`, `An`(dest)=`ad`→`DRC_EA_AD`, and that the `(d16,An)` source `das` maps to a **different** constant (`DRC_EA_DAS`) that is NOT in `REGIND`. Confirm `drc_base_mnemonic` returns `'movea'` (not `'move'`) for the MOVEA handlers — if MOVEA shares the `move` mnemonic in the descriptor, filter on the dest mode (`dst_ea == DRC_EA_AD` already isolates MOVEA, since only MOVEA writes an An). Adjust the names to whatever the map actually defines.

> **Single-source discipline (R-A):** do **not** add any MOVEA-specific substate or charge logic. The 3a forms fall out of the existing per-opcode iteration and the existing read-step + predecrement parser for free, because the filter now admits them. The only edit is the admit clause.

- [ ] **Step 4: Regenerate and prove additivity.**
```bash
python m68000gen.py decode  m68000.lst m68000-decode.cpp
python m68000gen.py header  m68000.lst m68000-head.h
python m68000gen.py sdf     m68000.lst m68000-sdf.cpp
python m68000gen.py sif     m68000.lst m68000-sif.cpp
python m68000gen.py sdp     m68000.lst m68000-sdp.cpp
python m68000gen.py sip     m68000.lst m68000-sip.cpp
python m68000gen.py drcdesc m68000.lst m68000-drcdesc.ipp
git diff --stat
```
Expected: **only** `m68000-drcdesc.ipp` appears (the 6 new MOVEA runs appended; all O-mem-1/2 rows byte-identical). If any of `m68000-decode.cpp`/`m68000-head.h`/the four `s*` files changed, the edit leaked into the wrong code path — fix before continuing.

- [ ] **Step 5: Eyeball the emitted rows against the handlers (R-A — catch it here, not in Leg B).** Confirm `m68000-drcdesc.ipp` now has runs for all 6 `{value, mask}` with the right counts:
```bash
grep -nE '0x3050, 0xf1f8|0x3058, 0xf1f8|0x3060, 0xf1f8' m68000-drcdesc.ipp   # movea.w: 2 read rows each
grep -nE '0x2050, 0xf1f8|0x2058, 0xf1f8|0x2060, 0xf1f8' m68000-drcdesc.ipp   # movea.l: 3 read rows each
grep -n '0x3068, 0xf1f8\|0x2068, 0xf1f8' m68000-drcdesc.ipp                  # MUST be empty (das = 3e, excluded)
```
Verify: every MOVEA row has `byte_lane == 0` and `has_addr_error == 1`; **no** `DRC_BUS_DATA_WRITE` row appears in any MOVEA run; the `-(An)` (`0x3060`/`0x2060`) **first** read row carries `pre_charge == 2` while `(An)`/`(An)+` carry `0`; word runs have completed-substate `4` at the final prefetch, long runs have `6`. A mismatch is the single highest-risk bug in the batch.

- [ ] **Step 6: Build + Leg A (descriptor inert until the emitter consumes it).**
```bash
MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd "$PWD"; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'
./mametests "[m68000]"
./mametests "[m68000][drc]"     # existing native opcodes (moveq/btst-abs/bit-ops) still exact -- the new rows are unread
./mame -validate
```
Expected: build clean; Leg A green; existing Leg B green (no emitter reads the MOVEA rows yet).

- [ ] **Step 7: `srcclean` + commit.**
```bash
git add src/devices/cpu/m68000/m68000gen.py src/devices/cpu/m68000/m68000-drcdesc.ipp
git commit -m "feat(m68000drc): generate MOVEA (An)/(An)+/-(An) word+long read runs (O-mem-3a, additive)"
```

---

## Task 2: The MOVEA emitter `generate_movea_mem` (no dispatch yet)

**Goal:** Add the single parameterized emitter that composes, per form: the **`m_irdi` latch** → `rx`/`ry` decode → source EA setup (per mode/size) → the source read(s) via the frozen `generate_bus_step()` (long: read-high → latch `m_alue` → read-low) → the final-prefetch setup **with the register write-back** (`ext32` `.w` / `set_16h`+`set_16l` `.l`) → the final prefetch read → retire. Driven by the generated bus-step run; the form (EA/size) is a compile-time parameter. Defined but unreferenced until Task 3.

**Files:**
- Modify: `src/devices/cpu/m68000/m68000.h` — the `movea_form` struct + enums + `generate_movea_mem` declaration, beside `bitop_form`/`generate_bitop_mem`.
- Modify: `src/devices/cpu/m68000/m68000drc.cpp` — `generate_movea_mem`, after `generate_bitop_mem`.

**Interfaces:**
- Consumes: `generate_bus_step` (frozen, read path), `s_drc_bus_run_table`/`s_drc_bus_step_table`, `find_bus_run(u16,u16)` (O-mem-2), `ext32`/`set_16h`/`set_16l`/`map_sp` semantics, the fields listed in Global Constraints.
- Produces (consumed by Task 3): `void generate_movea_mem(drcuml_block &, const movea_form &, uml::code_label lbl_delegate);` — emits one full form natively; on a fully-granted instruction it runs every read then retires; on a mid-instruction yield `generate_bus_step` JMPs to `lbl_delegate` and the partial interpreter handler resumes (OQ-1). Clobbers I0–I6; preserves I7 (`m_ird`).

- [ ] **Step 1: Declare the form descriptor + emitter in the header.** In `m68000.h`, beside `bitop_form`/`generate_bitop_mem`:
```cpp
	enum movea_ea   : u8 { MEA_AIS, MEA_AIPS, MEA_PAIS };   // (An), (An)+, -(An)
	enum movea_size : u8 { MEA_W, MEA_L };                  // word (ext32), long (set_16h+set_16l)
	struct movea_form { u16 value; u16 mask; u8 ea; u8 size; };
	void generate_movea_mem(drcuml_block &block, const struct movea_form &form, uml::code_label lbl_delegate); // native MOVEA (An)/(An)+/-(An)->An (O-mem-3a)
```

- [ ] **Step 2: Implement `generate_movea_mem`.** In `m68000drc.cpp`, after `generate_bitop_mem`. Read each handler line-by-line while transcribing (R-A). The emitter defines its own local lambdas mirroring the byte-identical helpers already in `generate_btst_imm8_absolute` (`ssw_program`/`commit_dbin`/`commit_irc_dbin`/`retire`, `m68000drc.cpp:600-698`) — duplication is acceptable and matches the O-mem-2 precedent (no cross-function refactor in this batch; a future cleanup MAY factor them into file-local statics, out of scope here).

```cpp
//-------------------------------------------------
//  generate_movea_mem - native UML for
//  movea.w/.l (An)/(An)+/-(An),An   (O-mem-3a, 6 forms)
//
//  Mirrors movea_{w,l}_{ais,aips,pais}_ad_df in m68000-sdf.cpp.  One
//  parameterized emitter; EA/size are compile-time constants.
//    word: EA setup -> data read -> final-prefetch(+reg writeback) -> retire   (2 reads)
//    long: EA setup -> read-HI -> latch m_alue + read-LO setup -> read-LO ->
//          final-prefetch(+reg writeback) -> retire                            (3 reads)
//  Dest is an ADDRESS register, so there is NO data-write bus step and NO flags.
//  Write-back: word = m_da[rx] = ext32(m_dbin); long = (m_alue<<16)|(m_dbin&0xffff).
//
//  CRITICAL (R-A): MOVEA's handler decodes rx/ry from m_irdi, and the DRC entry
//  point does NOT latch m_irdi (static_generate_entry_point only checks the three
//  guards + loads I7=m_ird).  This emitter therefore STORES m_irdi=m_ird first, so
//  the interpreter's PARTIAL handler reads the correct m_irdi when it resumes after
//  a mid-instruction yield.  rx/ry are then decoded from I7 (== m_ird == m_irdi here).
//
//  Substates/charges/pre_charge come from the generated run -- never hard-coded.
//  I7 holds m_ird (the opword), preserved across generate_bus_step.
//-------------------------------------------------

void m68000_device::generate_movea_mem(drcuml_block &block, const struct movea_form &form, uml::code_label lbl_delegate)
{
	const drc_bus_run &run = find_bus_run(form.value, form.mask);
	u16 si = run.first;                 // running index into s_drc_bus_step_table for THIS run
	const bool is_long = (form.size == MEA_L);

	// ---- m_irdi = m_ird  (THE LATCH; see header note -- load-bearing for resume) ----
	UML_STORE(block, &m_irdi, 0, I7, SIZE_WORD, SCALE_x1);

	// ---- ry = map_sp((m_irdi & 7) | 8): if (op & 7)==7 use m_sp, else (op&7)|8 ----
	//      (decoded from I7; equals m_irdi after the latch).  Left in I6 across the EA
	//      setup only (the EA writeback is BEFORE the first generate_bus_step, so ry is
	//      never needed across a clobbering bus step).
	auto load_ry = [&]() {
		uml::code_label const lbl_a7 = m_drc_labelnum++;
		uml::code_label const lbl_done = m_drc_labelnum++;
		UML_AND(block, I6, I7, 7);                                    // i6 = op & 7
		UML_CMP(block, I6, 7);
		UML_JMPc(block, COND_E, lbl_a7);
			UML_OR(block, I6, I6, 8);                                 // A0..A6 -> (op&7)|8
			UML_JMP(block, lbl_done);
		UML_LABEL(block, lbl_a7);
			UML_LOAD(block, I6, &m_sp, 0, SIZE_DWORD, SCALE_x1);      // A7 -> m_sp (15 or 16)
		UML_LABEL(block, lbl_done);
	};
	// ---- rx = map_sp(((m_irdi >> 9) & 7) | 8): the DEST An index, into the given reg ----
	//      Recomputed at the write-back (after the reads); m_sp is stable across MOVEA.
	auto load_rx = [&](uml::parameter dst) {
		uml::code_label const lbl_a7 = m_drc_labelnum++;
		uml::code_label const lbl_done = m_drc_labelnum++;
		UML_SHR(block, dst, I7, 9);
		UML_AND(block, dst, dst, 7);                                  // (op>>9)&7
		UML_CMP(block, dst, 7);
		UML_JMPc(block, COND_E, lbl_a7);
			UML_OR(block, dst, dst, 8);
			UML_JMP(block, lbl_done);
		UML_LABEL(block, lbl_a7);
			UML_LOAD(block, dst, &m_sp, 0, SIZE_DWORD, SCALE_x1);
		UML_LABEL(block, lbl_done);
	};
	auto ssw_data = [&]() {
		UML_MOV(block, I0, u32(u16(SSW_DATA | SSW_R)));
		UML_STORE(block, &m_base_ssw, 0, I0, SIZE_WORD, SCALE_x1);
	};
	auto ssw_program = [&]() {
		UML_MOV(block, I0, u32(u16(SSW_PROGRAM | SSW_R)));
		UML_STORE(block, &m_base_ssw, 0, I0, SIZE_WORD, SCALE_x1);
	};
	auto commit_dbin = [&]() {                                        // m_dbin = m_edb
		UML_LOAD(block, I0, &m_edb, 0, SIZE_WORD, SCALE_x1);
		UML_STORE(block, &m_dbin, 0, I0, SIZE_WORD, SCALE_x1);
	};

	// ===== source EA setup (read-HI setup for long; the only read setup for word) =====
	// VERIFY per EA/size against the handler:
	//   word ais :57167 / aips :57229 / pais :57298 ; long ais :41268 / aips :41356 / pais :41446
	load_ry();
	switch(form.ea)
	{
	case MEA_AIS:   // (An): m_aob = m_at = m_da[ry] (word); m_aob = m_da[ry], m_au = m_da[ry]+2 (long)
		UML_LOAD(block, I0, &m_da[0], I6, SIZE_DWORD, SCALE_x4);
		UML_STORE(block, &m_aob, 0, I0, SIZE_DWORD, SCALE_x1);
		if(!is_long)
			UML_STORE(block, &m_at, 0, I0, SIZE_DWORD, SCALE_x1);     // word: m_at = m_da[ry]
		else {
			UML_ADD(block, I1, I0, 2);
			UML_STORE(block, &m_au, 0, I1, SIZE_DWORD, SCALE_x1);     // long: m_au = m_da[ry] + 2
		}
		break;
	case MEA_AIPS:  // (An)+: aob/at = old m_da[ry]; writeback += (2 word / 4 long)
		// VERIFY: word does m_au=m_da[ry]+2 then m_da[ry]=m_au at pinw2 (:57233-57235);
		//         long defers m_da[ry]=m_au to pinl3 (read-low setup, :41384) -- emit the
		//         long writeback in the read-low setup block, NOT here.  Transcribe exactly.
		UML_LOAD(block, I0, &m_da[0], I6, SIZE_DWORD, SCALE_x4);      // old m_da[ry]
		UML_STORE(block, &m_aob, 0, I0, SIZE_DWORD, SCALE_x1);
		UML_STORE(block, &m_at, 0, I0, SIZE_DWORD, SCALE_x1);
		if(!is_long) {
			UML_ADD(block, I1, I0, 2);
			UML_STORE(block, &m_au, 0, I1, SIZE_DWORD, SCALE_x1);     // m_au = old + 2
			UML_STORE(block, &m_da[0], I6, I1, SIZE_DWORD, SCALE_x4); // m_da[ry] = m_au  (pinw2)
			// (m_au = m_pc + 2 is also set at pinw2 -- VERIFY :57236 and emit it)
		} else {
			UML_ADD(block, I1, I0, 2);
			UML_STORE(block, &m_au, 0, I1, SIZE_DWORD, SCALE_x1);     // long: m_au = old + 2 (pinl1)
			// the +4 writeback (m_da[ry] = m_au at pinl3) is emitted in the read-low setup
		}
		break;
	case MEA_PAIS:  // -(An): predecrement; the internal -2 is pre_charge on the first read
		// VERIFY: word m_au=m_da[ry]-2; m_aob=m_at=m_au; m_da[ry]=m_au (:57298-57303);
		//         long m_au=m_da[ry]-4; m_aob=m_au; m_da[ry]=m_au (:41447,41452-41453).
		UML_LOAD(block, I0, &m_da[0], I6, SIZE_DWORD, SCALE_x4);
		UML_SUB(block, I1, I0, is_long ? 4 : 2);                      // m_da[ry] - delta
		UML_STORE(block, &m_aob, 0, I1, SIZE_DWORD, SCALE_x1);
		if(!is_long)
			UML_STORE(block, &m_at, 0, I1, SIZE_DWORD, SCALE_x1);     // word sets m_at; long sets m_at at read-low setup
		UML_STORE(block, &m_da[0], I6, I1, SIZE_DWORD, SCALE_x4);     // m_da[ry] = m_au
		// (word also sets m_au = m_pc / m_pc = m_au etc. at pdcw1/pdcw2 -- VERIFY and emit;
		//  long sets m_au = m_au + 2 at pdcl2 :41454 -- VERIFY and emit)
		break;
	}
	ssw_data();

	// ===== source read =====
	if(!is_long)
	{
		// word: single data read (byte_lane=0; pais carries pre_charge=2 in the descriptor)
		generate_bus_step(block, s_drc_bus_step_table[si++], lbl_delegate);   // DATA read ; substates 1/2
		commit_dbin();                                                        // m_dbin = source word
	}
	else
	{
		// long: read-HIGH, then the LATCH + read-low setup, then read-LOW
		generate_bus_step(block, s_drc_bus_step_table[si++], lbl_delegate);   // read-HI ; substates 1/2
		commit_dbin();                                                        // m_dbin = HIGH word
		// read-low setup (adrl2 / pinl2-3 / pdcl2): m_aob = m_au; m_alue = m_dbin (LATCH);
		//   m_at = m_au; m_au = m_pc + 2 (ais/pais); aips also m_da[ry] = m_au (pinl3).
		// VERIFY per EA: ais :41290-41294 ; aips :41377-41385 ; pais :41472-41477.
		UML_LOAD(block, I0, &m_au, 0, SIZE_DWORD, SCALE_x1);
		UML_STORE(block, &m_aob, 0, I0, SIZE_DWORD, SCALE_x1);                // m_aob = m_au
		UML_STORE(block, &m_at, 0, I0, SIZE_DWORD, SCALE_x1);                 // m_at = m_au
		UML_LOAD(block, I1, &m_dbin, 0, SIZE_WORD, SCALE_x1);
		UML_STORE(block, &m_alue, 0, I1, SIZE_WORD, SCALE_x1);                // m_alue = HIGH word (LOAD-BEARING)
		// (m_alub = m_dbin is DEAD for MOVEA -- skip per the O-mem-1 dead-scratch precedent; VERIFY)
		if(form.ea == MEA_AIPS)
		{
			// pinl3: m_da[ry] = m_au (the +4 writeback) BEFORE m_au = m_pc + 2.  VERIFY :41384.
			load_ry();                                                       // recompute ry (I6 was clobbered by the read)
			UML_STORE(block, &m_da[0], I6, I0, SIZE_DWORD, SCALE_x4);         // m_da[ry] = m_au (old+4)
		}
		// m_au = m_pc + 2  (VERIFY the exact m_au target per EA: ais/pais use m_pc+2; aips pinl2
		//   does m_au=m_au+2 then pinl3 m_au=m_pc+2 -- net m_au=m_pc+2 at the read).
		UML_LOAD(block, I2, &m_pc, 0, SIZE_DWORD, SCALE_x1);
		UML_ADD(block, I2, I2, 2);
		UML_STORE(block, &m_au, 0, I2, SIZE_DWORD, SCALE_x1);
		ssw_data();
		generate_bus_step(block, s_drc_bus_step_table[si++], lbl_delegate);   // read-LO ; substates 3/4
		commit_dbin();                                                        // m_dbin = LOW word (m_alue = HIGH)
	}

	// ===== final prefetch setup + REGISTER write-back =====
	// VERIFY: word mrgm1/mmrw3 :57190-57199 ; long mrgl1/mrgl2 :41312-41326.
	//   m_aob = m_au; m_ir = m_irc; m_pc = m_au; <reg writeback>; m_au += 2;
	//   m_ird = m_ir; if(m_next_state != S_TRACE) m_next_state = m_int_next_state; SSW_PROGRAM.
	UML_LOAD(block, I0, &m_au, 0, SIZE_DWORD, SCALE_x1);
	UML_STORE(block, &m_aob, 0, I0, SIZE_DWORD, SCALE_x1);                    // m_aob = m_au
	UML_STORE(block, &m_pc, 0, I0, SIZE_DWORD, SCALE_x1);                     // m_pc = m_au
	UML_LOAD(block, I1, &m_irc, 0, SIZE_WORD, SCALE_x1);
	UML_STORE(block, &m_ir, 0, I1, SIZE_WORD, SCALE_x1);                      // m_ir = m_irc
	// --- register write-back (the only per-size difference) ---
	load_rx(I5);                                                             // I5 = dest An index (from m_irdi via I7)
	if(!is_long)
	{
		UML_LOAD(block, I2, &m_dbin, 0, SIZE_WORD, SCALE_x1);
		UML_SEXT(block, I2, I2, SIZE_WORD);                                   // ext32(m_dbin) = s32(s16(low word))
		UML_STORE(block, &m_da[0], I5, I2, SIZE_DWORD, SCALE_x4);             // m_da[rx] = ext32(m_dbin)
	}
	else
	{
		UML_LOAD(block, I2, &m_alue, 0, SIZE_WORD, SCALE_x1);                 // HIGH word
		UML_SHL(block, I2, I2, 16);                                           // << 16
		UML_LOAD(block, I3, &m_dbin, 0, SIZE_WORD, SCALE_x1);                 // LOW word (zero-extended u16)
		UML_OR(block, I2, I2, I3);                                            // (m_alue<<16) | (m_dbin & 0xffff)
		UML_STORE(block, &m_da[0], I5, I2, SIZE_DWORD, SCALE_x4);             // m_da[rx] = set_16h+set_16l
	}
	// --- continue the prefetch setup ---
	UML_ADD(block, I0, I0, 2);
	UML_STORE(block, &m_au, 0, I0, SIZE_DWORD, SCALE_x1);                     // m_au += 2
	UML_STORE(block, &m_ird, 0, I1, SIZE_WORD, SCALE_x1);                     // m_ird = m_ir (== old m_irc, in I1)
	UML_LOAD(block, I2, &m_next_state, 0, SIZE_DWORD, SCALE_x1);
	UML_LOAD(block, I3, &m_int_next_state, 0, SIZE_DWORD, SCALE_x1);
	UML_CMP(block, I2, u32(S_TRACE));
	UML_MOVc(block, COND_NE, I2, I3);                                        // (next != S_TRACE) ? int_next : kept
	UML_STORE(block, &m_next_state, 0, I2, SIZE_DWORD, SCALE_x1);
	ssw_program();

	// ===== final prefetch read =====
	generate_bus_step(block, s_drc_bus_step_table[si++], lbl_delegate);       // prefetch ; substates 3/4 (word) or 5/6 (long)

	// ===== retire (byte-identical to generate_btst_imm8_absolute's retire, :680-698) =====
	//   m_irc = m_dbin = m_edb; set_ftu_const(); m_inst_state = m_next_state ? m_next_state
	//   : m_decode_table[m_ird]; if(m_sr & SR_T) m_next_state = S_TRACE.
	{
		UML_LOAD(block, I0, &m_edb, 0, SIZE_WORD, SCALE_x1);
		UML_STORE(block, &m_irc, 0, I0, SIZE_WORD, SCALE_x1);
		UML_STORE(block, &m_dbin, 0, I0, SIZE_WORD, SCALE_x1);
		UML_CALLC(block, &m68000_device::cfunc_set_ftu_const, this);
		uml::code_label const lbl_have_next = m_drc_labelnum++;
		uml::code_label const lbl_no_trace  = m_drc_labelnum++;
		UML_LOAD(block, I0, &m_next_state, 0, SIZE_DWORD, SCALE_x1);
		UML_CMP(block, I0, 0);
		UML_JMPc(block, COND_NE, lbl_have_next);
			UML_LOAD(block, I1, &m_ird, 0, SIZE_WORD, SCALE_x1);
			UML_LOAD(block, I0, m_decode_table.data(), I1, SIZE_WORD, SCALE_x2);
		UML_LABEL(block, lbl_have_next);
		UML_STORE(block, &m_inst_state, 0, I0, SIZE_WORD, SCALE_x1);
		UML_LOAD(block, I2, &m_sr, 0, SIZE_WORD, SCALE_x1);
		UML_TEST(block, I2, u32(u16(SR_T)));
		UML_JMPc(block, COND_Z, lbl_no_trace);
			UML_MOV(block, I3, u32(S_TRACE));
			UML_STORE(block, &m_next_state, 0, I3, SIZE_DWORD, SCALE_x1);
		UML_LABEL(block, lbl_no_trace);
	}
}
```

> **Five load-bearing verifications (R-A) — make these explicit checks, not assumptions:**
> 1. **The `m_irdi` latch must precede every yield.** `UML_STORE(&m_irdi, I7)` is the first emitted op; it makes the interpreter's partial handler decode the correct `rx`/`ry` on resume. **Confirm `static_generate_entry_point` does not already set `m_irdi`** (verified at plan time: it does not). The 1-cycle pass *will* fail loudly if this is wrong (it forces a yield after read-high, then the interpreter resumes and reads `m_irdi`).
> 2. **`rx`/`ry` source.** Decode from `I7` (== `m_ird` == `m_irdi` at the native-dispatch boundary, after the latch). This matches the handler's `m_irdi` exactly. Do **not** assume `m_ird`/`m_irdi` differ here — they are equal at a true first-grant (the three guards guarantee it) — but the *latch* is still required for the interpreter's resume path.
> 3. **The per-EA/size architectural choreography is derive-from-handler.** The literal UML above pins `ais` and the read-low latch; the `aips`/`pais` `m_au`/`m_pc`/`m_at`/`m_da[ry]` sequencing differs subtly per handler (e.g. word `pais` sets `m_pc = m_au` at `pdcw1` then `m_au = m_pc` at `pdcw2`; long `aips` defers the `m_da[ry]` writeback to `pinl3`). **Transcribe each from its handler line-by-line** behind the `// VERIFY against <handler>:<line>` markers — do not hand-wave the `m_au` targets. A wrong `m_au` is a wrong final-prefetch address → Leg-B RAM/cycle failure.
> 4. **`m_alue` is load-bearing; `m_alub`/`m_dcr` are dead.** Set `m_alue` at the read-low setup (the interpreter's `set_16h` on resume/full-grant reads it). Skip `m_alub` and `m_dcr` per the O-mem-1 dead-scratch precedent — but **VERIFY** neither is in the oracle's compared state (if either is, set it; both are cheap).
> 5. **`generate_bus_step` clobbers I0–I6.** `ry` (I6) is consumed before the first read, so it is safe; for the long `aips` writeback at the read-low setup, **recompute `ry`** (the `load_ry()` call inside the read-low block) because the read-high `generate_bus_step` clobbered I6. `rx` is recomputed at the write-back (`load_rx(I5)`), after the reads, from the preserved I7.

- [ ] **Step 3: Build (compile-only — no dispatch arm yet).**
```bash
MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd "$PWD"; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'
```
Expected: clean build. `generate_movea_mem` is defined but unreferenced (acceptable; Task 3 calls it). Fix any UML-scope/enum/`uml::parameter` errors now.

- [ ] **Step 4: Oracle unchanged (emitter dormant).**
```bash
./mametests "[m68000][drc]"
CPUORACLE_M68_DRC_FULLGRANT=1 ./mametests "[m68000][drc]"
```
Expected: green — nothing dispatches to `generate_movea_mem` yet.

- [ ] **Step 5: `srcclean` + commit.**
```bash
git add src/devices/cpu/m68000/m68000.h src/devices/cpu/m68000/m68000drc.cpp
git commit -m "feat(m68000drc): generate_movea_mem() long-read+latch+register-writeback emitter (no dispatch yet)"
```

---

## Task 3: Wire the 6 native forms + dispatch arms

**Goal:** Add the 6 `{value, mask}` patterns to `is_native_opcode` and the gated dispatch arms in `generate_native_dispatch`, each calling `generate_movea_mem` with the form constants. Then run the oracle under all three stepping modes — the **full-grant pass is where the native long latch + register write-back are first validated**, and the **1-cycle pass is where the `m_irdi` latch is validated**.

**Files:**
- Modify: `src/devices/cpu/m68000/m68000drc.cpp` — `is_native_opcode` (`:210`); `generate_native_dispatch` (`:250`), after the O-mem-2 bit-op block (`:329`).

**Interfaces:**
- Consumes: `generate_movea_mem` (Task 2), `drc_native_mem_ea_allowed()` (the gate, unchanged — M5/W6), `m_drc_native_mem_ea_arms` (probe counter).

- [ ] **Step 1: Add the 6 patterns to `is_native_opcode`.** After the O-mem-2 `Dn`-source bit-op switch (`m68000drc.cpp:227-235`), add the MOVEA patterns (mask `0xf1f8`; word bases `0x3050/0x3058/0x3060`, long bases `0x2050/0x2058/0x2060`). **Exclude** `0x3068`/`0x2068` (`(d16,An)` = 3e):
```cpp
	// O-mem-3a: movea.w/.l (An)/(An)+/-(An),An  (mask 0xf1f8; (d16,An)=0x3068/0x2068 is 3e)
	switch(opword & 0xf1f8)
	{
	case 0x3050: case 0x3058: case 0x3060:   // movea.w (An)/(An)+/-(An)
	case 0x2050: case 0x2058: case 0x2060:   // movea.l (An)/(An)+/-(An)
		return true;
	}
```
> **Verify each constant against the handler `// xxxx ffff` comment** (`movea_w_ais_ad_df // 3050 f1f8`, `movea_l_pais_ad_df // 2060 f1f8`, …). A wrong base silently mis-classifies. Confirm no overlap with the existing arms: moveq `0xf100/0x7000`, btst-abs `0xfffe/0x0838`, bit-ops `0xfff8`/`0xf1f8` bases `0x01xx/0x08xx` — the MOVEA bases (`0x20xx/0x30xx`) share no value with any of them.

- [ ] **Step 2: Add the gated dispatch arms.** In `generate_native_dispatch`, after the O-mem-2 bit-op block (`:329`), add a single `if (drc_native_mem_ea_allowed())` block with the 6-entry form table and a per-form compile-time arm (mirroring the O-mem-2 loop, `:288-329`):
```cpp
	// O-mem-3a: movea.w/.l (An)/(An)+/-(An),An -- native ONLY behind the space-topology
	// gate (ADR 0007 O-mem-3 addendum; M5).  Each arm calls generate_movea_mem with its
	// form constants; the bus-step run (substates/charges/pre_charge) is single-sourced
	// from the generator.  (d16,An) (0x3068/0x2068) is deliberately NOT here -- O-mem-3e.
	if (drc_native_mem_ea_allowed())
	{
		static const movea_form k_movea_forms[] = {
			{ 0x3050, 0xf1f8, MEA_AIS,  MEA_W },
			{ 0x3058, 0xf1f8, MEA_AIPS, MEA_W },
			{ 0x3060, 0xf1f8, MEA_PAIS, MEA_W },
			{ 0x2050, 0xf1f8, MEA_AIS,  MEA_L },
			{ 0x2058, 0xf1f8, MEA_AIPS, MEA_L },
			{ 0x2060, 0xf1f8, MEA_PAIS, MEA_L },
		};
		for(const movea_form &f : k_movea_forms)
		{
			uml::code_label const lbl_next = m_drc_labelnum++;
			UML_AND(block, I0, I7, f.mask);
			UML_CMP(block, I0, f.value);
			UML_JMPc(block, COND_NE, lbl_next);
			generate_movea_mem(block, f, lbl_delegate);            // emit the form (suspend yields JMP lbl_delegate from within)
			UML_JMP(block, lbl_delegate);                          // fully-granted: retired -> hand the tail to the interpreter
			UML_LABEL(block, lbl_next);
			m_drc_native_mem_ea_arms++;                            // emission probe (gate/coverage test)
		}
	}
```

- [ ] **Step 3: Build.**
```bash
MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd "$PWD"; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'
```
Expected: clean build.

- [ ] **Step 4: 1-cycle Leg B (x64 + C) — native step-1 + the suspend handoff + THE `m_irdi` LATCH.**
```bash
./mametests "[m68000]"                            # Leg A
./mametests "[m68000][drc]"                       # Leg B 1-cycle, x64
CPUORACLE_M68_DRC_C=1 ./mametests "[m68000][drc]" # Leg B 1-cycle, C
```
Expected: green. The 1-cycle pass forces a yield after read-1, then the interpreter resumes via the partial handler — which decodes `rx`/`ry` from `m_irdi`. **If a MOVEA form fails here with a wrong An register, suspect the `m_irdi` latch first** (Task 2 Step 2 / verification 1).

- [ ] **Step 5: FULL-GRANT Leg B (x64 + C) — THE native long-latch + register-write-back gate.**
```bash
CPUORACLE_M68_DRC_FULLGRANT=1 ./mametests "[m68000][drc]"                       # x64
CPUORACLE_M68_DRC_FULLGRANT=1 CPUORACLE_M68_DRC_C=1 ./mametests "[m68000][drc]" # C backend
```
Expected: **green.** This is the only pass that runs the **whole** native instruction in one go — the long read-high → `m_alue` latch → read-low → `set_16h`+`set_16l` register assembly (and, for word, `ext32`), plus the EA writeback (`(An)+`/`-(An)`) and the predecrement `−2`. **If a form fails:** `superpowers:systematic-debugging` — map the divergence to the step (A-reg value → the write-back `ext32` vs `set_16h/set_16l`, or `m_alue` latch, or the `(An)+`/`-(An)` writeback/delta; cycle → a charge or the predecrement `−2`; RAM unchanged — MOVEA writes no RAM). The fallback is to drop the failing form from `is_native_opcode` + the dispatch table (route it back to `cfunc_`) and report — never approximate.

- [ ] **Step 6: PARTIAL-GRANT Leg B (x64 + C) — the second-to-last-access resume handoff.**
```bash
CPUORACLE_M68_DRC_PARTGRANT=1 ./mametests "[m68000][drc]"                       # x64
CPUORACLE_M68_DRC_PARTGRANT=1 CPUORACLE_M68_DRC_C=1 ./mametests "[m68000][drc]" # C backend
```
Expected: green. Single `length-4` offset (OQ-10: no sweep). For long MOVEA this resumes the interpreter at the final prefetch (after both reads ran native), exercising the native read-high→read-low→final handoff with the register write-back having run natively.

- [ ] **Step 7: `srcclean` + commit.**
```bash
git add src/devices/cpu/m68000/m68000drc.cpp
git commit -m "feat(m68000drc): native MOVEA (An)/(An)+/-(An)->An word+long -- 6 forms (O-mem-3a)"
```

---

## Task 4: Coverage assertion + cut-line doc

**Goal:** Machine-assert that all 6 MOVEA forms dispatch native (behind the gate), and record them in the cut-line doc with the long-read + register-write-back note.

**Files:**
- Modify: `tests/emu/cpu/cpuoracle.cpp` — the native-coverage assertion (in the `[m68000][drc][gate]` case where `is_native_opcode` is exercised via the harness wrapper) and, if the AS_OPCODES differential restricts to a bit-op corpus subset, extend that subset to include the 6 MOVEA forms.
- Modify: `src/devices/cpu/m68000/README-drc.md` — the cut-line doc.

**Interfaces:**
- Consumes: `is_native_opcode(u16)` (Task 3) for the 6 patterns; the harness wrapper used for the O-mem-1/2 coverage checks.

- [ ] **Step 1: Extend the native-coverage assertion.** Add the 6 representative encodings to the asserted-native set in the `[m68000][drc][gate]` case (one per form; the predicate is mask-based), reusing the wrapper O-mem-1/2 added to reach the protected `is_native_opcode`:
```cpp
	// O-mem-3a: movea.w/.l (An)/(An)+/-(An),An are native (behind the gate).
	for(u16 op : { (u16)0x3050,(u16)0x3058,(u16)0x3060, (u16)0x2050,(u16)0x2058,(u16)0x2060 })
		CHECK(harness_is_native_opcode(op));
	// anti-vacuity: (d16,An) MOVEA stays cfunc_ in 3a (it is O-mem-3e).
	CHECK_FALSE(harness_is_native_opcode((u16)0x3068));
	CHECK_FALSE(harness_is_native_opcode((u16)0x2068));
```

- [ ] **Step 2: Confirm the AS_OPCODES differential exercises MOVEA (or extend its subset).** Read the O-mem-2 `[m68000][drc][asopcodes]` case (`cpuoracle.cpp`). If it runs the full corpus, MOVEA forms are already covered (gate off → `cfunc_` → interpreter ≡ DRC). If O-mem-2 restricted the differential to a bit-op corpus subset (permitted by O-mem-2 Task 0), **extend the subset to include the 6 MOVEA forms** so the differential proves the gate keeps MOVEA on `cfunc_` on an `AS_OPCODES` topology and the fallback matches. No new config beyond the batch's opcodes (M6 item 4).

- [ ] **Step 3: Update the cut-line doc.** In `src/devices/cpu/m68000/README-drc.md`:
  - In "Native opcodes shipped (current)", add a row:
```markdown
| `movea.w`/`movea.l` `(An)`/`(An)+`/`-(An)`,`An` | O-mem-3a | 6 forms. Source-EA read via the frozen `generate_bus_step()` read path (`byte_lane=0` word read, `has_addr_error=1`); **long = two-word read** (high then low) with the high word latched in `m_alue`. Dest is an **address register**: write-back is `ext32(m_dbin)` (`.w`) / `set_16h`+`set_16l` (`.l`) at the final-prefetch state. **No data-write step, no flags, no `generate_bus_step()` edit.** Decodes `rx`/`ry` from `m_irdi` (latched `m_irdi = m_ird` at emit). **Native ONLY on a flat-topology, non-MMU bus (`drc_native_mem_ea_allowed()`)** — `AS_OPCODES`/user-space/MMU machines stay `cfunc_`. Validated by the 1-cycle (incl. the `m_irdi` resume), full-grant (the native latch + register write-back), and single-offset partial-grant Leg-B passes (ADR 0007 O-mem-3 addendum). `(d16,An)` MOVEA is deferred to O-mem-3e. |
```
  - In "Explicitly NOT native" / the MOVE notes, reword so the 6 MOVEA reg-indirect forms are excluded from the deferred set ("memory-EA MOVE/MOVEA except MOVEA `(An)`/`(An)+`/`-(An)`→An, native as of O-mem-3a on a flat-topology non-MMU bus").
  - In "Known limitations", append: the read-high→read-low *redo-resume* intermediate boundary for long MOVEA is covered **by construction** (OQ-10 owner-decided 2026-06-28 — no partial-grant sweep; the full-grant pass exercises the native latch end-to-end, the 1-cycle pass exercises the interpreter-side handoff); and the native **retire** lambda remains the one native residual not oracle-exercised (the snapshot model cannot reach an overshoot; mirrors the validated `moveq`/`btst` tail).

- [ ] **Step 4: Full local gate (every pass, both backends).**
```bash
MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd "$PWD"; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'
./mametests "[m68000]"                                                          # Leg A
./mametests "[m68000][drc]"                                                     # Leg B 1-cycle x64
CPUORACLE_M68_DRC_C=1 ./mametests "[m68000][drc]"                              # Leg B 1-cycle C
CPUORACLE_M68_DRC_FULLGRANT=1 ./mametests "[m68000][drc]"                      # full-grant x64 (latch+writeback gate)
CPUORACLE_M68_DRC_FULLGRANT=1 CPUORACLE_M68_DRC_C=1 ./mametests "[m68000][drc]" # full-grant C
CPUORACLE_M68_DRC_PARTGRANT=1 ./mametests "[m68000][drc]"                      # partial-grant x64
CPUORACLE_M68_DRC_PARTGRANT=1 CPUORACLE_M68_DRC_C=1 ./mametests "[m68000][drc]" # partial-grant C
./mametests "[m68000][drc][gate]"                                              # gate predicate + coverage + MMU regen
./mametests "[m68000][drc][asopcodes]"                                         # AS_OPCODES differential
./mame -validate
```
Expected: all green; `-validate` clean. This is the **necessary** local gate.

- [ ] **Step 5: `srcclean` + commit.**
```bash
git add tests/emu/cpu/cpuoracle.cpp src/devices/cpu/m68000/README-drc.md
git commit -m "test+docs(m68000drc): assert 6 MOVEA forms native; record O-mem-3a long-read+register-writeback in cut-line doc"
```

---

## Task 5: Merge gate — push, Linux `oracle` green, merge

**Goal:** Get every required gate GREEN — the three oracle grant modes (x64 + C) on the appserver Linux `oracle` CI job, the AS_OPCODES differential, the gate-predicate + coverage + MMU-regen tests, generator additivity, and code review — then merge per the auto-merge policy.

**Files:** none (CI + PR).

- [ ] **Step 1: Push the branch and open the PR.** PR body includes a **Docs Impact** section (README-drc.md updated; ADR 0007 O-mem-3 addendum is the source of truth) and the gate evidence (all three passes green on Linux, both backends).
```bash
git push -u origin <branch>
gh pr create --fill
```

- [ ] **Step 2: Get the appserver Linux `oracle` job GREEN — the SUFFICIENT gate.** Confirm the Linux `oracle` job runs **all three** grant modes (1-cycle + full-grant + partial-grant), x64 + C, plus the `[gate]` and `[asopcodes]` cases. A red Linux job with green Windows is almost always an ABI-safety regression (a plain `mem(&field)` slipped in — audit every new UML operand in `generate_movea_mem`) or a backend-divergent emission (R-C). Do not merge until the Linux `oracle` job is green.

- [ ] **Step 3: Throughput bench (KNOWN ENVIRONMENT GAP — flag, do NOT hard-gate).** Per the §5 addendum honest-number note, MOVEA throughput would be measured `-drc 0` vs `-drc 1` on a **gate-eligible flat-topology** driver where MOVEA is hot (not an FD1094/`AS_OPCODES` set, which runs `cfunc_`). **The throughput bench has been blocked in this environment by absent ROMs** — treat it as evidence-when-available, **not** a blocking gate. The correctness gates (Steps 2 + the local gate) gate merge. Record the ROM-blocked status in the PR.

- [ ] **Step 4: Merge per the auto-merge policy.** When all of: the three oracle passes green (x64 + C) on the Linux `oracle` job, the gate-predicate + coverage + MMU-regen + AS_OPCODES differential green, generator additive (only `m68000-drcdesc.ipp` grew), code review clean, `-validate` clean — **merge** (implementation cycle complete, parity gates green, review clean, issues addressed; the throughput bench is a documented known-gap, not a hard gate). Do not stop to ask.

---

## Self-review

**Spec coverage (ADR 0007 O-mem-3 addendum → tasks):**
- M1 word/long data bus-steps, **no new `step.kind`, no `generate_bus_step()` edit** (MOVEA reads via the frozen `byte_lane=0` read branch; `has_addr_error=1` already kind-agnostic) → honored across all tasks (Task 1 emits the read rows; Task 2 consumes the frozen primitive). **No write branch is touched** (that is 3b).
- M2 `(d16,An)` → **excluded from 3a** (Planner resolution; the `das` source stays out of the generator filter and the dispatch table; deferred to 3e).
- M3 long two-word read + high-word latch (`m_alue`) + register write-back (`ext32` `.w` / `set_16h`+`set_16l` `.l`), no flags, no data-write step → Task 2 (the emitter).
- M4 the 5-sub-batch split: **3a is the opener, 6 forms** (Planner resolution: `(d16,An)`→3e) → this plan.
- M5 gate continuity (`drc_native_mem_ea_allowed()` covers 3a unchanged; no new clause, no `SR_S` branch) → Task 3 (arms wrapped in the gate); MMU-attach safety already discharged by O-mem-2 OQ-9.
- M6 merge-gate guidance (three grant modes both backends, AS_OPCODES differential, gate+coverage+MMU-regen, generator additivity, code review + Linux oracle, throughput as evidence-when-available) → Tasks 4 + 5.
- OQ-10 (no partial-grant sweep; accept the by-construction residual) → owner-decided 2026-06-28, recorded in the ADR; **no oracle change in this plan** (the three existing grant modes suffice); the long forms gate on the single `length-4` partial-grant offset (Task 3 Step 6).

**Placeholder scan:** The emitter is literal UML; the per-form variation is one parameterized emitter driven by the generated run, not 6 copies. The `// VERIFY against <handler>:<line>` markers are deliberate single-source gates (the addendum's explicit derive-from-handler requirement), not deferred work. The `aips`/`pais` `m_au`/`m_pc`/`m_at`/`m_da[ry]` choreography is intentionally left as VERIFY-tagged transcription points (the addendum pins the *mechanism*; the exact per-substate field advances are derive-from-handler, R-A) — the `ais` path and the load-bearing latch + write-back are given literally. The retire block is transcribed literally from the existing byte-identical lambda (`m68000drc.cpp:680-698`) rather than referenced, so the emitter is self-contained.

**Type consistency:** `generate_movea_mem(drcuml_block&, const movea_form&, code_label)`; `movea_form{u16 value; u16 mask; u8 ea; u8 size}`; `find_bus_run(u16,u16)` (O-mem-2). `m_dbin`/`m_edb`/`m_alue`/`m_alub`/`m_irc`/`m_ir`/`m_ird`/`m_irdi`/`m_sr`/`m_base_ssw` are `u16` (SIZE_WORD); `m_da[17]`/`m_sp`/`m_aob`/`m_at`/`m_au`/`m_pc`/`m_icount`/`m_inst_state`/`m_next_state`/`m_int_next_state` are `u32` (SIZE_DWORD); `m_inst_substate` is `u16` (SIZE_WORD). `ext32` = `UML_SEXT(SIZE_WORD)`; `set_16h`/`set_16l` combine to `(m_alue<<16)|(m_dbin&0xffff)`. Substates/charges/`pre_charge` are read from the descriptor, never hard-coded. `generate_bus_step` is unchanged (read path; `byte_lane=0`).

**Genuinely-new open questions (design is settled; few expected):**
1. **`m_irdi` equality at the native boundary.** The plan latches `m_irdi = m_ird` and decodes from `I7`. If a future change makes the native dispatch reachable when `m_irdi != m_ird` would matter, the latch (not the `I7` decode) is the safeguard — it is unconditional and cheap. Not blocking.
2. **`m_alub`/`m_dcr` dead-scratch skip.** The plan skips them per the O-mem-1 precedent; the Builder must VERIFY neither is in the oracle compare set at first build. If either is, set it (cheap). Not blocking.
3. **`drc_base_mnemonic` returns `'movea'` vs `'move'`.** Task 1 filters on `base == 'movea'`; if the descriptor folds MOVEA under `move`, the `dst_ea == DRC_EA_AD` clause alone isolates MOVEA (only MOVEA writes an An). The Builder confirms the actual mnemonic/EA-constant names against the `drc_ea_mode` map. A generator-side naming choice the Builder resolves at first regenerate; the additivity + eyeball checks (Task 1 Steps 4–5) catch any over/under-admit.

Neither these nor any M-section item changes the architecture; all are local implementation choices the Builder resolves at the first build.

---

## Merge gate (O-mem-3a)

A batch merges only on **all** of (ADR 0007 O-mem-3 addendum M6, OQ-10-adjusted):
1. **1-cycle Leg B GREEN** (Leg A unchanged; Leg B register/flag/RAM/**cycle** exact), x64 + C — native step-1 + the suspend handoff + the `m_irdi` resume.
2. **Full-grant Leg B GREEN** (`CPUORACLE_M68_DRC_FULLGRANT=1`), x64 + C — the native long latch + register write-back. **Blocking.**
3. **Partial-grant Leg B GREEN** (`CPUORACLE_M68_DRC_PARTGRANT=1`, single `length-4` offset), x64 + C — the mid-instruction resume handoff. **No parameterized sweep** (OQ-10 owner-decided 2026-06-28; the read-high→read-low redo-resume boundary is by-construction).
4. **AS_OPCODES differential GREEN** (`[m68000][drc][asopcodes]`) — the gate keeps `AS_OPCODES`/user-space/MMU machines on `cfunc_`, the fallback matches, and the DRC does not mis-read the opcode space.
5. **Gate-predicate + coverage + MMU-regen GREEN** (`[m68000][drc][gate]`) — the 6 MOVEA forms asserted native; `(d16,An)` MOVEA asserted `cfunc_`; the OQ-9 MMU-regen still green.
6. **Generator additive** — only `m68000-drcdesc.ipp` grew (the 6 new MOVEA read runs), single-sourced from the microcode walk. **No new `step.kind`, no new descriptor field, no `generate_bus_step()` edit.**
7. **Code review clean** + the **appserver Linux `oracle` job GREEN** (Windows-green is necessary-not-sufficient; the Linux job must run all three grant modes; audit every new UML operand for a stray `mem(&field)`).
8. **Throughput bench** — evidence-when-available on a gate-eligible flat-topology driver; **known environment gap (absent ROMs), NOT a hard gate.**

When 1–7 are green, **merge per the auto-merge policy** — do not stop to ask. Subsequent batches: **O-mem-3b** (the word DATA WRITE — the one `generate_bus_step()` primitive edit, reviewed in isolation), then 3c → 3d → 3e (3e absorbs MOVEA `(d16,An)→An`).
