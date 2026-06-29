# O-mem-3b Implementation Plan — native single-access (`.b`+`.w`) MOVE reg↔mem `(An)`/`(An)+`/`-(An)`, the one `generate_bus_step()` write-branch primitive edit

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the byte+word MOVE forms that move data between a CPU register and a register-indirect memory EA — **MOVE `.b`/`.w` `(An)`/`(An)+`/`-(An)` reg↔mem, both directions (15 forms)** — by (a) making the **one** load-bearing primitive change of the whole MOVE arc: the `generate_bus_step()` **write branch honors `step.byte_lane`** (`byte_lane==0` → full-word `UML_WRITE`; `byte_lane==1` → the existing masked `UML_WRITEM`, byte-identical), and (b) adding one parameterized native emitter (`generate_move_regmem`) that composes, per form, the source-EA read (load) or the data write (store) through the now-`byte_lane`-aware `generate_bus_step()`, computes the **MOVE flags** (`sr_nzvc`: N/Z from the moved value, V=C=0, X untouched), and does the **register source/dest** path (load → `Dn` write with `set_8`/`set_16l` preserving the unwritten half, no data-write step; store → register source into `m_dbout`). Gated cycle-exact by the three existing oracle grant modes (1-cycle, full-grant, single-offset partial-grant) on x64 + C.

**Architecture:** O-mem-3b is the second sub-batch of **O-mem-3** (phase-2 Task 10 / boundary O, increment plan §6 row 3), pinned by the **[ADR 0007 O-mem-3 addendum](../adr/0007-m68000-native-memory-ea-suspend-mechanism.md#addendum--resolution-2026-06-28--o-mem-3-memory-ea-movemovea)** (sections M1–M6, the 5-sub-batch split, OQ-10). Per the addendum's mechanism-novelty-isolation rule, **3b introduces exactly one new primitive mechanism: the word `DATA WRITE` (`byte_lane==0` full-word `UML_WRITE`).** This is **the only edit to `generate_bus_step()` in the entire MOVE arc** (M1, "Mechanism-novelty isolation that falls out of M1") — 3a touched it not at all; 3c/3d/3e reuse it frozen. The edit is therefore **its own first task, reviewed in isolation**, with an oracle re-run proving O-mem-2's byte writes (the `byte_lane==1` masked path) are byte-identical. Everything else 3b adds — the emitter, MOVE flags, the register source/dest paths, the gated dispatch arms, the generator-descriptor widening — is additive over the now-frozen-again primitive. All per-step charges, substate pairs, and the `-(An)`/`-(An)` predecrement internal `−2` are **single-sourced from `m68000gen.py`** (ADR R-A is the top risk; do not hand-transcribe).

**Scope resolution — the exact form count (15, not the addendum's "~16" estimate):** the addendum's M4 table lists 3b as "~16"; the precise roster, enumerated from the `_df` handler table (`m68000-sdf.cpp`), is **15 forms**:

| Direction | size | EAs | forms | handlers (`// value mask`) |
|---|---|---|---|---|
| **mem→Dn** (load, `dd` dest) | `.b` | `(An)`/`(An)+`/`-(An)` | 3 | `move_b_ais_dd_df // 1010`, `move_b_aips_dd_df // 1018`, `move_b_pais_dd_df // 1020` |
| **mem→Dn** (load, `dd` dest) | `.w` | `(An)`/`(An)+`/`-(An)` | 3 | `move_w_ais_dd_df // 3010`, `move_w_aips_dd_df // 3018`, `move_w_pais_dd_df // 3020` |
| **Dn→mem** (store, `ds` src) | `.b` | `(An)`/`(An)+`/`-(An)` | 3 | `move_b_ds_aid_df // 1080`, `move_b_ds_aipd_df // 10c0`, `move_b_ds_paid_df // 1100` |
| **Dn→mem** (store, `ds` src) | `.w` | `(An)`/`(An)+`/`-(An)` | 3 | `move_w_ds_aid_df // 3080`, `move_w_ds_aipd_df // 30c0`, `move_w_ds_paid_df // 3100` |
| **An→mem** (store, `as` src) | `.w` | `(An)`/`(An)+`/`-(An)` | 3 | `move_w_as_aid_df // 3088`, `move_w_as_aipd_df // 30c8`, `move_w_as_paid_df // 3108` |

**Derivation:** in-scope = both directions of MOVE between a register and a `{(An),(An)+,-(An)}` memory EA, single-access sizes `.b`+`.w`. mem→Dn load: 3 EAs × 2 sizes = **6**. Dn→mem store: 3 EAs × 2 sizes = **6**. An→mem store: 3 EAs × 1 size (An has **no** byte form — `grep 'move_b_as_'` is empty, confirmed) = **3**. Total **15**. **mem→An is MOVEA (already native in 3a), not 3b.** `(d16,An)` (`das`/`dad`) and long `.l` are **OUT** — `(d16,An)` → 3e, `.l` → 3d. (The addendum's "~16" is a rounded estimate; 15 is the handler-verified count.)

**Tech Stack:** C++17 (the `m68000_device` / DRCUML emitter, drcbe_x64 + drcbec backends), Python 3 (the `m68000gen.py` generator), GENie/`mingw32-make` build, Catch2 oracle harness (`tests/emu/cpu/cpuoracle.cpp` + `cpu_test_harness.{cpp,h}`). MSYS2 UCRT64/MINGW64 toolchain on Windows; appserver Linux for the authoritative `oracle` CI job.

> ⚠ **REQUIRED before any task — read the [ADR 0007 O-mem-3 addendum](../adr/0007-m68000-native-memory-ea-suspend-mechanism.md#addendum--resolution-2026-06-28--o-mem-3-memory-ea-movemovea) in full** (M1–M6, the pinned-vs-derive list, OQ-10) **and the as-built [`phase-2-o-mem-3a-movea.md`](phase-2-o-mem-3a-movea.md)** (3a froze the read path and established the `m_irdi` latch + register-write pattern that 3b reuses). Every mechanism decision is owner-decided in the addendum; this plan implements those answers, it does not re-derive them. **Five facts gate everything 3b-specific:**
> 1. **The ONE primitive edit (M1).** `generate_bus_step()`'s write branch today (`m68000drc.cpp:497-513`) **always** emits a masked `UML_WRITEM` (it computes the `0x00ff`/`0xff00` lane mask unconditionally). For word stores the write must be a **full-word `UML_WRITE`** with no lane. Make the branch honor `step.byte_lane`: `byte_lane==1` → the existing masked path (byte-identical); `byte_lane==0` → the new `UML_WRITE`. **This is the only `generate_bus_step()` change in the whole MOVE arc** — Task 1, reviewed in isolation.
> 2. **The shared address-error branch already handles word writes — NO extra edit (M1).** Word stores carry `has_addr_error==1` (verified `move_w_ds_aid_df:58043`: `if(m_aob & 1){ m_icount -= 4; m_inst_state = S_ADDRESS_ERROR; return; }` **after** the write). The branch at `m68000drc.cpp:564-577` is already kind-agnostic — emitted after the shared charge+checkpoint, keyed solely on `step.has_addr_error`, and `I1`(=`m_aob`) is preserved across the write emission and the checkpoint, so `I1 & 1` is correct for a write step. **Do not add a write-specific fault branch.** The only Task-1 source change besides the `byte_lane` `if/else` is updating the now-stale comment that claims "no address-error branch (has_addr_error == 0 for writes)".
> 3. **Byte writes/reads reuse O-mem-2's `byte_lane==1` path unchanged.** Byte stores write `(m_aob&1)?0x00ff:0xff00`-masked (`move_b_ds_aid_df:30852`, `byte_lane==1`, `has_addr_error==0`); byte loads read the lane-masked byte (`move_b_ais_dd_df:29991`, `byte_lane==1`, `has_addr_error==0`). The Task-1 edit's `byte_lane==1` arm is the **verbatim** existing code (only indentation changes), so the byte path is **byte-identical** and the O-mem-2 bit-op oracle stays green.
> 4. **MOVE decodes `rx`/`ry` from `m_irdi`, not `m_ird` (carried from 3a).** Every 3b handler opens `int rx = ...m_irdi...; int ry = ...m_irdi...;`. `static_generate_entry_point` does **NOT** latch `m_irdi` (it loads `I7 = m_ird` and checks the three guards). The emitter **must** `UML_STORE(&m_irdi, I7)` before any yield, or the interpreter's partial handler decodes from a stale `m_irdi` on resume → wrong register → Leg-B failure. 3a's `generate_movea_mem` already does this (`m68000drc.cpp:1186-1187`); 3b mirrors it. The field roles **differ by direction** (see the shapes section): load `rx`=Dn dest (raw `(m_irdi>>9)&7`), store `rx`=dest-EA An (`map_sp(((m_irdi>>9)&7)|8)`).
> 5. **OQ-10 is resolved (no sweep).** All three grant modes already exist in `cpu_test_harness.cpp`; **no oracle change is needed for 3b.** These are single-access forms (2 bus steps), so there are no intermediate long-access boundaries — the single `length-4` partial-grant offset lands the resume between the two accesses exactly as O-mem-2.

## Global Constraints

These apply to **every** task below — copied forward from ADR 0007 (body + O-mem-1/2/3 addenda) + the O-mem-2/3a plans + the cut-line doc + the user's workflow rules, with the O-mem-3b deltas marked.

- **THE GATE for every DRC-touching task is the oracle GREEN on the appserver LINUX `oracle` CI job.** Local Windows green is **necessary-not-sufficient** — boundary M passed Windows but failed Linux SysV twice on `drcbe_x64 offset_from_rbp`. The `oracle` CI job is NOT preflight (preflight is a tiny-build smoke).
- **O-mem-3b gate = THREE grant modes, both backends.** A batch merges only when **all of**: (1) 1-cycle Leg B (`drcbex64` + `drcbec`), (2) full-grant Leg B (`CPUORACLE_M68_DRC_FULLGRANT=1`, both backends — **the gate that first runs the native word `DATA WRITE` end-to-end**, i.e. the primitive edit), and (3) single-offset partial-grant Leg B (`CPUORACLE_M68_DRC_PARTGRANT=1`, both backends — the mid-instruction resume handoff between the two accesses), with **Leg A unchanged**. **No new oracle mode and no parameterized sweep** (OQ-10, owner-decided 2026-06-28).
- **`generate_bus_step()` is edited EXACTLY ONCE, in Task 1** (the `byte_lane` write `if/else`). After Task 1 it is frozen again for the rest of the MOVE arc. Tasks 2–5 are additive over the frozen primitive (generator filter widening, the emitter, dispatch arms, coverage assertion, doc). **If a MOVE form needs any further `generate_bus_step()` change, stop — that is a bug in this plan.**
- **The byte path stays byte-identical.** Task 1's `byte_lane==1` arm wraps the existing masked-write code **verbatim** (same UML ops, same order, same single `m_drc_labelnum++` for `lbl_mask_done`). The O-mem-2 bit-op RMW oracle (1-cycle + full-grant + partial-grant) must stay green with no change.
- **ABI-safe codegen:** zero plain `mem(&device_field)` operands. Every device-state field is accessed via `UML_LOAD`/`UML_STORE` with a pointer base (the boundary-M / O-mem-1/2/3a pattern in `m68000drc.cpp`). `UML_READ`/`UML_WRITE`'s address/value are UML registers, so the access ops are inherently ABI-safe; the field plumbing around them (`m_irdi`, `m_aob`, `m_at`, `m_au`, `m_pc`, `m_da[]`, `m_sp`, `m_dbin`, `m_dbout`, `m_edb`, `m_irc`, `m_ir`, `m_ird`, `m_icount`, `m_inst_substate`, `m_inst_state`, `m_next_state`, `m_int_next_state`, `m_base_ssw`, `m_sr`) all goes through LOAD/STORE.
- **Generator additivity:** regenerating must leave existing generated decode files **byte-identical** (empty `git diff` on `m68000-decode.cpp`, `m68000-head.h`, `m68000-s{d,i}{f,p}.cpp`). Only `m68000-drcdesc.ipp` may grow (the new MOVE reg↔mem read/write runs). **No new `step.kind`, no new descriptor field** (M1/M6) — the existing read/write kinds + `byte_lane`/`has_addr_error`/`pre_charge` fields already express every 3b access. The `enum_str()` shim preserves byte-identity on Python ≥ 3.11 — do not remove it.
- **Single-source rule (ADR 0007 R-A, the top risk):** the bus-step list — kinds, charges, the redo/completed substate pair, the predecrement internal `−2`, the `byte_lane`/`has_addr_error` flags — is emitted from the SAME microcode walk that generates the interpreter handler (`drc_bus_steps()` parses the handler text `generate_source_from_code` emits). **Never hand-type a substate number or charge into the emitter.** A hand-written step table is *not* an acceptable fallback for O-mem-3 (it was a documented temporary fallback for O-mem-1 only; the generator path is proven through O-mem-2/3a).
- **Build env (verified recipe):** `MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd <worktree>; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'`, then `./mametests "[m68000]"`. Do **NOT** use `make SOURCES=...m68000.cpp` (SOURCES filters by *driver*; a CPU device has no driver and GENie errors). `make REGENIE=1` is required whenever a `.cpp`/`.h` is added and after Task 2 regenerates the `.ipp`.
- **Backend matrix:** every oracle pass on **x64 (`drcbex64`)** AND the **C backend (`CPUORACLE_M68_DRC_C=1`)**. arm64 (`drcbearm64`) is deferred to the CI matrix.
- **Cycle truth:** the interpreter (`m68000.cpp` under `-drc 0`) is the authority. The DRC mirrors its cycle count; divergence is always a DRC bug, resolved by routing the offending form back to `cfunc_` (drop it from `is_native_opcode` / the dispatch table) — never by editing the interpreter or weakening an assertion.
- **Scope lock:** O-mem-3b = the **15 reg↔mem forms** only (mem→Dn `.b`/`.w`, Dn→mem `.b`/`.w`, An→mem `.w`, each × `(An)`/`(An)+`/`-(An)`). MOVE mem→mem (3c), long `.l` MOVE (3d), every `(d16,An)` form (3e), MOVEA mem→An (already 3a), and native `AS_OPCODES` space-selection are **out of scope** (the gate keeps `AS_OPCODES`/user-space/MMU machines on `cfunc_`).
- **Style:** match the existing m68000 brace/whitespace style (tabs, the K&R-ish style already in `m68000drc.cpp`); license header `// license:BSD-3-Clause` / `// copyright-holders:Mark Mackelprang` on any new file (none expected); run `srcclean` (built via `TOOLS=1`) on touched files before committing. Work on the implementation branch and open a PR (never commit DRC source straight to `main`).

---

## The interpreter shapes O-mem-3b must mirror (read before any task)

Every native form is a *transcription* of its `_df` handler in `m68000-sdf.cpp` — the one the plain `m68000_device` runs. **Read each handler line-by-line while emitting its form** (R-A is the top risk). The shapes below are verified against the tree. **The 3a Builder found two skeleton inaccuracies (a `byte_lane`/`size` misclassification and a writeback delta); accordingly, the per-mode `m_aob`/`m_au`/`m_at`/`m_da[]` choreography below is given as VERIFY-gated derive-from-handler guidance, and the literals that ARE given (the primitive edit, the flag computation, the register write, the dispatch arms) were double-checked against the handlers cited.**

### Bus-step ladders (single-source the substates/charges/flags from the generator; do NOT hand-transcribe)

Both directions are **2 bus steps** (single-access):

| Direction | step 1 | step 2 | notes |
|---|---|---|---|
| **mem→Dn load** | DATA READ (substates 1/2) | final PREFETCH (substates 3/4) | byte read: `byte_lane=1`, `has_addr_error=0`. word read: `byte_lane=0`, `has_addr_error=1`. prefetch: `byte_lane=0`, `has_addr_error=1`. **No data-write step** (dest is a register). |
| **reg→mem store** (Dn→mem, An→mem) | DATA WRITE (substates 1/2) | final PREFETCH (substates 3/4) | byte write: `byte_lane=1`, `has_addr_error=0` (existing masked path). word write: `byte_lane=0`, `has_addr_error=1` (**the primitive edit**). prefetch: `byte_lane=0`, `has_addr_error=1`. **No source read** (source is a register). |

`-(An)` (load `pais` / store `paid`) carries the predecrement internal `−2` as **`pre_charge` on the first bus step** (the data read for a load, the data write for a store), folded by the generator exactly as O-mem-2's `pais` — the emitter does **not** charge it. **VERIFY** the `pre_charge` value from the generated descriptor, never hand-type it.

### mem→Dn LOAD skeleton (verified against `move_w_ais_dd_df:56194` word, `move_b_ais_dd_df:29979` byte)

```cpp
int rx = (m_irdi >> 9) & 7;            // dest Dn index (RAW data register 0..7 -- NOT map_sp)
int ry = map_sp((m_irdi & 7) | 8);     // src  An index (address register, map_sp'd)
// --- source EA setup (adrw1/adrw2 ; pinw* ; pdcw*) : substates feed the data read ---
m_aob = m_da[ry]; /* m_at = m_da[ry]; */  // (An): VERIFY per mode (aips/pais writeback differs)
m_base_ssw = SSW_DATA | SSW_R;
m_edb = m_program.read_interruptible(m_aob & ~1 [, lane mask if .b]);  // <generate_bus_step DATA read ; substates 1/2 ; .b byte_lane=1/no-fault, .w byte_lane=0/fault>
// (commit) m_dbin = m_edb;             // the source byte/word
// --- final prefetch + REGISTER write-back + MOVE FLAGS (mrgw1/mmrw3) : substates 3/4 ; PROGRAM ; fault ---
m_aob = m_au; m_ir = m_irc; m_pc = m_au;
set_8(m_da[rx], m_dbin);   // .b : low byte of Dn, PRESERVE bits 8..31    (verified :30008)
// set_16l(m_da[rx], m_dbin);  // .w : low word of Dn, PRESERVE bits 16..31 (verified :56226)
m_au = m_au + 2;
// MOVE flags: alu_and(m_dbin,0xffff)+sr_nzvc()  ==  N=MSB, Z=(==0), V=C=0, X untouched  (verified :56229-56230 / :30011-30012)
m_ird = m_ir; if(m_next_state != S_TRACE) m_next_state = m_int_next_state;
m_base_ssw = SSW_PROGRAM | SSW_R;
m_edb = m_opcodes.read_interruptible(m_aob & ~1);  // <generate_bus_step prefetch ; byte_lane=0 ; fault ; substates 3/4>
// (retire) m_irc = m_dbin = m_edb; set_ftu_const(); m_inst_state = m_next_state ? m_next_state : m_decode_table[m_ird]; trace
```

> **Load flag/value source:** flags come from **`m_dbin`** (the just-read value) at the final-prefetch setup state (mrgw1), AFTER the read. The register write-back is `set_8`/`set_16l` (the unwritten half of `Dn` is **preserved** — this is the `.b`/`.w` MOVE-to-data-register semantic, distinct from MOVEA's `ext32` full-32-bit write). **VERIFY** the `aips`/`pais` source-An writeback (`m_da[ry] += 2` post-inc / `m_da[ry] -= 2` pre-dec) and the exact `m_au`/`m_at`/`m_pc` advances per handler (`move_w_aips_dd_df:56259`, `move_w_pais_dd_df:56328`, and the byte siblings) — these differ subtly per mode (the 3a writeback-delta inaccuracy was exactly this class of detail).

### reg→mem STORE skeleton (verified against `move_w_ds_aid_df:58019` Dn-src, `move_w_as_aid_df:58080` An-src, `move_b_ds_aid_df:30838` byte)

```cpp
int rx = map_sp(((m_irdi >> 9) & 7) | 8);  // dest An index (the EA's address register, map_sp'd)
int ry = m_irdi & 7;                        // src Dn (RAW, store-Dn)   -- OR map_sp((m_irdi&7)|8) for store-An
// --- write setup (rmrw1) + MOVE FLAGS (BEFORE the write) : substates 1/2 ; DATA ; fault (word) ---
m_aob = m_da[rx]; m_ir = m_irc; m_pc = m_au;          // dest EA into m_aob   (VERIFY per dest mode)
m_dbout = m_da[ry];                                   // .w store: source low word into m_dbout (verified :58026 / :58087)
// set_8xl(m_dbout, m_da[ry]);                         // .b store: replicate source byte to both lanes (verified :30845)
m_au = m_da[rx] + 2;                                  // (An) dest: VERIFY aipd post-inc / paid pre-dec writeback to m_da[rx]
// MOVE flags: alu_and(m_da[ry],0xffff)+sr_nzvc()  ==  N=MSB, Z=(==0), V=C=0, X untouched  (verified :58030-58031 / :30852-30853)
m_base_ssw = SSW_DATA;                                // write: SSW_R clear
m_program.write_interruptible(m_aob & ~1, m_dbout [, lane mask if .b]);  // <generate_bus_step DATA WRITE ; substates 1/2 ; .b byte_lane=1/no-fault (masked, UNCHANGED), .w byte_lane=0/fault (FULL-WORD -- THE PRIMITIVE EDIT)>
// --- final prefetch (mmrw2/mmrw3) : substates 3/4 ; PROGRAM ; fault ---
m_aob = m_pc; m_ir = m_irc; m_au = m_pc + 2;          // VERIFY :58048-58051
m_ird = m_ir; if(m_next_state != S_TRACE) m_next_state = m_int_next_state;
m_base_ssw = SSW_PROGRAM | SSW_R;
m_edb = m_opcodes.read_interruptible(m_aob & ~1);     // <generate_bus_step prefetch ; byte_lane=0 ; fault ; substates 3/4>
// (retire) m_irc = m_dbin = m_edb; set_ftu_const(); m_inst_state = ...; trace
```

> **Store flag/value source:** flags come from **`m_da[ry]`** (the source register's low byte/word) at the write-setup state (rmrw1), **BEFORE** the write bus step — exactly as the bit-op RMW computes `compute_z` before its DATA WRITE (`m68000drc.cpp:1088`). This ordering is correct for both resume cases: a redo-resume (substate 1) replays the whole rmrw1 (flags + write); a completed-resume (substate 2) resumes at mmrw2 after the write, with the native-set flags standing. **An-src store (`as`) differs from Dn-src store (`ds`) ONLY in `ry` decode** (`map_sp((m_irdi&7)|8)` address register vs raw `m_irdi&7` data register) and the source register array slot — same write step, same flags, same ladder (verified `move_w_as_aid_df:58080` ≡ `move_w_ds_aid_df:58019` except `ry`). **VERIFY** the `aipd`/`paid` dest-An writeback and the exact `m_au`/`m_pc` advances per handler (`move_w_ds_aipd_df:59216`, `move_w_ds_paid_df:60410`, the byte siblings, and the An-src siblings `:59276`/`:60470`).

### MOVE flags — `sr_nzvc()` semantics (verified `m68000.h:1037`)

`sr_nzvc()` is `m_sr = (m_sr & ~(SR_N|SR_Z|SR_V|SR_C)) | (m_isr & (SR_N|SR_Z|SR_V|SR_C))`. The handler's preceding `alu_and(v,0xffff)` (word) / `alu_and8(v,0xffff)` (byte) sets `m_isr`: `Z` if `v==0`, `N` if the size's MSB is set, and leaves `V=C=0` (`alu_and` never sets `V`/`C`); `X` is untouched by `sr_nzvc`. Net MOVE-flag effect: **N = value MSB (bit 15 word / bit 7 byte), Z = (value == 0 over the size's width), V = 0, C = 0, X/S/T/I untouched.** SR bits (`m68000.h:107-110`): `SR_C=0x0001, SR_V=0x0002, SR_Z=0x0004, SR_N=0x0008`. Since `V`/`C` are always cleared, a direct N/Z computation is admissible (ADR M3 "compute the equivalent N/Z directly and prove equivalence under the full-grant oracle") — the literal lambda in Task 3 does exactly this and is validated by the full-grant + 1-cycle passes. **VERIFY** the value source per direction (load: `m_dbin`; store: `m_da[ry]`) and the size against each handler's `alu_and`/`alu_and8` call.

### EA arithmetic per mode (M3 — byte delta 1 except `(A7)`±=2; word delta 2; NO byte A7 exception applies to An-src/.w forms since those are word)

| Mode | load token | store token | reg writeback | internal charge |
|---|---|---|---|---|
| `(An)` | `ais` | `aid` | none | none |
| `(An)+` | `aips` | `aipd` | post-inc the EA's An by the size delta | none |
| `-(An)` | `pais` | `paid` | pre-dec the EA's An by the size delta | **`m_icount -= 2`** at the `pdc*` state, **no** suspend checkpoint (folded onto the first bus step's `pre_charge` by the generator) |

> **Byte `(A7)` delta exception (M3 / O-mem-2 W2 generalization):** for **byte** `(A7)+`/`-(A7)` the increment/decrement is **2**, not 1 (keeps SP word-aligned); A0–A6 byte use delta 1; word always uses 2. This is the same per-mode delta logic O-mem-2's `load_delta` lambda already implements (`m68000drc.cpp:892-902`). **VERIFY** each writeback delta against its handler (the 3a writeback-delta inaccuracy was in this exact area) and reuse the O-mem-2 `load_delta` pattern rather than hand-typing constants. The `-(An)`/`(A7)` predecrement **address** delta (1/2) is distinct from the predecrement **internal `−2` charge** (always 2, generator `pre_charge`).

### Fields SET by the handler but DEAD for MOVE (skip per the O-mem-1/3a precedent — VERIFY not in the oracle compare set)

- `m_dcr` (load `adrw2`: `m_dcr = m_da[rx]`; mem→mem word: `m_dcr = 0`): written, never read for reg↔mem MOVE. Skip per the 3a `m_dcr`/`m_alub` dead-scratch precedent (`m68000drc.cpp` movea comment). VERIFY `m_dcr` is not in the oracle's compared architectural state; if uncertain, setting it is cheap and harmless.
- `m_alub` (load `aips`/`pais` setup: `m_alub = m_dbin`): written, then only consumed by a by-value `alu_and(m_alub,0xffff)` (writes uncompared scratch). Skip. Same VERIFY/escape.
- `alu_and(...)`/`alu_and8(...)` *before* the flag-bearing `sr_nzvc()` (e.g. load `adrw1`): by-value, write only `m_aluo`/`m_isr` (uncompared scratch), no `sr_*` follows — **not** emitted, exactly as `generate_btst_imm8_absolute` omits its `alu_eor8`/`alu_and8` and `generate_movea_mem` omits the MOVEA `alu_and`s. **Only the `alu_and(...)+sr_nzvc()` pair that updates `m_sr` is reproduced** (as the direct N/Z computation).

---

## File Structure

| File | Responsibility | Touched in |
|---|---|---|
| `src/devices/cpu/m68000/m68000drc.cpp` | **Task 1:** the `generate_bus_step()` write-branch `byte_lane` `if/else` (the ONE primitive edit) + comment fix. **Task 3:** `generate_move_regmem()` emitter; `is_native_opcode` patterns; the 15 gated dispatch arms | Tasks 1, 3 |
| `src/devices/cpu/m68000/m68000gen.py` | Generator — widen `drc_bus_steps()` to admit the 15 MOVE reg↔mem forms; additive-only (reuse 3a's lane-mask-keyed parser) | Task 2 |
| `src/devices/cpu/m68000/m68000-drcdesc.ipp` | Generated descriptor table — regenerated; existing rows byte-identical, new MOVE runs appended | Task 2 |
| `src/devices/cpu/m68000/m68000.h` | declare `generate_move_regmem`, the `moverm_form` struct + `moverm_dir`/`moverm_size`/`moverm_ea`/`moverm_reg` enums (beside `movea_form`) | Task 3 |
| `tests/emu/cpu/cpuoracle.cpp` | native-coverage assertion for the 15 forms (in the `[m68000][drc][gate]` case); the AS_OPCODES differential opcode subset if it is restricted | Tasks 3, 4 |
| `src/devices/cpu/m68000/README-drc.md` | move the 15 MOVE reg↔mem forms from `cfunc_` to native (behind the gate); record the word-write primitive + MOVE flags + register source/dest | Task 4 |

`make REGENIE=1` is run per build because Task 2 regenerates the `.ipp` (and Task 1 touches only `m68000drc.cpp`, no REGENIE needed for Task 1 alone, but it is harmless).

---

## Task 1: The `generate_bus_step()` write-branch primitive edit — `byte_lane==0` → full-word `UML_WRITE` (ISOLATED)

**Goal:** Make the **one** primitive change of the entire MOVE arc: `generate_bus_step()`'s `DATA_WRITE` branch honors `step.byte_lane`. `byte_lane==1` keeps the existing masked `UML_WRITEM` **verbatim** (byte-identical); `byte_lane==0` emits a full-word `UML_WRITE` with no lane mask. Reviewed in isolation, with an oracle re-run proving O-mem-2's byte writes (`byte_lane==1`) are unaffected. The edit is **dormant until Task 2** generates the first `byte_lane==0` write descriptor — so this task's correctness claim is "no regression to existing (byte) writes"; the word-write *correctness* is established by Task 3's full-grant pass.

**Files:**
- Modify: `src/devices/cpu/m68000/m68000drc.cpp` — `generate_bus_step()`, the `if(step.kind == DRC_BUS_DATA_WRITE)` branch (`:497-513`).

**Interfaces:**
- Produces (consumed by Task 3 via the descriptor): a `DATA_WRITE` step with `byte_lane==0` now emits `UML_WRITE(block, I2, I0, SIZE_WORD, SPACE_PROGRAM)` (addr `I2`=`m_aob & ~1`, value `I0`=`m_dbout`); the shared address-error branch (`:564-577`) already fires for `has_addr_error==1` writes (no change).

- [ ] **Step 1: Pin the as-built write branch and confirm the shared fault branch is kind-agnostic.** Re-read `m68000drc.cpp:497-513` (the write branch) and `:564-577` (the address-error branch). Confirm: (a) the write branch loads `m_dbout` into `I0` and writes via `UML_WRITEM(I2, I0, I4, ...)` with `I4` = `(m_aob&1)?0x00ff:0xff00`; (b) `I1`(=`m_aob`, loaded at `:488`) and `I2`(=`m_aob & ~1`, `:489`) are **not** clobbered by the write branch, so the post-checkpoint fault branch's `UML_TEST(I1, 1)` is valid for a write; (c) the charge (`:539-541`) and suspend checkpoint (`:543-562`) are **shared** (after the kind branch), so the write reuses them unchanged.

- [ ] **Step 2: Make the write branch honor `step.byte_lane`.** Replace the body of `if(step.kind == DRC_BUS_DATA_WRITE){ ... }` with the `byte_lane` `if/else`. The `byte_lane` arm is the **existing** masked code verbatim (only re-indented one level); the `else` arm is the new full-word write. Update the stale comment (the write **does** now carry an address-error branch for word writes via the shared kind-agnostic branch):
```cpp
	if(step.kind == DRC_BUS_DATA_WRITE)
	{
		// WRITE: m_program.write_interruptible(m_aob & ~1, m_dbout [, lane mask]).
		// The CALLER has already set m_dbout and m_base_ssw = SSW_DATA.  step.byte_lane
		// selects the bus shape, single-sourced from the handler's write call:
		//   byte_lane==1 (O-mem-2 byte RMW / O-mem-3b byte store): masked word write at
		//     the even address, lane (m_aob&1)?0x00ff:0xff00 -- UML_WRITEM (UNCHANGED).
		//   byte_lane==0 (O-mem-3 word/long MOVE store): full-word write, no lane --
		//     UML_WRITE (ADR 0007 M1, the one generate_bus_step primitive edit).
		// No m_edb commit.  The shared address-error branch below is kind-agnostic and
		// FIRES for word writes (has_addr_error==1) and NOT for byte writes (==0); I1
		// (= m_aob) is preserved across this branch so its m_aob&1 test is correct.
		if(step.byte_lane)
		{
			uml::code_label const lbl_mask_done = m_drc_labelnum++;
			UML_TEST(block, I1, 1);                                     // m_aob & 1 ?
			UML_MOV(block, I4, u32(0xff00));                           // even -> high lane
			UML_JMPc(block, COND_Z, lbl_mask_done);
			UML_MOV(block, I4, u32(0x00ff));                           // odd -> low lane
			UML_LABEL(block, lbl_mask_done);
			UML_LOAD(block, I0, &m_dbout, 0, SIZE_WORD, SCALE_x1);     // i0 = m_dbout (replicated byte)
			UML_WRITEM(block, I2, I0, I4, SIZE_WORD, SPACE_PROGRAM);   // masked word write, byte lane
		}
		else
		{
			UML_LOAD(block, I0, &m_dbout, 0, SIZE_WORD, SCALE_x1);     // i0 = m_dbout (full word)
			UML_WRITE(block, I2, I0, SIZE_WORD, SPACE_PROGRAM);       // full-word write, no lane (O-mem-3 word/long)
		}
	}
```
> **Byte-identity guarantee:** the `byte_lane==1` arm is the existing `:505-512` code re-indented, emitting the identical UML op stream (same single `lbl_mask_done` label allocation, same `TEST/MOV/JMPc/MOV/LABEL/LOAD/WRITEM` order). The `else` arm allocates **no** label, so the running `m_drc_labelnum` for any byte-write call is unchanged → the byte path is byte-identical. **VERIFY** `UML_WRITE` arity against `src/devices/cpu/drcumlsh.h:66` — `UML_WRITE(block, addr, src, size, space)` (confirmed; matches `ppcdrc.cpp:1122`).

- [ ] **Step 3: Build (no descriptor consumes `byte_lane==0` writes yet — edit is dormant).**
```bash
MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd "$PWD"; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'
```
Expected: clean build. No emitter yet produces a `byte_lane==0` write descriptor, so the `else` arm is unreached at runtime.

- [ ] **Step 4: Prove O-mem-2's byte writes are byte-identical (the isolation gate).** Re-run the full O-mem-2 bit-op RMW oracle (which exercises `byte_lane==1` writes) under all three grant modes, both backends — the byte path must be unchanged:
```bash
./mametests "[m68000]"                                                          # Leg A unchanged
./mametests "[m68000][drc]"                                                     # Leg B 1-cycle x64 (incl. O-mem-2 bchg/bclr/bset (An)/(An)+/-(An) byte writes)
CPUORACLE_M68_DRC_C=1 ./mametests "[m68000][drc]"                              # Leg B 1-cycle C
CPUORACLE_M68_DRC_FULLGRANT=1 ./mametests "[m68000][drc]"                      # full-grant x64 (byte writes run end-to-end)
CPUORACLE_M68_DRC_FULLGRANT=1 CPUORACLE_M68_DRC_C=1 ./mametests "[m68000][drc]" # full-grant C
CPUORACLE_M68_DRC_PARTGRANT=1 ./mametests "[m68000][drc]"                      # partial-grant x64 (resume at the byte write)
CPUORACLE_M68_DRC_PARTGRANT=1 CPUORACLE_M68_DRC_C=1 ./mametests "[m68000][drc]" # partial-grant C
./mame -validate
```
Expected: **all green** — identical to pre-edit. This proves the `byte_lane==1` path is byte-identical (O-mem-2 forms write `byte_lane==1`, so the new `else` arm is never taken). If anything is red, the byte arm was altered — revert to the verbatim existing ops.

- [ ] **Step 5: `srcclean` + commit (the primitive edit alone).**
```bash
git add src/devices/cpu/m68000/m68000drc.cpp
git commit -m "feat(m68000drc): generate_bus_step word DATA WRITE -- byte_lane==0 full-word UML_WRITE (O-mem-3b primitive edit)"
```
> This commit is the entirety of the `generate_bus_step()` change for the whole MOVE arc — small, self-contained, separately reviewable. Tasks 2–5 do not touch `generate_bus_step()`.

---

## Task 2: Generator extension — admit the 15 MOVE reg↔mem read/write runs (additive)

**Goal:** Single-source the 3b bus-step lists (OQ-2) from `m68000gen.py` so the emitter takes each form's read/write charges + substate pairs + the `-(An)` predecrement `−2` + the `byte_lane`/`has_addr_error` flags from the same microcode walk the interpreter uses. Purely a **filter widening** — the existing read-step parser (O-mem-1), write-step parser (O-mem-2), the lane-mask-keyed `byte_lane`/`has_addr_error` recognizer (refined in 3a), and the predecrement-`−2` recognizer (O-mem-2) already emit every row 3b needs. Additive-only: existing generated files stay byte-identical; only `m68000-drcdesc.ipp` grows.

**Files:**
- Modify: `src/devices/cpu/m68000/m68000gen.py` — the `drc_bus_steps()` opcode filter (the O-mem-1/2/3a admit clause).
- Regenerate: `src/devices/cpu/m68000/m68000-drcdesc.ipp`.
- Validate against: `m68000-sdf.cpp` (the 15 MOVE handlers).

**Interfaces:**
- Produces (consumed by Task 3): bus runs for the 15 `{value, mask}` patterns (all mask `0xf1f8`). Load runs = `[DATA_READ, prefetch]`; store runs = `[DATA_WRITE, prefetch]`. `.b` data access `byte_lane=1`/`has_addr_error=0`; `.w` data access `byte_lane=0`/`has_addr_error=1`; every prefetch `byte_lane=0`/`has_addr_error=1`. The `-(An)`/`-(An)` (`0x1020/0x3020` load, `0x1100/0x3100` store, `0x3108` An-store) first bus step carries `pre_charge`.

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

- [ ] **Step 2: Pin the 15 handlers' bus-step truth.** Read each of the load handlers (`move_{b,w}_{ais,aips,pais}_dd_df`), Dn-store handlers (`move_{b,w}_ds_{aid,aipd,paid}_df`), and An-store handlers (`move_w_as_{aid,aipd,paid}_df`). Record per step: the `m_icount -= N` charge(s) (note the `−2` at the `pdc*` state for `pais`/`paid`, which has **no** suspend checkpoint), the redo/completed substate pair, whether the data access is followed by `if(m_aob & 1){ ... S_ADDRESS_ERROR ... }` (word: yes → `has_addr_error=1`; byte: no → `has_addr_error=0`), and whether the access carries a lane-mask arg (`.b` → `byte_lane=1`; `.w` no mask → `byte_lane=0`). The data **read** vs **write** kind is set by `read_interruptible` vs `write_interruptible`. This is the ladder table above; the generator must emit *these exact values*.

- [ ] **Step 3: Widen the `drc_bus_steps()` opcode filter to admit MOVE reg↔mem.** In `m68000gen.py`, in the O-mem-1/2/3a admit clause (the `is_o1`/`is_o2`/`is_o3a` predicate), add the 3b clause. **Confirm the actual `drc_ea_mode`/mnemonic constant names against the map definition in `m68000gen.py`** (the 3a Builder confirmed `DRC_EA_AIS/AIPS/PAIS` and `DRC_EA_AD`; for 3b also confirm the **register** EA constants for `dd`=Dn-dest, `ds`=Dn-src, `as`=An-src, and the memory **dest** modes `aid`/`aipd`/`paid`):
```python
    base   = drc_base_mnemonic(ii[2][0])
    src_ea = drc_ea_mode[ii[2][1]]
    dst_ea = drc_ea_mode[ii[2][2]]
    REGIND_S = (DRC_EA_AIS, DRC_EA_AIPS, DRC_EA_PAIS)    # src  (An), (An)+, -(An)
    REGIND_D = (DRC_EA_AID, DRC_EA_AIPD, DRC_EA_PAID)    # dst  (An), (An)+, -(An)   <-- VERIFY these names
    # ... is_o1 / is_o2 / is_o3a unchanged ...
    # O-mem-3b: single-access (.b+.w) MOVE reg<->mem (An)/(An)+/-(An), both directions.
    #   load  (mem->Dn) : base 'move', src in REGIND_S, dst == DRC_EA_DD   (.b and .w)
    #   store (Dn->mem) : base 'move', src == DRC_EA_DS, dst in REGIND_D   (.b and .w)
    #   store (An->mem) : base 'move', src == DRC_EA_AS, dst in REGIND_D   (.w only -- no .b An handler exists)
    # (d16,An) (das/dad) is EXCLUDED (3e); long .l is EXCLUDED (3d) -- size filter below.
    is_o3b_load  = (base == 'move' and src_ea in REGIND_S and dst_ea == DRC_EA_DD)
    is_o3b_store = (base == 'move' and dst_ea in REGIND_D and src_ea in (DRC_EA_DS, DRC_EA_AS))
    is_o3b = (is_o3b_load or is_o3b_store) and drc_size(ii) in (DRC_SZ_B, DRC_SZ_W)   # .l -> 3d
    if not (is_o1 or is_o2 or is_o3a or is_o3b):
        return []
```
> **Verify the EA-mode + size constant names** against the map definitions in `m68000gen.py`: `dd`→Dn-dest, `ds`→Dn-src, `as`→An-src, `aid`/`aipd`/`paid`→memory dest modes, and that `das`/`dad` (the `(d16,An)` source/dest) map to **different** constants **not** in `REGIND_S`/`REGIND_D`. Confirm the size accessor (named `drc_size`/`ii[...]` here as a placeholder) and the `.l` constant so the `.l` forms are excluded (3d). If `drc_base_mnemonic` does not distinguish MOVE from MOVEA, note MOVEA is already isolated by `dst_ea == DRC_EA_AD` (a different dest constant than `DRC_EA_DD`/`REGIND_D`), so MOVEA cannot leak into `is_o3b`. **Adjust all names to whatever the map actually defines** (the 3a clause is the reference for the established names).

> **Single-source discipline (R-A):** do **not** add any MOVE-specific substate, charge, `byte_lane`, or `has_addr_error` logic. The 15 forms fall out of the existing per-opcode iteration and the existing read/write-step + lane-mask + predecrement parsers for free, because the filter now admits them. The only edit is the admit clause.

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
Expected: **only** `m68000-drcdesc.ipp` appears (the 15 new MOVE runs appended; all O-mem-1/2/3a rows byte-identical). If any of `m68000-decode.cpp`/`m68000-head.h`/the four `s*` files changed, the edit leaked into the wrong code path — fix before continuing.

- [ ] **Step 5: Eyeball the emitted rows against the handlers (R-A — catch it here, not in Leg B).** Confirm `m68000-drcdesc.ipp` now has runs for all 15 `{value, mask}` with the right kinds/flags:
```bash
# load runs: [DATA_READ, prefetch]
grep -nE '0x1010, 0xf1f8|0x1018, 0xf1f8|0x1020, 0xf1f8|0x3010, 0xf1f8|0x3018, 0xf1f8|0x3020, 0xf1f8' m68000-drcdesc.ipp
# store runs: [DATA_WRITE, prefetch]
grep -nE '0x1080, 0xf1f8|0x10c0, 0xf1f8|0x1100, 0xf1f8|0x3080, 0xf1f8|0x30c0, 0xf1f8|0x3100, 0xf1f8' m68000-drcdesc.ipp
grep -nE '0x3088, 0xf1f8|0x30c8, 0xf1f8|0x3108, 0xf1f8' m68000-drcdesc.ipp   # An-src stores (word only)
# excluded: no (d16,An) or .l reg<->mem rows from 3b
grep -nE '0x1028, 0xf1f8|0x3028, 0xf1f8|0x2010, 0xf1f8' m68000-drcdesc.ipp   # MUST be empty here (das load=*028, .l load=0x2010 -> 3e/3d)
```
Verify against the handlers: **byte** data access (`0x10xx`) has `byte_lane==1`/`has_addr_error==0`; **word** data access (`0x30xx`) has `byte_lane==0`/`has_addr_error==1`; the store runs' first row is `DRC_BUS_DATA_WRITE` (the load runs' first row is `DRC_BUS_DATA_READ`); **no load run contains a `DATA_WRITE` row and no store run contains a `DATA_READ` row**; every prefetch (second row) is `byte_lane==0`/`has_addr_error==1`; the `-(An)`/`-(An)` runs (`0x1020/0x3020` load, `0x1100/0x3100/0x3108` store) carry `pre_charge` on the first row while the others carry `0`; word runs' final-prefetch completed-substate is `4`. A kind/lane mismatch is the single highest-risk bug in the batch (R-A) — and note that the **word store's `byte_lane==0` `DATA_WRITE` is what exercises the Task-1 primitive edit**, so confirm those rows specifically.

- [ ] **Step 6: Build + Leg A (descriptor inert until the emitter consumes it).**
```bash
MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd "$PWD"; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'
./mametests "[m68000]"          # Leg A
./mametests "[m68000][drc]"     # existing native opcodes (moveq/btst-abs/bit-ops/MOVEA) still exact -- the new rows are unread
./mame -validate
```
Expected: build clean; Leg A green; existing Leg B green (no emitter reads the MOVE rows yet).

- [ ] **Step 7: `srcclean` + commit.**
```bash
git add src/devices/cpu/m68000/m68000gen.py src/devices/cpu/m68000/m68000-drcdesc.ipp
git commit -m "feat(m68000drc): generate MOVE reg<->mem (An)/(An)+/-(An) byte+word runs (O-mem-3b, additive)"
```

---

## Task 3: The MOVE reg↔mem emitter `generate_move_regmem` + the 15 gated dispatch arms

**Goal:** Add the single parameterized emitter that composes, per form: the **`m_irdi` latch** → direction-specific `rx`/`ry` decode → **load** (EA setup → DATA READ via the frozen-since-Task-1 `generate_bus_step()` → final-prefetch setup with the `Dn` write + MOVE flags → final prefetch → retire) **or store** (write-setup with the register source into `m_dbout` + MOVE flags BEFORE the write → DATA WRITE via `generate_bus_step()` [word store = the Task-1 full-word path] → final-prefetch setup → final prefetch → retire). Then wire the 15 `{value,mask}` patterns into `is_native_opcode` + gated dispatch arms and run all three oracle grant modes. **The full-grant pass is where the native word `DATA WRITE` (the Task-1 primitive edit) and the MOVE flags are first validated end-to-end; the 1-cycle pass validates the `m_irdi` latch + the suspend handoff.**

**Files:**
- Modify: `src/devices/cpu/m68000/m68000.h` — the `moverm_form` struct + enums + `generate_move_regmem` declaration, beside `movea_form`/`generate_movea_mem`.
- Modify: `src/devices/cpu/m68000/m68000drc.cpp` — `generate_move_regmem`, after `generate_movea_mem`; `is_native_opcode` (`:210`); the 15 gated dispatch arms in `generate_native_dispatch` after the O-mem-3a MOVEA block (`:342-360`).

**Interfaces:**
- Consumes: `generate_bus_step` (frozen after Task 1; read path AND the now-`byte_lane`-aware write path), `s_drc_bus_run_table`/`s_drc_bus_step_table`, the run-find lambda pattern (mirror `generate_movea_mem`/`generate_bitop_mem`), `set_8`/`set_16l`/`set_8xl`/`map_sp`/`sr_nzvc` semantics, the fields in Global Constraints, `m_drc_native_mem_ea_arms` (probe counter), `drc_native_mem_ea_allowed()` (the gate).
- Produces (consumed by Task 4): `void generate_move_regmem(drcuml_block &, const moverm_form &, uml::code_label lbl_delegate);` — emits one full form natively; on a fully-granted instruction it runs both accesses then retires; on a mid-instruction yield `generate_bus_step` JMPs to `lbl_delegate` and the partial interpreter handler resumes (OQ-1). Clobbers I0–I6; preserves I7 (`m_ird`).

- [ ] **Step 1: Declare the form descriptor + emitter in the header.** In `m68000.h`, beside `movea_form`/`generate_movea_mem`:
```cpp
	enum moverm_dir  : u8 { MRM_LOAD, MRM_STORE };          // mem->Dn ; reg->mem
	enum moverm_size : u8 { MRM_B, MRM_W };                 // byte (byte_lane=1) ; word (byte_lane=0, the primitive-edit write path)
	enum moverm_ea   : u8 { MRM_AIS, MRM_AIPS, MRM_PAIS };  // (An), (An)+, -(An)  (load src / store dst topology)
	enum moverm_reg  : u8 { MRM_DREG, MRM_AREG };           // store SOURCE reg kind (DREG for every load)
	struct moverm_form { u16 value; u16 mask; u8 dir; u8 size; u8 ea; u8 reg; };
	void generate_move_regmem(drcuml_block &block, const struct moverm_form &form, uml::code_label lbl_delegate); // native MOVE .b/.w reg<->mem (An)/(An)+/-(An) (O-mem-3b)
```

- [ ] **Step 2: Implement `generate_move_regmem`.** In `m68000drc.cpp`, after `generate_movea_mem`. **Read each handler line-by-line while transcribing (R-A).** Define file-local lambdas mirroring the byte-identical helpers already in `generate_movea_mem`/`generate_bitop_mem` (`ssw_data`/`ssw_program`/`commit_dbin`/`retire`, plus the `load_ry`/`load_rx`/`load_delta` register/delta decoders) — duplication is acceptable and matches the O-mem-2/3a precedent (no cross-function refactor in this batch). The structure (literal for the load-bearing pieces; **VERIFY-gated** for the per-mode EA choreography that the 3a Builder found error-prone):

```cpp
//-------------------------------------------------
//  generate_move_regmem - native UML for
//  move.b/.w (An)/(An)+/-(An) reg<->mem  (O-mem-3b, 15 forms)
//
//  Mirrors move_{b,w}_{ais,aips,pais}_dd_df (load, mem->Dn),
//  move_{b,w}_ds_{aid,aipd,paid}_df (store, Dn->mem) and
//  move_w_as_{aid,aipd,paid}_df (store, An->mem) in m68000-sdf.cpp.
//  One parameterized emitter; dir/size/ea/reg are compile-time constants.
//    load  : EA setup -> DATA READ -> final-prefetch(+Dn write + MOVE flags) -> retire  (2 steps)
//    store : write-setup(+m_dbout + MOVE flags) -> DATA WRITE -> final-prefetch -> retire (2 steps)
//  Load dest is a DATA register (set_8/set_16l, unwritten half PRESERVED) -- no data-write
//  step.  Store source is Dn (raw) or An (map_sp).  Word store DATA WRITE uses the Task-1
//  byte_lane==0 full-word UML_WRITE path; byte store uses the byte_lane==1 masked path.
//  MOVE flags (sr_nzvc): N=MSB, Z=(value==0 over size), V=C=0, X untouched -- load value =
//  m_dbin (after the read); store value = m_da[ry] (before the write).
//
//  CRITICAL (R-A, carried from O-mem-3a): decodes rx/ry from m_irdi; the DRC entry point
//  does NOT latch m_irdi, so this STORES m_irdi=m_ird FIRST (load-bearing for the
//  interpreter's partial-handler resume).  Substates/charges/pre_charge/byte_lane/
//  has_addr_error come from the generated run -- never hard-coded.  I7 holds m_ird, preserved
//  across generate_bus_step.
//-------------------------------------------------

void m68000_device::generate_move_regmem(drcuml_block &block, const struct moverm_form &form, uml::code_label lbl_delegate)
{
	// locate this form's generated bus-step run by {value, mask} (mirror generate_movea_mem).
	auto find_run = [](u16 value, u16 mask) -> const drc_bus_run & {
		for(const drc_bus_run &r : s_drc_bus_run_table)
			if(r.value == value && r.mask == mask)
				return r;
		return s_drc_bus_run_table[0]; // unreachable for the wired opcodes
	};
	const drc_bus_run &run = find_run(form.value, form.mask);
	u16 si = run.first;                          // running index into s_drc_bus_step_table
	const bool is_word  = (form.size == MRM_W);
	const u32  zmask    = is_word ? 0xffffu : 0xffu;
	const u32  nmask    = is_word ? 0x8000u : 0x80u;

	// ---- THE LATCH: m_irdi = m_ird  (load-bearing for the interpreter's resume; O-mem-3a) ----
	UML_STORE(block, &m_irdi, 0, I7, SIZE_WORD, SCALE_x1);

	// ssw_data / ssw_program / commit_dbin / retire : byte-identical to generate_movea_mem.
	// (transcribe them, or factor file-local statics -- out of scope to refactor here.)

	// MOVE flags == alu_and(v,0xffff)+sr_nzvc(): N=MSB(v), Z=(v==0 over size), V=C=0, X kept.
	// VERIFY against the handler's alu_and(...)+sr_nzvc() pair (load mrgw1 ; store rmrw1).
	auto move_flags = [&](uml::parameter Iv) {              // Iv holds the value (low bits significant; TEST masks)
		UML_LOAD(block, I4, &m_sr, 0, SIZE_WORD, SCALE_x1);
		UML_AND(block, I4, I4, ~u32(SR_N|SR_Z|SR_V|SR_C)); // clear N,Z,V,C ; keep X + system bits
		uml::code_label const lbl_nz = m_drc_labelnum++;
		UML_TEST(block, Iv, zmask);
		UML_JMPc(block, COND_NZ, lbl_nz);
			UML_OR(block, I4, I4, u32(SR_Z));              // value == 0 -> Z
		UML_LABEL(block, lbl_nz);
		uml::code_label const lbl_nn = m_drc_labelnum++;
		UML_TEST(block, Iv, nmask);
		UML_JMPc(block, COND_Z, lbl_nn);
			UML_OR(block, I4, I4, u32(SR_N));              // MSB set -> N
		UML_LABEL(block, lbl_nn);
		UML_STORE(block, &m_sr, 0, I4, SIZE_WORD, SCALE_x1);
	};

	if(form.dir == MRM_LOAD)
	{
		// rx = (m_irdi>>9)&7  (RAW Dn dest) ; ry = map_sp((m_irdi&7)|8)  (An src).
		// ---- source EA setup : substates feed the data read ----
		// VERIFY per ea/size against the handler: word ais :56199 / aips :56264 / pais :56336 ;
		//   byte ais :29984 / aips :30041+ / pais :30107+.  Set m_aob (and m_at where the handler
		//   does); for aips post-inc m_da[ry] by the size delta, for pais pre-dec (the -2 internal
		//   charge is the descriptor's pre_charge -- do NOT emit it).  Use the O-mem-2 load_delta
		//   pattern (m68000drc.cpp:892) for the byte (A7)=2 exception.  m_base_ssw = SSW_DATA|SSW_R.
		//   <<< transcribe the m_aob/m_au/m_at/m_da[ry] choreography here, per mode, behind VERIFY >>>
		generate_bus_step(block, s_drc_bus_step_table[si++], lbl_delegate);   // DATA read (substates 1/2 ; .b byte_lane=1/no-fault, .w byte_lane=0/fault ; pais pre_charge)
		// commit_dbin();  // m_dbin = m_edb  (the source byte/word)

		// ---- final prefetch setup + Dn write-back + MOVE flags (mrgw1/mmrw3) ----
		// VERIFY against move_{b,w}_ais_dd_df:56222-56234 / 30004-30016.
		//   m_aob=m_au; m_ir=m_irc; m_pc=m_au; <Dn write>; m_au+=2; <flags>;
		//   m_ird=m_ir; if(m_next_state!=S_TRACE) m_next_state=m_int_next_state; SSW_PROGRAM.
		// load_rx(I5) -> Dn index (raw (m_irdi>>9)&7, NO map_sp -- a data register).
		// Dn write PRESERVES the unwritten half:
		//   .b : set_8(m_da[rx], m_dbin)   = (m_da[rx] & 0xffffff00) | (m_dbin & 0xff)
		//   .w : set_16l(m_da[rx], m_dbin) = (m_da[rx] & 0xffff0000) | (m_dbin & 0xffff)
		// flags from m_dbin: move_flags(<m_dbin loaded into a reg>).
		//   <<< transcribe the Dn read-modify-write + move_flags(m_dbin) + prefetch setup here >>>
		generate_bus_step(block, s_drc_bus_step_table[si++], lbl_delegate);   // final prefetch (substates 3/4 ; byte_lane=0 ; fault)
	}
	else // MRM_STORE
	{
		// rx = map_sp(((m_irdi>>9)&7)|8)  (dest-EA An) ; ry = store source:
		//   MRM_DREG -> (m_irdi&7)         (RAW Dn) ;  MRM_AREG -> map_sp((m_irdi&7)|8)  (An).
		// ---- write setup + MOVE flags (BEFORE the write) (rmrw1) ----
		// VERIFY against move_w_ds_aid_df:58023-58033 (Dn) / move_w_as_aid_df:58085-58094 (An) /
		//   move_b_ds_aid_df:30842-30854 (byte) ; aipd :59216/59276 ; paid :60410/60470.
		//   m_aob=m_da[rx]; m_ir=m_irc; m_dbout=<source>; m_pc=m_au; m_au=m_da[rx]+2 (or aipd/paid
		//   writeback to m_da[rx]); <flags from m_da[ry]>; m_base_ssw=SSW_DATA.
		//   m_dbout source:  .w : m_dbout = (u16)m_da[ry]  ;  .b : set_8xl(m_dbout, m_da[ry])
		//                        = (b & 0x00ff) | (b << 8)  (mirror generate_bitop_mem:1061-1064).
		//   flags from m_da[ry] (low byte/word): move_flags(<m_da[ry] loaded into a reg>).
		//   <<< transcribe the dest-EA setup + m_dbout + move_flags(m_da[ry]) here, per mode/reg >>>
		generate_bus_step(block, s_drc_bus_step_table[si++], lbl_delegate);   // DATA WRITE (substates 1/2 ; .b byte_lane=1 masked, .w byte_lane=0 FULL-WORD [Task-1 edit] ; paid pre_charge ; fault on .w)
		// ---- final prefetch setup (mmrw2/mmrw3) ----
		// VERIFY against move_w_ds_aid_df:58048-58055.
		//   m_aob=m_pc; m_ir=m_irc; m_au=m_pc+2; m_ird=m_ir;
		//   if(m_next_state!=S_TRACE) m_next_state=m_int_next_state; SSW_PROGRAM.
		//   <<< transcribe the prefetch setup here >>>
		generate_bus_step(block, s_drc_bus_step_table[si++], lbl_delegate);   // final prefetch (substates 3/4 ; byte_lane=0 ; fault)
	}

	// ===== retire (byte-identical to generate_movea_mem's retire) =====
	//   m_irc = m_dbin = m_edb; set_ftu_const(); m_inst_state = m_next_state ? m_next_state
	//   : m_decode_table[m_ird]; if(m_sr & SR_T) m_next_state = S_TRACE.
	//   <<< transcribe the retire block here (or call the shared lambda) >>>
}
```

> **Six load-bearing verifications (R-A) — make these explicit checks, not assumptions:**
> 1. **The `m_irdi` latch must precede every yield** (carried from 3a). `UML_STORE(&m_irdi, I7)` is the first emitted op. The 1-cycle pass forces a yield after access 1, then the interpreter resumes and decodes `rx`/`ry` from `m_irdi` — wrong latch ⇒ wrong register ⇒ Leg-B failure.
> 2. **`rx`/`ry` roles differ by direction** (double-checked against the handlers): **load** `rx = (m_irdi>>9)&7` (raw Dn, no map_sp), `ry = map_sp((m_irdi&7)|8)` (An). **store** `rx = map_sp(((m_irdi>>9)&7)|8)` (dest-EA An), `ry = (m_irdi&7)` raw Dn for `MRM_DREG` **or** `map_sp((m_irdi&7)|8)` for `MRM_AREG`. Decode from `I7` (== `m_ird` == `m_irdi` after the latch).
> 3. **The Dn write preserves the unwritten half.** `.b` → `set_8` (keep bits 8..31); `.w` → `set_16l` (keep bits 16..31). This is the MOVE-to-data-register semantic and is **different from MOVEA's `ext32`** (which overwrites all 32 bits). Emit a read-modify-write of `m_da[rx]` (LOAD the current Dn, mask off the low byte/word, OR in the new value). A wrong mask corrupts the high half ⇒ Leg-B register failure.
> 4. **MOVE flags use the correct value source and ordering.** Load: `move_flags(m_dbin)` at the final-prefetch setup (after the read). Store: `move_flags(m_da[ry])` at the write-setup, **BEFORE** the DATA WRITE `generate_bus_step` (so a completed-resume at substate 2 inherits the native flags; a redo-resume at substate 1 replays the whole rmrw1). `move_flags` clears V/C and keeps X — confirm against the handler's `alu_and(...)+sr_nzvc()` pair (the `alu_and` *before* it that has no `sr_*` is dead-scratch — do not emit it).
> 5. **The word store's DATA WRITE is the only consumer of the Task-1 primitive edit.** Its descriptor row is `byte_lane==0` (Task 2 Step 5), so `generate_bus_step` takes the new full-word `UML_WRITE` arm. The byte store's row is `byte_lane==1` (the masked arm). Do not special-case this in the emitter — it is entirely descriptor-driven; the emitter just sets `m_dbout`/`m_aob`/`m_base_ssw` and calls `generate_bus_step`.
> 6. **`generate_bus_step` clobbers I0–I6; preserves I7.** Decode `ry`/the EA before the first access; recompute `rx` (load) at the final-prefetch setup, after the read, from the preserved `I7`. The per-mode EA choreography (`m_aob`/`m_au`/`m_at`/`m_da[]`/`m_pc` advances) is **derive-from-handler** behind the `// VERIFY against <handler>:<line>` markers — transcribe each line-by-line; do not hand-wave the `m_au` targets (a wrong `m_au` ⇒ wrong final-prefetch address ⇒ Leg-B RAM/cycle failure). The two 3a skeleton inaccuracies were a `byte_lane`/size misclassification (here: single-sourced from the descriptor, Task 2 — not hand-typed) and a writeback delta (here: use the O-mem-2 `load_delta` pattern and VERIFY each delta).

- [ ] **Step 3: Build (compile-only — no dispatch arm yet).**
```bash
MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd "$PWD"; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'
```
Expected: clean build. `generate_move_regmem` is defined but unreferenced (acceptable; the dispatch arms below call it). Fix any UML-scope/enum/`uml::parameter` errors now.

- [ ] **Step 4: Add the 15 patterns to `is_native_opcode`.** After the O-mem-3a MOVEA switch (`m68000drc.cpp` near the MOVEA block), add the MOVE reg↔mem patterns (mask `0xf1f8`). **Exclude** `(d16,An)` (`das`/`dad` = `*028`/`*1c0`/`*168`…→ 3e) and `.l` (`0x20xx`→ 3d):
```cpp
	// O-mem-3b: move.b/.w (An)/(An)+/-(An) reg<->mem (mask 0xf1f8).
	switch(opword & 0xf1f8)
	{
	// mem->Dn load:
	case 0x1010: case 0x1018: case 0x1020:   // move.b (An)/(An)+/-(An),Dn
	case 0x3010: case 0x3018: case 0x3020:   // move.w (An)/(An)+/-(An),Dn
	// Dn->mem store:
	case 0x1080: case 0x10c0: case 0x1100:   // move.b Dn,(An)/(An)+/-(An)
	case 0x3080: case 0x30c0: case 0x3100:   // move.w Dn,(An)/(An)+/-(An)
	// An->mem store (word only):
	case 0x3088: case 0x30c8: case 0x3108:   // move.w An,(An)/(An)+/-(An)
		return true;
	}
```
> **Verify each constant against the handler `// xxxx ffff` comment** (`move_b_ais_dd_df // 1010 f1f8`, `move_w_ds_paid_df // 3100 f1f8`, `move_w_as_paid_df // 3108 f1f8`, …). Confirm **no overlap** with the existing arms — in particular MOVEA word (`0x3050/3058/3060`) is disjoint from every 3b `0x30xx` base (`0x3010/18/20/80/88/c0/c8/100/108`), and the O-mem-2 bit-op bases (`0x0xxx`) and moveq (`0x7000`) share no value with `0x10xx`/`0x30xx`. A wrong base silently mis-classifies.

- [ ] **Step 5: Add the gated dispatch arms.** In `generate_native_dispatch`, after the O-mem-3a MOVEA block (`:342-360`), add a single `if (drc_native_mem_ea_allowed())` block with the 15-entry form table and the per-form compile-time arm (mirroring the MOVEA loop, `:342-360`):
```cpp
	// O-mem-3b: move.b/.w (An)/(An)+/-(An) reg<->mem -- native ONLY behind the space-
	// topology gate (ADR 0007 O-mem-3 addendum; M5).  Each arm calls generate_move_regmem
	// with its form constants; the bus-step run (kinds/substates/charges/byte_lane/
	// has_addr_error/pre_charge) is single-sourced from the generator.  (d16,An) and .l
	// reg<->mem are NOT here (O-mem-3e / O-mem-3d).
	if (drc_native_mem_ea_allowed())
	{
		static const moverm_form k_moverm_forms[] = {
			// mem->Dn load (dest Dn; reg field unused for loads)
			{ 0x1010, 0xf1f8, MRM_LOAD,  MRM_B, MRM_AIS,  MRM_DREG },
			{ 0x1018, 0xf1f8, MRM_LOAD,  MRM_B, MRM_AIPS, MRM_DREG },
			{ 0x1020, 0xf1f8, MRM_LOAD,  MRM_B, MRM_PAIS, MRM_DREG },
			{ 0x3010, 0xf1f8, MRM_LOAD,  MRM_W, MRM_AIS,  MRM_DREG },
			{ 0x3018, 0xf1f8, MRM_LOAD,  MRM_W, MRM_AIPS, MRM_DREG },
			{ 0x3020, 0xf1f8, MRM_LOAD,  MRM_W, MRM_PAIS, MRM_DREG },
			// Dn->mem store
			{ 0x1080, 0xf1f8, MRM_STORE, MRM_B, MRM_AIS,  MRM_DREG },
			{ 0x10c0, 0xf1f8, MRM_STORE, MRM_B, MRM_AIPS, MRM_DREG },
			{ 0x1100, 0xf1f8, MRM_STORE, MRM_B, MRM_PAIS, MRM_DREG },
			{ 0x3080, 0xf1f8, MRM_STORE, MRM_W, MRM_AIS,  MRM_DREG },
			{ 0x30c0, 0xf1f8, MRM_STORE, MRM_W, MRM_AIPS, MRM_DREG },
			{ 0x3100, 0xf1f8, MRM_STORE, MRM_W, MRM_PAIS, MRM_DREG },
			// An->mem store (word only)
			{ 0x3088, 0xf1f8, MRM_STORE, MRM_W, MRM_AIS,  MRM_AREG },
			{ 0x30c8, 0xf1f8, MRM_STORE, MRM_W, MRM_AIPS, MRM_AREG },
			{ 0x3108, 0xf1f8, MRM_STORE, MRM_W, MRM_PAIS, MRM_AREG },
		};
		for(const moverm_form &f : k_moverm_forms)
		{
			uml::code_label const lbl_next = m_drc_labelnum++;
			UML_AND(block, I0, I7, f.mask);
			UML_CMP(block, I0, f.value);
			UML_JMPc(block, COND_NE, lbl_next);
			generate_move_regmem(block, f, lbl_delegate);         // emit the form (suspend yields JMP lbl_delegate from within)
			UML_JMP(block, lbl_delegate);                         // fully-granted: retired -> hand the tail to the interpreter
			UML_LABEL(block, lbl_next);
			m_drc_native_mem_ea_arms++;                           // emission probe (gate/coverage test)
		}
	}
```
> **Note** the `MRM_DREG`/`MRM_AREG` in the `ea`-position vs `reg`-position: for loads the source EA is `(An)/(An)+/-(An)` (MRM_AIS/AIPS/PAIS) and `reg` is unused (dest is always Dn); for stores the **dest** EA is `(An)/(An)+/-(An)` (the `ea` field, same enum since the topology/arithmetic is identical aid≡ais etc.) and `reg` selects the source register kind (Dn vs An). Confirm the emitter reads `form.ea` as the memory-EA topology in both directions and `form.reg` only in the store path.

- [ ] **Step 6: Build, then 1-cycle Leg B (x64 + C) — native step-1 + the suspend handoff + the `m_irdi` latch.**
```bash
MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd "$PWD"; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'
./mametests "[m68000]"                            # Leg A
./mametests "[m68000][drc]"                       # Leg B 1-cycle, x64
CPUORACLE_M68_DRC_C=1 ./mametests "[m68000][drc]" # Leg B 1-cycle, C
```
Expected: green. The 1-cycle pass forces a yield after access 1, then the interpreter resumes via the partial handler (decoding `rx`/`ry` from `m_irdi`). **If a form fails here with a wrong register, suspect the `m_irdi` latch first** (verification 1); a wrong Dn high-half suggests the `set_8`/`set_16l` mask (verification 3).

- [ ] **Step 7: FULL-GRANT Leg B (x64 + C) — THE native word DATA WRITE (primitive edit) + MOVE flags gate.**
```bash
CPUORACLE_M68_DRC_FULLGRANT=1 ./mametests "[m68000][drc]"                       # x64
CPUORACLE_M68_DRC_FULLGRANT=1 CPUORACLE_M68_DRC_C=1 ./mametests "[m68000][drc]" # C backend
```
Expected: **green.** This is the only pass that runs the **whole** native instruction in one go — for stores, the native word `DATA WRITE` via the Task-1 full-word `UML_WRITE` (the primitive edit's first real exercise) and the MOVE flags before the write; for loads, the read + `Dn` write-back + flags. **If a form fails:** `superpowers:systematic-debugging` — map the divergence to the step (RAM at the dest → the word write / `m_dbout` / `m_aob`; SR → `move_flags` value source or V/C/X handling; Dn value → `set_8`/`set_16l` mask; A-reg → the `(An)+`/`-(An)` writeback/delta; cycle → a charge or the predecrement `−2`). The fallback is to drop the failing form from `is_native_opcode` + the dispatch table (route it back to `cfunc_`) and report — never approximate.

- [ ] **Step 8: PARTIAL-GRANT Leg B (x64 + C) — the between-access resume handoff.**
```bash
CPUORACLE_M68_DRC_PARTGRANT=1 ./mametests "[m68000][drc]"                       # x64
CPUORACLE_M68_DRC_PARTGRANT=1 CPUORACLE_M68_DRC_C=1 ./mametests "[m68000][drc]" # C backend
```
Expected: green. Single `length-4` offset (OQ-10: no sweep). For these 2-access forms this resumes the interpreter at the final prefetch after access 1 ran native — for stores, validating the native left RAM/`m_sr` correct after the word write; for loads, after the read + `Dn` write + flags.

- [ ] **Step 9: `srcclean` + commit.**
```bash
git add src/devices/cpu/m68000/m68000.h src/devices/cpu/m68000/m68000drc.cpp
git commit -m "feat(m68000drc): native MOVE .b/.w reg<->mem (An)/(An)+/-(An) -- 15 forms (O-mem-3b)"
```

---

## Task 4: Coverage assertion + AS_OPCODES subset + cut-line doc

**Goal:** Machine-assert that all 15 MOVE reg↔mem forms dispatch native (behind the gate), confirm the AS_OPCODES differential covers them (gate off → `cfunc_`), and record them in the cut-line doc with the word-write-primitive + MOVE-flags + register-source/dest note.

**Files:**
- Modify: `tests/emu/cpu/cpuoracle.cpp` — the native-coverage assertion (in the `[m68000][drc][gate]` case) and, if the AS_OPCODES differential restricts to a corpus subset, extend that subset to include the 15 forms.
- Modify: `src/devices/cpu/m68000/README-drc.md` — the cut-line doc.

**Interfaces:**
- Consumes: `is_native_opcode(u16)` (Task 3) for the 15 patterns; the harness wrapper used for the O-mem-1/2/3a coverage checks.

- [ ] **Step 1: Extend the native-coverage assertion.** Add the 15 representative encodings to the asserted-native set in the `[m68000][drc][gate]` case (the predicate is mask-based), reusing the wrapper O-mem-1/2/3a added to reach the protected `is_native_opcode`:
```cpp
	// O-mem-3b: move.b/.w (An)/(An)+/-(An) reg<->mem are native (behind the gate).
	for(u16 op : { (u16)0x1010,(u16)0x1018,(u16)0x1020, (u16)0x3010,(u16)0x3018,(u16)0x3020,   // mem->Dn
	               (u16)0x1080,(u16)0x10c0,(u16)0x1100, (u16)0x3080,(u16)0x30c0,(u16)0x3100,   // Dn->mem
	               (u16)0x3088,(u16)0x30c8,(u16)0x3108 })                                       // An->mem (.w)
		CHECK(harness_is_native_opcode(op));
	// anti-vacuity: (d16,An) reg<->mem and .l reg<->mem stay cfunc_ in 3b (3e / 3d).
	CHECK_FALSE(harness_is_native_opcode((u16)0x1028));   // move.b (d16,An),Dn -> 3e
	CHECK_FALSE(harness_is_native_opcode((u16)0x3028));   // move.w (d16,An),Dn -> 3e
	CHECK_FALSE(harness_is_native_opcode((u16)0x2010));   // move.l (An),Dn     -> 3d
	CHECK_FALSE(harness_is_native_opcode((u16)0x2080));   // move.l Dn,(An)     -> 3d
```
> **VERIFY** the anti-vacuity encodings against the handler table (`move_b_das_dd_df`, `move_w_das_dd_df`, `move_l_ais_dd_df`, `move_l_ds_aid_df`) — the point is to prove the filter is neither over- nor under-admitting at the 3b/3d/3e boundary.

- [ ] **Step 2: Confirm the AS_OPCODES differential exercises the 15 forms (or extend its subset).** Read the O-mem-2 `[m68000][drc][asopcodes]` case (`cpuoracle.cpp`). If it runs the full corpus, the MOVE forms are already covered (gate off → `cfunc_` → interpreter ≡ DRC, and the DRC does not mis-read the opcode space). If O-mem-2/3a restricted the differential to a corpus subset, **extend the subset to include the 15 MOVE forms** so the differential proves the gate keeps them on `cfunc_` on an `AS_OPCODES` topology and the fallback matches. No new config beyond the batch's opcodes (M6 item 4).

- [ ] **Step 3: Update the cut-line doc.** In `src/devices/cpu/m68000/README-drc.md`:
  - In "Native opcodes shipped (current)", add a row:
```markdown
| `move.b`/`move.w` `(An)`/`(An)+`/`-(An)` reg↔mem | O-mem-3b | 15 forms (mem→Dn load 6, Dn→mem store 6, An→mem store 3 word). **Load:** source-EA read via the frozen `generate_bus_step()` (`.b` `byte_lane=1`/no-fault, `.w` `byte_lane=0`/fault); dest is a **data register** — write-back `set_8`/`set_16l` **preserves the unwritten half** (distinct from MOVEA's `ext32`); no data-write step. **Store:** the source register feeds `m_dbout`; the **word DATA WRITE** uses the one O-mem-3 `generate_bus_step()` primitive edit — `byte_lane==0` full-word `UML_WRITE` (byte store reuses O-mem-2's `byte_lane==1` masked path); the word write carries `has_addr_error=1` via the shared kind-agnostic fault branch. **MOVE flags** (`sr_nzvc`: N=MSB, Z=(value==0), V=C=0, X untouched) computed from the moved value (load: after the read; store: before the write). Decodes `rx`/`ry` from `m_irdi` (latched `m_irdi=m_ird`). **Native ONLY on a flat-topology, non-MMU bus (`drc_native_mem_ea_allowed()`)** — `AS_OPCODES`/user-space/MMU stay `cfunc_`. Validated by the 1-cycle, full-grant (the native word write + flags), and single-offset partial-grant Leg-B passes. `(d16,An)`→3e, `.l`→3d, mem→mem→3c. |
```
  - In "Explicitly NOT native" / the MOVE notes, reword so the 15 reg↔mem forms are excluded from the deferred set ("memory-EA MOVE except MOVE `.b`/`.w` reg↔mem `(An)`/`(An)+`/`-(An)`, native as of O-mem-3b on a flat-topology non-MMU bus").
  - In "Known limitations" / the mechanism notes, record: **the word `DATA WRITE` `byte_lane==0` full-word `UML_WRITE` is the single `generate_bus_step()` primitive edit of the entire MOVE arc** (O-mem-3a touched it not at all; 3c/3d/3e reuse it frozen); the byte write path (`byte_lane==1` masked) is byte-identical to O-mem-2; the native **retire** lambda remains the one native residual not oracle-exercised (the snapshot model cannot reach an overshoot; mirrors the validated `moveq`/`btst`/MOVEA tail).

- [ ] **Step 4: Full local gate (every pass, both backends).**
```bash
MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd "$PWD"; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'
./mametests "[m68000]"                                                          # Leg A
./mametests "[m68000][drc]"                                                     # Leg B 1-cycle x64
CPUORACLE_M68_DRC_C=1 ./mametests "[m68000][drc]"                              # Leg B 1-cycle C
CPUORACLE_M68_DRC_FULLGRANT=1 ./mametests "[m68000][drc]"                      # full-grant x64 (word write + flags gate)
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
git commit -m "test+docs(m68000drc): assert 15 MOVE reg<->mem forms native; record O-mem-3b word-write primitive + flags in cut-line doc"
```

---

## Task 5: Merge gate — push, Linux `oracle` green, merge

**Goal:** Get every required gate GREEN — the three oracle grant modes (x64 + C) on the appserver Linux `oracle` CI job, the AS_OPCODES differential, the gate-predicate + coverage + MMU-regen tests, generator additivity, and code review (with the primitive edit reviewed in isolation) — then merge per the auto-merge policy.

**Files:** none (CI + PR).

- [ ] **Step 1: Push the branch and open the PR.** PR body includes a **Docs Impact** section (README-drc.md updated; ADR 0007 O-mem-3 addendum is the source of truth) and the gate evidence (all three passes green on Linux, both backends). **Call out in the PR that Task 1 (the `generate_bus_step()` `byte_lane` write edit) is the one primitive change of the MOVE arc and is a self-contained first commit** — reviewers should review it in isolation against the byte-identity claim (O-mem-2 byte writes unchanged) before the consumers.
```bash
git push -u origin docs/plan-o-mem-3b-move-regmem   # (implementation branch is separate; this is the plan branch)
gh pr create --fill
```
> **Builder note:** the *implementation* lands on its own `feat/o-mem-3b-move-regmem` branch (not this `docs/` plan branch). The five commits above (primitive edit; generator; emitter+dispatch; tests+docs) form that PR.

- [ ] **Step 2: Get the appserver Linux `oracle` job GREEN — the SUFFICIENT gate.** Confirm the Linux `oracle` job runs **all three** grant modes (1-cycle + full-grant + partial-grant), x64 + C, plus the `[gate]` and `[asopcodes]` cases. A red Linux job with green Windows is almost always an ABI-safety regression (a plain `mem(&field)` slipped into `generate_move_regmem` or the Task-1 edit — audit every new UML operand) or a backend-divergent emission (R-C). Do not merge until the Linux `oracle` job is green.

- [ ] **Step 3: Throughput bench (KNOWN ENVIRONMENT GAP — flag, do NOT hard-gate).** Per the §5/M6 addendum honest-number note, MOVE reg↔mem throughput would be measured `-drc 0` vs `-drc 1` on a **gate-eligible flat-topology** driver where reg↔mem MOVE is hot (not an FD1094/`AS_OPCODES` set, which runs `cfunc_`). **The throughput bench has been blocked in this environment by absent ROMs** — treat it as evidence-when-available, **not** a blocking gate. The correctness gates (Step 2 + the local gate) gate merge. Record the ROM-blocked status in the PR.

- [ ] **Step 4: Merge per the auto-merge policy.** When all of: the three oracle passes green (x64 + C) on the Linux `oracle` job, the gate-predicate + coverage + MMU-regen + AS_OPCODES differential green, generator additive (only `m68000-drcdesc.ipp` grew), the primitive edit reviewed in isolation (byte path byte-identical), code review clean, `-validate` clean — **merge** (implementation cycle complete, parity gates green, review clean, issues addressed; the throughput bench is a documented known-gap, not a hard gate). Do not stop to ask.

---

## Self-review

**Spec coverage (ADR 0007 O-mem-3 addendum → tasks):**
- M1 word/long data bus-steps, **the one primitive edit** (`byte_lane==0` → full-word `UML_WRITE` on the write branch) → **Task 1, isolated**; the shared address-error branch already handles word writes (no extra edit); the read path is untouched. **No new `step.kind`, no new descriptor field.**
- M2 `(d16,An)` → **excluded from 3b** (3e): the `das`/`dad` source/dest stay out of the generator filter (size+EA clauses) and the dispatch table.
- M3 two-EA composition → 3b is the **single-EA** sub-case (one side is a register): load = read → `Dn` write (no write step); store = register → write. MOVE flags (`sr_nzvc`, single-access) computed at the dest-write setup before the write (store) / final-prefetch setup after the read (load). Register-dest load uses `set_8`/`set_16l` (preserve unwritten half); register-source store uses `m_dbout`/`set_8xl`. → Tasks 2 (descriptor), 3 (emitter).
- M4 the 5-sub-batch split: **3b is the second opener, 15 forms** (handler-verified; refines "~16") → this plan. Byte siblings ride along with the word forms (M4 "byte fold note"), not a separate 3f.
- M5 gate continuity (`drc_native_mem_ea_allowed()` covers 3b unchanged; no new clause, no `SR_S` branch) → Task 3 (arms wrapped in the gate); MMU-attach safety already discharged by O-mem-2 OQ-9.
- M6 merge-gate guidance (three grant modes both backends, AS_OPCODES differential, gate+coverage+MMU-regen, generator additivity, code review + Linux oracle, throughput as evidence-when-available) → Tasks 4 + 5.
- OQ-10 (no partial-grant sweep) → owner-decided 2026-06-28; **no oracle change in this plan** (the three existing grant modes suffice). 3b forms are single-access (2 steps), so there are no intermediate long-access boundaries — the single `length-4` partial-grant offset is exactly right.

**Primitive-edit isolation (the prompt's load-bearing requirement):** the `generate_bus_step()` change is **Task 1 alone** — its own commit, with its own isolation gate (re-run O-mem-2's byte-write oracle to prove the `byte_lane==1` path is byte-identical). It is the **only** `generate_bus_step()` edit in 3b (and in the whole MOVE arc per M1). Tasks 2–5 do not touch `generate_bus_step()`. The edit is dormant until Task 2 generates the first `byte_lane==0` write descriptor; its word-write correctness is then proven by Task 3's full-grant pass.

**Placeholder scan:** the emitter is one parameterized function driven by the generated run, not 15 copies. The `// VERIFY against <handler>:<line>` markers are deliberate single-source gates (the addendum's explicit derive-from-handler requirement + the 3a Builder's two-inaccuracy lesson), not deferred work: the per-mode EA choreography (`m_aob`/`m_au`/`m_at`/`m_da[]`/`m_pc` advances) and the writeback deltas are VERIFY-tagged transcription points; the **literal** code given (the Task-1 primitive edit, the `move_flags` lambda, the `is_native_opcode` switch, the dispatch table, the structural skeleton) was double-checked against the cited handler lines. The retire/`ssw_*`/`commit_dbin` lambdas are transcribed from the byte-identical `generate_movea_mem` helpers.

**Type consistency:** `generate_move_regmem(drcuml_block&, const moverm_form&, code_label)`; `moverm_form{u16 value; u16 mask; u8 dir; u8 size; u8 ea; u8 reg}`. `m_dbin`/`m_dbout`/`m_edb`/`m_irc`/`m_ir`/`m_ird`/`m_irdi`/`m_sr`/`m_base_ssw` are `u16` (SIZE_WORD); `m_da[17]`/`m_sp`/`m_aob`/`m_at`/`m_au`/`m_pc`/`m_icount`/`m_inst_state`/`m_next_state`/`m_int_next_state` are `u32` (SIZE_DWORD); `m_inst_substate` is `u16`. `set_8`/`set_16l` are read-modify-write (preserve the unwritten half); `set_8xl(m_dbout, v)` = `(v & 0xff) | (v << 8)`. `UML_WRITE(block, addr, src, size, space)` (drcumlsh.h:66). MOVE flags via `move_flags` (direct N/Z, V=C=0, X kept) ≡ `sr_nzvc()`. Substates/charges/`pre_charge`/`byte_lane`/`has_addr_error` come from the descriptor, never hard-coded.

**Genuinely-new open questions (design is settled; few expected):**
1. **`move_flags` vs replicating `sr_nzvc`/`m_isr`.** The plan computes N/Z directly (V/C always 0) per ADR M3's explicit allowance, validated by the full-grant + 1-cycle passes. If the full-grant pass shows any SR divergence, the Builder falls back to replicating the exact `alu_and`→`sr_nzvc` `m_isr` sequence. Not blocking.
2. **`DRC_EA_AID/AIPD/PAID` / `DRC_EA_DD/DS/AS` / `drc_size` constant names.** Task 2 filters on these; the Builder confirms the actual names against the `drc_ea_mode`/size maps in `m68000gen.py` (3a's clause is the reference). The additivity + eyeball checks (Task 2 Steps 4–5) catch any over/under-admit. Not blocking.
3. **`m_dcr`/`m_alub` dead-scratch skip.** Skipped per the O-mem-1/3a precedent; the Builder VERIFYs neither is in the oracle compare set at first build. If either is, set it (cheap). Not blocking.

Neither these nor any M-section item changes the architecture; all are local implementation choices the Builder resolves at the first build.

---

## Merge gate (O-mem-3b)

A batch merges only on **all** of (ADR 0007 O-mem-3 addendum M6, OQ-10-adjusted):
1. **1-cycle Leg B GREEN** (Leg A unchanged; Leg B register/flag/RAM/**cycle** exact), x64 + C — native step-1 + the suspend handoff + the `m_irdi` resume.
2. **Full-grant Leg B GREEN** (`CPUORACLE_M68_DRC_FULLGRANT=1`), x64 + C — the native word `DATA WRITE` (the Task-1 primitive edit) + MOVE flags + the `Dn` write-back. **Blocking.**
3. **Partial-grant Leg B GREEN** (`CPUORACLE_M68_DRC_PARTGRANT=1`, single `length-4` offset), x64 + C — the between-access resume handoff. **No parameterized sweep** (OQ-10; single-access forms have no intermediate boundary).
4. **AS_OPCODES differential GREEN** (`[m68000][drc][asopcodes]`) — the gate keeps `AS_OPCODES`/user-space/MMU machines on `cfunc_`, the fallback matches, and the DRC does not mis-read the opcode space.
5. **Gate-predicate + coverage + MMU-regen GREEN** (`[m68000][drc][gate]`) — the 15 MOVE forms asserted native; `(d16,An)`/`.l` reg↔mem asserted `cfunc_`; the OQ-9 MMU-regen still green.
6. **Generator additive** — only `m68000-drcdesc.ipp` grew (the 15 new MOVE runs), single-sourced from the microcode walk. **No new `step.kind`, no new descriptor field.**
7. **Primitive edit reviewed in isolation + byte-identical** — Task 1's `generate_bus_step()` write `byte_lane` edit is a self-contained commit; the `byte_lane==1` byte-write path is byte-identical (O-mem-2 bit-op oracle green under all three grant modes).
8. **Code review clean** + the **appserver Linux `oracle` job GREEN** (Windows-green is necessary-not-sufficient; the Linux job must run all three grant modes; audit every new UML operand for a stray `mem(&field)`).
9. **Throughput bench** — evidence-when-available on a gate-eligible flat-topology driver; **known environment gap (absent ROMs), NOT a hard gate.**

When 1–8 are green, **merge per the auto-merge policy** — do not stop to ask. Subsequent batches: **O-mem-3c** (single-access mem→mem — the two-EA composition over the now-frozen read + write steps), then **O-mem-3d** (long `.l` reg↔mem + mem→mem — the long two-word write + split flags), then **O-mem-3e** (`(d16,An)` across all sizes/topologies, absorbing MOVEA `(d16,An)→An`).
