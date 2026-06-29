# O-mem-3c Implementation Plan — native single-access (`.b`+`.w`) MOVE mem→mem `(An)`/`(An)+`/`-(An)`, the two-EA read→write composition + dual auto-inc/dec (NO `generate_bus_step` edit)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the byte+word MOVE forms that move data between **two memory EAs** — **MOVE `.b`/`.w` `(An)`/`(An)+`/`-(An)` → `(An)`/`(An)+`/`-(An)`, mem→mem (18 forms)** — by composing the **source-EA read** (the read path frozen since O-mem-2/3a) with the **dest-EA write** (the full-word/byte write path frozen since O-mem-3b), with **MOVE flags** (`sr_nzvc`: N/Z from the moved value, V=C=0, X untouched) computed from the source value at the dest-setup state before the write, and the **dual, independent auto-inc/dec** writeback (each operand updates its own `An`, source and dest separately). **There is NO `generate_bus_step()` change in 3c** — O-mem-3b already added the `byte_lane==0` full-word `UML_WRITE`; 3c reuses the read + write + prefetch steps frozen. The only new mechanism is the **two-EA composition** (both operands are memory: value flows `m_dbin` → flags → `m_dbout`, no register read/write) plus the **dest-mode-dependent bus-step ordering** (dest `-(An)` interleaves the final prefetch *before* the dest write). Gated cycle-exact by the three existing oracle grant modes (1-cycle, full-grant, single-offset partial-grant) on x64 + C.

**Architecture:** O-mem-3c is the third sub-batch of **O-mem-3** (phase-2 Task 10 / boundary O, increment plan §6 row 3), pinned by the **[ADR 0007 O-mem-3 addendum](../adr/0007-m68000-native-memory-ea-suspend-mechanism.md#addendum--resolution-2026-06-28--o-mem-3-memory-ea-movemovea)** (sections M1–M6, the 5-sub-batch split, OQ-10). Per the addendum's mechanism-novelty-isolation rule (M4), **3c introduces exactly one new mechanism: the general two-EA read-EA→write-EA composition with dual independent auto-inc/dec** — it touches `generate_bus_step()` **not at all** (M4: "3c adds the two-EA composition on the proven single-access write"; "the only primitive edit is confined to 3b"). Everything 3c adds — a new emitter (`generate_move_memmem`), the generator-descriptor widening, the gated dispatch arms, the coverage assertion — is additive over the now-frozen primitive. All per-step charges, substate pairs, the `byte_lane`/`has_addr_error` flags, and the `-(An)` predecrement internal `−2` are **single-sourced from `m68000gen.py`** (ADR R-A is the top risk; do not hand-transcribe).

**Scope resolution — the exact form count (18, matching the addendum's "~18" estimate):** the precise roster, enumerated from the `_df` handler table (`m68000-sdf.cpp`, grep-verified), is **18 forms**:

| size | src \ dst | `(An)` (aid) | `(An)+` (aipd) | `-(An)` (paid) |
|---|---|---|---|---|
| **`.b`** | `(An)` (ais) | `move_b_ais_aid_df // 1090` | `move_b_ais_aipd_df // 10d0` | `move_b_ais_paid_df // 1110` |
| **`.b`** | `(An)+` (aips) | `move_b_aips_aid_df // 1098` | `move_b_aips_aipd_df // 10d8` | `move_b_aips_paid_df // 1118` |
| **`.b`** | `-(An)` (pais) | `move_b_pais_aid_df // 10a0` | `move_b_pais_aipd_df // 10e0` | `move_b_pais_paid_df // 1120` |
| **`.w`** | `(An)` (ais) | `move_w_ais_aid_df // 3090` | `move_w_ais_aipd_df // 30d0` | `move_w_ais_paid_df // 3110` |
| **`.w`** | `(An)+` (aips) | `move_w_aips_aid_df // 3098` | `move_w_aips_aipd_df // 30d8` | `move_w_aips_paid_df // 3118` |
| **`.w`** | `-(An)` (pais) | `move_w_pais_aid_df // 30a0` | `move_w_pais_aipd_df // 30e0` | `move_w_pais_paid_df // 3120` |

**Derivation:** in-scope = MOVE where **both** operands are register-indirect memory EAs in `{(An),(An)+,-(An)}`, single-access sizes `.b`+`.w`. src `{ais,aips,pais}` (3) × dst `{aid,aipd,paid}` (3) × size `{.b,.w}` (2) = **18**. Grep confirms exactly 9 byte + 9 word `_df` handlers (mask `0xf1f8`). **`(d16,An)` on either side (`das`/`dad`) is OUT → 3e; long `.l` is OUT → 3d.** Reg↔mem (one side a register) is 3b (already native); MOVEA is 3a (already native). The addendum's "~18" estimate is exact for 3c.

**Tech Stack:** C++17 (the `m68000_device` / DRCUML emitter, drcbe_x64 + drcbec backends), Python 3 (the `m68000gen.py` generator), GENie/`mingw32-make` build, Catch2 oracle harness (`tests/emu/cpu/cpuoracle.cpp` + `cpu_test_harness.{cpp,h}`). MSYS2 UCRT64/MINGW64 toolchain on Windows; appserver Linux for the authoritative `oracle` CI job.

> ⚠ **REQUIRED before any task — read the [ADR 0007 O-mem-3 addendum](../adr/0007-m68000-native-memory-ea-suspend-mechanism.md#addendum--resolution-2026-06-28--o-mem-3-memory-ea-movemovea) in full** (M1–M6, the pinned-vs-derive list, OQ-10) **and the as-built [`phase-2-o-mem-3b-move-regmem.md`](phase-2-o-mem-3b-move-regmem.md)** (3b froze the write path — `byte_lane==0` full-word `UML_WRITE` — and established the `m_irdi` latch + the `move_flags` lambda + the register source/dest paths that 3c's two-EA composition reuses) **and [`phase-2-o-mem-3a-movea.md`](phase-2-o-mem-3a-movea.md)** (3a froze the read path + the `m_irdi` latch + run-find pattern). Every mechanism decision is owner-decided in the addendum; this plan implements those answers, it does not re-derive them. **Five facts gate everything 3c-specific:**
> 1. **NO `generate_bus_step()` edit (M1/M4).** 3b made the one and only primitive edit of the whole MOVE arc (the `byte_lane` write `if/else` at `m68000drc.cpp:549-595`). For 3c that primitive is **frozen**: the source read uses the existing read path (`byte_lane=1`/no-fault for `.b`, `byte_lane=0`/fault for `.w`), the dest write uses the existing write path (`byte_lane=1` masked for `.b`, `byte_lane=0` full-word for `.w` — the 3b edit), the prefetch uses the existing prefetch path (`byte_lane=0`/fault). **If a 3c form seems to need a `generate_bus_step()` change, stop — that is a bug in this plan.**
> 2. **The two-EA composition (the one new mechanism).** Both operands are memory, so the value flows entirely in memory: source DATA READ → `m_dbin` → MOVE flags (`sr_nzvc` from the source value) → `m_dbout` → dest DATA WRITE. There is **no register read and no register write-back** (unlike 3b's load/store, where one side is a register). The emitter is a true `read → flags → write → prefetch` composition over the frozen steps.
> 3. **The bus-step ORDER depends on the DEST mode — `-(An)` puts the final prefetch BEFORE the dest write (verified).** For dest `(An)` (aid) and `(An)+` (aipd) the order is **[DATA READ, DATA WRITE, prefetch]** (prefetch last — verified `move_w_ais_aid_df:58141`, `move_w_aips_aipd_df:59420`). For dest `-(An)` (paid) the order is **[DATA READ, prefetch, DATA WRITE]** — the final prefetch is the *middle* access, before the dest write (verified `move_w_pais_paid_df:60702` and **byte** `move_b_ais_paid_df:32999` — dest-mode-tied, both sizes). This is the same prefetch-first-on-`-(An)` shape the 3b Builder confirmed for reg→mem stores, now confirmed for mem→mem dest `-(An)`. The substate pairs are still assigned in **bus-access order** (read 1/2, then whichever access is 2nd is 3/4, then 3rd is 5/6), single-sourced from the handler — so the emitter MUST iterate the generated run in order and place each access's setup per the handler, **not** assume a fixed read→write→prefetch template.
> 4. **MOVE decodes BOTH `rx` and `ry` as address registers from `m_irdi` (verified).** Every mem→mem handler opens `int rx = map_sp(((m_irdi >> 9) & 7) | 8); int ry = map_sp((m_irdi & 7) | 8);` — `rx` = **dest** EA An, `ry` = **src** EA An, **both `map_sp`'d** (both are `(An)`-family address registers; unlike 3b where the register side was a raw `Dn`). `static_generate_entry_point` does **NOT** latch `m_irdi`; the emitter **must** `UML_STORE(&m_irdi, I7)` before any yield (load-bearing for the interpreter's partial-handler resume) — exactly as 3a/3b/MOVEA do.
> 5. **OQ-10 is resolved (no sweep).** These are single-access forms (3 bus steps), so there are no intermediate long-access boundaries; the single `length-4` partial-grant offset lands the resume between the second-to-last and last access exactly as 3b. **No oracle change is needed for 3c.** Note: the partial-grant pass now has **TWO data accesses** (a source read AND a dest write) plus a prefetch to resume between — a richer resume surface than 3b's single data access, but still covered by the existing three modes.

## Global Constraints

These apply to **every** task below — copied forward from ADR 0007 (body + O-mem-1/2/3 addenda) + the O-mem-2/3a/3b plans + the cut-line doc + the user's workflow rules, with the O-mem-3c deltas marked.

- **THE GATE for every DRC-touching task is the oracle GREEN on the appserver LINUX `oracle` CI job.** Local Windows green is **necessary-not-sufficient** — boundary M passed Windows but failed Linux SysV twice on `drcbe_x64 offset_from_rbp`. The `oracle` CI job is NOT preflight (preflight is a tiny-build smoke).
- **O-mem-3c gate = THREE grant modes, both backends.** A batch merges only when **all of**: (1) 1-cycle Leg B (`drcbex64` + `drcbec`), (2) full-grant Leg B (`CPUORACLE_M68_DRC_FULLGRANT=1`, both backends — the gate that first runs the native source-read → dest-write composition end-to-end, incl. the word DATA WRITE and the MOVE flags), and (3) single-offset partial-grant Leg B (`CPUORACLE_M68_DRC_PARTGRANT=1`, both backends — the mid-instruction resume handoff, now with **two data accesses** to resume between), with **Leg A unchanged**. **No new oracle mode and no parameterized sweep** (OQ-10, owner-decided 2026-06-28).
- **`generate_bus_step()` is NOT edited in 3c.** It was frozen after the single O-mem-3b edit. Tasks below are additive over the frozen primitive (generator filter widening, the new emitter, dispatch arms, coverage assertion, doc). **If a MOVE mem→mem form needs any `generate_bus_step()` change, stop — that is a bug in this plan.**
- **The byte path stays byte-identical.** 3c byte reads/writes reuse O-mem-2/3b's `byte_lane==1` path unchanged (source read lane-masked byte; dest write masked `(m_aob&1)?0x00ff:0xff00`). The O-mem-2/3b oracle (1-cycle + full-grant + partial-grant) must stay green with no change to the read/write primitive.
- **ABI-safe codegen:** zero plain `mem(&device_field)` operands. Every device-state field is accessed via `UML_LOAD`/`UML_STORE` with a pointer base (the boundary-M / O-mem-1/2/3a/3b pattern in `m68000drc.cpp`). `UML_READ`/`UML_WRITE`'s address/value are UML registers, so the access ops are inherently ABI-safe; the field plumbing around them (`m_irdi`, `m_aob`, `m_at`, `m_au`, `m_pc`, `m_da[]`, `m_sp`, `m_dbin`, `m_dbout`, `m_edb`, `m_irc`, `m_ir`, `m_ird`, `m_aluo`, `m_alub`, `m_icount`, `m_inst_substate`, `m_inst_state`, `m_next_state`, `m_int_next_state`, `m_base_ssw`, `m_sr`) all goes through LOAD/STORE.
- **Generator additivity:** regenerating must leave existing generated decode files **byte-identical** (empty `git diff` on `m68000-decode.cpp`, `m68000-head.h`, `m68000-s{d,i}{f,p}.cpp`). Only `m68000-drcdesc.ipp` may grow (the new MOVE mem→mem read/write/prefetch runs). **No new `step.kind`, no new descriptor field** (M1/M6) — the existing read/write/prefetch kinds + `byte_lane`/`has_addr_error`/`pre_charge` fields already express every 3c access. The `enum_str()` shim preserves byte-identity on Python ≥ 3.11 — do not remove it.
- **Single-source rule (ADR 0007 R-A, the top risk):** the bus-step list — kinds, the per-dest-mode access ORDER (incl. the `-(An)` prefetch-before-write), charges, the redo/completed substate pairs, the predecrement internal `−2`, the `byte_lane`/`has_addr_error` flags — is emitted from the SAME microcode walk that generates the interpreter handler (`drc_bus_steps()` parses the handler text `generate_source_from_code` emits). **Never hand-type a substate number, an access order, or a charge into the emitter.** A hand-written step table is *not* an acceptable fallback for O-mem-3.
- **Build env (verified recipe):** `MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd <worktree>; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'`, then `./mametests "[m68000]"`. Do **NOT** use `make SOURCES=...m68000.cpp` (SOURCES filters by *driver*; a CPU device has no driver and GENie errors). `make REGENIE=1` is required whenever a `.cpp`/`.h` is added and after Task 1 regenerates the `.ipp`.
- **Backend matrix:** every oracle pass on **x64 (`drcbex64`)** AND the **C backend (`CPUORACLE_M68_DRC_C=1`)**. arm64 (`drcbearm64`) is deferred to the CI matrix.
- **Cycle truth:** the interpreter (`m68000.cpp` under `-drc 0`) is the authority. The DRC mirrors its cycle count; divergence is always a DRC bug, resolved by routing the offending form back to `cfunc_` (drop it from `is_native_opcode` / the dispatch table) — never by editing the interpreter or weakening an assertion.
- **Scope lock:** O-mem-3c = the **18 mem→mem forms** only (src `{(An),(An)+,-(An)}` × dst `{(An),(An)+,-(An)}` × `.b`/`.w`). MOVE reg↔mem (3b, already native), MOVEA (3a, already native), long `.l` MOVE (3d), every `(d16,An)` form (3e), and native `AS_OPCODES` space-selection are **out of scope** (the gate keeps `AS_OPCODES`/user-space/MMU machines on `cfunc_`).
- **Style:** match the existing m68000 brace/whitespace style (tabs, the K&R-ish style already in `m68000drc.cpp`); license header `// license:BSD-3-Clause` / `// copyright-holders:Mark Mackelprang` on any new file (none expected); run `srcclean` (built via `TOOLS=1`) on touched files before committing. Work on the implementation branch and open a PR (never commit DRC source straight to `main`).

---

## The interpreter shapes O-mem-3c must mirror (read before any task)

Every native form is a *transcription* of its `_df` handler in `m68000-sdf.cpp` — the one the plain `m68000_device` runs. **Read each handler line-by-line while emitting its form** (R-A is the top risk). The shapes below are verified against the tree. **The 3a/3b Builders found FOUR skeleton inaccuracies total** (a `byte_lane`/size misclassification, an AIPS writeback delta, the `-(An)` prefetch-first ordering, and the `is_byte_lane` generalization) — accordingly the per-mode `m_aob`/`m_au`/`m_at`/`m_da[]`/`m_pc` choreography below is given as **VERIFY-gated derive-from-handler guidance**, and the literals that ARE given (the dispatch arms, `is_native_opcode`, the `move_flags` reuse, the enum/struct, the run-find lambda) were double-checked against the handlers cited. **Prefer reading the handler over trusting any literal here.**

### Bus-step ladders (single-source the order/substates/charges/flags from the generator; do NOT hand-transcribe)

All 18 forms are **3 bus steps** (single source-access + single dest-access + final prefetch). The ORDER of the 2nd/3rd step depends on the **dest** mode:

| dest mode | step 1 | step 2 | step 3 | substates |
|---|---|---|---|---|
| `(An)` (aid), `(An)+` (aipd) | **DATA READ** (src) | **DATA WRITE** (dst) | final **PREFETCH** | read 1/2, write 3/4, prefetch 5/6 |
| `-(An)` (paid) | **DATA READ** (src) | final **PREFETCH** | **DATA WRITE** (dst) | read 1/2, prefetch 3/4, write 5/6 |

Per-access flag derivation (single-sourced; do not hand-type):
- **`.b` (byte)** data read: `byte_lane=1`, `has_addr_error=0`. data write: `byte_lane=1`, `has_addr_error=0`. prefetch: `byte_lane=0`, `has_addr_error=1`. (Verified `move_b_aips_aipd_df:32023` — byte read/write carry no `if(m_aob&1)` fault; the prefetch does.)
- **`.w` (word)** data read: `byte_lane=0`, `has_addr_error=1`. data write: `byte_lane=0`, `has_addr_error=1` (the 3b full-word write path). prefetch: `byte_lane=0`, `has_addr_error=1`. (Verified `move_w_ais_aid_df:58163,58188,58211` — word read, write, AND prefetch all fault on odd `m_aob`.)

`-(An)` (source `pais` / dest `paid`) carries the predecrement internal `−2` as **`pre_charge`** on **that side's** bus step, folded by the generator exactly as O-mem-2/3a/3b's `pais` — the emitter does **not** charge it. **VERIFY** each `pre_charge` value and which step carries it from the generated descriptor, never hand-type it. (For dest `paid`, the predecrement `−2` is on the dest write step even though that step is emitted *third* in the run — single-sourced; eyeball it in Task 1 Step 5.)

### mem→mem composition skeleton (verified against `move_w_ais_aid_df:58141` word `(An)→(An)`)

```cpp
int rx = map_sp(((m_irdi >> 9) & 7) | 8);   // dest EA An (map_sp'd address register)
int ry = map_sp((m_irdi & 7) | 8);          // src  EA An (map_sp'd address register)
// --- source EA setup (adrw1/adrw2 ; pinw* ; pdcw*) : substates feed the DATA READ ---
m_aob = m_da[ry]; /* m_at = m_da[ry]; */    // (An): VERIFY per src mode (aips post-inc / pais pre-dec writeback to m_da[ry])
m_base_ssw = SSW_DATA | SSW_R;
m_edb = m_program.read_interruptible(m_aob & ~1 [, lane mask if .b]);   // <generate_bus_step DATA READ ; substates 1/2 ; .b byte_lane=1/no-fault, .w byte_lane=0/fault ; pais pre_charge>
// (commit) m_dbin = m_edb;                  // the source byte/word
// --- dest-write setup + MOVE FLAGS (BEFORE the write) (mmrw1) : substates 3/4 ; DATA ; .w fault ---
m_aob = m_da[rx];                            // dest EA into m_aob  (VERIFY per dest mode; READ m_da[rx] FRESH -- see aliasing)
m_dbout = m_dbin;                            // .w: source word into m_dbout (verified :58171; paid uses m_dbout=m_aluo, equivalent)
// set_8xl(m_dbout, m_dbin);                  // .b: replicate source byte to both lanes (verified move_b_aips_aipd_df:32055)
m_pc = m_au; m_au = m_da[rx] + 2;            // (aipd dest post-inc / paid dest pre-dec writeback to m_da[rx]: VERIFY)
// MOVE flags: alu_and(m_dbin,0xffff)+sr_nzvc()  ==  N=MSB, Z=(==0), V=C=0, X untouched  (verified :58175-58176)
m_base_ssw = SSW_DATA;
m_program.write_interruptible(m_aob & ~1, m_dbout [, lane mask if .b]);  // <generate_bus_step DATA WRITE ; substates 3/4 ; .b byte_lane=1 masked, .w byte_lane=0 FULL-WORD (3b path, frozen) ; paid pre_charge ; .w fault>
// --- final prefetch (mmrw2/mmrw3) : substates 5/6 ; PROGRAM ; fault ---  (for dest -(An) THIS RUNS BEFORE THE WRITE -- see below)
m_aob = m_pc; m_ir = m_irc; m_au = m_pc + 2;
m_ird = m_ir; if(m_next_state != S_TRACE) m_next_state = m_int_next_state;
m_base_ssw = SSW_PROGRAM | SSW_R;
m_edb = m_opcodes.read_interruptible(m_aob & ~1);  // <generate_bus_step prefetch ; byte_lane=0 ; fault ; substates 5/6>
// (retire) m_irc = m_dbin = m_edb; set_ftu_const(); m_inst_state = m_next_state ? m_next_state : m_decode_table[m_ird]; trace
```

> **Dest `-(An)` (paid) re-orders steps 2/3 (verified `move_w_pais_paid_df:60702`, `move_b_ais_paid_df:32999`).** For dest `-(An)` the handler is: source DATA READ (substate 1/2) → **mmmw1**: `m_aob = m_au` (the prefetch addr = old `m_au`/pc); `m_au = m_da[rx] - <delta>` (compute the dest predecremented addr); `alu_and(m_dbin,0xffff); sr_nzvc()` (MOVE flags); then the **final PREFETCH** (`m_opcodes.read_interruptible`, substate 3/4) → **mmmw2**: `m_aob = m_au` (= the dest predecremented addr); `m_dbout = m_aluo`; `m_da[rx] = m_au` (dest predec writeback); `m_au = m_pc + 2`; then the **dest DATA WRITE** (substate 5/6). So for dest `-(An)`: **flags + dest-addr COMPUTE happen at the prefetch-setup state; the dest-addr LOAD into `m_aob`, `m_dbout`, and the dest An writeback happen at the write state, after the prefetch.** The emitter MUST switch the step order on dest mode — the run from the generator already has the prefetch as the 2nd step for paid, so iterating the run in order is correct, but the architectural setup before each step differs from the aid/aipd case. **Transcribe each paid handler line-by-line.**

> **Load-bearing per-mode VERIFY points** (these are exactly the classes the 3a/3b Builders got wrong — derive from the handler, do not trust the skeleton):
> - **src `(An)+` (aips) post-inc:** `m_da[ry] += <delta>` at the source-setup state (pinw2) BEFORE the read commits (verified `move_w_aips_aipd_df:59431`). **src `-(An)` (pais) pre-dec:** `m_au = m_da[ry] - <delta>` (internal `−2` = `pre_charge`), `m_da[ry] = m_au` at pdcw2 (verified `move_w_pais_paid_df:60710,60715`).
> - **dest `(An)+` (aipd) post-inc:** `m_da[rx] += <delta>` — note for aipd dest this writeback is at the **prefetch-setup** state (mmiw2), AFTER the write (verified `move_w_aips_aipd_df:59481`). **dest `-(An)` (paid) pre-dec:** `m_au = m_da[rx] - <delta>` computed at the prefetch-setup (mmmw1), `m_da[rx] = m_au` at the write state (mmmw2) (verified `move_w_pais_paid_df:60740,60767`).
> - **delta per side (M3 / O-mem-2 W2 generalization):** word delta = **2** always; byte delta = **1** for A0–A6, **2** for A7 (`ry < 15 ? 1 : 2` source, `rx < 15 ? 1 : 2` dest — verified `move_b_aips_aipd_df:32032,32057`). **Each side checks its OWN register independently.** Reuse the O-mem-2 `load_delta` pattern (`m68000drc.cpp:892`), VERIFY each side's delta against its handler. The predecrement **address** delta (1/2) is distinct from the predecrement **internal `−2` charge** (always 2, generator `pre_charge`).

### MOVE flags — `sr_nzvc()` semantics (reuse 3b's `move_flags` lambda verbatim)

`sr_nzvc()` is `m_sr = (m_sr & ~(SR_N|SR_Z|SR_V|SR_C)) | (m_isr & (SR_N|SR_Z|SR_V|SR_C))`. The handler's preceding `alu_and(v,0xffff)` (word) / `alu_and8(v,0xffff)` (byte) sets `m_isr`: `Z` if `v==0`, `N` if the size's MSB is set, `V=C=0`; `X` untouched. Net MOVE-flag effect: **N = value MSB (bit 15 word / bit 7 byte), Z = (value == 0 over the size's width), V = 0, C = 0, X/S/T/I untouched.** SR bits (`m68000.h:107-110`): `SR_C=0x0001, SR_V=0x0002, SR_Z=0x0004, SR_N=0x0008`. The **value source for mem→mem is always `m_dbin`** (the just-read source value), computed at the dest-setup state BEFORE the write (aid/aipd) or before the prefetch (paid). Reuse 3b's `move_flags(uml::parameter Iv)` lambda **verbatim** (`m68000drc.cpp`, in `generate_move_regmem`) — it computes the direct N/Z with V=C=0, X kept, validated by 3b's full-grant + 1-cycle passes. **VERIFY** the size (`zmask`/`nmask` = `0xffff`/`0x8000` word, `0xff`/`0x80` byte) and the value source (`m_dbin`) against each handler's `alu_and`/`alu_and8 + sr_nzvc` pair.

### Fields SET by the handler but DEAD for MOVE mem→mem (skip per the O-mem-1/3a/3b precedent — VERIFY not in the oracle compare set)

- `m_dcr` (`m_dcr = 0` at adrw2/pdcw1 for mem→mem word): written, never read for mem→mem MOVE. Skip per the 3a/3b `m_dcr`/`m_alub` dead-scratch precedent. VERIFY `m_dcr` is not in the oracle's compared architectural state; if uncertain, setting it is cheap and harmless.
- `m_alub` (`m_alub = m_dbin` at the source-setup of aips/pais): written, then consumed only by a by-value `alu_and(m_alub,0xffff)` (writes uncompared scratch). Skip. Same VERIFY/escape.
- `m_at` (`m_at = m_da[ry]` at the source-setup): the architectural transfer-address latch; written, read only on a fault frame (which is `cfunc_`'d). Skip per the O-mem-1/2 `m_at` precedent. VERIFY not compared.
- `alu_and(...)`/`alu_and8(...)` *before* the flag-bearing `sr_nzvc()` (e.g. source adrw1's `alu_and(m_dbin,0xffff)` with no `sr_*` after): by-value, write only `m_aluo`/`m_isr` (uncompared scratch) — **not** emitted. **Only the `alu_and(...)+sr_nzvc()` pair that updates `m_sr` is reproduced** (via the `move_flags` lambda). NOTE the paid-dest subtlety: the handler's `m_dbout = m_aluo` at mmmw2 reads `m_aluo` left by the flag-bearing `alu_and(m_dbin,0xffff)` at mmmw1 — so for paid the emitter must either set `m_dbout` from the same value it fed to `move_flags` (i.e. `m_dbin`, equivalent since `m_aluo == m_dbin & 0xffff` there) or replicate the `m_aluo` dataflow. **VERIFY** the `m_dbout` source per dest mode (aid/aipd: `m_dbout = m_dbin`; paid: `m_dbout = m_aluo == m_dbin & 0xffff`).

---

## File Structure

| File | Responsibility | Touched in |
|---|---|---|
| `src/devices/cpu/m68000/m68000gen.py` | Generator — widen `drc_bus_steps()` to admit the 18 MOVE mem→mem forms; additive-only (reuse the lane-mask-keyed read/write parsers + the dest-mode-driven access order from the handler walk) | Task 1 |
| `src/devices/cpu/m68000/m68000-drcdesc.ipp` | Generated descriptor table — regenerated; existing rows byte-identical, new MOVE mem→mem runs appended (incl. the `-(An)`-dest prefetch-before-write runs) | Task 1 |
| `src/devices/cpu/m68000/m68000.h` | declare `generate_move_memmem`, the `memmem_form` struct + `mvmm_size`/`mvmm_ea` enums (beside `moverm_form`/`movea_form`) | Task 2 |
| `src/devices/cpu/m68000/m68000drc.cpp` | **Task 2:** `generate_move_memmem()` emitter; `is_native_opcode` patterns; the 18 gated dispatch arms in `generate_native_dispatch` after the O-mem-3b reg↔mem block | Task 2 |
| `tests/emu/cpu/cpuoracle.cpp` | native-coverage assertion for the 18 forms (in `[m68000][drc][gate]`); the AS_OPCODES differential subset if it is restricted | Task 3 |
| `src/devices/cpu/m68000/README-drc.md` | move the 18 MOVE mem→mem forms from `cfunc_` to native (behind the gate); record the two-EA composition + dual auto-inc/dec + the `-(An)`-dest prefetch ordering | Task 3 |

`make REGENIE=1` is run per build because Task 1 regenerates the `.ipp` (and adds no new `.cpp`/`.h`, but REGENIE is harmless).

---

## Task 1: Generator extension — admit the 18 MOVE mem→mem read/write/prefetch runs (additive)

**Goal:** Single-source the 3c bus-step lists (OQ-2) from `m68000gen.py` so the emitter takes each form's read/write/prefetch kinds, the **per-dest-mode access order** (incl. the `-(An)`-dest prefetch-before-write), the charges + substate pairs, the `-(An)` predecrement `−2`, and the `byte_lane`/`has_addr_error` flags from the same microcode walk the interpreter uses. Purely a **filter widening** — the existing read-step parser (O-mem-1), write-step parser (O-mem-2), the lane-mask-keyed `byte_lane`/`has_addr_error` recognizer (3a/3b), the predecrement-`−2` recognizer (O-mem-2), and the **handler-text access-order walk** (already produces the run order, so the `-(An)`-dest prefetch-first ordering falls out for free) already emit every row 3c needs. Additive-only: existing generated files stay byte-identical; only `m68000-drcdesc.ipp` grows. **No `generate_bus_step()` change, no new `step.kind`, no new descriptor field.**

**Files:**
- Modify: `src/devices/cpu/m68000/m68000gen.py` — the `drc_bus_steps()` opcode filter (the O-mem-1/2/3a/3b admit clause).
- Regenerate: `src/devices/cpu/m68000/m68000-drcdesc.ipp`.
- Validate against: `m68000-sdf.cpp` (the 18 MOVE mem→mem handlers).

**Interfaces:**
- Produces (consumed by Task 2): bus runs for the 18 `{value, mask}` patterns (all mask `0xf1f8`). aid/aipd-dest runs = `[DATA_READ, DATA_WRITE, prefetch]`; **paid-dest runs = `[DATA_READ, prefetch, DATA_WRITE]`**. `.b` data accesses `byte_lane=1`/`has_addr_error=0`; `.w` data accesses `byte_lane=0`/`has_addr_error=1`; every prefetch `byte_lane=0`/`has_addr_error=1`. Source `-(An)` (pais) carries `pre_charge` on the data-read step; dest `-(An)` (paid) carries `pre_charge` on the data-write step.

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

- [ ] **Step 2: Pin the 18 handlers' bus-step truth (R-A — the access ORDER is part of the truth).** Read each of the 18 handlers (`move_{b,w}_{ais,aips,pais}_{aid,aipd,paid}_df`). Record per step: the `m_icount -= N` charge(s) (note the `−2` at the `pdc*` state for `pais`/`paid`, which has **no** suspend checkpoint), the redo/completed substate pair, whether the data access is followed by `if(m_aob & 1){ ... S_ADDRESS_ERROR ... }` (word: yes → `has_addr_error=1`; byte: no → `has_addr_error=0`), whether the access carries a lane-mask arg (`.b` → `byte_lane=1`; `.w` no mask → `byte_lane=0`), and **the ORDER of the three accesses** — for dest `aid`/`aipd` it is read→write→prefetch; for dest `paid` it is **read→prefetch→write** (verified). The data **read** vs **write** vs **prefetch** kind is set by `m_program.read_interruptible` vs `m_program.write_interruptible` vs `m_opcodes.read_interruptible`. The generator must emit *these exact values in this exact order*.

- [ ] **Step 3: Widen the `drc_bus_steps()` opcode filter to admit MOVE mem→mem.** In `m68000gen.py`, in the O-mem-1/2/3a/3b admit clause (the `is_o1`/`is_o2`/`is_o3a`/`is_o3b` predicate), add the 3c clause. **Confirm the actual `drc_ea_mode`/mnemonic/size constant names against the map definitions in `m68000gen.py`** (3b confirmed `REGIND_S = (DRC_EA_AIS, DRC_EA_AIPS, DRC_EA_PAIS)` for source and `REGIND_D = (DRC_EA_AID, DRC_EA_AIPD, DRC_EA_PAID)` for dest — reuse those exact tuples):
```python
    base   = drc_base_mnemonic(ii[2][0])
    src_ea = drc_ea_mode[ii[2][1]]
    dst_ea = drc_ea_mode[ii[2][2]]
    REGIND_S = (DRC_EA_AIS, DRC_EA_AIPS, DRC_EA_PAIS)    # src  (An), (An)+, -(An)   (3b-confirmed names)
    REGIND_D = (DRC_EA_AID, DRC_EA_AIPD, DRC_EA_PAID)    # dst  (An), (An)+, -(An)   (3b-confirmed names)
    # ... is_o1 / is_o2 / is_o3a / is_o3b unchanged ...
    # O-mem-3c: single-access (.b+.w) MOVE mem->mem (An)/(An)+/-(An) both operands memory.
    #   base 'move', src in REGIND_S, dst in REGIND_D, size in (.b, .w).
    # (d16,An) (das/dad) EXCLUDED (3e -- not in REGIND_S/REGIND_D); long .l EXCLUDED (3d -- size filter).
    is_o3c = (base == 'move' and src_ea in REGIND_S and dst_ea in REGIND_D
              and drc_size(ii) in (DRC_SZ_B, DRC_SZ_W))   # .l -> 3d
    if not (is_o1 or is_o2 or is_o3a or is_o3b or is_o3c):
        return []
```
> **Verify the EA-mode + size constant names** against the map definitions in `m68000gen.py` (3b's clause is the reference). Confirm `das`/`dad` (the `(d16,An)` source/dest) map to constants **not** in `REGIND_S`/`REGIND_D` (so they cannot leak into `is_o3c` → stay `cfunc_` for 3e), and that the `.l` size constant is excluded (3d). Note `is_o3b` admits `src in REGIND_S and dst == DRC_EA_DD` (load) or `src in (DS,AS) and dst in REGIND_D` (store) — `is_o3c` (`src in REGIND_S and dst in REGIND_D`) is **disjoint** from `is_o3b` (3b's load has dst `Dn`, not a REGIND_D; 3b's store has src `Dn`/`An`, not a REGIND_S), so no overlap. **Adjust all names to whatever the map actually defines.**

> **Single-source discipline (R-A):** do **not** add any MOVE-specific substate, charge, `byte_lane`, `has_addr_error`, or **access-order** logic. The 18 forms fall out of the existing per-opcode iteration and the existing read/write/prefetch-step + lane-mask + predecrement parsers + handler-text order walk for free, because the filter now admits them. The only edit is the admit clause. **The `-(An)`-dest prefetch-before-write order is part of the handler text the generator already walks — it must NOT be special-cased; confirm it emitted correctly in Step 5.**

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
Expected: **only** `m68000-drcdesc.ipp` appears (the 18 new MOVE mem→mem runs appended; all O-mem-1/2/3a/3b rows byte-identical). If any of `m68000-decode.cpp`/`m68000-head.h`/the four `s*` files changed, the edit leaked into the wrong code path — fix before continuing.

- [ ] **Step 5: Eyeball the emitted rows against the handlers (R-A — catch it here, not in Leg B).** Confirm `m68000-drcdesc.ipp` now has runs for all 18 `{value, mask}` with the right kinds/flags/**order**:
```bash
# aid/aipd dest -> [DATA_READ, DATA_WRITE, prefetch]
grep -nE '0x1090, 0xf1f8|0x1098, 0xf1f8|0x10a0, 0xf1f8|0x10d0, 0xf1f8|0x10d8, 0xf1f8|0x10e0, 0xf1f8' m68000-drcdesc.ipp   # byte aid/aipd
grep -nE '0x3090, 0xf1f8|0x3098, 0xf1f8|0x30a0, 0xf1f8|0x30d0, 0xf1f8|0x30d8, 0xf1f8|0x30e0, 0xf1f8' m68000-drcdesc.ipp   # word aid/aipd
# paid dest -> [DATA_READ, prefetch, DATA_WRITE]  (prefetch is the MIDDLE step)
grep -nE '0x1110, 0xf1f8|0x1118, 0xf1f8|0x1120, 0xf1f8|0x3110, 0xf1f8|0x3118, 0xf1f8|0x3120, 0xf1f8' m68000-drcdesc.ipp
# excluded: no (d16,An) or .l mem->mem rows from 3c
grep -nE '0x10a8, 0xf1f8|0x1150, 0xf1f8|0x1168, 0xf1f8|0x2090, 0xf1f8' m68000-drcdesc.ipp   # MUST be empty (das=*0a8, dad=*150/*168 -> 3e; .l (An),(An)=0x2090 -> 3d)
```
Verify against the handlers: **byte** data accesses (`0x10xx`) have `byte_lane==1`/`has_addr_error==0`; **word** data accesses (`0x30xx`) have `byte_lane==0`/`has_addr_error==1`; **for aid/aipd dest the 2nd row is `DRC_BUS_DATA_WRITE` and the 3rd is the prefetch; for paid dest the 2nd row is the PREFETCH and the 3rd is `DRC_BUS_DATA_WRITE`**; the 1st row is always `DRC_BUS_DATA_READ`; every prefetch is `byte_lane==0`/`has_addr_error==1`. The `pais`-src runs carry `pre_charge` on the data-read (1st) row; the `paid`-dest runs carry `pre_charge` on the data-write row (3rd for paid). A kind/lane/**order** mismatch is the single highest-risk bug in the batch (R-A) — confirm the paid-dest order rows specifically, and confirm the word DATA WRITE rows are `byte_lane==0` (they exercise the frozen 3b primitive).

- [ ] **Step 6: Build + Leg A (descriptor inert until the emitter consumes it).**
```bash
MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd "$PWD"; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'
./mametests "[m68000]"          # Leg A
./mametests "[m68000][drc]"     # existing native opcodes (moveq/btst-abs/bit-ops/MOVEA/reg<->mem) still exact -- the new rows are unread
./mame -validate
```
Expected: build clean; Leg A green; existing Leg B green (no emitter reads the MOVE mem→mem rows yet).

- [ ] **Step 7: `srcclean` + commit.**
```bash
git add src/devices/cpu/m68000/m68000gen.py src/devices/cpu/m68000/m68000-drcdesc.ipp
git commit -m "feat(m68000drc): generate MOVE mem->mem (An)/(An)+/-(An) byte+word runs (O-mem-3c, additive)"
```

---

## Task 2: The MOVE mem→mem emitter `generate_move_memmem` + the 18 gated dispatch arms

**Goal:** Add the single parameterized emitter that composes, per form: the **`m_irdi` latch** → `rx`/`ry` decode (both `map_sp` address registers) → **source EA setup → DATA READ** via the frozen `generate_bus_step()` → **MOVE flags from `m_dbin`** + **dest EA setup → DATA WRITE** (word = the frozen 3b full-word path; byte = the frozen `byte_lane==1` masked path), **interleaving the final prefetch per dest mode** (aid/aipd: write→prefetch; paid: prefetch→write) → retire. The emitter drives off the generated run order (so the dest-mode ordering is descriptor-driven), with the per-step architectural setup (addresses, `m_dbout`, flags, the **dual independent** auto-inc/dec writebacks) transcribed from each handler behind `// VERIFY` gates. Then wire the 18 `{value,mask}` patterns into `is_native_opcode` + gated dispatch arms and run all three oracle grant modes. **The full-grant pass is where the native two-EA composition (read → flags → write) is first validated end-to-end; the 1-cycle pass validates the `m_irdi` latch + the suspend handoff at the source read; the partial-grant pass validates the resume between accesses (now two data accesses + a prefetch).**

**Files:**
- Modify: `src/devices/cpu/m68000/m68000.h` — the `memmem_form` struct + enums + `generate_move_memmem` declaration, beside `moverm_form`/`movea_form`.
- Modify: `src/devices/cpu/m68000/m68000drc.cpp` — `generate_move_memmem`, after `generate_move_regmem`; `is_native_opcode` (`:210`); the 18 gated dispatch arms in `generate_native_dispatch` after the O-mem-3b reg↔mem block (`:378-413`).

**Interfaces:**
- Consumes: `generate_bus_step` (frozen — read path AND the `byte_lane`-aware write path AND the prefetch path; NO edit), `s_drc_bus_run_table`/`s_drc_bus_step_table`, the run-find lambda pattern (mirror `generate_move_regmem`/`generate_movea_mem`/`generate_bitop_mem`), the `move_flags` lambda (reuse 3b's verbatim), `set_8xl`/`map_sp`/`sr_nzvc` semantics, the O-mem-2 `load_delta` pattern (`:892`) for the per-side byte A7 delta, the fields in Global Constraints, `m_drc_native_mem_ea_arms` (probe counter), `drc_native_mem_ea_allowed()` (the gate).
- Produces (consumed by Task 3): `void generate_move_memmem(drcuml_block &, const memmem_form &, uml::code_label lbl_delegate);` — emits one full form natively; on a fully-granted instruction it runs all three accesses then retires; on a mid-instruction yield `generate_bus_step` JMPs to `lbl_delegate` and the partial interpreter handler resumes (OQ-1). Clobbers I0–I6; preserves I7 (`m_ird`).

- [ ] **Step 1: Declare the form descriptor + emitter in the header.** In `m68000.h`, beside `moverm_form`/`generate_move_regmem`:
```cpp
	enum mvmm_size : u8 { MVMM_B, MVMM_W };                  // byte (byte_lane=1) ; word (byte_lane=0, frozen 3b write path)
	enum mvmm_ea   : u8 { MVMM_AIS, MVMM_AIPS, MVMM_PAIS };  // (An), (An)+, -(An)  (same topology for src and dst)
	struct memmem_form { u16 value; u16 mask; u8 size; u8 src_ea; u8 dst_ea; };
	void generate_move_memmem(drcuml_block &block, const struct memmem_form &form, uml::code_label lbl_delegate); // native MOVE .b/.w mem->mem (An)/(An)+/-(An) (O-mem-3c)
```
> **Note** the `mvmm_ea` enum is reused for both `src_ea` and `dst_ea` (the `(An)`/`(An)+`/`-(An)` topology is the same set on both sides; `ais≡aid`, `aips≡aipd`, `pais≡paid` arithmetic-wise — the only difference is which register field and whether the prefetch interleaves, both handled by the emitter's src/dst branches + the descriptor order).

- [ ] **Step 2: Implement `generate_move_memmem`.** In `m68000drc.cpp`, after `generate_move_regmem`. **Read each handler line-by-line while transcribing (R-A).** Reuse the file-local helpers already proven in `generate_move_regmem`/`generate_movea_mem` (`ssw_data`/`ssw_program`/`commit_dbin`/`retire`/`move_flags`, and the `load_delta` per-side byte-A7 delta decoder) — duplication is acceptable and matches the O-mem-2/3a/3b precedent (no cross-function refactor in this batch). The structure (literal for the load-bearing pieces; **VERIFY-gated** for the per-mode EA choreography the 3a/3b Builders found error-prone):

```cpp
//-------------------------------------------------
//  generate_move_memmem - native UML for
//  move.b/.w (An)/(An)+/-(An) -> (An)/(An)+/-(An)  (O-mem-3c, 18 forms)
//
//  Mirrors move_{b,w}_{ais,aips,pais}_{aid,aipd,paid}_df in m68000-sdf.cpp.
//  TWO-EA composition: both operands are memory.  Value flows entirely in memory:
//    source DATA READ -> m_dbin -> MOVE flags (sr_nzvc from m_dbin) -> m_dbout ->
//    dest DATA WRITE.  NO register read, NO register write-back.
//
//  Bus-step ORDER is DEST-mode dependent (single-sourced from the generated run):
//    dest (An)/(An)+ (aid/aipd): [DATA READ, DATA WRITE, prefetch]   (prefetch last)
//    dest -(An)      (paid)     : [DATA READ, prefetch, DATA WRITE]   (prefetch MIDDLE)
//  The emitter iterates the run in order; the architectural setup before each access
//  is transcribed per handler (see the dest-mode branches below).
//
//  Word DATA WRITE uses the FROZEN O-mem-3b byte_lane==0 full-word UML_WRITE path;
//  byte uses the FROZEN byte_lane==1 masked path.  NO generate_bus_step edit in 3c.
//
//  Dual INDEPENDENT auto-inc/dec: src An (m_da[ry]) and dest An (m_da[rx]) each update
//  separately; word delta 2; byte delta 1 (A7: 2) PER SIDE.  ALIASING (ry==rx, e.g.
//  (A0)+->(A0)+): m_da[rx] MUST be read FRESH at the dest-setup state, AFTER the source
//  writeback to m_da[ry] -- never cache m_da[rx] from before the source update.
//
//  CRITICAL (R-A): decodes rx/ry from m_irdi; the DRC entry point does NOT latch m_irdi,
//  so this STORES m_irdi=m_ird FIRST (load-bearing for the interpreter partial-handler
//  resume).  Substates/charges/pre_charge/byte_lane/has_addr_error/ACCESS-ORDER come from
//  the generated run -- never hard-coded.  I7 holds m_ird, preserved across generate_bus_step.
//-------------------------------------------------

void m68000_device::generate_move_memmem(drcuml_block &block, const struct memmem_form &form, uml::code_label lbl_delegate)
{
	// locate this form's generated bus-step run by {value, mask} (mirror generate_move_regmem).
	auto find_run = [](u16 value, u16 mask) -> const drc_bus_run & {
		for(const drc_bus_run &r : s_drc_bus_run_table)
			if(r.value == value && r.mask == mask)
				return r;
		return s_drc_bus_run_table[0]; // unreachable for the wired opcodes
	};
	const drc_bus_run &run = find_run(form.value, form.mask);
	u16 si = run.first;                          // running index into s_drc_bus_step_table
	const bool is_word = (form.size == MVMM_W);
	const u32  zmask   = is_word ? 0xffffu : 0xffu;
	const u32  nmask   = is_word ? 0x8000u : 0x80u;

	// ---- THE LATCH: m_irdi = m_ird  (load-bearing for the interpreter's resume; O-mem-3a/3b) ----
	UML_STORE(block, &m_irdi, 0, I7, SIZE_WORD, SCALE_x1);

	// ssw_data / ssw_program / commit_dbin / retire / move_flags : reuse the generate_move_regmem
	// versions verbatim (transcribe or factor file-local statics -- out of scope to refactor here).
	// move_flags(Iv): N=MSB(v over size), Z=(v==0 over size), V=C=0, X kept  (== sr_nzvc()).

	// rx = dest EA An = map_sp(((m_irdi>>9)&7)|8) ; ry = src EA An = map_sp((m_irdi&7)|8).
	// BOTH are address registers (map_sp'd) -- unlike 3b where the register side was a raw Dn.

	// ===== STEP 1: source EA setup + DATA READ (substates 1/2) =====
	// VERIFY per src mode against the handler:
	//   ais  (move_w_ais_aid_df:58146)  : m_aob = m_da[ry]; m_at = m_da[ry];
	//   aips (move_w_aips_aid_df:58229+) : m_aob = m_da[ry]; m_au = m_da[ry]+delta; m_da[ry] = m_au;  (post-inc, delta per load_delta)
	//   pais (move_w_pais_paid_df:60706+): m_au = m_da[ry]-delta [pre_charge -2]; m_da[ry] = m_au; m_aob = m_au;  (pre-dec)
	//   m_base_ssw = SSW_DATA|SSW_R.  Use load_delta(ry) for the byte (A7)=2 exception.
	//   <<< transcribe the m_aob/m_au/m_at/m_da[ry] choreography here, per src mode, behind VERIFY >>>
	generate_bus_step(block, s_drc_bus_step_table[si++], lbl_delegate);   // DATA READ (substates 1/2 ; .b byte_lane=1/no-fault, .w byte_lane=0/fault ; pais pre_charge)
	// commit_dbin();  // m_dbin = m_edb  (the source byte/word)

	if(form.dst_ea == MVMM_PAIS)
	{
		// ===== dest -(An) (paid): order is [READ, PREFETCH, WRITE] =====
		// STEP 2 = final PREFETCH (substates 3/4).  At the prefetch SETUP (mmmw1) the handler
		// computes flags + the dest predecremented addr but does NOT yet write the dest.
		// VERIFY against move_w_pais_paid_df:60736-60745 / move_b_ais_paid_df:33024+ :
		//   m_aob = m_au (prefetch addr); m_ir = m_irc; m_pc = m_au;
		//   m_au = m_da[rx] - load_delta(rx);   // dest predec COMPUTE (read m_da[rx] FRESH here)
		//   move_flags(m_dbin);                 // MOVE flags BEFORE the prefetch and the write
		//   m_base_ssw = SSW_PROGRAM|SSW_R.
		//   <<< transcribe the prefetch setup + flags + dest-addr compute here, behind VERIFY >>>
		generate_bus_step(block, s_drc_bus_step_table[si++], lbl_delegate);   // PREFETCH (substates 3/4 ; byte_lane=0 ; fault)
		// STEP 3 = dest DATA WRITE (substates 5/6).  At the write setup (mmmw2):
		// VERIFY against move_w_pais_paid_df:60762-60770 :
		//   m_aob = m_au (= dest predec addr); m_ird = m_ir;
		//   if(m_next_state!=S_TRACE) m_next_state = m_int_next_state;
		//   m_dbout = <source value: .w m_aluo==m_dbin ; .b set_8xl(m_dbout,m_dbin)>;
		//   m_da[rx] = m_au;                    // dest predec WRITEBACK
		//   m_au = m_pc + 2; m_base_ssw = SSW_DATA.
		//   <<< transcribe the write setup + m_dbout + dest writeback here, behind VERIFY >>>
		generate_bus_step(block, s_drc_bus_step_table[si++], lbl_delegate);   // DATA WRITE (substates 5/6 ; .b byte_lane=1 masked, .w byte_lane=0 FULL-WORD [frozen 3b] ; paid pre_charge ; .w fault)
	}
	else
	{
		// ===== dest (An)/(An)+ (aid/aipd): order is [READ, WRITE, PREFETCH] =====
		// STEP 2 = dest DATA WRITE (substates 3/4) + MOVE FLAGS (BEFORE the write).
		// VERIFY against move_w_ais_aid_df:58169-58178 (aid) / move_w_aips_aipd_df:59452-59462 (aipd) /
		//   move_b_aips_aipd_df:32052-32062 (byte):
		//   m_aob = m_da[rx];                   // dest addr (read m_da[rx] FRESH here, AFTER src writeback)
		//   m_ir = m_irc;
		//   m_dbout = <source value: .w m_dbin ; .b set_8xl(m_dbout,m_dbin)>;
		//   m_pc = m_au; m_au = m_da[rx] + load_delta(rx);   // aipd dest post-inc COMPUTE (aid: no inc; writeback at step 3 for aipd)
		//   move_flags(m_dbin);                 // MOVE flags BEFORE the write
		//   m_base_ssw = SSW_DATA.
		//   <<< transcribe the write setup + m_dbout + flags here, per dest mode, behind VERIFY >>>
		generate_bus_step(block, s_drc_bus_step_table[si++], lbl_delegate);   // DATA WRITE (substates 3/4 ; .b byte_lane=1 masked, .w byte_lane=0 FULL-WORD [frozen 3b] ; .w fault)
		// STEP 3 = final PREFETCH (substates 5/6).  At the prefetch setup (mmrw2/mmiw2):
		// VERIFY against move_w_ais_aid_df:58193-58200 (aid) / move_w_aips_aipd_df:59477-59483 (aipd):
		//   m_aob = m_pc; m_ir = m_irc; m_au = m_pc + 2; m_ird = m_ir;
		//   if(m_next_state!=S_TRACE) m_next_state = m_int_next_state;
		//   m_da[rx] = m_au;   // aipd dest post-inc WRITEBACK happens HERE (after the write); aid: none
		//   m_base_ssw = SSW_PROGRAM|SSW_R.
		//   <<< transcribe the prefetch setup + (aipd) dest writeback here, behind VERIFY >>>
		generate_bus_step(block, s_drc_bus_step_table[si++], lbl_delegate);   // PREFETCH (substates 5/6 ; byte_lane=0 ; fault)
	}

	// ===== retire (byte-identical to generate_move_regmem's retire) =====
	//   m_irc = m_dbin = m_edb; set_ftu_const(); m_inst_state = m_next_state ? m_next_state
	//   : m_decode_table[m_ird]; if(m_sr & SR_T) m_next_state = S_TRACE.
	//   <<< transcribe the retire block here (or call the shared lambda) >>>
}
```

> **Seven load-bearing verifications (R-A) — make these explicit checks, not assumptions:**
> 1. **The `m_irdi` latch must precede every yield** (carried from 3a/3b). `UML_STORE(&m_irdi, I7)` is the first emitted op. The 1-cycle pass forces a yield after the source read, then the interpreter resumes and decodes `rx`/`ry` from `m_irdi` — wrong latch ⇒ wrong register ⇒ Leg-B failure.
> 2. **BOTH `rx` and `ry` are `map_sp` address registers.** `rx = map_sp(((m_irdi>>9)&7)|8)` (dest An), `ry = map_sp((m_irdi&7)|8)` (src An). This differs from 3b (where one side was a raw `Dn`). Decode both from `I7` (== `m_ird` == `m_irdi` after the latch).
> 3. **The bus-step ORDER is dest-mode-dependent and descriptor-driven.** For dest `-(An)` (paid) the run is `[READ, PREFETCH, WRITE]`; for dest `(An)`/`(An)+` the run is `[READ, WRITE, PREFETCH]`. The emitter switches on `form.dst_ea == MVMM_PAIS` and places the architectural setup per handler. A wrong order (e.g. writing before the prefetch for paid) ⇒ wrong RAM/prefetch state ⇒ Leg-B failure. **Eyeball that the paid branch's 2nd `generate_bus_step` consumes the PREFETCH descriptor row and the 3rd consumes the WRITE row (Task 1 Step 5 confirmed the run order).**
> 4. **ALIASING — `m_da[rx]` MUST be read FRESH at the dest-setup state, AFTER the source writeback to `m_da[ry]`.** When `ry == rx` (src An == dest An, e.g. `move.w (A0)+,(A0)+` or `move.w -(A0),-(A0)`), the dest base depends on the source's already-applied inc/dec. The handler reads `m_da[rx]` only at the dest-setup state, which is *after* the source-setup wrote `m_da[ry]`. **Do NOT load `m_da[rx]` early and cache it** — emit the `UML_LOAD` of `m_da[rx]` at the dest-setup. Concrete cases to reason through (and that the oracle corpus exercises): `(A0)+→(A0)+` (src reads X, A0=X+2; dest writes X+2, A0=X+4), `-(A0)→-(A0)` (src reads X−2, A0=X−2; dest writes X−4, A0=X−4), `(A0)+→-(A0)` (src reads X, A0=X+2; dest writes X, A0=X — write lands where it read!), `-(A0)→(A0)+` (src reads X−2, A0=X−2; dest writes X−2, A0=X). All correct iff `m_da[rx]` is read live at the dest-setup.
> 5. **Dual INDEPENDENT delta + writeback timing.** Source writeback to `m_da[ry]` is at the source-setup (post-inc at pinw2 before the read; pre-dec at pdcw2). Dest writeback to `m_da[rx]`: for `aipd` it is at the **prefetch-setup** (mmiw2, AFTER the write); for `paid` it is at the **write state** (mmmw2). Word delta = 2 always; byte delta = `load_delta(reg)` = 1 (reg<15) / 2 (A7). **Each side uses its OWN register for the A7 check** — `move.b (A7)+,(A0)+` ⇒ src delta 2, dest delta 1. VERIFY each delta + writeback point against its handler (the 3a AIPS-writeback inaccuracy was exactly this).
> 6. **MOVE flags use `m_dbin` and the right timing.** Flags = `move_flags(m_dbin)` at the dest-setup state, BEFORE the dest write (aid/aipd) or before the prefetch (paid). `move_flags` clears V/C and keeps X — reuse 3b's lambda verbatim. The `alu_and(m_dbin,0xffff)` *before* the source read (adrw1) that has no `sr_*` is dead-scratch — do not emit it; only the `alu_and+sr_nzvc` pair at the dest-setup is reproduced.
> 7. **`m_dbout` source per dest mode.** aid/aipd word: `m_dbout = m_dbin`. paid word: `m_dbout = m_aluo` (== `m_dbin & 0xffff` at that point — equivalent; feed `move_flags` and `m_dbout` from the same value). byte (all dest modes): `set_8xl(m_dbout, m_dbin)` = `(m_dbin & 0xff) | (m_dbin << 8)` (replicate to both lanes; mirror `generate_bitop_mem`/`generate_move_regmem`'s `set_8xl`). The word write is `byte_lane==0` → the frozen 3b full-word `UML_WRITE`; the byte write is `byte_lane==1` → the frozen masked path. The emitter does NOT special-case the write primitive — it is entirely descriptor-driven; the emitter only sets `m_dbout`/`m_aob`/`m_base_ssw` and calls `generate_bus_step`.

- [ ] **Step 3: Build (compile-only — no dispatch arm yet).**
```bash
MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd "$PWD"; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'
```
Expected: clean build. `generate_move_memmem` is defined but unreferenced (acceptable; the dispatch arms below call it). Fix any UML-scope/enum/`uml::parameter` errors now.

- [ ] **Step 4: Add the 18 patterns to `is_native_opcode`.** After the O-mem-3b reg↔mem switch (`m68000drc.cpp:210` region), add the MOVE mem→mem patterns (mask `0xf1f8`). **Exclude** `(d16,An)` (`das`/`dad`→3e) and `.l` (`0x20xx`→3d):
```cpp
	// O-mem-3c: move.b/.w mem->mem (An)/(An)+/-(An) -> (An)/(An)+/-(An) (mask 0xf1f8).
	switch(opword & 0xf1f8)
	{
	// byte:
	case 0x1090: case 0x1098: case 0x10a0:   // move.b (An)/(An)+/-(An),(An)
	case 0x10d0: case 0x10d8: case 0x10e0:   // move.b (An)/(An)+/-(An),(An)+
	case 0x1110: case 0x1118: case 0x1120:   // move.b (An)/(An)+/-(An),-(An)
	// word:
	case 0x3090: case 0x3098: case 0x30a0:   // move.w (An)/(An)+/-(An),(An)
	case 0x30d0: case 0x30d8: case 0x30e0:   // move.w (An)/(An)+/-(An),(An)+
	case 0x3110: case 0x3118: case 0x3120:   // move.w (An)/(An)+/-(An),-(An)
		return true;
	}
```
> **Verify each constant against the handler `// xxxx ffff` comment** (`move_b_ais_aid_df // 1090 f1f8`, `move_w_pais_paid_df // 3120 f1f8`, …). Confirm **no overlap** with the existing arms — the 3b reg↔mem bases (`0x1010/18/20/80/88/c0/c8/100/108`, `0x3010/…`) are disjoint from every 3c mem→mem base (`0x1090/98/a0/d0/d8/e0/110/118/120`, `0x3090/…`); the O-mem-2 bit-op bases (`0x0xxx`), moveq (`0x7000`), and MOVEA word (`0x3050/58/60`) share no value with the 3c bases. A wrong base silently mis-classifies.

- [ ] **Step 5: Add the gated dispatch arms.** In `generate_native_dispatch`, after the O-mem-3b reg↔mem block (`:378-413`), add a single `if (drc_native_mem_ea_allowed())` block with the 18-entry form table and the per-form compile-time arm (mirroring the 3b loop, `:382-413`):
```cpp
	// O-mem-3c: move.b/.w mem->mem (An)/(An)+/-(An) -- native ONLY behind the space-
	// topology gate (ADR 0007 O-mem-3 addendum; M5).  Each arm calls generate_move_memmem
	// with its form constants; the bus-step run (kinds/order/substates/charges/byte_lane/
	// has_addr_error/pre_charge) is single-sourced from the generator.  (d16,An) and .l
	// mem->mem are NOT here (O-mem-3e / O-mem-3d).
	if (drc_native_mem_ea_allowed())
	{
		static const memmem_form k_memmem_forms[] = {
			// byte
			{ 0x1090, 0xf1f8, MVMM_B, MVMM_AIS,  MVMM_AIS  },  // (An)  ,(An)   [dst aid]
			{ 0x1098, 0xf1f8, MVMM_B, MVMM_AIPS, MVMM_AIS  },  // (An)+ ,(An)
			{ 0x10a0, 0xf1f8, MVMM_B, MVMM_PAIS, MVMM_AIS  },  // -(An) ,(An)
			{ 0x10d0, 0xf1f8, MVMM_B, MVMM_AIS,  MVMM_AIPS },  // (An)  ,(An)+  [dst aipd]
			{ 0x10d8, 0xf1f8, MVMM_B, MVMM_AIPS, MVMM_AIPS },  // (An)+ ,(An)+
			{ 0x10e0, 0xf1f8, MVMM_B, MVMM_PAIS, MVMM_AIPS },  // -(An) ,(An)+
			{ 0x1110, 0xf1f8, MVMM_B, MVMM_AIS,  MVMM_PAIS },  // (An)  ,-(An)  [dst paid -> prefetch-first]
			{ 0x1118, 0xf1f8, MVMM_B, MVMM_AIPS, MVMM_PAIS },  // (An)+ ,-(An)
			{ 0x1120, 0xf1f8, MVMM_B, MVMM_PAIS, MVMM_PAIS },  // -(An) ,-(An)
			// word
			{ 0x3090, 0xf1f8, MVMM_W, MVMM_AIS,  MVMM_AIS  },
			{ 0x3098, 0xf1f8, MVMM_W, MVMM_AIPS, MVMM_AIS  },
			{ 0x30a0, 0xf1f8, MVMM_W, MVMM_PAIS, MVMM_AIS  },
			{ 0x30d0, 0xf1f8, MVMM_W, MVMM_AIS,  MVMM_AIPS },
			{ 0x30d8, 0xf1f8, MVMM_W, MVMM_AIPS, MVMM_AIPS },
			{ 0x30e0, 0xf1f8, MVMM_W, MVMM_PAIS, MVMM_AIPS },
			{ 0x3110, 0xf1f8, MVMM_W, MVMM_AIS,  MVMM_PAIS },
			{ 0x3118, 0xf1f8, MVMM_W, MVMM_AIPS, MVMM_PAIS },
			{ 0x3120, 0xf1f8, MVMM_W, MVMM_PAIS, MVMM_PAIS },
		};
		for(const memmem_form &f : k_memmem_forms)
		{
			uml::code_label const lbl_next = m_drc_labelnum++;
			UML_AND(block, I0, I7, f.mask);
			UML_CMP(block, I0, f.value);
			UML_JMPc(block, COND_NE, lbl_next);
			generate_move_memmem(block, f, lbl_delegate);         // emit the form (suspend yields JMP lbl_delegate from within)
			UML_JMP(block, lbl_delegate);                         // fully-granted: retired -> hand the tail to the interpreter
			UML_LABEL(block, lbl_next);
			m_drc_native_mem_ea_arms++;                           // emission probe (gate/coverage test)
		}
	}
```
> **Confirm the form table's `{value, src_ea, dst_ea}` triples** against the handler-name → encoding map (e.g. `0x10e0` = `move_b_pais_aipd_df` ⇒ src `-(An)` (PAIS), dst `(An)+` (AIPS); `0x3110` = `move_w_ais_paid_df` ⇒ src `(An)` (AIS), dst `-(An)` (PAIS, prefetch-first)). A swapped src/dst enum is a silent mis-emit caught only by the oracle — double-check each row against its `// xxxx f1f8` handler.

- [ ] **Step 6: Build, then 1-cycle Leg B (x64 + C) — native source read + the suspend handoff + the `m_irdi` latch.**
```bash
MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd "$PWD"; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'
./mametests "[m68000]"                            # Leg A
./mametests "[m68000][drc]"                       # Leg B 1-cycle, x64
CPUORACLE_M68_DRC_C=1 ./mametests "[m68000][drc]" # Leg B 1-cycle, C
```
Expected: green. The 1-cycle pass forces a yield after the source read, then the interpreter resumes via the partial handler (decoding `rx`/`ry` from `m_irdi`). **If a form fails here with a wrong register, suspect the `m_irdi` latch first** (verification 1); a wrong source-EA address suspects the `m_da[ry]`/`m_aob` setup or the src delta.

- [ ] **Step 7: FULL-GRANT Leg B (x64 + C) — THE native two-EA composition (read → flags → write) + the dest-mode ordering gate.**
```bash
CPUORACLE_M68_DRC_FULLGRANT=1 ./mametests "[m68000][drc]"                       # x64
CPUORACLE_M68_DRC_FULLGRANT=1 CPUORACLE_M68_DRC_C=1 ./mametests "[m68000][drc]" # C backend
```
Expected: **green.** This is the only pass that runs the **whole** native instruction in one go — the source read, the MOVE flags from `m_dbin`, the dest write (word = frozen 3b full-word, byte = frozen masked), and the dest-mode-dependent prefetch ordering. **If a form fails:** `superpowers:systematic-debugging` — map the divergence to the step (RAM at the dest → the write / `m_dbout` / `m_aob` / aliasing `m_da[rx]` freshness; SR → `move_flags` value source or V/C/X; A-reg → the per-side inc/dec writeback/delta or aliasing; prefetch addr or cycle → the dest-mode ordering / a charge / the predecrement `−2`). The fallback is to drop the failing form from `is_native_opcode` + the dispatch table (route it back to `cfunc_`) and report — never approximate. **Pay special attention to the paid-dest forms (prefetch-first) and the aliasing forms (`(A0)+→(A0)+`, `-(A0)→-(A0)`, `(A0)+→-(A0)`, `-(A0)→(A0)+`).**

- [ ] **Step 8: PARTIAL-GRANT Leg B (x64 + C) — the between-access resume handoff (TWO data accesses now).**
```bash
CPUORACLE_M68_DRC_PARTGRANT=1 ./mametests "[m68000][drc]"                       # x64
CPUORACLE_M68_DRC_PARTGRANT=1 CPUORACLE_M68_DRC_C=1 ./mametests "[m68000][drc]" # C backend
```
Expected: green. Single `length-4` offset (OQ-10: no sweep). For these 3-access forms this resumes the interpreter at the **last** access after the first two ran native — validating the native left RAM/`m_sr`/`m_da[]` correct after the source read + (for aid/aipd) the dest write, or after the source read + (for paid) the prefetch. This is a richer resume surface than 3b (two data accesses), so a wrong intermediate scratch (`m_dbin`/`m_dbout`/the An writebacks) at the second-to-last access fails here. **If a form fails only here**, the native first-pass left a field the interpreter's resumed partial handler reads stale — most likely an An writeback applied at the wrong step or the aliasing `m_da[rx]` freshness.

- [ ] **Step 9: `srcclean` + commit.**
```bash
git add src/devices/cpu/m68000/m68000.h src/devices/cpu/m68000/m68000drc.cpp
git commit -m "feat(m68000drc): native MOVE .b/.w mem->mem (An)/(An)+/-(An) -- 18 forms (O-mem-3c)"
```

---

## Task 3: Coverage assertion + AS_OPCODES subset + cut-line doc

**Goal:** Machine-assert that all 18 MOVE mem→mem forms dispatch native (behind the gate), confirm the AS_OPCODES differential covers them (gate off → `cfunc_`), and record them in the cut-line doc with the two-EA composition + dual auto-inc/dec + the `-(An)`-dest prefetch-ordering note.

**Files:**
- Modify: `tests/emu/cpu/cpuoracle.cpp` — the native-coverage assertion (in `[m68000][drc][gate]`) and, if the AS_OPCODES differential restricts to a corpus subset, extend that subset to include the 18 forms.
- Modify: `src/devices/cpu/m68000/README-drc.md` — the cut-line doc.

**Interfaces:**
- Consumes: `is_native_opcode(u16)` (Task 2) for the 18 patterns; the harness wrapper used for the O-mem-1/2/3a/3b coverage checks.

- [ ] **Step 1: Extend the native-coverage assertion.** Add the 18 representative encodings to the asserted-native set in the `[m68000][drc][gate]` case (the predicate is mask-based), reusing the wrapper O-mem-1/2/3a/3b added to reach the protected `is_native_opcode`:
```cpp
	// O-mem-3c: move.b/.w mem->mem (An)/(An)+/-(An) -> (An)/(An)+/-(An) are native (behind the gate).
	for(u16 op : { (u16)0x1090,(u16)0x1098,(u16)0x10a0, (u16)0x10d0,(u16)0x10d8,(u16)0x10e0,   // byte dst (An)/(An)+
	               (u16)0x1110,(u16)0x1118,(u16)0x1120,                                          // byte dst -(An)
	               (u16)0x3090,(u16)0x3098,(u16)0x30a0, (u16)0x30d0,(u16)0x30d8,(u16)0x30e0,   // word dst (An)/(An)+
	               (u16)0x3110,(u16)0x3118,(u16)0x3120 })                                        // word dst -(An)
		CHECK(harness_is_native_opcode(op));
	// anti-vacuity: (d16,An) mem<->mem and .l mem->mem stay cfunc_ in 3c (3e / 3d).
	CHECK_FALSE(harness_is_native_opcode((u16)0x10a8));   // move.b (d16,An),(An)   -> 3e
	CHECK_FALSE(harness_is_native_opcode((u16)0x1150));   // move.b (An),(d16,An)   -> 3e
	CHECK_FALSE(harness_is_native_opcode((u16)0x2090));   // move.l (An),(An)       -> 3d
```
> **VERIFY** the anti-vacuity encodings against the handler table (`move_b_das_aid_df // 10a8`, `move_b_ais_dad_df // 1150`, `move_l_ais_aid_df // 2090`) — the point is to prove the filter is neither over- nor under-admitting at the 3c/3d/3e boundary.

- [ ] **Step 2: Confirm the AS_OPCODES differential exercises the 18 forms (or extend its subset).** Read the O-mem-2/3b `[m68000][drc][asopcodes]` case (`cpuoracle.cpp`). If it runs the full corpus, the MOVE mem→mem forms are already covered (gate off → `cfunc_` → interpreter ≡ DRC, and the DRC does not mis-read the opcode space). If the differential is restricted to a corpus subset, **extend the subset to include the 18 MOVE mem→mem forms** so the differential proves the gate keeps them on `cfunc_` on an `AS_OPCODES` topology and the fallback matches. No new config beyond the batch's opcodes (M6 item 4).

- [ ] **Step 3: Update the cut-line doc.** In `src/devices/cpu/m68000/README-drc.md`:
  - In "Native opcodes shipped (current)", add a row:
```markdown
| `move.b`/`move.w` mem→mem `(An)`/`(An)+`/`-(An)` | O-mem-3c | 18 forms (src {(An),(An)+,-(An)} × dst {(An),(An)+,-(An)} × {.b,.w}). The **two-EA composition**: source-EA read via the frozen `generate_bus_step()` (`.b` `byte_lane=1`/no-fault, `.w` `byte_lane=0`/fault) → **MOVE flags** (`sr_nzvc`: N=MSB, Z=(value==0), V=C=0, X untouched) from the source value (`m_dbin`) at the dest-setup state → dest-EA write via the frozen `generate_bus_step()` (`.w` = the O-mem-3b `byte_lane==0` full-word `UML_WRITE`; `.b` = the O-mem-2 `byte_lane==1` masked path). **No register read/write** (both operands memory). **Dual INDEPENDENT auto-inc/dec**: src and dest each update their own `An` (word delta 2; byte delta 1, A7=2, per side). **Dest `-(An)` interleaves the final prefetch BEFORE the dest write** (`[read, prefetch, write]`); dest `(An)`/`(An)+` is `[read, write, prefetch]` — both single-sourced from the generated run. Decodes `rx`/`ry` (both `map_sp` address registers) from `m_irdi` (latched `m_irdi=m_ird`). **NO `generate_bus_step()` edit** (the one MOVE-arc primitive edit was O-mem-3b's word write). **Native ONLY on a flat-topology, non-MMU bus (`drc_native_mem_ea_allowed()`)** — `AS_OPCODES`/user-space/MMU stay `cfunc_`. Validated by the 1-cycle, full-grant (the two-EA composition end-to-end), and single-offset partial-grant (resume between the two data accesses) Leg-B passes. `(d16,An)`→3e, `.l`→3d. |
```
  - In "Explicitly NOT native" / the MOVE notes, reword so the 18 mem→mem forms are excluded from the deferred set ("memory-EA MOVE mem→mem `.b`/`.w` `(An)`/`(An)+`/`-(An)` native as of O-mem-3c on a flat-topology non-MMU bus; `(d16,An)`/`.l` mem→mem still `cfunc_`").
  - In "Known limitations" / the mechanism notes, record: **O-mem-3c adds NO `generate_bus_step()` change** (it reuses the read path frozen since O-mem-2/3a and the write path frozen since O-mem-3b); the new mechanism is the two-EA composition + dual auto-inc/dec + the dest-mode-dependent prefetch ordering; the native **retire** lambda remains the one native residual not oracle-exercised (the snapshot model cannot reach an overshoot; mirrors the validated `moveq`/`btst`/MOVEA/reg↔mem tail).

- [ ] **Step 4: Full local gate (every pass, both backends).**
```bash
MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd "$PWD"; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'
./mametests "[m68000]"                                                          # Leg A
./mametests "[m68000][drc]"                                                     # Leg B 1-cycle x64
CPUORACLE_M68_DRC_C=1 ./mametests "[m68000][drc]"                              # Leg B 1-cycle C
CPUORACLE_M68_DRC_FULLGRANT=1 ./mametests "[m68000][drc]"                      # full-grant x64 (two-EA composition)
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
git commit -m "test+docs(m68000drc): assert 18 MOVE mem->mem forms native; record O-mem-3c two-EA composition in cut-line doc"
```

---

## Task 4: Merge gate — push, Linux `oracle` green, merge

**Goal:** Get every required gate GREEN — the three oracle grant modes (x64 + C) on the appserver Linux `oracle` CI job, the AS_OPCODES differential, the gate-predicate + coverage + MMU-regen tests, generator additivity, and code review — then merge per the auto-merge policy.

**Files:** none (CI + PR).

- [ ] **Step 1: Push the branch and open the PR.** PR body includes a **Docs Impact** section (README-drc.md updated; ADR 0007 O-mem-3 addendum is the source of truth) and the gate evidence (all three passes green on Linux, both backends). **Call out in the PR that 3c touches `generate_bus_step()` NOT AT ALL** — it is a pure emitter + generator-descriptor addition over the frozen primitive (the one MOVE-arc primitive edit was O-mem-3b). Note the two new mechanisms reviewers should focus on: the **two-EA composition** (no register side) and the **dest-`-(An)` prefetch-before-write ordering** + the **aliasing** (`m_da[rx]` fresh read).
```bash
git push -u origin <impl-branch>   # the implementation branch (feat/o-mem-3c-move-memmem), NOT this docs/ plan branch
gh pr create --fill
```
> **Builder note:** the *implementation* lands on its own `feat/o-mem-3c-move-memmem` branch (not the `docs/` plan branch). The three commits above (generator; emitter+dispatch; tests+docs) form that PR.

- [ ] **Step 2: Get the appserver Linux `oracle` job GREEN — the SUFFICIENT gate.** Confirm the Linux `oracle` job runs **all three** grant modes (1-cycle + full-grant + partial-grant), x64 + C, plus the `[gate]` and `[asopcodes]` cases. A red Linux job with green Windows is almost always an ABI-safety regression (a plain `mem(&field)` slipped into `generate_move_memmem` — audit every new UML operand) or a backend-divergent emission (R-C). Do not merge until the Linux `oracle` job is green.

- [ ] **Step 3: Throughput bench (KNOWN ENVIRONMENT GAP — flag, do NOT hard-gate).** Per the §5/M6 addendum honest-number note, MOVE mem→mem throughput would be measured `-drc 0` vs `-drc 1` on a **gate-eligible flat-topology** driver where mem→mem MOVE is hot (not an FD1094/`AS_OPCODES` set, which runs `cfunc_`). **The throughput bench has been blocked in this environment by absent ROMs** — treat it as evidence-when-available, **not** a blocking gate. The correctness gates (Step 2 + the local gate) gate merge. Record the ROM-blocked status in the PR.

- [ ] **Step 4: Merge per the auto-merge policy.** When all of: the three oracle passes green (x64 + C) on the Linux `oracle` job, the gate-predicate + coverage + MMU-regen + AS_OPCODES differential green, generator additive (only `m68000-drcdesc.ipp` grew), `generate_bus_step()` unchanged (confirm an empty diff on that function), code review clean, `-validate` clean — **merge** (implementation cycle complete, parity gates green, review clean, issues addressed; the throughput bench is a documented known-gap, not a hard gate). Do not stop to ask.

---

## Self-review

**Spec coverage (ADR 0007 O-mem-3 addendum → tasks):**
- M1 word/long data bus-steps — 3c uses the **frozen** primitive: source read (existing read path), dest write (the O-mem-3b `byte_lane==0` full-word path / `byte_lane==1` byte path), prefetch (existing). **No `generate_bus_step()` edit, no new `step.kind`, no new descriptor field.** The shared address-error branch already fires for word read AND word write (`has_addr_error==1`). → Tasks 1 (descriptor), 2 (emitter).
- M2 `(d16,An)` → **excluded from 3c** (3e): `das`/`dad` stay out of the generator filter (not in `REGIND_S`/`REGIND_D`) and the dispatch table.
- M3 the **two-EA read→write composition** → 3c is exactly this mechanism: source-EA read → `m_dbin` → MOVE flags (`sr_nzvc`, single-access) at the dest-setup BEFORE the write → `m_dbout` → dest-EA write, with **dual independent** auto-inc/dec writeback per side. The `-(An)`-dest prefetch-before-write ordering and the aliasing (`m_da[rx]` fresh read) are the load-bearing subtleties. → Tasks 1 (descriptor order), 2 (emitter).
- M4 the 5-sub-batch split: **3c is the third batch, 18 forms** (handler-verified; matches the "~18" estimate) → this plan. 3c adds the two-EA composition over the frozen single-access write (3b); it touches `generate_bus_step()` not at all.
- M5 gate continuity (`drc_native_mem_ea_allowed()` covers 3c unchanged; no new clause, no `SR_S` branch) → Task 2 (arms wrapped in the gate); MMU-attach safety already discharged by O-mem-2 OQ-9.
- M6 merge-gate guidance (three grant modes both backends, AS_OPCODES differential, gate+coverage+MMU-regen, generator additivity, code review + Linux oracle, throughput as evidence-when-available) → Tasks 3 + 4.
- OQ-10 (no partial-grant sweep) → owner-decided 2026-06-28; **no oracle change in this plan** (the three existing grant modes suffice). 3c forms are single-access (3 steps: read, write, prefetch), so there are no intermediate long-access boundaries — the single `length-4` partial-grant offset lands the resume at the last access. (The partial-grant pass now has two data accesses + a prefetch to resume between, a richer surface than 3b — covered by the existing mode.)

**No-primitive-edit confirmation (the prompt's load-bearing requirement):** 3c contains **zero** `generate_bus_step()` changes. The frozen primitive (read byte_lane=0/1, write byte_lane=0/1, prefetch, has_addr_error, pre_charge — all already present after O-mem-2/3b) expresses every 3c access. Task 4 Step 4 confirms an empty diff on `generate_bus_step()` before merge. If any form appears to need a primitive change, that is a plan bug (Global Constraints).

**Placeholder scan:** the emitter is one parameterized function driven by the generated run, not 18 copies. The `// VERIFY against <handler>:<line>` markers are deliberate single-source gates (the addendum's explicit derive-from-handler requirement + the 3a/3b Builders' FOUR-inaccuracy lesson), not deferred work: the per-mode EA choreography (`m_aob`/`m_au`/`m_at`/`m_da[]`/`m_pc` advances), the per-side writeback deltas, the dest-mode prefetch ordering, the aliasing `m_da[rx]` freshness, and the `m_dbout` source are VERIFY-tagged transcription points; the **literal** code given (the `is_native_opcode` switch, the dispatch table, the enum/struct, the run-find lambda, the structural skeleton, the `move_flags` reuse) was double-checked against the cited handler lines and grep-verified encodings. The retire/`ssw_*`/`commit_dbin`/`move_flags` lambdas are reused from the byte-identical `generate_move_regmem` helpers.

**Type consistency:** `generate_move_memmem(drcuml_block&, const memmem_form&, code_label)`; `memmem_form{u16 value; u16 mask; u8 size; u8 src_ea; u8 dst_ea}`. `m_dbin`/`m_dbout`/`m_edb`/`m_irc`/`m_ir`/`m_ird`/`m_irdi`/`m_sr`/`m_base_ssw`/`m_aluo`/`m_alub` are `u16` (SIZE_WORD); `m_da[17]`/`m_sp`/`m_aob`/`m_at`/`m_au`/`m_pc`/`m_icount`/`m_inst_state`/`m_next_state`/`m_int_next_state` are `u32` (SIZE_DWORD); `m_inst_substate` is `u16`. `set_8xl(m_dbout, v)` = `(v & 0xff) | (v << 8)`. MOVE flags via the reused `move_flags` (direct N/Z, V=C=0, X kept) ≡ `sr_nzvc()`. The word write is the frozen `UML_WRITE(block, addr, src, SIZE_WORD, SPACE_PROGRAM)` (3b primitive); the byte write the frozen masked `UML_WRITEM`. Substates/charges/`pre_charge`/`byte_lane`/`has_addr_error`/**access-order** come from the descriptor, never hard-coded.

**Genuinely-new open questions (design is settled; few expected):**
1. **The dest-mode prefetch ordering in the emitter.** The plan switches the emitter on `form.dst_ea == MVMM_PAIS` to place the architectural setup, while the bus-step KIND/order comes from the descriptor. If the Builder prefers, the emitter could iterate the run and branch on `step.kind` at each index instead of the explicit dst-mode `if` — equivalent, as long as the per-step setup matches the handler. Either is acceptable; the VERIFY gates anchor on the handlers. Not blocking.
2. **`move_flags`/`m_dbout` source for paid-dest word (`m_aluo` vs `m_dbin`).** They are equal (`m_aluo == m_dbin & 0xffff` at mmmw1). The plan feeds both from `m_dbin`; if the full-grant pass shows any divergence, replicate the exact `m_aluo` dataflow. Not blocking.
3. **`DRC_EA_*` / `DRC_SZ_*` / `drc_size` / `REGIND_S`/`REGIND_D` constant names.** Task 1 filters on these; the Builder confirms against the maps in `m68000gen.py` (3b's clause is the reference; 3b already confirmed `REGIND_S`/`REGIND_D`). The additivity + eyeball checks (Task 1 Steps 4–5) catch any over/under-admit. Not blocking.
4. **`m_dcr`/`m_alub`/`m_at` dead-scratch skip.** Skipped per the O-mem-1/2/3a/3b precedent; the Builder VERIFYs none is in the oracle compare set at first build. If any is, set it (cheap). Not blocking.

Neither these nor any M-section item changes the architecture; all are local implementation choices the Builder resolves at the first build.

---

## Merge gate (O-mem-3c)

A batch merges only on **all** of (ADR 0007 O-mem-3 addendum M6, OQ-10-adjusted):
1. **1-cycle Leg B GREEN** (Leg A unchanged; Leg B register/flag/RAM/**cycle** exact), x64 + C — native source-read + the suspend handoff + the `m_irdi` resume.
2. **Full-grant Leg B GREEN** (`CPUORACLE_M68_DRC_FULLGRANT=1`), x64 + C — the native two-EA composition end-to-end (source read → MOVE flags → dest write, word = frozen 3b full-word, byte = frozen masked) + the dest-mode prefetch ordering. **Blocking.**
3. **Partial-grant Leg B GREEN** (`CPUORACLE_M68_DRC_PARTGRANT=1`, single `length-4` offset), x64 + C — the between-access resume handoff (two data accesses + a prefetch). **No parameterized sweep** (OQ-10; single-access forms have no intermediate long boundary).
4. **AS_OPCODES differential GREEN** (`[m68000][drc][asopcodes]`) — the gate keeps `AS_OPCODES`/user-space/MMU machines on `cfunc_`, the fallback matches, and the DRC does not mis-read the opcode space.
5. **Gate-predicate + coverage + MMU-regen GREEN** (`[m68000][drc][gate]`) — the 18 MOVE mem→mem forms asserted native; `(d16,An)`/`.l` mem→mem asserted `cfunc_`; the OQ-9 MMU-regen still green.
6. **Generator additive** — only `m68000-drcdesc.ipp` grew (the 18 new MOVE mem→mem runs, incl. the paid-dest prefetch-before-write order), single-sourced from the microcode walk. **No new `step.kind`, no new descriptor field.**
7. **`generate_bus_step()` unchanged** — confirm an empty diff on that function; the one MOVE-arc primitive edit was O-mem-3b's word write, frozen for 3c.
8. **Code review clean** + the **appserver Linux `oracle` job GREEN** (Windows-green is necessary-not-sufficient; the Linux job must run all three grant modes; audit every new UML operand for a stray `mem(&field)`).
9. **Throughput bench** — evidence-when-available on a gate-eligible flat-topology driver; **known environment gap (absent ROMs), NOT a hard gate.**

When 1–8 are green, **merge per the auto-merge policy** — do not stop to ask. Subsequent batches: **O-mem-3d** (long `.l` reg↔mem + mem→mem — the long two-word write + split flags `sr_nzvc`+`sr_nz_u`, highest substate count 1/2…9/10), then **O-mem-3e** (`(d16,An)` source & dest across all sizes/topologies, absorbing MOVEA `(d16,An)→An`).
