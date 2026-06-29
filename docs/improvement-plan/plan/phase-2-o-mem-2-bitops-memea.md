# O-mem-2 Implementation Plan — write-side `generate_bus_step()` + bit-ops on `(An)`/`(An)+`/`-(An)`

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the write side of the native memory-EA mechanism — `btst`/`bchg`/`bclr`/`bset` with the three register-indirect data EAs `(An)`/`(An)+`/`-(An)`, in **both** the `#imm8` and `Dn` source forms (**24 forms**: `btst`×6 read-only, `bchg`/`bclr`/`bset`×18 RMW) — by extending `generate_bus_step()` with a `UML_WRITEM` data-write step, adding the auto-increment/decrement EA arithmetic, and reusing O-mem-1's read step and per-bus-cycle suspend checkpoint. Gated cycle-exact by the dual-leg oracle **plus** a new fully-granted Leg-B pass that actually exercises the native write.

**Architecture:** O-mem-2 is the second increment of **boundary O** (phase-2 Task 10), the write-side design pinned by the **[ADR 0007 O-mem-2 addendum](../adr/0007-m68000-native-memory-ea-suspend-mechanism.md#addendum--resolution-2026-06-28--o-mem-2-write-side-mechanism)** (sections W1–W5/W3b, Task 0a, OQ-6…OQ-9). The write reuses the *identical* flag-not-longjmp checkpoint as the read (`write_interruptible` queries the same `m_access_to_be_redone`); the only differences from a read step are the access op (`UML_WRITEM` with the byte-lane mask, value from `m_dbout`), the value source (the caller's `set_8xl`), and the absence of an address-error branch. The RMW opcodes compose **ext-fetch (imm8 only) → EA setup → data-read → modify+refill-prefetch → data-write → retire**; `btst` is read-only (no write). The bit is modified at the refill-prefetch state, but **Z is set from the ORIGINAL byte before the write** (W3, the highest-risk ordering bug). All per-step cycle charges, substate pairs, the predecrement internal `−2`, and the new `DATA_WRITE` rows are **single-sourced from `m68000gen.py`** (ADR R-A is the top risk; do not hand-transcribe). Every native arm stays wrapped in the O-mem-1 compile-time `drc_native_mem_ea_allowed()` gate — no new gate clause, no `SR_S` runtime branch.

**Tech Stack:** C++17 (the `m68000_device` / DRCUML emitter, drcbe_x64 + drcbec backends), Python 3 (the `m68000gen.py` generator), GENie/`mingw32-make` build, Catch2 oracle harness (`tests/emu/cpu/cpuoracle.cpp` + `cpu_test_harness.{cpp,h}`). MSYS2 UCRT64/MINGW64 toolchain on Windows; appserver Linux for the authoritative `oracle` CI job.

> ⚠ **REQUIRED before any task — read the [ADR 0007 O-mem-2 addendum](../adr/0007-m68000-native-memory-ea-suspend-mechanism.md#addendum--resolution-2026-06-28--o-mem-2-write-side-mechanism) in full.** Every mechanism decision (the `UML_WRITEM` write step, the EA arithmetic with the A7-byte-by-2 rule, the predecrement internal `−2`, the fully-granted Leg-B pass, OQ-6/7/8/9) is already owner-decided there. This plan implements those answers; it does not re-derive them. **Two facts gate everything:** (1) the native write is **unreachable under the current 1-cycle oracle** — the fully-granted Leg-B pass (Task 0a) is mandatory new scaffolding and a **blocking merge gate**, run FIRST against the existing native `btst`-absolute to baseline O-mem-1's tail; (2) `SPACE_PROGRAM` for the write is correct **only** behind the O-mem-1 `drc_native_mem_ea_allowed()` gate, which already covers O-mem-2 unchanged (W6).

## Global Constraints

These apply to **every** task below — copied forward from ADR 0007 + the O-mem-1 plan + the cut-line doc + the user's workflow rules, with the O-mem-2 deltas marked.

- **THE GATE for every DRC-touching task is the dual-leg oracle (Leg A + Leg B, register/flag/RAM/CYCLE-exact) GREEN on the appserver LINUX `oracle` CI job.** Local Windows green is **necessary-not-sufficient** — boundary M passed Windows but failed Linux twice on `drcbe_x64 offset_from_rbp`. The `oracle` CI job is NOT preflight (preflight is a tiny-build smoke; do not confuse them).
- **O-mem-2 delta — the fully-granted Leg-B pass is a SECOND required gate.** Standard Leg B (1-cycle stepping) only validates each native opcode's *first* bus step; the native write is bus step 4 (RMW) and is unreachable under it. The fully-granted pass (Task 0a, env `CPUORACLE_M68_DRC_FULLGRANT=1`) grants `length` then drains at 1 so the whole instruction — including the write — runs native. **A batch merges only when BOTH passes are green, on x64 (`drcbex64`) AND C (`drcbec`), Leg A unchanged.**
- **ABI-safe codegen:** zero plain `mem(&device_field)` operands. Every device-state field is accessed via `UML_LOAD`/`UML_STORE` with a pointer base (the boundary-M / O-mem-1 pattern in `m68000drc.cpp`). Plain `mem(&m_field)` is lowered RBP-relative by drcbe_x64 and throws `offset_from_rbp` when the heap device is >2 GiB from the high-mmap'd RWX cache on Linux/SysV. `UML_WRITEM`'s address/value/mask are UML registers, so the write op itself is inherently ABI-safe; the field plumbing around it (`m_aob`, `m_dbout`, `m_da[]`, `m_sp`, `m_icount`, `m_inst_substate`, `m_sr`, …) all goes through LOAD/STORE.
- **Generator additivity:** regenerating must leave existing generated decode files **byte-identical** (empty `git diff` on `m68000-decode.cpp`, `m68000-head.h`, `m68000-s{d,i}{f,p}.cpp`). Only `m68000-drcdesc.ipp` may grow (the new `DATA_WRITE` rows + the `(An)/(An)+/-(An)` runs + the predecrement internal `−2`). The `enum_str()` shim preserves byte-identity on Python ≥ 3.11 — do not remove it.
- **Single-source rule (ADR 0007 R-A, the top risk):** the bus-step list — kinds, charges, the redo/completed substate pair, the predecrement internal `−2`, the `DATA_WRITE` lane flag — is emitted from the SAME microcode walk that generates the interpreter handler (`drc_bus_steps()` parses the very handler text `generate_source_from_code` emits). **Never hand-type a substate number or charge into the emitter.** A hand-written step table is *not* an acceptable fallback for O-mem-2 (it was a documented temporary fallback for O-mem-1 only; the generator path is now proven).
- **Build env (verified recipe):** `MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd <worktree>; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'`, then `./mametests "[m68000]"`. Do **NOT** use `make SOURCES=...m68000.cpp` (SOURCES filters by *driver*; a CPU device has no driver and GENie errors). `make REGENIE=1` is required whenever a `.cpp`/`.h` file is added; the m68000 core builds as part of the `TESTS=1` target.
- **Backend matrix:** test both oracle passes on **x64 (`drcbex64`)** AND the **C backend (`CPUORACLE_M68_DRC_C=1`)**. arm64 (`drcbearm64`) is deferred to the CI matrix.
- **Cycle truth:** the interpreter (`m68000.cpp` under `-drc 0`) is the authority. The DRC mirrors its cycle count; divergence is always a DRC bug, resolved by routing the offending form back to `cfunc_` (drop it from `is_native_opcode` / the dispatch table) — never by editing the interpreter or weakening an assertion.
- **Scope lock:** O-mem-2 = the **24 bit-op forms** only (`btst`/`bchg`/`bclr`/`bset` × `(An)`/`(An)+`/`-(An)` × {`#imm8`, `Dn`}). Memory-EA `MOVE` (O-mem-3), `ALU` (O-mem-4), the `(d16,An)`/`(d16,PC)` EAs, `movem`/`movep`/`tas`/BCD, and **native `AS_OPCODES` space-selection** are all **out of scope** (the gate keeps `AS_OPCODES` machines on `cfunc_`; Task 0 only *validates* the gate, it does not lift it). The deferring-tap oracle config (OQ-6) stays deferred.
- **Style:** match the existing m68000 brace/whitespace style (tabs, the K&R-ish style already in `m68000drc.cpp`); license header `// license:BSD-3-Clause` / `// copyright-holders:Mark Mackelprang` on any new file; run `srcclean` (built via `TOOLS=1`) on touched files before committing. Work on a short-lived branch and open a PR (never commit DRC source straight to `main`).

---

## The interpreter shapes O-mem-2 must mirror (read before any task)

Every native form is a *transcription* of its `_df` handler in `m68000-sdf.cpp` — the one the plain `m68000_device` runs (the `_dp`/`_dfm`/`_df8` variants belong to other cores/subclasses, out of scope). **Read each handler line-by-line while emitting its form** (R-A is the top risk). The shapes below are verified against the tree.

### The RMW skeleton (verified against `bchg_imm8_ais_df`, `m68000-sdf.cpp:20703`)

```cpp
// --- ext-fetch (o#w1) : imm8 source ONLY ; substates 1/2 ; PROGRAM ; addr-error ---
m_aob = m_au; m_pc = m_au; set_16l(m_dt, m_dbin); m_au += 2; m_base_ssw = SSW_PROGRAM|SSW_R;
m_edb = m_opcodes.read_interruptible(m_aob & ~1);   // <generate_bus_step prefetch read>
// (commit) m_irc = m_edb; m_dbin = m_edb;
// --- EA setup (adrw1/adrw2 for (An)) : m_dcr is the bit number source ---
m_aob = m_da[ry]; m_at = m_da[ry];                  // (An): no reg writeback
m_dcr = m_dt;                                        // imm8: bit# from the ext word ; Dn: m_dcr = m_da[rx]
m_base_ssw = SSW_DATA|SSW_R;
m_edb = m_program.read_interruptible(m_aob & ~1, m_aob & 1 ? 0x00ff : 0xff00);  // DATA read, byte-lane
if(!(m_aob & 1)) m_edb >>= 8;                        // <generate_bus_step DATA read ; substates 3/4 ; byte_lane=1 ; NO addr-error>
// (commit) m_dbin = m_edb;                          // m_dbin = the ORIGINAL byte
// --- modify + refill prefetch (bcsm1) : substates 5/6 ; PROGRAM ; addr-error ---
m_aob = m_au; m_ir = m_irc; m_alub = m_dbin; m_pc = m_au; m_au += 2;   // m_alub = ORIGINAL byte (saved for Z)
alu_eor8(m_dbin, 1 << (m_dcr & 7));                  // m_aluo = MODIFIED byte (bchg=eor; bclr=and-not; bset=or)
m_base_ssw = SSW_PROGRAM|SSW_R;
m_edb = m_opcodes.read_interruptible(m_aob & ~1);   // <generate_bus_step prefetch read>
// (commit) m_irc = m_edb; m_dbin = m_edb;           // m_dbin now holds the prefetch word (original is in m_alub)
// --- data write (bcsm2) : substates 7/8 ; DATA ; NO addr-error ---
m_aob = m_at; m_ird = m_ir; if(m_next_state != S_TRACE) m_next_state = m_int_next_state;
set_8xl(m_dbout, m_aluo);                            // m_dbout = MODIFIED byte replicated into both lanes
alu_and8(m_alub, 1 << (m_dcr & 7)); sr_z();          // Z from the ORIGINAL byte (m_alub), set BEFORE the write
m_base_ssw = SSW_DATA;                               // SSW_DATA only (R bit CLEAR — it is a write)
m_program.write_interruptible(m_aob & ~1, m_dbout, (m_aob & 1) ? 0x00ff : 0xff00);  // <generate_bus_step WRITE>
// (retire) set_ftu_const(); m_inst_state = m_next_state ? m_next_state : m_decode_table[m_ird];
//          if(m_sr & SR_T) m_next_state = S_TRACE;
```

### Per-family ALU op at the modify step (W3 — the ONLY per-family difference)

| Family | Encoding op-field | Modify (`modified` from `original`) | UML | Verify against |
|---|---|---|---|---|
| `bchg` | 01 | `original ^ (1 << bit)` (toggle) | `UML_XOR` | `bchg_imm8_ais_df:20759` |
| `bclr` | 10 | `original & ~(1 << bit)` (clear) | `UML_AND` with `~mask` | `bclr_imm8_ais_df` (verify the `alu_or8`+`alu_eor8` net == and-not) |
| `bset` | 11 | `original \| (1 << bit)` (set) | `UML_OR` | `bset_imm8_ais_df` |
| `btst` | 00 | *(no modify, no write)* | — | `btst_imm8_ais_df:19582` |

`Z` is identical for all four: `Z = !(original & (1 << (m_dcr & 7)))`, set at the write state (bcsm2) for RMW, or at the final-prefetch state for `btst`, from the original byte. `N`/`V`/`C`/`X` untouched.

### EA arithmetic per mode (W2 — byte access; verified)

`ry = map_sp((m_ird & 7) | 8)`: if `(m_ird & 7) == 7` use `m_sp` (the active-A7 bank index, 15 or 16); else `(m_ird & 7) | 8`. `delta = (ry < 15 ? 1 : 2)` — byte access to `(A7)+`/`-(A7)` adjusts SP by **2** to stay word-aligned (A0–A6 = indices 8–14 → 1; either A7 bank = 15/16 → 2).

| Mode | token | Setup (verified) | reg writeback | internal charge |
|---|---|---|---|---|
| `(An)` | `ais` | `m_aob = m_at = m_da[ry]` | none | none |
| `(An)+` | `aips` | `m_aob = m_at = m_da[ry]` (old); `m_da[ry] = m_da[ry] + delta` after latch | post-inc | none |
| `-(An)` | `pais` | `m_au = m_da[ry] - delta; m_aob = m_at = m_au; m_da[ry] = m_au` | pre-dec | **`m_icount -= 2`** at `pdcw1`, **no** suspend checkpoint |

Verified: `(An)` `bchg_imm8_ais_df:20731`; `(An)+` `bchg_imm8_aips_df:20832-20840`; `-(An)` `bchg_imm8_pais_df:20937-20948` (the internal `−2` at `:20944`).

### `Dn`-source forms (W3b — verified against `bchg_dd_ais_df:6770`)

Identical write step, EA arithmetic, and Z/modify as `#imm8`, with two differences only: (1) **no ext-fetch** (`m_dcr = m_da[rx]`, `rx = (m_ird >> 9) & 7`, a register read with no bus cycle); (2) the substate ladder shifts down by one read. Encodings: family base `0x0100` (mask `0xf1f8`), `+0x40/0x80/0xC0` for `bchg`/`bclr`/`bset`, `+0x10/0x18/0x20` for `(An)`/`(An)+`/`-(An)`.

### The 24 forms and their substate ladders (single-source these from the generator; do NOT hand-transcribe)

| # | Form | Encoding `{value, mask}` | bus steps | substate ladder (kind) |
|---|---|---|---|---|
| 1–3 | `btst #n,(An)/(An)+/-(An)` | `0x0810/0x0818/0x0820, 0xfff8` | 3 | ext 1/2 · data 3/4 · final-prefetch 5/6 |
| 4–6 | `bchg #n,(An)/(An)+/-(An)` | `0x0850/0x0858/0x0860, 0xfff8` | 3R+1W | ext 1/2 · data 3/4 · refill 5/6 · **write 7/8** |
| 7–9 | `bclr #n,(An)/(An)+/-(An)` | `0x0890/0x0898/0x08A0, 0xfff8` | 3R+1W | ext 1/2 · data 3/4 · refill 5/6 · **write 7/8** |
| 10–12 | `bset #n,(An)/(An)+/-(An)` | `0x08D0/0x08D8/0x08E0, 0xfff8` | 3R+1W | ext 1/2 · data 3/4 · refill 5/6 · **write 7/8** |
| 13–15 | `btst Dn,(An)/(An)+/-(An)` | `0x0110/0x0118/0x0120, 0xf1f8` | 2 | data 1/2 · final-prefetch 3/4 |
| 16–18 | `bchg Dn,(An)/(An)+/-(An)` | `0x0150/0x0158/0x0160, 0xf1f8` | 2R+1W | data 1/2 · refill 3/4 · **write 5/6** |
| 19–21 | `bclr Dn,(An)/(An)+/-(An)` | `0x0190/0x0198/0x01A0, 0xf1f8` | 2R+1W | data 1/2 · refill 3/4 · **write 5/6** |
| 22–24 | `bset Dn,(An)/(An)+/-(An)` | `0x01D0/0x01D8/0x01E0, 0xf1f8` | 2R+1W | data 1/2 · refill 3/4 · **write 5/6** |

For `-(An)` forms the `pais` internal `−2` is folded before the data read (no substate). **Builder must confirm every `{value, mask}` against the handler's `// xxxx ffff` comment** (e.g. `bchg_imm8_ais_df // 0850 fff8`, `bchg_dd_ais_df // 0150 f1f8`) before wiring — a wrong mask silently mis-dispatches.

---

## File Structure

| File | Responsibility | Touched in |
|---|---|---|
| `tests/emu/cpu/cpu_test_harness.cpp` | `oracle_m68000_device::step_instruction` — add the full-grant mode; the AS_OPCODES differential driver + opcode-space load/read plumbing | Tasks 0a, 0 |
| `tests/emu/cpu/cpu_test_harness.h` | harness passthroughs for the AS_OPCODES differential descriptor | Task 0 |
| `tests/emu/cpu/cpuoracle.cpp` | the AS_OPCODES differential `TEST_CASE`; the MMU-regen `TEST_CASE`; coverage assertions | Tasks 0, 5, 6 |
| `src/devices/cpu/m68000/m68000gen.py` | Generator — widen `drc_bus_steps()` to the 24 forms; parse the `write_interruptible` step (`DATA_WRITE`) + the predecrement internal `−2`; additive-only | Task 1 |
| `src/devices/cpu/m68000/m68000-drcdesc.ipp` | Generated descriptor table — regenerated; existing rows byte-identical, new bit-op runs appended | Task 1 |
| `src/devices/cpu/m68000/m68000.h` | declare `generate_bitop_mem`, the `bitop_form` struct, `find_bus_run`; the `pre_charge` field on `drc_bus_step` (if chosen) is generated into the `.ipp`, not here | Tasks 2, 3 |
| `src/devices/cpu/m68000/m68000drc.cpp` | `generate_bus_step()` write branch; the EA/bit-modify helpers; `generate_bitop_mem()`; the 24 dispatch arms; `is_native_opcode` patterns | Tasks 2, 3, 4 |
| `src/devices/cpu/m68000/m68000.cpp` | OQ-9: `m_cache_dirty = true` in `set_current_mmu()` / `enable_mmu()` | Task 5 |
| `src/devices/cpu/m68000/README-drc.md` | move the 24 forms from `cfunc_` to native (behind the gate); record the write side | Task 6 |

No new `.cpp`/`.h` file is created (all emitters live in the existing `m68000drc.cpp`); `make REGENIE=1` is run once per build because Task 1 regenerates the `.ipp`.

---

## Task 0a: The fully-granted Leg-B pass — baseline O-mem-1's native tail FIRST

**Goal:** Add a second DRC stepping mode to the oracle stepper that grants the whole instruction's cycles on the first iteration (then drains at 1), so a multi-step native opcode runs **every** bus step natively — including, for O-mem-2, the **data write**. Run it FIRST against the **already-merged** native `btst`-absolute to establish a clean baseline of O-mem-1's native tail (reads 2–5, the byte-lane data read, `compute_z`) under the correctness gate — to date validated only by the throughput benchmark. **Any divergence this surfaces on `btst`-absolute is in-scope for this PR to fix** (ADR W4, OQ-7). This task ships no opcode; it is the scaffolding every later O-mem-2 task depends on.

**Files:**
- Modify: `tests/emu/cpu/cpu_test_harness.cpp` — `oracle_m68000_device::step_instruction(int budget)` (`:374-453`).

**Interfaces:**
- Produces (consumed by Tasks 0, 1–6 oracle runs): a `CPUORACLE_M68_DRC_FULLGRANT=1` env toggle that switches the m68000 stepper to grant `budget` (= the corpus `length`, already passed at `cpuoracle.cpp:1102`) on the first iteration. The comparison, snapshot, retirement detection, and `SR.T` freeze logic are **unchanged**.

> **Design (ADR W4, baked in):**
> - **Grant `length` first, then drain at 1.** The current loop hard-codes `*m_icountptr = 1` every iteration (`:431`). Full-grant sets `*m_icountptr = budget` on the **first** iteration and `1` thereafter; the accumulation becomes `consumed += before - *m_icountptr` on the first iteration (`1 - *m_icountptr` is valid only for a unit grant).
> - **Why it runs the WHOLE native instruction incl. the write.** With `m_inst_substate == 0` the boundary-M entry guard admits the native block; with `m_icount == length`, every `generate_bus_step` finds `m_icount > 0` until the *last* access, so read-EA → data-read → modify → **data-write** all run native in one pass. After the final access's `−4`, `m_icount == 0`, the suspend checkpoint fires (`≤ 0`), stores the *completed* substate, and yields — exactly as the interpreter leg. The bus-free **retire** runs under the interpreter in the 1-cycle drain.
> - **NOT `length + headroom`.** Overshoot makes the final access find `m_icount > 0`, so the native retire runs, then the block JMPs to `cfunc_interpret_quantum` which runs the *next* instruction's prefetch, advancing `m_ipc`/`m_pc`/`m_au` before `run()` returns — the harness can no longer snapshot instruction-1's retired state. `length` is the exact sweet spot.
> - **`SR.T` handled for free.** The native phase stops at `m_icount == 0` before retire, never reaching the post-retire `S_TRACE` set; the retire+trace run in the 1-cycle drain under the unchanged `m_inst_state == S_TRACE` freeze (`:424-430`). No special handling.

- [x] **Step 1: Read the stepper and confirm `budget` is `length`.** Read `cpu_test_harness.cpp:374-453` (`oracle_m68000_device::step_instruction`) and the call site `cpuoracle.cpp:1102` (`harness.step_one_instruction(expected_cycles)` where `expected_cycles = test["length"]`). Confirm `budget` carries `length` end-to-end (`oracle_step(int budget)` `:511` → `step_instruction(budget)`), and that today it is discarded at `:376` (`(void)budget;`).

- [x] **Step 2: Add the env toggle + first-grant sizing.** In `oracle_m68000_device::step_instruction`, replace the `(void)budget;` discard and the in-loop `*m_icountptr = 1;` / `consumed += 1 - *m_icountptr;` with a full-grant-aware version. Read the env flag once.

Replace `(void)budget;` (`:376`) with:
```cpp
		// Full-grant Leg-B pass (ADR 0007 W4 / OQ-7): grant the whole instruction's
		// cycles on the FIRST iteration so a multi-step native opcode runs every bus
		// step -- including the data WRITE -- natively, then drain at 1.  Selected by
		// env (alongside CPUORACLE_M68_DRC_C).  When unset, behaviour is the legacy
		// 1-cycle-per-iteration stepping (validates native step 1 + the suspend handoff).
		static const bool s_full_grant = (std::getenv("CPUORACLE_M68_DRC_FULLGRANT") != nullptr);
		const int first_grant = s_full_grant ? (budget > 0 ? budget : 1) : 1;
```
Replace the in-loop grant `*m_icountptr = 1; run();` (`:431-432`) with:
```cpp
			const int grant = (guard == 0) ? first_grant : 1;
			*m_icountptr = grant;
			run();
```
Replace the accumulation `consumed += 1 - *m_icountptr;` (`:449`) with:
```cpp
			consumed += grant - *m_icountptr;
```

> **Note:** `<cstdlib>` must be included for `std::getenv` — confirm it is already pulled in (the C-backend toggle uses the same header pattern; grep `CPUORACLE_M68_DRC_C` in `cpuoracle.cpp` / harness). If not, add `#include <cstdlib>` at the top of `cpu_test_harness.cpp`. Read the env once into a `static const` so the corpus loop pays no per-case cost.

- [x] **Step 3: Build.**
```bash
MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd "$PWD"; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'
```
Expected: clean build.

- [x] **Step 4: Legacy pass unchanged (regression guard).**
```bash
./mametests "[m68000]"            # Leg A
./mametests "[m68000][drc]"       # Leg B, legacy 1-cycle (env unset) -- must be IDENTICAL to pre-change
```
Expected: both green. With the env unset, `first_grant == 1` and `grant == 1` always, so this is byte-equivalent to the old stepper.

- [x] **Step 5: Full-grant pass against the EXISTING native `btst`-absolute (the baseline).**
```bash
CPUORACLE_M68_DRC_FULLGRANT=1 ./mametests "[m68000][drc]"                       # x64
CPUORACLE_M68_DRC_FULLGRANT=1 CPUORACLE_M68_DRC_C=1 ./mametests "[m68000][drc]" # C backend
```
Expected: **green.** This is the first correctness-gate coverage of O-mem-1's native tail (reads 2–5, byte-lane data read, `compute_z`, the full 4-/5-read sequence run native in one pass). **If it fails:** O-mem-1's tail has a latent bug the 1-cycle pass never reached — use `superpowers:systematic-debugging`; the failure names register/flag/RAM/cycle and the case. Fix it **in this PR** (it is the same mechanism family). Do not proceed to Task 0 until both backends are green here — a clean baseline makes a later write-path failure unambiguous.

- [x] **Step 6: `srcclean` + commit.**
```bash
git add tests/emu/cpu/cpu_test_harness.cpp
git commit -m "test(m68000drc): add fully-granted Leg-B oracle pass (ADR 0007 W4); baselines O-mem-1 native tail"
```

---

## Task 0: AS_OPCODES differential oracle — prove the gate keeps such drivers on `cfunc_`

**Goal:** Build the second Leg-B config the §5 addendum parked as "O-mem-2's opening task" (O-mem-1 Task 8 deferred it): a real separate `AS_OPCODES` space holding content **distinct** from `AS_PROGRAM`, run `-drc 0` vs `-drc 1`. With the gate in place the native arm is **not** emitted there, so this proves (a) the `cfunc_` fallback matches the interpreter and (b) the DRC does not mis-read the opcode space — i.e. it is the test that would have caught the original O-mem-1 wrong-space bug. It does **not** enable native `AS_OPCODES` (that stays out of scope). A merge gate for O-mem-2.

**Files:**
- Modify: `tests/emu/cpu/cpu_test_harness.cpp` — a new driver variant `oracle_m68000_asopcodes_diff_state` with a *distinct-content* `AS_OPCODES` map + opcode-space load path; a `m68000_asopcodes_diff_core_descriptor()`; `GAME`/`driver_list` entries.
- Modify: `tests/emu/cpu/cpu_test_harness.h` — declare the descriptor + the opcode-space write passthrough.
- Modify: `tests/emu/cpu/cpuoracle.cpp` — the differential `TEST_CASE`.

> **Reuse the O-mem-1 gate scaffolding.** `cpu_test_harness.cpp:790+` already has `oracle_m68000_asopcodes_state` (a *content-shared* AS_OPCODES variant used by the Task-7 gate-predicate unit test). Task 0 needs a *content-distinct* variant: the `AS_OPCODES` map must hold different bytes than `AS_PROGRAM` at the opcode-fetched cells, so a hypothetical un-gated native prefetch (reading `SPACE_PROGRAM`) would observably diverge from the interpreter (reading `m_opcodes`). Model the new variant on the existing one; the delta is the distinct backing store + the corpus PROGRAM/OPCODES split.

**Interfaces:**
- Consumes: the corpus, `drc_native_mem_ea_allowed()` (false on this topology by construction), the fully-granted pass from Task 0a (run this differential under both stepping modes).

- [x] **Step 1: Read the existing AS_OPCODES gate variant + the harness RAM paths.** Read `cpu_test_harness.cpp:790-840` (`oracle_m68000_asopcodes_state`, `prog_map`/`opc_map`) and the RAM I/O paths `write_ram`/`read_ram`/`snapshot_retired` (`:462-475`, `:1184-1192` — they hard-code `AS_PROGRAM`). The differential config needs `AS_OPCODES` to resolve to a **separate** `address_space` whose RAM is loaded *separately* from `AS_PROGRAM`.

- [x] **Step 2: Add the distinct-content AS_OPCODES driver variant.** In `cpu_test_harness.cpp`, beside the existing `oracle_m68000_asopcodes_state`:
```cpp
// Differential AS_OPCODES variant (O-mem-2 Task 0): AS_OPCODES is a SEPARATE
// space with its OWN 16 MiB RAM, loaded with the corpus opcode bytes; AS_PROGRAM
// holds the data bytes.  gate FALSE (m_s_program != m_s_opcodes) -> native arm
// not emitted -> proves cfunc_ ≡ interpreter AND that the DRC reads the right
// space.  (Distinct CONTENT is what separates this from the Task-7 gate-predicate
// variant, which shares content.)
class oracle_m68000_asopcodes_diff_state : public driver_device
{
public:
	oracle_m68000_asopcodes_diff_state(const machine_config &mconfig, device_type type, const char *tag) :
		driver_device(mconfig, type, tag), m_cpu(*this, "maincpu") { }
	void m68000_asopcodes_diff_machine(machine_config &config) ATTR_COLD;
	virtual std::vector<std::string> searchpath() const override { return {}; }
private:
	void prog_map(address_map &map) ATTR_COLD { map(0x000000, 0xffffff).ram(); }
	void opc_map (address_map &map) ATTR_COLD { map(0x000000, 0xffffff).ram(); } // SEPARATE backing RAM
	required_device<oracle_m68000_device> m_cpu;
};
void oracle_m68000_asopcodes_diff_state::m68000_asopcodes_diff_machine(machine_config &config)
{
	ORACLE_M68000(config, m_cpu, 8_MHz_XTAL);
	m_cpu->set_addrmap(AS_PROGRAM, &oracle_m68000_asopcodes_diff_state::prog_map);
	m_cpu->set_addrmap(AS_OPCODES, &oracle_m68000_asopcodes_diff_state::opc_map);
}
```
Add `INPUT_PORTS_START`/`ROM_START`/`GAME(...)` (copy the `oraclem68000` block) and a hand-sorted `driver_list::s_drivers_sorted[]` entry + `s_driver_count` bump (a short name that keeps the array sorted, e.g. `oraclem68kad`). Add the descriptor:
```cpp
const cpu_core_descriptor &m68000_asopcodes_diff_core_descriptor()
{
	static const cpu_core_descriptor desc = { "m68000_asopcodes_diff", &GAME_NAME(oraclem68kad), s_m68000_regmap };
	return desc;
}
```

- [x] **Step 3: Add the opcode-space load path.** The differential test must seed `AS_OPCODES` independently of `AS_PROGRAM`. Add a harness method that writes a byte to `AS_OPCODES` (mirroring `write_ram`'s `AS_PROGRAM` path but with `space(AS_OPCODES)`), exposed only on the differential stepper:
```cpp
// In oracle_m68000_device (cpu_test_harness.cpp), beside the AS_PROGRAM write_ram path:
void write_opcode_ram(u32 addr, u8 val) { space(AS_OPCODES).write_byte(addr, val); }
```
Add a virtual on `oracle_stepper` (default no-op) + a `cpu_test_harness::write_opcode_ram(addr,val)` passthrough in `cpu_test_harness.{h,cpp}`, mirroring `write_ram`. When the bound device has no separate `AS_OPCODES`, the call routes (via fallback) to `AS_PROGRAM` — harmless for the flat configs.

- [x] **Step 4: Write the differential `TEST_CASE`.** In `cpuoracle.cpp`, a case that loads the corpus instruction stream into `AS_OPCODES` (opcode bytes) and the data into `AS_PROGRAM`, runs the same `run_one_case` comparison `-drc 0` vs `-drc 1`, and asserts equality. The PROGRAM/OPCODES split: opcode/prefetch addresses (the instruction stream the corpus places at the PC and the bit-op extension words) load into `AS_OPCODES`; the data EA cells load into both (or perturb the opcode-only cells so an un-gated native prefetch would diverge). Tag `[cpu][m68000][drc][asopcodes]`.
```cpp
TEST_CASE("CPU oracle m68000 Leg B -- separate AS_OPCODES (gate keeps cfunc_)", "[cpu][m68000][drc][asopcodes]")
{
	// Same replay_all/run_one_case shape as the flat Leg-B case, but:
	//   - descriptor = m68000_asopcodes_diff_core_descriptor()
	//   - seed the instruction/prefetch words via harness.write_opcode_ram(...) into
	//     AS_OPCODES with content DISTINCT from the AS_PROGRAM data bytes
	//   - assert drc_native_mem_ea_allowed() == false (anti-vacuity: the gate IS off)
	//   - REQUIRE interpreter == DRC (register/flag/RAM/cycle) -- the cfunc_ fallback
	//     matches AND the DRC did not mis-read the opcode space.
	// Run under BOTH stepping modes (legacy + CPUORACLE_M68_DRC_FULLGRANT).
}
```

> **Design note (carried from O-mem-1 Task 8):** deciding which corpus cells are opcode bytes vs data bytes — and perturbing an opcode-only cell so a buggy un-gated native prefetch would *observably* diverge — is the non-trivial part. Keep it minimal: the corpus's PC-region words (the instruction + its extension words) are opcode-space; everything the EA touches is data-space. Document the split inline. If the corpus's flat model makes a clean split impractical for some cases, it is acceptable to restrict this differential to the bit-op corpus subset (the opcodes O-mem-2 makes native) — that is the set whose native path the gate must guard.

- [x] **Step 5: Build + run (both stepping modes, both backends).**
```bash
MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd "$PWD"; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'
./mametests "[m68000][drc][asopcodes]"
CPUORACLE_M68_DRC_C=1 ./mametests "[m68000][drc][asopcodes]"
CPUORACLE_M68_DRC_FULLGRANT=1 ./mametests "[m68000][drc][asopcodes]"
```
Expected: green — gate off (anti-vacuity), interpreter ≡ DRC on the AS_OPCODES topology.

- [x] **Step 6: `srcclean` + commit.**
```bash
git add tests/emu/cpu/cpu_test_harness.h tests/emu/cpu/cpu_test_harness.cpp tests/emu/cpu/cpuoracle.cpp
git commit -m "test(m68000drc): AS_OPCODES differential oracle (gate keeps cfunc_; distinct opcode-space content)"
```

---

## Task 1: Generator extension — emit `DATA_WRITE` rows + predecrement `−2` for the 24 forms (additive)

**Goal:** Single-source the O-mem-2 bus-step lists (OQ-2/W5) from `m68000gen.py` so the emitter takes the write step's charge + substate pair, the predecrement internal `−2`, and the byte-lane flag from the same microcode walk the interpreter uses. Additive-only: existing generated files stay byte-identical; only `m68000-drcdesc.ipp` grows.

**Files:**
- Modify: `src/devices/cpu/m68000/m68000gen.py` — `drc_bus_steps()` (`:2423`), the `drc_bus_step` struct emission (`:2761`), the run-collection loop (`:2718-2741`).
- Regenerate: `src/devices/cpu/m68000/m68000-drcdesc.ipp`.
- Validate against: `m68000-sdf.cpp` (the 24 handlers).

**Interfaces:**
- Produces (consumed by Tasks 2–4): bus runs for all 24 `{value, mask}` patterns; the flat step table gains `DATA_WRITE` rows (kind 2) and a predecrement representation. The chosen predecrement model is a **`pre_charge` field on the data-read step** (W5's first option): `struct drc_bus_step` gains a trailing `u8 pre_charge;` (cycles charged with **no** suspend checkpoint immediately before the access — 2 for `-(An)`, 0 otherwise). This keeps the run a clean per-bus-access sequence; the emitter charges `pre_charge` before issuing the access.

- [x] **Step 1: Verify the round-trip is additive-clean (baseline).** Regenerate all outputs and confirm an empty diff (from `src/devices/cpu/m68000/`):
```bash
python m68000gen.py decode m68000.lst m68000-decode.cpp
python m68000gen.py header m68000.lst m68000-head.h
python m68000gen.py sdf    m68000.lst m68000-sdf.cpp
python m68000gen.py sif    m68000.lst m68000-sif.cpp
python m68000gen.py sdp    m68000.lst m68000-sdp.cpp
python m68000gen.py sip    m68000.lst m68000-sip.cpp
python m68000gen.py drcdesc m68000.lst m68000-drcdesc.ipp
git diff --stat
```
Expected: empty. If `m68000-drcdesc.ipp` differs, the tree is stale or Python differs — resolve before editing.

- [x] **Step 2: Pin the 24 handlers' bus-step truth.** Read each family's three EA handlers (`btst`/`bchg`/`bclr`/`bset` × `ais`/`aips`/`pais`, `#imm8` and `dd`). Record per step: the `m_icount -= N` charge(s) (note the `−2` at `pdcw1` for `pais`, which has **no** suspend checkpoint), the redo/completed substate pair, whether an `if(m_aob & 1)` branch follows (prefetch reads only), and the write line `m_program.write_interruptible(m_aob & ~1, m_dbout, (m_aob&1)?0x00ff:0xff00)` (the write step — DATA, byte-lane, **no** addr-error). This is the table in the plan header; the generator must emit *these exact numbers*.

- [x] **Step 3: Add the `pre_charge` field to `drc_bus_step`.** In `generate_drcdesc_file` (`:2761`), extend the struct and the row format:
```python
    print("struct drc_bus_step {", file=out)
    print("\tu8 kind; u8 size; u8 charge;", file=out)
    print("\tu8 redo_substate; u8 completed_substate;", file=out)
    print("\tu8 has_addr_error; u8 byte_lane; u8 pre_charge;", file=out)   # + pre_charge
    print("};", file=out)
```
and the per-row print (`:2772-2775`) gains the trailing field:
```python
    for kind, size, charge, redo, completed, addr_err, lane, pre, disp in bus_steps:
        print("\t{ %-21s, %-12s, %d, %d, %d, %d, %d, %d }, // %s" % (
            bus_kind_name[kind], drc_size_name[size], charge,
            redo, completed, addr_err, lane, pre, disp), file=out)
```
(The flat-table append at `collect_bus_run` `:2724-2725` must carry the extra tuple element — see Step 5.)

- [x] **Step 4: Widen `drc_bus_steps()` to the 24 forms + parse writes and the `−2`.** In `m68000gen.py`, replace the O-mem-1 opcode filter (`:2443-2447`) and extend the parser (`:2456-2531`):
```python
    base = drc_base_mnemonic(ii[2][0])
    src_ea = drc_ea_mode[ii[2][1]]
    dst_ea = drc_ea_mode[ii[2][2]]
    # O-mem-1: btst absolute.  O-mem-2: btst/bchg/bclr/bset with (An)/(An)+/-(An),
    # #imm8 AND Dn source.  (Everything else still returns [] -- additive.)
    BITOPS = ('btst', 'bchg', 'bclr', 'bset')
    REGIND = (DRC_EA_AIS, DRC_EA_AIPS, DRC_EA_PAIS)   # (An), (An)+, -(An)
    is_o1 = (base == 'btst' and src_ea == DRC_EA_IMM and dst_ea in (DRC_EA_ABSW, DRC_EA_ABSL))
    is_o2 = (base in BITOPS and src_ea in (DRC_EA_IMM, DRC_EA_DD) and dst_ea in REGIND)
    if not (is_o1 or is_o2):
        return []
```
> **Verify the EA-mode constant names** (`DRC_EA_AIS`/`DRC_EA_AIPS`/`DRC_EA_PAIS`/`DRC_EA_DD`) against the `drc_ea_mode` map definition in `m68000gen.py` and confirm `(An)`=`ais`, `(An)+`=`aips`, `-(An)`=`pais`, `Dn`=`dd` map to those constants. Adjust the names to whatever the map actually defines.

In the parse loop, add a `write_interruptible` recognizer alongside the existing `read_interruptible` one (`:2460`), and a bare-`m_icount -= 2` (predecrement) recognizer:
```python
        # A bare 'm_icount -= N;' NOT preceded by a read/write in this state is the
        # predecrement micro-charge (pdcw1, '-(An)'): no suspend checkpoint follows.
        # Attribute it as pre_charge on the NEXT access step.
        if line.startswith('m_icount -= ') and not _pending_access(lines, i):
            pending_pre_charge += int(line[len('m_icount -= '):].rstrip(';'))
            i += 1
            continue
        # A bus WRITE: 'm_program.write_interruptible(m_aob & ~1, m_dbout, <mask>);'
        if line.startswith('m_program.write_interruptible') or \
           line.startswith('m_opcodes.write_interruptible'):
            # charge + suspend checkpoint parse: IDENTICAL to the read path, but
            # there is NO 'if(m_aob & 1)' address-error branch after a data write.
            charge, redo, completed = _parse_charge_and_substates(lines, i)
            steps.append((
                DRC_BUS_DATA_WRITE,
                DRC_SIZE_B,          # byte data write (the bit ops are always byte)
                charge, redo, completed,
                0,                   # has_addr_error: NEVER for a write
                1,                   # byte_lane: the 0x00ff/0xff00 mask applies
                pending_pre_charge,  # folded predecrement -2 (0 for (An)/(An)+)
            ))
            pending_pre_charge = 0
            i = _advance_past_checkpoint(lines, i)
            continue
```
Initialize `pending_pre_charge = 0` before the loop; on each **read** step, append `pending_pre_charge` as the new trailing tuple element and reset it to 0 (so a `-(An)` data read carries the `−2`). Factor the existing charge/substate/addr-error parse into the `_parse_charge_and_substates` / `_advance_past_checkpoint` helpers so read and write share it (the read path keeps its `if(m_aob & 1)` address-error scan; the write path skips it).

> **Single-source discipline (R-A):** do **not** special-case substate numbers by opcode. The parser reads them from the handler text. The only O-mem-2-specific logic is recognizing the `write_interruptible` line and the bare predecrement `−2`. The 24 forms then fall out of the existing per-opcode iteration (`:2728`) for free, because the opcode filter now admits them.

- [x] **Step 5: Thread `pre_charge` through the flat-table build.** In `collect_bus_run` (`:2721-2726`) the tuple `st` now has 8 elements (incl. `pre_charge`); `bus_steps.append(st + (disp,))` makes 9. Update the unpack in the row-print loop (Step 3) to 9 fields. Update the `s_drc_bus_step_table[]` comment (`:2750`, `:2769`) from "btst-absolute only" to "btst-absolute (O-mem-1) + bit-ops (An)/(An)+/-(An) #imm8/Dn (O-mem-2)". Update the `DRC_BUS_DATA_WRITE` enum comment (`:2758`, `:2420`, `m68000-drcdesc.ipp` enum) from "reserved/unused" to "m_program.write_interruptible, SSW_DATA, byte-lane".

- [x] **Step 6: Regenerate and prove additivity.**
```bash
python m68000gen.py decode m68000.lst m68000-decode.cpp
python m68000gen.py header m68000.lst m68000-head.h
python m68000gen.py sdf    m68000.lst m68000-sdf.cpp
python m68000gen.py sif    m68000.lst m68000-sif.cpp
python m68000gen.py sdp    m68000.lst m68000-sdp.cpp
python m68000gen.py sip    m68000.lst m68000-sip.cpp
python m68000gen.py drcdesc m68000.lst m68000-drcdesc.ipp
git diff --stat
```
Expected: **only** `m68000-drcdesc.ipp` appears. The `drc_bus_step` struct gained a field and the new bit-op runs/rows appended; the O-mem-1 btst-absolute rows must be **byte-identical except** the new trailing `, 0` `pre_charge` column (and the struct line). If any of `m68000-decode.cpp`/`m68000-head.h`/the four `s*` files changed, the edit leaked into the wrong code path — fix before continuing.

- [x] **Step 7: Eyeball the emitted rows against the handlers (R-A — catch it here, not in Leg B).** Confirm `m68000-drcdesc.ipp` now has runs for all 24 `{value, mask}` with the right counts and substates:
```bash
grep -nE '0x0850, 0xfff8|0x0858, 0xfff8|0x0860, 0xfff8' m68000-drcdesc.ipp   # bchg #imm8 (An)/(An)+/-(An): count 4 each
grep -nE '0x0150, 0xf1f8|0x0158, 0xf1f8|0x0160, 0xf1f8' m68000-drcdesc.ipp   # bchg Dn ...: count 3 each
grep -nE '0x0810, 0xfff8|0x0110, 0xf1f8' m68000-drcdesc.ipp                  # btst (An): count 3 (#imm8) / 2 (Dn)
grep -n 'DRC_BUS_DATA_WRITE' m68000-drcdesc.ipp                              # the new write rows exist
```
Verify each write row has `has_addr_error == 0, byte_lane == 1`, the right `completed_substate` (8 for `#imm8` RMW, 6 for `Dn` RMW), and that the `-(An)` data-read row carries `pre_charge == 2` while `(An)`/`(An)+` carry `0`. A mismatch is the single highest-risk bug in the batch.

- [x] **Step 8: Build + Leg A (descriptor inert until the emitter consumes it) + validate.**
```bash
MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd "$PWD"; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'
./mametests "[m68000]"
./mame -validate
```
Expected: build clean; Leg A green (the new rows are not yet read by any emitter, and `generate_bus_step`/`generate_btst_imm8_absolute` ignore the new `pre_charge`/write rows — confirm the existing btst-absolute Leg B still green: `./mametests "[m68000][drc]"`).

- [x] **Step 9: `srcclean` + commit.**
```bash
git add src/devices/cpu/m68000/m68000gen.py src/devices/cpu/m68000/m68000-drcdesc.ipp
git commit -m "feat(m68000drc): generate DATA_WRITE + predecrement bus-step rows for bit-ops (An)/(An)+/-(An) (additive)"
```

---

## Task 2: Write-side `generate_bus_step()` — the `DATA_WRITE` branch

**Goal:** Extend the one bus primitive with a `step.kind == DRC_BUS_DATA_WRITE` branch — `UML_WRITEM` at `m_aob & ~1` with the byte-lane mask and value from `m_dbout` — reusing the read step's charge + two-way suspend checkpoint verbatim, with **no** address-error branch. Switch on `step.kind`; do not fork the function (OQ-4 inline-first preserved, W1).

**Files:**
- Modify: `src/devices/cpu/m68000/m68000drc.cpp` — `generate_bus_step()` (`:361-437`).

**Interfaces:**
- Consumes: `struct drc_bus_step` (now with `kind`, `pre_charge`), `DRC_BUS_DATA_WRITE`; `m_dbout` (u16); `m_drc_redo_scratch`, `cfunc_take_access_to_be_redone`.
- Produces (consumed by Task 3): the same `generate_bus_step(block, step, lbl_delegate)` signature, now handling writes. Contract unchanged for reads. For a write: the *caller* has already set `m_aob`, `m_dbout` (`set_8xl`'d), and `m_base_ssw = SSW_DATA`; the primitive charges `pre_charge` (if any) then issues `UML_WRITEM`, charges `−N`, runs the shared suspend checkpoint, and on the clean path falls through (no addr-error, no `m_edb` commit). Clobbers I0-I6; preserves I7.

- [x] **Step 1: Add the `pre_charge` charge + the kind switch.** In `generate_bus_step` (`:361`), before the read (`:367`), charge `pre_charge` with no checkpoint (the predecrement `−2`):
```cpp
	// predecrement internal micro-charge (-(An)): charged with NO suspend
	// checkpoint, exactly as the interpreter's pdcw1 'm_icount -= 2;' (W2/W5).
	if(step.pre_charge)
	{
		UML_LOAD(block, I3, &m_icount, 0, SIZE_DWORD, SCALE_x1);
		UML_SUB(block, I3, I3, step.pre_charge);
		UML_STORE(block, &m_icount, 0, I3, SIZE_DWORD, SCALE_x1);
	}
```
Then branch the access emission on `step.kind`. Replace the read-only access block (`:367-392`, the `UML_LOAD m_aob` → `UML_STORE m_edb`) with:
```cpp
	// address = m_aob & ~1 (the interpreter accesses the word at the even address)
	UML_LOAD(block, I1, &m_aob, 0, SIZE_DWORD, SCALE_x1);            // i1 = m_aob
	UML_AND(block, I2, I1, ~u32(1));                                 // i2 = m_aob & ~1

	if(step.kind == DRC_BUS_DATA_WRITE)
	{
		// WRITE: m_program.write_interruptible(m_aob & ~1, m_dbout, (m_aob&1)?0x00ff:0xff00)
		// The CALLER has already done set_8xl(m_dbout, modified) and m_base_ssw = SSW_DATA.
		// UML_WRITEM is the masked word write mirroring the interpreter (a SIZE_BYTE write
		// at the byte address would present a different bus shape -- W1: do NOT decompose).
		uml::code_label const lbl_odd = m_drc_labelnum++;
		uml::code_label const lbl_mask_done = m_drc_labelnum++;
		UML_TEST(block, I1, 1);                                      // m_aob & 1 ?
		UML_MOV(block, I4, u32(0xff00));                             // even -> high lane
		UML_JMPc(block, COND_Z, lbl_mask_done);
		UML_LABEL(block, lbl_odd);
		UML_MOV(block, I4, u32(0x00ff));                             // odd -> low lane
		UML_LABEL(block, lbl_mask_done);
		UML_LOAD(block, I0, &m_dbout, 0, SIZE_WORD, SCALE_x1);       // i0 = m_dbout (replicated byte)
		UML_WRITEM(block, I2, I0, I4, SIZE_WORD, SPACE_PROGRAM);     // masked word write, byte lane
	}
	else
	{
		// READ (unchanged from O-mem-1): UML_READ word, byte-lane select for data reads.
		UML_READ(block, I0, I2, SIZE_WORD, SPACE_PROGRAM);          // i0 = read word at (m_aob & ~1)
		if(step.byte_lane)
		{
			uml::code_label const lbl_odd = m_drc_labelnum++;
			UML_TEST(block, I1, 1);                                 // m_aob & 1 ?
			UML_JMPc(block, COND_NZ, lbl_odd);                      // odd -> low byte already in place
				UML_SHR(block, I0, I0, 8);                          // even -> high byte to low
			UML_LABEL(block, lbl_odd);
			UML_AND(block, I0, I0, 0xff);                           // keep the selected byte
		}
		// commit the read into m_edb (u16 -> SIZE_WORD; a DWORD store clobbers m_irc)
		UML_STORE(block, &m_edb, 0, I0, SIZE_WORD, SCALE_x1);       // m_edb = read
	}
```
Leave the charge + suspend checkpoint (`:394-417`) **unchanged** — it is shared. After the checkpoint, guard the address-error branch so it is emitted only for reads that carry it (it already keys on `step.has_addr_error`, which the generator sets `0` for writes — so the existing `if(step.has_addr_error)` block at `:421` is already correct and emits nothing for a write). Confirm no `m_edb`/byte-lane code runs for the write path.

> **Verify before building:** `m_dbout` is `u16` (`m68000.h:197`) → `SIZE_WORD`. `UML_WRITEM(block, addr, src, mask, size, space)` is `drcumlsh.h:67`. The mask is a UML register (`I4`), matching the interpreter's runtime `(m_aob & 1) ? 0x00ff : 0xff00`. `SPACE_PROGRAM` is correct only behind the gate (W6) — the dispatch arm wrapper (Task 4) provides it.

- [x] **Step 2: Build (compile-only — no write step is dispatched yet).**
```bash
MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd "$PWD"; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'
```
Expected: clean build. The write branch is dead until Task 4 wires a write-bearing opcode.

- [x] **Step 3: Oracle unchanged — btst-absolute (read-only) still exact, both passes.**
```bash
./mametests "[m68000][drc]"
CPUORACLE_M68_DRC_FULLGRANT=1 ./mametests "[m68000][drc]"
CPUORACLE_M68_DRC_C=1 ./mametests "[m68000][drc]"
```
Expected: green. btst-absolute uses no `DATA_WRITE` step and `pre_charge == 0`, so the new branch/charge are inert — Leg B is byte-identical to Task 0a's baseline. (This proves the refactor did not perturb the read path.)

- [x] **Step 4: `srcclean` + commit.**
```bash
git add src/devices/cpu/m68000/m68000drc.cpp
git commit -m "feat(m68000drc): generate_bus_step() DATA_WRITE branch (UML_WRITEM + lane mask + predecrement pre_charge)"
```

---

## Task 3: EA-arithmetic + bit-modify helpers + the generic bit-op emitter

**Goal:** Add the reusable `(An)`/`(An)+`/`-(An)` EA-setup emission (with the A7-byte-by-2 rule and the predecrement folded into the descriptor's `pre_charge`), the per-family bit-modify + Z, and the single `generate_bitop_mem()` emitter that composes ext-fetch → EA → data-read → modify+refill → write → retire for the RMW forms and ext-fetch → data-read → final-prefetch for `btst`. Driven by the generated bus-step run; the form (family/EA/source) is a compile-time parameter.

**Files:**
- Modify: `src/devices/cpu/m68000/m68000.h` — the `bitop_form` enum/struct + `generate_bitop_mem` declaration, beside `generate_btst_imm8_absolute` (`:262`).
- Modify: `src/devices/cpu/m68000/m68000drc.cpp` — the helpers + `generate_bitop_mem`, after `generate_btst_imm8_absolute` (`:675`).

**Interfaces:**
- Consumes: `generate_bus_step` (Task 2), `s_drc_bus_run_table`/`s_drc_bus_step_table` (Task 1), `set_8xl` semantics, `m_da[]`/`m_sp`/`m_dcr`/`m_alub`/`m_dbout`/`m_at`/`m_au` fields, `cfunc_set_ftu_const` (`:268`).
- Produces (consumed by Task 4): `void generate_bitop_mem(drcuml_block &, const bitop_form &, uml::code_label lbl_delegate);` — emits one full form natively; on a fully-granted instruction it runs every bus step then retires; on a mid-instruction yield `generate_bus_step` JMPs to `lbl_delegate` and the partial interpreter handler resumes (OQ-1). Clobbers I0-I6; preserves I7 (`m_ird`).

- [x] **Step 1: Declare the form descriptor + emitter in the header.** In `m68000.h`, beside `generate_btst_imm8_absolute` (`:262`):
```cpp
	enum bitop_family : u8 { BITOP_BTST, BITOP_BCHG, BITOP_BCLR, BITOP_BSET };
	enum bitop_ea     : u8 { BITEA_AIS, BITEA_AIPS, BITEA_PAIS };   // (An), (An)+, -(An)
	enum bitop_src    : u8 { BITSRC_IMM8, BITSRC_DN };
	struct bitop_form { u16 value; u16 mask; u8 family; u8 ea; u8 src; };
	void generate_bitop_mem(drcuml_block &block, const struct bitop_form &form, uml::code_label lbl_delegate); // native bit-ops (An)/(An)+/-(An) (O-mem-2)
```

- [x] **Step 2: Add the shared `find_bus_run(value, mask)` helper.** O-mem-1's `find_run` is a local lambda matching by value only; O-mem-2 needs value+mask (the `Dn` forms reuse low bits). Add a file-local static in `m68000drc.cpp` above `generate_bitop_mem`:
```cpp
static const drc_bus_run &find_bus_run(u16 value, u16 mask)
{
	for(const drc_bus_run &r : s_drc_bus_run_table)
		if(r.value == value && r.mask == mask)
			return r;
	return s_drc_bus_run_table[0]; // unreachable for wired opcodes
}
```

- [x] **Step 3: Implement `generate_bitop_mem`.** In `m68000drc.cpp`, after `generate_btst_imm8_absolute` (`:675`). Read each handler line-by-line while transcribing (R-A). The emitter computes `ry` (and `rx` for `Dn`) at runtime from `I7`, emits the EA setup per `form.ea`, the bit number into `m_dcr`, then walks the run's steps via `generate_bus_step`, interleaving the per-state architectural setup, the modify, the Z, and the retire.

```cpp
//-------------------------------------------------
//  generate_bitop_mem - native UML for
//  btst/bchg/bclr/bset #n|Dn, (An)/(An)+/-(An)
//
//  RMW shape (mirrors bchg_imm8_ais_df etc., m68000-sdf.cpp):
//    [#imm8] ext-fetch (o#w1)            -> generate_bus_step prefetch
//    EA setup (adrw1/pinw1/pdcw1)        -> m_aob=m_at=EA ; reg writeback ; m_dcr
//    data read (adrw2/pinw2/pdcw2)       -> generate_bus_step DATA (byte_lane, no AE)
//    modify + refill (bcsm1)             -> save original, compute modified, prefetch
//    data write (bcsm2)                  -> set_8xl(m_dbout), Z from original, WRITE
//    retire                              -> set_ftu_const, m_inst_state, trace
//  btst: read-only -> ext-fetch, data read, FINAL prefetch (no modify, no write).
//  The bit number m_dcr: #imm8 = m_dt (from the ext word); Dn = m_da[rx].
//  Substates/charges/pre_charge come from the generated run -- never hard-coded.
//  I7 holds m_ird (the opword), preserved across all steps.
//-------------------------------------------------

void m68000_device::generate_bitop_mem(drcuml_block &block, const struct bitop_form &form, uml::code_label lbl_delegate)
{
	const drc_bus_run &run = find_bus_run(form.value, form.mask);
	u16 si = run.first;   // running index into s_drc_bus_step_table for THIS run

	const bool is_rmw = (form.family != BITOP_BTST);

	// --- ry = map_sp((m_ird & 7) | 8): if (m_ird & 7)==7 use m_sp, else (m_ird&7)|8.
	//     Result (the m_da[] index) left in I6 across the whole emit (generate_bus_step
	//     clobbers I0-I5 but NOT I6/I7 -- confirm I6 is preserved; if not, park ry in a
	//     scratch field).  Verify generate_bus_step's clobber set before relying on I6.
	auto load_ry = [&]() {
		uml::code_label const lbl_a7 = m_drc_labelnum++;
		uml::code_label const lbl_ry_done = m_drc_labelnum++;
		UML_AND(block, I6, I7, 7);                                    // i6 = m_ird & 7
		UML_CMP(block, I6, 7);
		UML_JMPc(block, COND_E, lbl_a7);
			UML_OR(block, I6, I6, 8);                                 // i6 = (m_ird & 7) | 8  (A0..A6)
			UML_JMP(block, lbl_ry_done);
		UML_LABEL(block, lbl_a7);
			UML_LOAD(block, I6, &m_sp, 0, SIZE_DWORD, SCALE_x1);      // i6 = m_sp (15 or 16)
		UML_LABEL(block, lbl_ry_done);
	};
	// delta = (ry < 15 ? 1 : 2) for byte access -- leaves the delta in Id.
	auto load_delta = [&](uml::parameter Id) {
		uml::code_label const lbl_two = m_drc_labelnum++;
		uml::code_label const lbl_d_done = m_drc_labelnum++;
		UML_CMP(block, I6, 15);
		UML_JMPc(block, COND_GE, lbl_two);                           // ry >= 15 (A7 bank) -> 2
			UML_MOV(block, Id, 1);
			UML_JMP(block, lbl_d_done);
		UML_LABEL(block, lbl_two);
			UML_MOV(block, Id, 2);
		UML_LABEL(block, lbl_d_done);
	};
	// m_base_ssw = SSW_PROGRAM | SSW_R   (reuse O-mem-1's pattern)
	auto ssw_program = [&]() {
		UML_MOV(block, I0, u32(u16(SSW_PROGRAM | SSW_R)));
		UML_STORE(block, &m_base_ssw, 0, I0, SIZE_WORD, SCALE_x1);
	};
	auto commit_irc_dbin = [&]() {
		UML_LOAD(block, I0, &m_edb, 0, SIZE_WORD, SCALE_x1);
		UML_STORE(block, &m_irc, 0, I0, SIZE_WORD, SCALE_x1);
		UML_STORE(block, &m_dbin, 0, I0, SIZE_WORD, SCALE_x1);
	};
	auto commit_dbin = [&]() {
		UML_LOAD(block, I0, &m_edb, 0, SIZE_WORD, SCALE_x1);
		UML_STORE(block, &m_dbin, 0, I0, SIZE_WORD, SCALE_x1);
	};

	// ===== ext-fetch (o#w1), #imm8 ONLY =====
	// VERIFY against bchg_imm8_ais_df:20707-20729: m_aob=m_au; m_pc=m_au;
	//   set_16l(m_dt,m_dbin); m_au+=2; SSW_PROGRAM; read; commit m_irc/m_dbin.
	if(form.src == BITSRC_IMM8)
	{
		UML_LOAD(block, I0, &m_au, 0, SIZE_DWORD, SCALE_x1);
		UML_STORE(block, &m_aob, 0, I0, SIZE_DWORD, SCALE_x1);
		UML_STORE(block, &m_pc, 0, I0, SIZE_DWORD, SCALE_x1);
		UML_ADD(block, I1, I0, 2);
		UML_STORE(block, &m_au, 0, I1, SIZE_DWORD, SCALE_x1);
		// set_16l(m_dt, m_dbin): m_dt = (m_dt & 0xffff0000) | (m_dbin & 0xffff)
		UML_LOAD(block, I2, &m_dt, 0, SIZE_DWORD, SCALE_x1);
		UML_AND(block, I2, I2, u32(0xffff0000));
		UML_LOAD(block, I3, &m_dbin, 0, SIZE_WORD, SCALE_x1);
		UML_OR(block, I2, I2, I3);
		UML_STORE(block, &m_dt, 0, I2, SIZE_DWORD, SCALE_x1);
		ssw_program();
		generate_bus_step(block, s_drc_bus_step_table[si++], lbl_delegate);   // ext read
		commit_irc_dbin();
	}

	// ===== EA setup + bit number =====
	// VERIFY per EA against the handler (ais :20730 / aips :20831 / pais :20937).
	load_ry();
	// m_aob = m_at = EA ; reg writeback per mode ; the predecrement -2 is folded
	// into the data-read step's pre_charge (Task 1), so DO NOT charge it here.
	switch(form.ea)
	{
	case BITEA_AIS:   // (An): m_aob = m_at = m_da[ry]
		UML_LOAD(block, I0, &m_da[0], I6, SIZE_DWORD, SCALE_x4);      // i0 = m_da[ry]
		UML_STORE(block, &m_aob, 0, I0, SIZE_DWORD, SCALE_x1);
		UML_STORE(block, &m_at, 0, I0, SIZE_DWORD, SCALE_x1);
		break;
	case BITEA_AIPS:  // (An)+: m_aob = m_at = m_da[ry] (old); m_da[ry] += delta
		UML_LOAD(block, I0, &m_da[0], I6, SIZE_DWORD, SCALE_x4);      // i0 = old m_da[ry]
		UML_STORE(block, &m_aob, 0, I0, SIZE_DWORD, SCALE_x1);
		UML_STORE(block, &m_at, 0, I0, SIZE_DWORD, SCALE_x1);
		load_delta(I1);                                              // i1 = delta
		UML_ADD(block, I2, I0, I1);                                   // i2 = old + delta
		UML_STORE(block, &m_da[0], I6, I2, SIZE_DWORD, SCALE_x4);     // m_da[ry] = old + delta
		break;
	case BITEA_PAIS:  // -(An): m_au = m_da[ry] - delta; m_aob = m_at = m_da[ry] = m_au
		UML_LOAD(block, I0, &m_da[0], I6, SIZE_DWORD, SCALE_x4);
		load_delta(I1);
		UML_SUB(block, I2, I0, I1);                                   // i2 = m_da[ry] - delta
		UML_STORE(block, &m_aob, 0, I2, SIZE_DWORD, SCALE_x1);
		UML_STORE(block, &m_at, 0, I2, SIZE_DWORD, SCALE_x1);
		UML_STORE(block, &m_da[0], I6, I2, SIZE_DWORD, SCALE_x4);
		break;
	}
	// bit number -> m_dcr: #imm8 = m_dt (low byte); Dn = m_da[rx], rx=(m_ird>>9)&7
	if(form.src == BITSRC_IMM8)
	{
		UML_LOAD(block, I0, &m_dt, 0, SIZE_DWORD, SCALE_x1);
		UML_STORE(block, &m_dcr, 0, I0, SIZE_BYTE, SCALE_x1);         // m_dcr = m_dt (u8)
	}
	else
	{
		UML_SHR(block, I0, I7, 9);
		UML_AND(block, I0, I0, 7);                                    // i0 = rx
		UML_LOAD(block, I1, &m_da[0], I0, SIZE_DWORD, SCALE_x4);      // i1 = m_da[rx]
		UML_STORE(block, &m_dcr, 0, I1, SIZE_BYTE, SCALE_x1);         // m_dcr = m_da[rx] (u8)
	}
	// data-read SSW: m_base_ssw = SSW_DATA | SSW_R
	UML_MOV(block, I0, u32(u16(SSW_DATA | SSW_R)));
	UML_STORE(block, &m_base_ssw, 0, I0, SIZE_WORD, SCALE_x1);

	// ===== data read (byte-lane; the descriptor carries the predecrement pre_charge) =====
	generate_bus_step(block, s_drc_bus_step_table[si++], lbl_delegate);       // DATA read
	commit_dbin();   // m_dbin = ORIGINAL byte

	// ===== Z (from the ORIGINAL byte) -- compute now while m_dbin holds it =====
	// Z = !(m_dbin & (1 << (m_dcr & 7))).  For RMW the interpreter sets Z at bcsm2
	// from m_alub(=original); computing it here from m_dbin is equivalent (SR is not
	// read between the data read and retire) and avoids a save/restore of the original
	// across the refill prefetch.  VERIFY the SR equality holds in the full-grant pass.
	auto compute_z_from_dbin = [&]() {
		UML_LOAD(block, I0, &m_dcr, 0, SIZE_BYTE, SCALE_x1);
		UML_AND(block, I0, I0, 7);
		UML_MOV(block, I1, 1);
		UML_SHL(block, I1, I1, I0);                                   // i1 = 1 << (m_dcr & 7)
		UML_LOAD(block, I2, &m_dbin, 0, SIZE_WORD, SCALE_x1);         // i2 = original byte
		UML_AND(block, I2, I2, I1);                                   // tested bit
		UML_LOAD(block, I3, &m_sr, 0, SIZE_WORD, SCALE_x1);
		UML_AND(block, I3, I3, u32(u16(~SR_Z)));
		UML_CMP(block, I2, 0);
		UML_SETc(block, COND_E, I4);                                  // Z = (bit == 0)
		UML_SHL(block, I4, I4, 2);                                    // SR_Z = 0x04
		UML_OR(block, I3, I3, I4);
		UML_STORE(block, &m_sr, 0, I3, SIZE_WORD, SCALE_x1);
	};

	if(!is_rmw)
	{
		// ===== btst: final prefetch, then retire (no modify, no write) =====
		// VERIFY against btst_imm8_ais_df / btst_dd_ais_df: bcsm1-equivalent final
		// prefetch (m_aob=m_au; m_ir=m_irc; m_pc=m_au; m_au+=2; m_ird=m_ir; next_state;
		// SSW_PROGRAM), Z set from the data byte, then retire.
		compute_z_from_dbin();
		// final-prefetch setup (reuse O-mem-1's setup_final_prefetch shape) + read:
		// <emit m_aob=m_au; m_ir=m_irc; m_pc=m_au; m_au+=2; m_ird=m_ir;
		//   if(next_state!=S_TRACE) next_state=int_next_state; SSW_PROGRAM>
		// (transcribe from generate_btst_imm8_absolute's setup_final_prefetch lambda,
		//  m68000drc.cpp:551-566 -- identical here.)
		generate_bus_step(block, s_drc_bus_step_table[si++], lbl_delegate);   // final prefetch
		// retire (transcribe generate_btst_imm8_absolute's retire lambda, :570-588)
		// <commit_irc_dbin(); cfunc_set_ftu_const; m_inst_state=...; trace>
		return;
	}

	// ===== RMW: modify + refill prefetch (bcsm1) =====
	// VERIFY against bchg_imm8_ais_df:20752-20777.  Save original (m_alub), compute the
	// MODIFIED byte, set_8xl into m_dbout, emit the refill prefetch, then commit.
	UML_LOAD(block, I0, &m_dbin, 0, SIZE_WORD, SCALE_x1);             // i0 = original byte
	UML_STORE(block, &m_alub, 0, I0, SIZE_WORD, SCALE_x1);            // m_alub = original (for parity/resume)
	UML_LOAD(block, I1, &m_dcr, 0, SIZE_BYTE, SCALE_x1);
	UML_AND(block, I1, I1, 7);
	UML_MOV(block, I2, 1);
	UML_SHL(block, I2, I2, I1);                                       // i2 = 1 << (m_dcr & 7)
	switch(form.family)
	{
	case BITOP_BCHG: UML_XOR(block, I0, I0, I2); break;              // modified = original ^ bit
	case BITOP_BCLR: UML_XOR(block, I2, I2, u32(0xff)); UML_AND(block, I0, I0, I2); break; // original & ~bit
	case BITOP_BSET: UML_OR (block, I0, I0, I2); break;             // modified = original | bit
	default: break; // unreachable (btst handled above)
	}
	UML_AND(block, I0, I0, 0xff);                                     // i0 = modified byte (8 bits)
	// set_8xl(m_dbout, modified) = (modified & 0xff) | (modified << 8)
	UML_SHL(block, I1, I0, 8);
	UML_OR(block, I0, I0, I1);
	UML_STORE(block, &m_dbout, 0, I0, SIZE_WORD, SCALE_x1);           // m_dbout = replicated modified byte
	// bcsm1 prefetch setup: m_aob=m_au; m_ir=m_irc; m_pc=m_au; m_au+=2; SSW_PROGRAM
	// (m_alub already set above; the interpreter also sets m_alub=m_dbin here.)
	UML_LOAD(block, I3, &m_au, 0, SIZE_DWORD, SCALE_x1);
	UML_STORE(block, &m_aob, 0, I3, SIZE_DWORD, SCALE_x1);
	UML_STORE(block, &m_pc, 0, I3, SIZE_DWORD, SCALE_x1);
	UML_LOAD(block, I4, &m_irc, 0, SIZE_WORD, SCALE_x1);
	UML_STORE(block, &m_ir, 0, I4, SIZE_WORD, SCALE_x1);
	UML_ADD(block, I3, I3, 2);
	UML_STORE(block, &m_au, 0, I3, SIZE_DWORD, SCALE_x1);
	ssw_program();
	generate_bus_step(block, s_drc_bus_step_table[si++], lbl_delegate);       // refill prefetch
	commit_irc_dbin();   // m_dbin <- prefetch word (original safe in m_alub/m_dbout)

	// ===== data write (bcsm2) =====
	// VERIFY against bchg_imm8_ais_df:20778-20801.  m_aob=m_at; m_ird=m_ir; next_state;
	//   (m_dbout already set); Z from m_alub; m_base_ssw = SSW_DATA (R CLEAR); WRITE.
	UML_LOAD(block, I0, &m_at, 0, SIZE_DWORD, SCALE_x1);
	UML_STORE(block, &m_aob, 0, I0, SIZE_DWORD, SCALE_x1);            // m_aob = m_at (validated by the data read)
	UML_LOAD(block, I1, &m_ir, 0, SIZE_WORD, SCALE_x1);
	UML_STORE(block, &m_ird, 0, I1, SIZE_WORD, SCALE_x1);            // m_ird = m_ir
	UML_LOAD(block, I2, &m_next_state, 0, SIZE_DWORD, SCALE_x1);
	UML_LOAD(block, I3, &m_int_next_state, 0, SIZE_DWORD, SCALE_x1);
	UML_CMP(block, I2, u32(S_TRACE));
	UML_MOVc(block, COND_NE, I2, I3);
	UML_STORE(block, &m_next_state, 0, I2, SIZE_DWORD, SCALE_x1);
	// Z from the ORIGINAL byte (m_alub) -- matches the interpreter's bcsm2 ordering.
	{
		UML_LOAD(block, I0, &m_dcr, 0, SIZE_BYTE, SCALE_x1);
		UML_AND(block, I0, I0, 7);
		UML_MOV(block, I1, 1);
		UML_SHL(block, I1, I1, I0);
		UML_LOAD(block, I2, &m_alub, 0, SIZE_WORD, SCALE_x1);         // original byte
		UML_AND(block, I2, I2, I1);
		UML_LOAD(block, I3, &m_sr, 0, SIZE_WORD, SCALE_x1);
		UML_AND(block, I3, I3, u32(u16(~SR_Z)));
		UML_CMP(block, I2, 0);
		UML_SETc(block, COND_E, I4);
		UML_SHL(block, I4, I4, 2);
		UML_OR(block, I3, I3, I4);
		UML_STORE(block, &m_sr, 0, I3, SIZE_WORD, SCALE_x1);
	}
	UML_MOV(block, I0, u32(u16(SSW_DATA)));                           // SSW_DATA only (write: R clear)
	UML_STORE(block, &m_base_ssw, 0, I0, SIZE_WORD, SCALE_x1);
	generate_bus_step(block, s_drc_bus_step_table[si++], lbl_delegate);       // DATA WRITE

	// ===== retire (transcribe generate_btst_imm8_absolute's retire lambda, :570-588) =====
	// <cfunc_set_ftu_const; m_inst_state = m_next_state ? m_next_state : m_decode_table[m_ird]; trace>
}
```

> **Three load-bearing verifications (R-A) — make these explicit checks, not assumptions:**
> 1. **`generate_bus_step` clobber set.** The lambda parks `ry` in `I6` across `generate_bus_step` calls. O-mem-1's header says it "Clobbers I0-I6; preserves I7." **If `generate_bus_step` clobbers `I6`, `ry` is lost.** Either (a) reload `ry` after each `generate_bus_step` (cheap, robust), or (b) tighten `generate_bus_step` to preserve `I6` and update its contract comment. **Pick (a) unless you verify (b).** The plan's lambda recomputes `ry` cheaply — call `load_ry()` again wherever a post-read `m_da[ry]` access is needed (only `(An)+`/`-(An)` writeback, both BEFORE the first read, so `ry` is not actually needed post-read — confirm).
> 2. **The Z timing.** The plan computes Z once at the write state (bcsm2) from `m_alub`, matching the handler. The `compute_z_from_dbin()` lambda is used only for `btst` (where Z is set at the final-prefetch state from `m_dbin`). Do **not** double-set Z. Confirm against `btst_imm8_ais_df` exactly where `sr_z()` sits.
> 3. **The retire/final-prefetch transcription.** Rather than re-typing, **factor O-mem-1's `setup_final_prefetch` and `retire` lambdas out of `generate_btst_imm8_absolute` into file-local statics** (or member helpers) and call them from both emitters — they are byte-identical (`m68000drc.cpp:549-588`). This removes the only "transcribe again" step and keeps a single source for the retire/trace logic.

- [x] **Step 4: Refactor the shared retire/final-prefetch out of `generate_btst_imm8_absolute`.** Extract `setup_final_prefetch` (`:549-566`) and `retire` (`:567-588`) into file-local helpers `static void emit_final_prefetch_setup(m68000_device&, drcuml_block&)` and `static void emit_retire(m68000_device&, drcuml_block&, code_label&)` (passing whatever `m_drc_labelnum` access they need), and call them from both `generate_btst_imm8_absolute` and `generate_bitop_mem`. Confirm the O-mem-1 btst-absolute Leg B is byte-identical after the refactor (it is a pure extraction).

- [x] **Step 5: Build (compile-only — no dispatch arm yet).**
```bash
MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd "$PWD"; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'
```
Expected: clean build. `generate_bitop_mem` is defined but unreferenced (acceptable; Task 4 calls it). Fix any UML-scope/enum errors now.

- [x] **Step 6: Oracle unchanged (emitter dormant; btst-absolute exact after the refactor).**
```bash
./mametests "[m68000][drc]"
CPUORACLE_M68_DRC_FULLGRANT=1 ./mametests "[m68000][drc]"
```
Expected: green — nothing dispatches to `generate_bitop_mem` yet, and the Step-4 extraction did not change the btst-absolute emission.

- [x] **Step 7: `srcclean` + commit.**
```bash
git add src/devices/cpu/m68000/m68000.h src/devices/cpu/m68000/m68000drc.cpp
git commit -m "feat(m68000drc): generate_bitop_mem() EA-arithmetic + bit-modify emitter (no dispatch yet)"
```

---

## Task 4: Wire the 24 native forms + dispatch arms

**Goal:** Add the 24 `{value, mask}` patterns to `is_native_opcode` and the gated dispatch arms in `generate_native_dispatch`, each calling `generate_bitop_mem` with the form constants. Then run the dual-leg oracle under both stepping modes — the **fully-granted pass is where the native write is first validated**.

**Files:**
- Modify: `src/devices/cpu/m68000/m68000drc.cpp` — `is_native_opcode` (`:207`); `generate_native_dispatch` (`:229`), after the btst-absolute arm (`:259`).

**Interfaces:**
- Consumes: `generate_bitop_mem` (Task 3), `drc_native_mem_ea_allowed()` (the O-mem-1 gate, unchanged — W6), `m_drc_native_mem_ea_arms` (probe counter).

- [x] **Step 1: Add the 24 patterns to `is_native_opcode`.** After the btst-absolute pattern (`:213-214`), add the bit-op `(An)/(An)+/-(An)` patterns. Keep it topology-independent (the gate is separate). The `#imm8` family bases are `0x0810/0x0850/0x0890/0x08D0` (btst/bchg/bclr/bset, mask `0xfff8`), `+0/0x08/0x10` for the three EAs; the `Dn` bases are `0x0110/0x0150/0x0190/0x01D0` (mask `0xf1f8`):
```cpp
	// O-mem-2: btst/bchg/bclr/bset #n,(An)/(An)+/-(An)  (mask 0xfff8)
	switch(opword & 0xfff8)
	{
	case 0x0810: case 0x0818: case 0x0820:   // btst #n
	case 0x0850: case 0x0858: case 0x0860:   // bchg #n
	case 0x0890: case 0x0898: case 0x08a0:   // bclr #n
	case 0x08d0: case 0x08d8: case 0x08e0:   // bset #n
		return true;
	}
	// O-mem-2: btst/bchg/bclr/bset Dn,(An)/(An)+/-(An)  (mask 0xf1f8)
	switch(opword & 0xf1f8)
	{
	case 0x0110: case 0x0118: case 0x0120:   // btst Dn
	case 0x0150: case 0x0158: case 0x0160:   // bchg Dn
	case 0x0190: case 0x0198: case 0x01a0:   // bclr Dn
	case 0x01d0: case 0x01d8: case 0x01e0:   // bset Dn
		return true;
	}
```
> **Verify each constant against the handler `// xxxx ffff` comment** before trusting it (e.g. `bchg_imm8_ais_df // 0850 fff8`, `bchg_dd_ais_df // 0150 f1f8`, `bset_imm8_pais_df`, `bclr_dd_aips_df`, …). A wrong base silently mis-classifies.

- [x] **Step 2: Add the gated dispatch arms.** In `generate_native_dispatch`, after the btst-absolute arm (`:259`), add a single `if (drc_native_mem_ea_allowed())` block with the 24-entry form table and a per-form compile-time arm. The probe counter increments per emitted arm (Task 7 of O-mem-1 reads it).
```cpp
	// O-mem-2: btst/bchg/bclr/bset (An)/(An)+/-(An), #imm8 and Dn source -- native
	// ONLY behind the space-topology gate (ADR 0007 Addendum; W6).  Each arm calls the
	// generic emitter with its form constants; the bus-step run (substates/charges/
	// pre_charge) is single-sourced from the generator.
	if (drc_native_mem_ea_allowed())
	{
		static const bitop_form k_forms[] = {
			// #imm8 source (mask 0xfff8)
			{ 0x0810, 0xfff8, BITOP_BTST, BITEA_AIS,  BITSRC_IMM8 },
			{ 0x0818, 0xfff8, BITOP_BTST, BITEA_AIPS, BITSRC_IMM8 },
			{ 0x0820, 0xfff8, BITOP_BTST, BITEA_PAIS, BITSRC_IMM8 },
			{ 0x0850, 0xfff8, BITOP_BCHG, BITEA_AIS,  BITSRC_IMM8 },
			{ 0x0858, 0xfff8, BITOP_BCHG, BITEA_AIPS, BITSRC_IMM8 },
			{ 0x0860, 0xfff8, BITOP_BCHG, BITEA_PAIS, BITSRC_IMM8 },
			{ 0x0890, 0xfff8, BITOP_BCLR, BITEA_AIS,  BITSRC_IMM8 },
			{ 0x0898, 0xfff8, BITOP_BCLR, BITEA_AIPS, BITSRC_IMM8 },
			{ 0x08a0, 0xfff8, BITOP_BCLR, BITEA_PAIS, BITSRC_IMM8 },
			{ 0x08d0, 0xfff8, BITOP_BSET, BITEA_AIS,  BITSRC_IMM8 },
			{ 0x08d8, 0xfff8, BITOP_BSET, BITEA_AIPS, BITSRC_IMM8 },
			{ 0x08e0, 0xfff8, BITOP_BSET, BITEA_PAIS, BITSRC_IMM8 },
			// Dn source (mask 0xf1f8)
			{ 0x0110, 0xf1f8, BITOP_BTST, BITEA_AIS,  BITSRC_DN },
			{ 0x0118, 0xf1f8, BITOP_BTST, BITEA_AIPS, BITSRC_DN },
			{ 0x0120, 0xf1f8, BITOP_BTST, BITEA_PAIS, BITSRC_DN },
			{ 0x0150, 0xf1f8, BITOP_BCHG, BITEA_AIS,  BITSRC_DN },
			{ 0x0158, 0xf1f8, BITOP_BCHG, BITEA_AIPS, BITSRC_DN },
			{ 0x0160, 0xf1f8, BITOP_BCHG, BITEA_PAIS, BITSRC_DN },
			{ 0x0190, 0xf1f8, BITOP_BCLR, BITEA_AIS,  BITSRC_DN },
			{ 0x0198, 0xf1f8, BITOP_BCLR, BITEA_AIPS, BITSRC_DN },
			{ 0x01a0, 0xf1f8, BITOP_BCLR, BITEA_PAIS, BITSRC_DN },
			{ 0x01d0, 0xf1f8, BITOP_BSET, BITEA_AIS,  BITSRC_DN },
			{ 0x01d8, 0xf1f8, BITOP_BSET, BITEA_AIPS, BITSRC_DN },
			{ 0x01e0, 0xf1f8, BITOP_BSET, BITEA_PAIS, BITSRC_DN },
		};
		for(const bitop_form &f : k_forms)
		{
			uml::code_label const lbl_next = m_drc_labelnum++;
			UML_AND(block, I0, I7, f.mask);
			UML_CMP(block, I0, f.value);
			UML_JMPc(block, COND_NE, lbl_next);
			generate_bitop_mem(block, f, lbl_delegate);              // emit the form (yields JMP lbl_delegate from within on suspend)
			UML_JMP(block, lbl_delegate);                            // fully-granted: retired -> hand the tail to the interpreter
			UML_LABEL(block, lbl_next);
			m_drc_native_mem_ea_arms++;                              // emission probe (gate test)
		}
	}
```
> **Dispatch-order note:** the btst-absolute arm tests `(m_ird & 0xfffe) == 0x0838`. The O-mem-2 `#imm8` arms test `(m_ird & 0xfff8) == 0x08{1,5,9,d}0` — `0x0838 & 0xfff8 == 0x0838`, which is NOT one of the O-mem-2 bases (those are `..10/..50/..90/..d0`), so absolute (`0x0838/0x0839`) never collides with the `(An)`-class arms. Confirm no overlap by inspection; the masks are disjoint on the EA-mode field.

- [x] **Step 3: Build.**
```bash
MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd "$PWD"; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'
```
Expected: clean build.

- [x] **Step 4: Legacy Leg B (x64) — native step-1 + suspend handoff for all 24 forms.**
```bash
./mametests "[m68000]"            # Leg A
./mametests "[m68000][drc]"       # Leg B 1-cycle, x64
```
Expected: green. The 1-cycle pass validates each form's native *first* step + the suspend yield; the tail (incl. the write) runs interpreter-side in both legs.

- [x] **Step 5: FULLY-GRANTED Leg B (x64 + C) — THE write-validation gate.**
```bash
CPUORACLE_M68_DRC_FULLGRANT=1 ./mametests "[m68000][drc]"                       # x64
CPUORACLE_M68_DRC_FULLGRANT=1 CPUORACLE_M68_DRC_C=1 ./mametests "[m68000][drc]" # C backend
```
Expected: **green.** This is the first and only correctness-gate coverage of the native **data write** (its RAM byte, the SR.Z it sets, the per-bus-cycle charge), the EA writeback (`(An)+`/`-(An)`), the predecrement `−2`, and the bit-modify. **If a form fails:** `superpowers:systematic-debugging` — the failure names register/flag/RAM/cycle and the case; map it to the step (RAM divergence → write lane/value or EA address; cycle divergence → a charge or the predecrement `−2`; flag divergence → Z timing/source; A-reg divergence → the `(An)+`/`-(An)` writeback or the A7-by-2 delta). The fallback is to drop the failing form from `is_native_opcode` + the dispatch table (route it back to `cfunc_`) and report — never approximate, never weaken the assertion.

- [x] **Step 6: Legacy Leg B (C backend).**
```bash
CPUORACLE_M68_DRC_C=1 ./mametests "[m68000][drc]"
```
Expected: green (catches backend-divergent emission, R-C).

- [x] **Step 7: `srcclean` + commit.**
```bash
git add src/devices/cpu/m68000/m68000drc.cpp
git commit -m "feat(m68000drc): native btst/bchg/bclr/bset (An)/(An)+/-(An) #imm8+Dn -- 24 forms (O-mem-2)"
```

---

## Task 5: OQ-9 MMU safeguard — regenerate the resident block when an MMU attaches

**Goal:** Make `set_current_mmu()` / `enable_mmu()` set `m_cache_dirty = true` so the resident block regenerates and `drc_native_mem_ea_allowed()` re-evaluates when an MMU is attached/enabled **after** the block was emitted (Apple Lisa / Sun-1 / SGI pm2 attach a custom MMU to a `type()==M68000` CPU). Plus a regen unit test. This hardens the gate O-mem-2 widens and discharges the §5 addendum's deferred safeguard.

**Files:**
- Modify: `src/devices/cpu/m68000/m68000.cpp` — `set_current_mmu()` (`:89`), `enable_mmu()` (`:129`).
- Modify: `tests/emu/cpu/cpuoracle.cpp` — the MMU-regen `TEST_CASE`.

**Interfaces:**
- Consumes: `m_cache_dirty` (`:58`, set `true` triggers resident-block regen at the next `code_flush_cache`, `:318`), `m_drc_native_mem_ea_arms` (probe), `drc_native_mem_ea_allowed()`.

- [x] **Step 1: Set `m_cache_dirty` on `m_mmu` change.** In `set_current_mmu()` (`:89-95`):
```cpp
void m68000_device::set_current_mmu(mmu *mmu)
{
	if(m_mmu != mmu)
		m_cache_dirty = true;   // OQ-9: regen the resident block so drc_native_mem_ea_allowed() re-evaluates
	m_mmu = mmu;

	if(m_mmu)
		m_mmu->set_super(m_sr & SR_S);
}
```
In `enable_mmu()` (`:129-133`):
```cpp
void m68000_device::enable_mmu(bool disable_spaces)
{
	if(m_mmu != &m_mmu_disabled || m_disable_spaces != disable_spaces)
		m_cache_dirty = true;   // OQ-9: gate re-evaluation (both m_mmu and m_disable_spaces feed it)
	m_mmu = &m_mmu_disabled;
	m_disable_spaces = disable_spaces;
}
```
> **Why both clauses:** `drc_native_mem_ea_allowed()` tests `!m_disable_spaces && m_mmu == nullptr && …`. Both `enable_mmu` paths flip the gate, so both must dirty the cache. The guards avoid a needless flush when the value is unchanged. **Note:** this is a behavior change on the interpreter-only path too (it dirties a cache the interpreter ignores) — confirm `m_cache_dirty` is harmless when `!m_isdrc` (it is: only `execute_run_drc`/`code_flush_cache` read it).

- [x] **Step 2: Add the MMU-regen unit test.** In `cpuoracle.cpp`, beside the gate-predicate test (`[m68000][drc][gate]`), add a case that boots the flat oracle device (gate true, native arm emitted), attaches an MMU after emit, forces a re-flush, and asserts the native arm drops out:
```cpp
TEST_CASE("m68000 DRC native memory-EA gate re-evaluates on MMU attach (OQ-9)", "[cpu][m68000][drc][gate]")
{
	using namespace cpuoracle;
	cpu_test_harness h(m68000_core_descriptor());   // flat bus
	h.set_drc(true);
	bool ran = h.run_with_machine([&]
	{
		REQUIRE(h.drc_engaged());
		// flat bus: gate true, native memory-EA arms emitted
		h.reset_cpu();
		h.force_block_emit();                         // code_flush_cache once (see harness hook below)
		CHECK(h.native_mem_ea_allowed());
		CHECK(h.native_arm_emit_count() > 0);
		// attach an MMU AFTER the block was emitted: m_cache_dirty must be set, so the
		// next emit drops the native arm and dispatch falls to cfunc_.
		h.attach_test_mmu();                          // calls device->set_current_mmu(&stub) (harness hook)
		h.force_block_emit();                         // regenerates because m_cache_dirty == true
		CHECK_FALSE(h.native_mem_ea_allowed());
		CHECK(h.native_arm_emit_count() == 0);
		// symmetry: detach -> arm returns
		h.detach_test_mmu();                          // set_current_mmu(nullptr)
		h.force_block_emit();
		CHECK(h.native_mem_ea_allowed());
		CHECK(h.native_arm_emit_count() > 0);
	});
	REQUIRE(ran);
}
```
> **Harness hooks needed:** `force_block_emit()` (a passthrough that calls the device's `code_flush_cache()` once — add it beside the existing emission-probe plumbing from O-mem-1 Task 7), `attach_test_mmu()`/`detach_test_mmu()` (call `set_current_mmu(&stub_mmu)` / `set_current_mmu(nullptr)` on the oracle device, with a trivial stub `mmu` subclass). If `code_flush_cache()` is not cleanly callable from the harness, the minimal alternative is to expose a method that returns `drc_native_mem_ea_allowed()` after toggling the MMU and asserts the gate predicate directly (the predicate is the load-bearing check; the emit-count is belt-and-suspenders). Builder's call at first build — the **predicate re-evaluation after `set_current_mmu`** is the must-have assertion.

- [x] **Step 3: Build + run the gate tests.**
```bash
MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd "$PWD"; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'
./mametests "[m68000][drc][gate]"
```
Expected: green — gate flips false on MMU attach (and the resident block regenerated), true on detach.

- [x] **Step 4: Full local gate (both passes, both backends) + `srcclean` + commit.**
```bash
./mametests "[m68000][drc]"
CPUORACLE_M68_DRC_FULLGRANT=1 ./mametests "[m68000][drc]"
./mame -validate
git add src/devices/cpu/m68000/m68000.cpp tests/emu/cpu/cpuoracle.cpp tests/emu/cpu/cpu_test_harness.h tests/emu/cpu/cpu_test_harness.cpp
git commit -m "fix(m68000drc): regen resident block on MMU attach/enable so the memory-EA gate re-evaluates (OQ-9)"
```

---

## Task 6: Coverage assertions + cut-line doc + the authoritative merge gate

**Goal:** Machine-assert that all 24 forms dispatch native (behind the gate), record them in the cut-line doc with the write-side note, and get every required gate GREEN — the dual-leg oracle (both stepping modes, x64 + C) on the appserver Linux `oracle` CI job, the AS_OPCODES differential, the gate + MMU-regen unit tests, code review, and the throughput bench on a gate-eligible flat-topology driver.

**Files:**
- Modify: `tests/emu/cpu/cpuoracle.cpp` (or `tests/emu/cpu/m68000_drc_coverage.cpp` if boundary N has landed) — the native-coverage assertion for the 24 forms.
- Modify: `src/devices/cpu/m68000/README-drc.md` — the cut-line doc.

**Interfaces:**
- Consumes: `is_native_opcode(u16)` (Task 4) for all 24 patterns.

- [x] **Step 1: Extend the native-coverage assertion.** Add the 24 representative encodings to the asserted-native set (one per form is enough; the predicate is mask-based). If `tests/emu/cpu/m68000_drc_coverage.cpp` does not exist (boundary N not landed — verified at O-mem-1 plan time), add the `CHECK`s to the existing `[m68000][drc][gate]` case in `cpuoracle.cpp` (where `is_native_opcode` is already exercised via the harness):
```cpp
	// O-mem-2: bit-ops on (An)/(An)+/-(An), #imm8 and Dn source, are native (behind the gate).
	for(u16 op : { (u16)0x0810,(u16)0x0818,(u16)0x0820, (u16)0x0850,(u16)0x0858,(u16)0x0860,
	               (u16)0x0890,(u16)0x0898,(u16)0x08a0, (u16)0x08d0,(u16)0x08d8,(u16)0x08e0,
	               (u16)0x0110,(u16)0x0118,(u16)0x0120, (u16)0x0150,(u16)0x0158,(u16)0x0160,
	               (u16)0x0190,(u16)0x0198,(u16)0x01a0, (u16)0x01d0,(u16)0x01d8,(u16)0x01e0 })
		CHECK(harness_is_native_opcode(op));   // via the harness wrapper used for btst-absolute
```
(Use whatever wrapper O-mem-1 Task 7 added to reach the protected `is_native_opcode` from the test TU; `is_native_opcode` is a protected static — the gate test already routes around this.)

- [x] **Step 2: Update the cut-line doc.** In `src/devices/cpu/m68000/README-drc.md`:
  - In "Native opcodes shipped (current)" (`:191`), add a row:
```markdown
| `btst`/`bchg`/`bclr`/`bset` `#n`\|`Dn`,`(An)`/`(An)+`/`-(An)` | O-mem-2 | 24 forms. RMW (`bchg`/`bclr`/`bset`) via the write-side `generate_bus_step()` (`UML_WRITEM`, byte-lane mask, value from `m_dbout`); `btst` read-only. EA arithmetic with the A7-byte-by-2 rule and the predecrement internal `−2`; Z from the original byte. **Native ONLY on a flat-topology, non-MMU bus (`drc_native_mem_ea_allowed()`)** — `AS_OPCODES`/user-space/MMU machines stay `cfunc_` (ADR 0007 Addendum). Native write validated by the fully-granted Leg-B pass (ADR 0007 W4). |
```
  - In the bit-ops / register-indirect notes, update "Explicitly NOT native" to exclude these 24 forms (reword: "`bchg`/`bclr`/`bset` memory-EA except `(An)`/`(An)+`/`-(An)`, native as of O-mem-2 on a flat-topology non-MMU bus").
  - In "Known limitations", append: the native **retire** lambda is the one native residual not oracle-exercised (the snapshot model cannot reach an overshoot; W4 — bounded, mirrors the validated `moveq` tail); and the deferring-tap redo path stays gated-by-absence (OQ-6).

- [x] **Step 3: Full local gate (every pass, both backends).**
```bash
MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd "$PWD"; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'
./mametests "[m68000]"                                                          # Leg A
./mametests "[m68000][drc]"                                                     # Leg B 1-cycle x64
CPUORACLE_M68_DRC_C=1 ./mametests "[m68000][drc]"                              # Leg B 1-cycle C
CPUORACLE_M68_DRC_FULLGRANT=1 ./mametests "[m68000][drc]"                      # Leg B full-grant x64 (WRITE gate)
CPUORACLE_M68_DRC_FULLGRANT=1 CPUORACLE_M68_DRC_C=1 ./mametests "[m68000][drc]" # Leg B full-grant C
./mametests "[m68000][drc][gate]"                                              # gate predicate + MMU regen
./mametests "[m68000][drc][asopcodes]"                                         # AS_OPCODES differential
./mame -validate
```
Expected: all green; `-validate` clean. This is the **necessary** local gate.

- [x] **Step 4: Push the branch and open the PR; get the appserver Linux `oracle` job GREEN.** This is the **sufficient** gate. Confirm the Linux `oracle` job runs all five oracle invocations above (1-cycle + full-grant, x64 + C, plus the gate/asopcodes cases) — if the CI matrix does not yet pass `CPUORACLE_M68_DRC_FULLGRANT=1`, **add that leg to the `oracle` workflow** (it is a required gate per the addendum; a full-grant pass that never runs in CI is not a gate). A red Linux job with green Windows is almost always an ABI-safety regression (a plain `mem(&field)` slipped in — audit every new UML operand) or a backend-divergent emission. Do not merge until the Linux `oracle` job is green.
```bash
git add tests/emu/cpu/cpuoracle.cpp src/devices/cpu/m68000/README-drc.md
git commit -m "test+docs(m68000drc): assert 24 bit-op forms native; record O-mem-2 write side in cut-line doc"
git push -u origin <branch>
gh pr create --fill   # PR body: Docs Impact + the gate evidence (both oracle passes green on Linux)
```

- [x] **Step 5: Re-run the throughput benchmark on a gate-eligible flat-topology driver.** Per the §5 addendum honest-number note, do **NOT** name an FD1094/`AS_OPCODES` set (its bit-ops run `cfunc_`). Measure `-drc 0` vs `-drc 1` on a flat-topology 68000 driver where the RMW bit-ops are hot; record the number in the PR. Does not gate merge (correctness gates merge), but it is the evidence the write side is worth it.

- [x] **Step 6: Merge per the auto-merge policy.** When all of: both oracle passes green (x64 + C) on the Linux `oracle` job, the gate-predicate + MMU-regen + AS_OPCODES differential green, generator additive (only `m68000-drcdesc.ipp` grew), code review clean, `-validate` clean — **merge** (implementation cycle complete, parity gates green, review clean, issues addressed). Do not stop to ask.

---

## Self-review

**Spec coverage (ADR 0007 O-mem-2 addendum → tasks):**
- W1 write-side `generate_bus_step()` (`UML_WRITEM`, byte-lane mask, `m_dbout`, shared checkpoint, no addr-error) → Task 2.
- W2 auto-inc/dec EA arithmetic (A7-byte-by-2, predecrement internal `−2`) → Task 1 (the `pre_charge` descriptor) + Task 3 (the EA-setup emission).
- W3 RMW composition (modify at refill, Z from the original byte, per-family ALU) → Task 3.
- W3b `Dn`-source forms (one fewer prefetch, shifted substates, separate dispatch family) → Tasks 1/3/4.
- W4 fully-granted Leg-B pass (grant `length`, drain at 1; `SR.T` for free; supplements the 1-cycle pass; runs against btst-absolute first) → Task 0a; consumed as a blocking gate in Tasks 4 & 6.
- W5 descriptor delta (single-sourced `DATA_WRITE` rows + predecrement `−2`) → Task 1.
- W6 gate continuity (existing `drc_native_mem_ea_allowed()` covers O-mem-2; no new clause, no `SR_S` branch) → Task 4 (arms wrapped in the gate).
- OQ-6 (keep non-interruptible simplification under the gate; deferring-tap config deferred) → honored (no interruptible path added; Global Constraints + README note).
- OQ-7 (add the full-grant pass) → Task 0a.
- OQ-8 (both source forms, 24 forms) → Tasks 1/3/4.
- OQ-9 (MMU `m_cache_dirty` + regen test) → Task 5.
- Task 0 (AS_OPCODES differential oracle, the §5 addendum's parked opening task) → Task 0.
- Merge-gate guidance (both passes, AS_OPCODES, gate+MMU tests, generator additivity, code review + Linux oracle, throughput on a gate-eligible driver) → Task 6.

**Placeholder scan:** The emitter code is literal UML; the per-form variation is a single parameterized emitter driven by the generated run, not 24 copies. The three "transcribe from the existing lambda" points (final-prefetch setup, retire) are resolved by Task 3 Step 4 **extracting O-mem-1's existing byte-identical lambdas into shared helpers** — a pure refactor, not a placeholder. The `// VERIFY against <handler>:<line>` markers are deliberate single-source gates (the ADR's explicit "verify against the handler" requirement), not deferred work.

**Type consistency:** `generate_bus_step(drcuml_block&, const drc_bus_step&, code_label)` (unchanged signature, now handles `kind==DRC_BUS_DATA_WRITE`); `drc_bus_step` gains `u8 pre_charge` (generated); `bitop_form{u16 value; u16 mask; u8 family; u8 ea; u8 src}`; `generate_bitop_mem(drcuml_block&, const bitop_form&, code_label)`; `find_bus_run(u16,u16)`. `m_dbout`/`m_alub`/`m_dbin`/`m_edb` are `u16` (SIZE_WORD); `m_da[17]`/`m_sp`/`m_aob`/`m_at`/`m_au` are `u32` (SIZE_DWORD); `m_dcr` is `u8` (SIZE_BYTE). Substates/charges/`pre_charge` are read from the descriptor, never hard-coded. `UML_WRITEM(block, addr, src, mask, size, space)` per `drcumlsh.h:67`.

**Genuinely-new open questions (design is settled; few expected):**
1. **`generate_bus_step` clobber set vs. `ry` in `I6`.** Task 3 parks `ry` in `I6` across `generate_bus_step` calls per O-mem-1's stated "preserves I7" (i.e. clobbers I0-I6). Since the only post-setup `m_da[ry]` accesses (the `(An)+`/`-(An)` writebacks) happen **before** the first `generate_bus_step`, `ry` is not actually needed across a read — but the Builder must confirm this at first build and, if a later reuse needs `ry` post-read, reload it (the lambda is cheap). Flagged, not blocking.
2. **Z-timing equivalence for RMW.** The plan sets Z at the write state from `m_alub` (matching the handler). The alternative (compute at data-read time from `m_dbin`) is noted as equivalent because SR is not read between; the Builder should keep the handler-matching version unless the full-grant pass shows a reason otherwise. Both produce identical retired SR. Not blocking.
3. **AS_OPCODES corpus PROGRAM/OPCODES split (Task 0).** Deciding which corpus cells are opcode vs data bytes is non-trivial (carried from O-mem-1 Task 8). The plan permits restricting the differential to the bit-op corpus subset if a clean split is impractical. A test-harness design choice the Builder resolves; the load-bearing assertion (gate off + interpreter ≡ DRC) holds regardless.

Neither these nor any W-section item changes the architecture; all are local implementation choices the Builder resolves at the first build.

---

## Merge gate (O-mem-2)

A batch merges only on **all** of (ADR 0007 O-mem-2 addendum "Merge-gate guidance"):
1. **Dual-leg flat-RAM oracle GREEN, 1-cycle pass** (Leg A unchanged; Leg B register/flag/RAM/cycle-exact), x64 + C.
2. **Fully-granted Leg-B pass GREEN** (`CPUORACLE_M68_DRC_FULLGRANT=1`), x64 + C — the gate that actually exercises the native data-write. **Blocking, not nice-to-have.**
3. **AS_OPCODES differential oracle GREEN** (`[m68000][drc][asopcodes]`) — gate keeps `AS_OPCODES` on `cfunc_`, fallback matches.
4. **Gate-predicate + coverage + MMU-regen tests GREEN** (`[m68000][drc][gate]`) — 24 forms asserted native; MMU attach drops the arm.
5. **Generator additive** — only `m68000-drcdesc.ipp` grew (new `DATA_WRITE` rows + `(An)/(An)+/-(An)` runs + predecrement `−2`), single-sourced from the microcode walk.
6. **Code review clean** + the **appserver Linux `oracle` job GREEN** (Windows-green is necessary-not-sufficient; the Linux job must run the full-grant leg).
7. **Throughput bench re-run on a gate-eligible, flat-topology driver** (not FD1094/`AS_OPCODES`).

When all are green, **merge per the auto-merge policy** — do not stop to ask. The deferring-tap oracle config (OQ-6) and native `AS_OPCODES` space-selection stay deferred to later, separately-scoped increments.
