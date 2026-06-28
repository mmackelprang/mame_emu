# O-mem-1 Implementation Plan — native `generate_bus_step()` + `btst #n,(xxx).W/.L`

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the first native memory-EA opcode — `btst #n,(xxx).W` and `btst #n,(xxx).L`, the 43%-of-aurail-cycles hot poll — by adding the reusable native bus-access primitive `generate_bus_step()` to the m68000 DRC, gated cycle-exact by the dual-leg oracle.

**Architecture:** This is the load-bearing increment of **boundary O** (phase-2 Task 10), the mechanism defined by **[ADR 0007](../adr/0007-m68000-native-memory-ea-suspend-mechanism.md)**. The DRC issues each 68000 bus access natively (`UML_READ` against `SPACE_PROGRAM`) and then replicates the interpreter's per-bus-cycle checkpoint inline in UML — the `m_icount -= N` charge, the two-way `m_icount<=0 && access_to_be_redone()` suspend, and the `m_aob&1 → S_ADDRESS_ERROR` fault branch — so the native path suspends and faults at the *same* bus cycle with the *same* cycle count as the interpreter. Cycle constants and substate numbers are single-sourced from `m68000gen.py`, never re-estimated. The first pass through a fully-granted instruction runs entirely in native UML; resume after a mid-instruction yield is owned by the interpreter (boundary M's `m_inst_substate==0` guard already routes a resuming instruction to the interpreter delegate).

**Tech Stack:** C++17 (the `m68000_device` / DRCUML emitter, drcbe_x64 + drcbec backends), Python 3 (the `m68000gen.py` generator), GENie/`mingw32-make` build, Catch2 oracle harness (`tests/emu/cpu/cpuoracle.cpp`). MSYS2 UCRT64/MINGW64 toolchain on Windows; appserver Linux for the authoritative `oracle` CI job.

> ⚠ **REQUIRED before Task 4 / Task 5 — read the [ADR 0007 Addendum (2026-06-28)](../adr/0007-m68000-native-memory-ea-suspend-mechanism.md#addendum--resolution-2026-06-28--o-mem-1-pre-merge-review).**
> A pre-merge review found the as-built dispatch emits native `btst`-absolute even when a driver
> configures `AS_OPCODES`/user spaces (wrong memory — a regression). Before merge the Builder must
> (1) add the compile-time predicate `drc_native_mem_ea_allowed()` and guard the `btst`-absolute arm in
> `generate_native_dispatch` with it (the `moveq` arm stays unguarded), and (2) add a gate-predicate unit
> test (device with `AS_OPCODES` / user space / MMU ⇒ arm not emitted, dispatch falls to `cfunc_`). The
> addendum has the exact predicate, the bounded directive, and the merge-gate delta. `SPACE_PROGRAM` for
> all reads is correct **only under that gate**; do not follow the bare "SPACE_PROGRAM per §1" note in the
> self-review below without it. Interruptible/wait-state reads (finding 2) are deferred (addendum OQ-6).

## Global Constraints

These apply to **every** task below — copied verbatim from ADR 0007 + the phase-2 plan + the cut-line doc + the user's workflow rules.

- **THE GATE for every DRC-touching task is the dual-leg oracle (Leg A + Leg B, register/flag/RAM/CYCLE-exact) GREEN on the appserver LINUX `oracle` CI job.** Local Windows green is **necessary-not-sufficient** — boundary M passed Windows but failed Linux twice on `drcbe_x64 offset_from_rbp`. The `oracle` CI job is NOT preflight (preflight is a tiny-build smoke; do not confuse them).
- **ABI-safe codegen:** zero plain `mem(&device_field)` operands. Every device-state field is accessed via `UML_LOAD`/`UML_STORE` with a pointer base (the boundary-M pattern in `m68000drc.cpp`). Plain `mem(&m_field)` is lowered RBP-relative by drcbe_x64 and throws `offset_from_rbp` when the heap device is >2 GiB from the high-mmap'd RWX cache on Linux/SysV.
- **Generator additivity:** regenerating must leave existing generated decode files **byte-identical** (empty `git diff` on `m68000-decode.cpp`, `m68000-head.h`, `m68000-s{d,i}{f,p}.cpp`). Only the new/extended `.ipp` may change (boundary K's additive-only rule, R3). The `enum_str()` shim preserves byte-identity on Python ≥ 3.11 — do not remove it.
- **Build env (verified recipe):** `MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd <worktree>; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'`, then `./mametests "[m68000]"`. Do **NOT** use `make SOURCES=...m68000.cpp` (SOURCES filters by *driver*; a CPU device has no driver and GENie errors). `make REGENIE=1` is required whenever a `.cpp`/`.h` file is added; the m68000 core builds as part of the `TESTS=1` target.
- **Backend matrix:** test Leg B on **x64 (`drcbex64`)** AND the **C backend (`CPUORACLE_M68_DRC_C=1`)**. arm64 (`drcbearm64`) is deferred to the CI matrix.
- **Cycle truth:** the interpreter (`m68000.cpp` under `-drc 0`) is the authority. The DRC mirrors its cycle count; divergence is always a DRC bug, resolved by routing the opcode back to `cfunc_` — never by editing the interpreter.
- **Scope lock:** O-mem-1 lands **first and ALONE** (the mechanism PR; reviewed in isolation). Only `generate_bus_step()` + `btst #n,(xxx).W` + `btst #n,(xxx).L` are native here. All other memory-EA opcodes (bit-ops on `(An)`/`(An)+`/`-(An)`, MOVE, ALU) are O-mem-2…5 and are **out of scope**.
- **Style:** match the existing m68000 brace/whitespace style (tabs, the K&R-ish style already in `m68000drc.cpp`); license header `// license:BSD-3-Clause` / `// copyright-holders:Mark Mackelprang` on any new file; run `srcclean` (built via `TOOLS=1`) on touched files before committing. Work on a short-lived branch and open a PR (never commit DRC source straight to `main`).

---

## The interpreter shape O-mem-1 must mirror (read before any task)

The native emission reproduces `m68000_device::btst_imm8_adr16_df()`
(`src/devices/cpu/m68000/m68000-sdf.cpp:20110`) for `.W` and
`btst_imm8_adr32_df()` (`:20218`) for `.L`. The `_df` variant is the one the
plain `m68000_device` runs (verified: `m68000-sdf.cpp:183982` registers
`&m68000_device::btst_imm8_adr16_df` in the device's own handler table; the
`_dp`/`_dfm`/`_df8` variants belong to other cores/subclasses and are out of
scope). The `.W` handler is a **single microcode state** with a
**4-read** bus sequence and substates 1…8; the `.L` adds one extra
absolute-address prefetch read, giving a **5-read** sequence with substates
1…10. Each read is the same shape:

```cpp
// (from m68000-sdf.cpp btst_imm8_adr16_df, read #1; the canonical step)
m_aob = m_au;                                  // address/architectural setup
m_pc  = m_au;
set_16l(m_dt, m_dbin);
m_au  = m_au + 2;
m_base_ssw = SSW_PROGRAM | SSW_R;              // bus-status field (matters on a fault)
m_edb = m_opcodes.read_interruptible(m_aob & ~1);   // THE READ
m_icount -= 4;                                 // the per-bus-cycle charge
if(m_icount <= 0) {                            // the SUSPEND checkpoint
    if(access_to_be_redone()) { m_icount += 4; m_inst_substate = 1; }  // redo
    else                      { m_inst_substate = 2; }                 // completed
    return;
}
if(m_aob & 1) {                                // the ADDRESS-ERROR fault branch
    m_icount -= 4;
    m_inst_state = S_ADDRESS_ERROR;
    return;
}
m_irc = m_edb; m_dbin = m_edb;                 // the commit
```

The **data read** (read #3 in `.W`, read #4 in `.L`) differs in three ways and
is the part most likely to be mis-emitted:

```cpp
m_base_ssw = SSW_DATA | SSW_R;                                       // DATA, not PROGRAM
m_edb = m_program.read_interruptible(m_aob & ~1, m_aob & 1 ? 0x00ff : 0xff00);  // byte-lane mask
if(!(m_aob & 1)) m_edb >>= 8;                                       // high-byte select when even
m_icount -= 4;
if(m_icount <= 0) { /* substate 5/6 (.W) or 7/8 (.L) */ return; }
// NO address-error branch here (byte read; even/odd both legal)
m_dbin = m_edb;
alu_and8(m_dbin, 1 << (m_dcr & 7)); sr_z();                         // compute Z from the read byte
```

The substate pairs per step (verified from the handler source — single-source
these from the generator in Task 1, do NOT hand-transcribe into the emitter):

| Step | `.W` (`adr16`, substates) | `.L` (`adr32`, substates) | space | kind |
|---|---|---|---|---|
| read 1 (ext word / abs-addr hi) | 1 / 2 | 1 / 2 | PROGRAM | prefetch-read |
| read 2 (refill / abs-addr lo) | 3 / 4 | 3 / 4 | PROGRAM | prefetch-read |
| read 3 (.L only: refill) | — | 5 / 6 | PROGRAM | prefetch-read |
| data read at `(xxx)` | 5 / 6 | 7 / 8 | DATA | data-read (byte-lane) |
| final prefetch | 7 / 8 | 9 / 10 | PROGRAM | prefetch-read |

Address-error branch present on every PROGRAM read; absent on the DATA read.

---

## File Structure

| File | Responsibility | Touched in |
|---|---|---|
| `src/devices/cpu/m68000/m68000gen.py` | Generator — **add** a per-opcode `drc_bus_step[]` emission (access kind/size/space/lane, `−N`, redo/completed substate pair), additive-only | Task 1 |
| `src/devices/cpu/m68000/m68000-drcdesc.ipp` | Generated descriptor table — **regenerated** to carry the new bus-step rows; existing rows byte-identical | Task 1 |
| `src/devices/cpu/m68000/README-drc.md` | Cut-line doc — record that `btst`-absolute moves from `cfunc_` to native | Task 5 |
| `src/devices/cpu/m68000/m68000.h` | Declare the new scratch field `m_drc_redo_scratch`, the cfunc `cfunc_take_access_to_be_redone`/`func_take_access_to_be_redone`, and the `generate_bus_step` / `generate_btst_imm8_absolute` emitters | Tasks 2, 3, 4 |
| `src/devices/cpu/m68000/m68000.cpp` | Define the redo-flag cfunc body; init `m_drc_redo_scratch` | Task 2 |
| `src/devices/cpu/m68000/m68000drc.cpp` | The `generate_bus_step()` emitter, the `generate_btst_imm8_absolute()` opcode emitter, the new predicate arm in `generate_native_dispatch` | Tasks 2, 3, 4 |
| `tests/emu/cpu/m68000_drc_coverage.cpp` | Native-coverage assertion — assert `btst`-absolute now dispatches native | Task 5 |

No new `.cpp`/`.h` file is created (all emitters live in the existing
`m68000drc.cpp`), so `make REGENIE=1` is needed only because Task 1 regenerates
the `.ipp` and Tasks 2–4 change the generated-table consumer — run it once per
build to be safe (it is cheap when scripts are unchanged).

---

## Task 1: Generator extension — emit the per-opcode bus-step descriptor (additive)

**Goal:** Single-source the bus-step list (OQ-2) from `m68000gen.py` so the DRC
takes the cycle `−N` and the redo/completed substate pair from the same
microcode truth the interpreter uses. Additive-only: existing generated files
stay byte-identical.

**Files:**
- Modify: `src/devices/cpu/m68000/m68000gen.py` (the `drcdesc` sub-command region, `generate_drcdesc_file` ~`:2448-2576`, and the helpers near `:2385`)
- Regenerate: `src/devices/cpu/m68000/m68000-drcdesc.ipp`
- Validate against: `src/devices/cpu/m68000/m68000-sdf.cpp` (the handler source the descriptors must match)

**Interfaces:**
- Produces (consumed by Tasks 3–4): a generated `static inline const drc_bus_step s_drc_bus_step_table[]` and a way to find a given opcode's step run. Exact emitted C++ shape:

```cpp
// emitted into m68000-drcdesc.ipp, AFTER the existing s_drc_desc_table[]
enum drc_bus_kind : u8 {
    DRC_BUS_PREFETCH_READ = 0, // m_opcodes.read_interruptible, SSW_PROGRAM
    DRC_BUS_DATA_READ     = 1, // m_program.read_interruptible, SSW_DATA, byte-lane
    DRC_BUS_DATA_WRITE    = 2  // (reserved for O-mem-2+; not emitted for btst-absolute)
};

struct drc_bus_step {
    u8  kind;             // drc_bus_kind
    u8  size;             // drc_size (B/W/L of the access)
    u8  charge;           // cycles charged at this step (the interpreter's -N; 4 here)
    u8  redo_substate;    // m_inst_substate set on redo (refund + replay)
    u8  completed_substate;// m_inst_substate set when the read completed
    u8  has_addr_error;   // 1 if the interpreter has the m_aob&1 -> S_ADDRESS_ERROR branch here
    u8  byte_lane;        // 1 if a byte-lane data read (mask + high-byte-when-even select)
};

// one contiguous run per supported opcode, indexed by {value,mask} the same way
// s_drc_desc_table is. For O-mem-1 only the btst-absolute rows are populated;
// every other opcode emits an empty run (count 0) so the table stays complete.
struct drc_bus_run { u16 value; u16 mask; u16 first; u16 count; };
static inline const drc_bus_step s_drc_bus_step_table[] = { /* ... */ };
static inline const drc_bus_run  s_drc_bus_run_table[]  = { /* ... */ };
```

- [ ] **Step 1: Verify the current generator round-trip is additive-clean (baseline).** Regenerate all outputs and confirm an empty diff, so any churn later is attributable to this task.

Run (from `src/devices/cpu/m68000/`):
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
Expected: empty (no files changed). If `m68000-drcdesc.ipp` or any handler file changes, the working tree is stale or Python differs — resolve before proceeding.

- [ ] **Step 2: Pin the btst-absolute bus-step truth from the handler source.** Read `m68000-sdf.cpp:20110-20216` (`btst_imm8_adr16_df`) and `:20218-20346` (`btst_imm8_adr32_df`). Record, per read, the `m_icount -= N` value, the two `m_inst_substate = X` numbers (redo first, completed second), whether an `if(m_aob & 1)` branch follows, and whether it is the byte-lane data read (`m_program.read_interruptible(..., mask)`). This is the table reproduced in the plan header. The generator must emit *these exact numbers* — do not invent them.

- [ ] **Step 3: Add the bus-step model to the generator.** In `m68000gen.py`, in the region that already classifies opcodes for `drcdesc` (`:2385-2576`), add a function that, for the btst-absolute family only (mnemonic `btst`, `src_ea == DRC_EA_IMM`, `dst_ea in (DRC_EA_ABSW, DRC_EA_ABSL)`), returns the ordered bus-step list. Derive the steps and substate numbers from the microcode state list the same way the handler emitter does (the generator already walks the microcode to emit `m68000-sdf.cpp`; reuse that walk to count `-icount` charges and substate transitions rather than re-deriving). For every other opcode return an empty list.

```python
# m68000gen.py -- near the other drc_* helpers (~:2385)
# DRC bus-step kinds (mirror the .ipp enum).
DRC_BUS_PREFETCH_READ = 0
DRC_BUS_DATA_READ     = 1
DRC_BUS_DATA_WRITE    = 2

def drc_bus_steps(ii):
    """Ordered list of (kind, size, charge, redo_substate, completed_substate,
    has_addr_error, byte_lane) for the opcodes O-mem-1 emits natively, derived
    from the SAME microcode walk that generates the interpreter handler so the
    -N charge and the substate numbers cannot drift.  Empty for everything else
    in O-mem-1 (later batches widen this; additive)."""
    base = drc_base_mnemonic(ii[2][0])
    src_ea = drc_ea_mode[ii[2][1]]
    dst_ea = drc_ea_mode[ii[2][2]]
    if base != 'btst' or src_ea != DRC_EA_IMM or dst_ea not in (DRC_EA_ABSW, DRC_EA_ABSL):
        return []
    steps = []
    # Walk the opcode's microcode state list, accumulating the per-read charge
    # and substate transitions exactly as the handler emitter does.  See the
    # handler-walk routine already used for the 's*f'/'s*p' commands; reuse its
    # per-read bookkeeping (the read_interruptible site, the m_icount-=N before
    # it, the m_inst_substate it sets in each arm, and whether an m_aob&1 branch
    # follows).  The btst-absolute family yields 4 reads (.W) / 5 reads (.L);
    # the DATA read is the byte-lane one (kind DATA_READ, byte_lane=1, no
    # addr-error); all others are PREFETCH_READ (addr-error present).
    for read in drc_microcode_reads(ii):   # <- the reused per-read walk
        steps.append((
            DRC_BUS_DATA_READ if read.is_data else DRC_BUS_PREFETCH_READ,
            read.size,                       # DRC_SIZE_W for the ext/abs reads; byte data read -> DRC_SIZE_B
            read.charge,                     # the interpreter's -N (4)
            read.redo_substate,
            read.completed_substate,
            0 if read.is_data else 1,        # data read has no m_aob&1 branch
            1 if read.is_data else 0,        # byte-lane select only on the data read
        ))
    return steps
```

> If reusing the handler-emitter's per-read walk proves more invasive than the
> schedule allows (ADR 0007 R-B), the documented fallback is a hand-written
> step table **for the btst-absolute family only**, with a `# TODO(O-mem-2):
> single-source from the microcode walk` marker. That is a temporary deviation,
> never the long-run state (OQ-2 resolved: generator-first).

- [ ] **Step 4: Emit the new tables into `m68000-drcdesc.ipp`.** In `generate_drcdesc_file` (`:2448`), after the existing `s_drc_desc_table[]` closing `};`, emit the `drc_bus_kind` enum, the `drc_bus_step` / `drc_bus_run` structs, and the two tables. Build `s_drc_bus_step_table[]` by concatenating `drc_bus_steps(ii)` for every `ii` in the same iteration order used for `s_drc_desc_table`, and `s_drc_bus_run_table[]` with `{value, mask, first, count}` per opcode (count 0 where the run is empty). Comment each btst row with its mnemonic so it is auditable against the handler.

```python
# generate_drcdesc_file, appended after the s_drc_desc_table emission (~:2576)
print("", file=out)
print("// DRC native bus-step descriptors (O-mem-1: btst-absolute only).", file=out)
print("// Each step's -charge and substate pair are taken from the SAME", file=out)
print("// microcode walk as the interpreter handler -- they cannot drift.", file=out)
print("enum drc_bus_kind : u8 {", file=out)
print("\tDRC_BUS_PREFETCH_READ = 0, // m_opcodes.read_interruptible, SSW_PROGRAM", file=out)
print("\tDRC_BUS_DATA_READ     = 1, // m_program.read_interruptible, SSW_DATA, byte-lane", file=out)
print("\tDRC_BUS_DATA_WRITE    = 2  // reserved for O-mem-2+ (write side); unused here", file=out)
print("};", file=out)
print("", file=out)
print("struct drc_bus_step {", file=out)
print("\tu8 kind; u8 size; u8 charge;", file=out)
print("\tu8 redo_substate; u8 completed_substate;", file=out)
print("\tu8 has_addr_error; u8 byte_lane;", file=out)
print("};", file=out)
print("struct drc_bus_run { u16 value; u16 mask; u16 first; u16 count; };", file=out)
print("", file=out)
# ... iterate instructions in the SAME order as s_drc_desc_table, building the
# flat step table and the per-opcode run table; print both as static inline.
```

- [ ] **Step 5: Regenerate and prove additivity.**

Run (from `src/devices/cpu/m68000/`):
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
Expected: **only** `m68000-drcdesc.ipp` appears in the diff. If any of `m68000-decode.cpp`, `m68000-head.h`, or the four `m68000-s{d,i}{f,p}.cpp` changed, the generator edit was mis-scoped — fix it so the change is purely additive before continuing.

- [ ] **Step 6: Eyeball the emitted btst rows against the handler.** Confirm `m68000-drcdesc.ipp` now has a `drc_bus_run` for `0x0838/0xffff` (btst.b imm8 adr16) with `count==4` and one for `0x0839/0xffff` (adr32) with `count==5`, and that the step substates match the plan-header table (`.W`: 1/2, 3/4, 5/6 (data, byte_lane=1, has_addr_error=0), 7/8; `.L`: 1/2, 3/4, 5/6, 7/8 (data), 9/10). A mismatch here is the single highest-risk bug in the batch (ADR 0007 R-A) — catch it now, not in Leg B.

Run:
```bash
grep -A2 '0x0838, 0xffff' m68000-drcdesc.ipp
grep -A2 '0x0839, 0xffff' m68000-drcdesc.ipp
```
Expected: the two run entries with the counts and substates above.

- [ ] **Step 7: Build + oracle (decode unchanged) + validate.**

Run (from the worktree root):
```bash
MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd "$PWD"; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'
./mametests "[m68000]"
./mame -validate
```
Expected: build clean; `[m68000]` Leg A green (decode is untouched — the `.ipp` is not yet `#include`d by any consumer, so this proves the generator change is inert); `-validate` clean.

- [ ] **Step 8: `srcclean` + commit.**

```bash
git add src/devices/cpu/m68000/m68000gen.py src/devices/cpu/m68000/m68000-drcdesc.ipp
git commit -m "feat(m68000drc): emit per-opcode DRC bus-step descriptors for btst-absolute (additive)"
```

---

## Task 2: The redo-flag cfunc + scratch field (OQ-5: cold-path read-and-clear)

**Goal:** Give the DRC a way to query the interpreter's exact
`access_to_be_redone()` read-and-clear semantics from a cold suspend path,
without touching the shared `cpu_device` core. The flag is `private` to
`cpu_device` and the read **must clear** it (`std::exchange`), so the DRC calls
the public accessor through a tiny cfunc that stores the result into a
DRC-owned scratch byte. This is one `UML_CALLC` on the cold path only.

**Files:**
- Modify: `src/devices/cpu/m68000/m68000.h` (add the scratch field + the two cfunc declarations, beside the existing `cfunc_interpret_quantum` at `:260-261` and `m_isdrc` at `:242`)
- Modify: `src/devices/cpu/m68000/m68000.cpp` (define the cfunc body, beside `cfunc_interpret_quantum` at `:265`; init the scratch field)

**Interfaces:**
- Produces (consumed by Task 3): `u8 m_drc_redo_scratch;` (DRC-owned), `static void cfunc_take_access_to_be_redone(void *param);`, `void func_take_access_to_be_redone();`. After the cfunc runs, `m_drc_redo_scratch` is `1` if the access must be redone (and the flag is now cleared, exactly as the interpreter would have cleared it), else `0`.

- [ ] **Step 1: Declare the scratch field and the cfunc in the header.**

In `src/devices/cpu/m68000/m68000.h`, beside `m_isdrc` (`:242`), add the scratch byte:
```cpp
	u8                         m_drc_redo_scratch; // DRC cold-path landing for access_to_be_redone()
```
And beside `cfunc_interpret_quantum` (`:260-261`), add:
```cpp
	void func_take_access_to_be_redone();       // read-and-clear the redo flag into m_drc_redo_scratch (cold path)
	static void cfunc_take_access_to_be_redone(void *param);
```

- [ ] **Step 2: Define the cfunc body.**

In `src/devices/cpu/m68000/m68000.cpp`, beside `cfunc_interpret_quantum` (`:265`), add:
```cpp
// The DRC suspend checkpoint queries the interpreter's EXACT access_to_be_redone()
// read-and-clear semantics from a COLD path (taken only when m_icount<=0 at a bus
// step -- never on the fully-granted hot path).  The flag is private to cpu_device
// and the read clears it (std::exchange), so we cannot UML_LOAD it; this cfunc
// calls the public accessor and parks the boolean in a DRC-owned scratch byte.
void m68000_device::func_take_access_to_be_redone()
{
	m_drc_redo_scratch = access_to_be_redone() ? 1 : 0;
}

void m68000_device::cfunc_take_access_to_be_redone(void *param)
{
	static_cast<m68000_device *>(param)->func_take_access_to_be_redone();
}
```

- [ ] **Step 3: Initialize the scratch field.** Find the `m68000_device` constructor member-init list (where `m_drc_labelnum(1)` is set, `m68000.cpp:60`) and add `m_drc_redo_scratch(0)` to it (any order consistent with declaration order — place it adjacent to the other DRC fields). Confirm there is no `-Wreorder` warning after building.

- [ ] **Step 4: Build (compile-only check — nothing calls the cfunc yet).**

Run:
```bash
MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd "$PWD"; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'
```
Expected: clean build, no `-Wreorder`/unused warnings. (The cfunc is defined but not yet emitted; this proves the plumbing compiles.)

- [ ] **Step 5: Oracle still green (no behavior change yet).**

Run:
```bash
./mametests "[m68000]"
./mametests "[m68000][drc]"
```
Expected: both green (the new cfunc and field are dormant — Leg B unchanged from boundary M).

- [ ] **Step 6: Commit.**

```bash
git add src/devices/cpu/m68000/m68000.h src/devices/cpu/m68000/m68000.cpp
git commit -m "feat(m68000drc): add cold-path access_to_be_redone() read-and-clear cfunc"
```

---

## Task 3: `generate_bus_step()` — the native bus primitive (read path + checkpoint)

**Goal:** Emit one 68000 bus read step natively in UML — the `UML_READ`, the
`m_icount -= N` charge, the two-way suspend checkpoint (refund-and-replay vs.
keep-and-advance, setting `m_inst_substate` from the descriptor), and the
`m_aob&1 → S_ADDRESS_ERROR` fault branch — reproducing the interpreter's exact
per-bus-cycle shape. This is the reusable mechanism; Task 4 calls it.

**Files:**
- Modify: `src/devices/cpu/m68000/m68000.h` (declare `generate_bus_step`, beside `generate_moveq` at `:258`)
- Modify: `src/devices/cpu/m68000/m68000drc.cpp` (the emitter; `#include "m68000-drcdesc.ipp"` is already pulled in via `m68000.h` class scope — confirm it is visible to the translator)

**Interfaces:**
- Consumes (from Tasks 1–2): `struct drc_bus_step`, `DRC_BUS_PREFETCH_READ`/`DRC_BUS_DATA_READ`, `s_drc_bus_step_table[]`; `m_drc_redo_scratch`, `cfunc_take_access_to_be_redone`.
- Produces (consumed by Task 4): the emitter
  `void generate_bus_step(drcuml_block &block, const drc_bus_step &step, uml::code_label lbl_delegate);`
  Contract: emits the address-setup-already-done assumption (the *caller* sets `m_aob`/`m_pc`/`m_au`/`m_base_ssw` for the step), issues the read into a known UML reg, charges cycles, and on suspend or fault **stores the descriptor's substate / `S_ADDRESS_ERROR` and `UML_JMP`s to `lbl_delegate`** (yield = exit the block; the interpreter resumes — OQ-1). On the non-suspended, non-faulting path it falls through with `m_edb` committed for the caller to consume. Clobbers I0-I6; preserves I7 (the opword).

> **Design decisions baked in (ADR 0007, owner-locked):**
> - **OQ-1 (interpreter resume):** the emitter emits the suspend checkpoint and **yields** (`UML_JMP lbl_delegate`); it does **not** emit a native resume. The boundary-M `m_inst_substate==0` guard guarantees the native path only ever *starts* at substate 0, so a resuming instruction is already routed to the interpreter delegate. Fully native on the dominant fully-granted case.
> - **OQ-4 (inline first):** `generate_bus_step()` is emitted **inline** (straight-line), not factored into a shared `UML_CALLH` subroutine. Factoring is deferred to a later batch once the common sub-shape is stable.
> - **OQ-5 (cfunc redo):** the redo flag is read via `cfunc_take_access_to_be_redone` (Task 2), on the cold suspend path only.

- [ ] **Step 1: Declare the emitter in the header.**

In `src/devices/cpu/m68000/m68000.h`, beside `generate_moveq` (`:258`):
```cpp
	void generate_bus_step(drcuml_block &block, const struct drc_bus_step &step, uml::code_label lbl_delegate); // one native 68000 bus read step (ADR 0007)
```

- [ ] **Step 2: Implement the emitter in `m68000drc.cpp`.** Add it after `generate_moveq` (`:314`). Emit, in order: the read (PROGRAM space for both prefetch and data reads — §1 of ADR 0007; the SSW_PROGRAM/SSW_DATA distinction is a field write the *caller* already did), the byte-lane select for a data read, the `−N` charge, the suspend checkpoint, and the address-error branch where the descriptor has one.

```cpp
//-------------------------------------------------
//  generate_bus_step - emit ONE native 68000 bus
//  read step: UML_READ + the interpreter's exact
//  per-bus-cycle checkpoint (charge, two-way
//  suspend, address-error).  The CALLER has already
//  emitted the step's address / m_base_ssw setup.
//  On suspend or fault this stores the descriptor's
//  substate (or S_ADDRESS_ERROR) and JMPs to
//  lbl_delegate (yield -> interpreter resumes, OQ-1).
//  On the clean path it falls through with the read
//  byte/word committed.  Clobbers I0-I6; preserves I7.
//-------------------------------------------------

void m68000_device::generate_bus_step(drcuml_block &block, const struct drc_bus_step &step, uml::code_label lbl_delegate)
{
	uml::code_label const lbl_not_suspended = m_drc_labelnum++;
	uml::code_label const lbl_completed     = m_drc_labelnum++;
	uml::code_label const lbl_no_fault      = m_drc_labelnum++;

	// address = m_aob & ~1  (the interpreter reads the word at the even address)
	UML_LOAD(block, I1, &m_aob, 0, SIZE_DWORD, SCALE_x1);            // i1 = m_aob
	UML_AND(block, I2, I1, ~u32(1));                                 // i2 = m_aob & ~1

	// THE READ.  Both prefetch and data reads go through SPACE_PROGRAM (the
	// 68000 has one program space; the SSW_PROGRAM/SSW_DATA difference is an
	// architectural field the caller wrote, not a UML space selection).  Read
	// the word; the data read then selects a byte lane.
	UML_READ(block, I0, I2, SIZE_WORD, SPACE_PROGRAM);              // i0 = read word at (m_aob & ~1)

	if(step.byte_lane)
	{
		// m_edb = read; if(!(m_aob & 1)) m_edb >>= 8; then keep the low byte.
		// (m_aob&1 ? low byte : high byte) -- matches the interpreter's lane mask
		// 0x00ff/0xff00 + the ">>8 when even" select.
		uml::code_label const lbl_odd = m_drc_labelnum++;
		uml::code_label const lbl_lane_done = m_drc_labelnum++;
		UML_TEST(block, I1, 1);                                     // m_aob & 1 ?
		UML_JMPc(block, COND_NZ, lbl_odd);                          // odd -> low byte already in place
			UML_SHR(block, I0, I0, 8);                              // even -> high byte to low
		UML_LABEL(block, lbl_odd);
		UML_AND(block, I0, I0, 0xff);                               // keep the selected byte
		UML_LABEL(block, lbl_lane_done);
	}

	// commit the read into m_edb (the interpreter stores read result in m_edb)
	UML_STORE(block, &m_edb, 0, I0, SIZE_DWORD, SCALE_x1);          // m_edb = read

	// m_icount -= charge
	UML_LOAD(block, I3, &m_icount, 0, SIZE_DWORD, SCALE_x1);        // i3 = m_icount
	UML_SUB(block, I3, I3, step.charge);                            // i3 -= N
	UML_STORE(block, &m_icount, 0, I3, SIZE_DWORD, SCALE_x1);       // m_icount = i3

	// --- suspend checkpoint: if(m_icount <= 0) ---
	UML_CMP(block, I3, 0);
	UML_JMPc(block, COND_G, lbl_not_suspended);                    // m_icount > 0 -> no suspend
		// out of budget this bus cycle: read-and-clear the redo flag (cold path)
		UML_CALLC(block, &m68000_device::cfunc_take_access_to_be_redone, this);
		UML_LOAD(block, I4, &m_drc_redo_scratch, 0, SIZE_BYTE, SCALE_x1); // i4 = redo?
		UML_CMP(block, I4, 0);
		UML_JMPc(block, COND_E, lbl_completed);                    // !redo -> read completed
			// redo: refund the charge and replay THIS read on resume
			UML_LOAD(block, I3, &m_icount, 0, SIZE_DWORD, SCALE_x1);
			UML_ADD(block, I3, I3, step.charge);                   // m_icount += N (refund)
			UML_STORE(block, &m_icount, 0, I3, SIZE_DWORD, SCALE_x1);
			UML_MOV(block, I5, step.redo_substate);
			UML_STORE(block, &m_inst_substate, 0, I5, SIZE_WORD, SCALE_x1);
			UML_JMP(block, lbl_delegate);                          // yield -> interpreter resumes at redo substate
		UML_LABEL(block, lbl_completed);
			UML_MOV(block, I5, step.completed_substate);
			UML_STORE(block, &m_inst_substate, 0, I5, SIZE_WORD, SCALE_x1);
			UML_JMP(block, lbl_delegate);                          // yield -> read DID happen, resume after it
	UML_LABEL(block, lbl_not_suspended);

	// --- address-error branch (PROGRAM reads only): if(m_aob & 1) ---
	if(step.has_addr_error)
	{
		UML_TEST(block, I1, 1);                                     // m_aob & 1 ?
		UML_JMPc(block, COND_Z, lbl_no_fault);                     // even -> no fault
			// the interpreter's extra -4 on fault, then transition to S_ADDRESS_ERROR
			UML_LOAD(block, I3, &m_icount, 0, SIZE_DWORD, SCALE_x1);
			UML_SUB(block, I3, I3, 4);
			UML_STORE(block, &m_icount, 0, I3, SIZE_DWORD, SCALE_x1);
			UML_MOV(block, I5, u32(S_ADDRESS_ERROR));
			UML_STORE(block, &m_inst_state, 0, I5, SIZE_WORD, SCALE_x1);
			UML_JMP(block, lbl_delegate);                          // route the group-0 frame to the interpreter
		UML_LABEL(block, lbl_no_fault);
	}

	// clean path: m_edb is committed; the caller continues to the next step
	// (m_irc/m_dbin commit and any ALU are emitted by the opcode emitter).
}
```

> **Verify before building:** `S_ADDRESS_ERROR` and `SPACE_PROGRAM` must be in
> scope in `m68000drc.cpp`. `S_ADDRESS_ERROR` is used by the interpreter
> handlers (defined in the generated head / `m68000.h` scope); `SPACE_PROGRAM`
> is the UML space constant (`cpu/drcuml.h`, already `#include`d). If either is
> not visible, add the include rather than redefining it.

- [ ] **Step 3: Build (compile-only — still no caller).**

Run:
```bash
MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd "$PWD"; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'
```
Expected: clean build. The emitter is defined but unreferenced — if the compiler warns "defined but not used", that is acceptable for this task (Task 4 calls it); if it *errors* on an undefined symbol (`S_ADDRESS_ERROR`, `drc_bus_step`, a UML opcode), fix the include/scope before continuing.

- [ ] **Step 4: Oracle unchanged (emitter dormant).**

Run:
```bash
./mametests "[m68000]"
./mametests "[m68000][drc]"
```
Expected: both green (nothing dispatches to the new emitter yet — Leg B identical to boundary M).

- [ ] **Step 5: Commit.**

```bash
git add src/devices/cpu/m68000/m68000.h src/devices/cpu/m68000/m68000drc.cpp
git commit -m "feat(m68000drc): add generate_bus_step() native bus primitive (read + checkpoint + addr-error)"
```

---

## Task 4: Wire `btst #n,(xxx).W/.L` native via the bus primitive

**Goal:** Emit the full native `btst_imm8_adr16_df` / `btst_imm8_adr32_df`
sequence — the per-step address/SSW setup, each `generate_bus_step()`, and the
`Z = !(data_byte & (1 << (m_dcr & 7)))` computation after the data read — and
add the predicate arm to `generate_native_dispatch`. This makes the profiled
hot opcode native.

**Files:**
- Modify: `src/devices/cpu/m68000/m68000.h` (declare `generate_btst_imm8_absolute`, beside `generate_moveq` / `generate_bus_step`)
- Modify: `src/devices/cpu/m68000/m68000drc.cpp` (`is_native_opcode` predicate at `:199`; the `generate_native_dispatch` arm at `:216`; the new `generate_btst_imm8_absolute` emitter)

**Interfaces:**
- Consumes (from Task 3): `generate_bus_step(block, step, lbl_delegate)`; the bus-step run for the matched opcode looked up in `s_drc_bus_run_table[]` / `s_drc_bus_step_table[]` by `{value,mask}`.
- Produces: `void generate_btst_imm8_absolute(drcuml_block &block, uml::code_label lbl_delegate);` — emits the whole opcode natively, ending either by yielding mid-step (suspend/fault) or, on a fully-granted instruction, by completing the final commit and `UML_JMP(lbl_delegate)` (the interpreter's quantum tail is a no-op once the instruction retired, exactly as boundary M's moveq hands off).

> **Per-step architectural setup the emitter must emit before each `generate_bus_step`** (lifted verbatim from the handler — these are the lines *before* each `read_interruptible`; the bus primitive deliberately does NOT emit them, so they stay opcode-specific and auditable). For `.W` the four reads' setups are: (1) `m_aob=m_au; m_pc=m_au; set_16l(m_dt,m_dbin); m_au+=2; m_base_ssw=SSW_PROGRAM|SSW_R`; (2) `m_aob=m_au; m_pc=m_au; m_dcr=m_dt; m_at=ext32(m_dbin); m_au=ext32(m_dbin); alu_eor8(m_dt,m_dbin); m_base_ssw=SSW_PROGRAM|SSW_R`; (3) the DATA read setup `m_aob=m_at; m_au=m_pc+2; alu_and8(m_dbin,0xffff); m_base_ssw=SSW_DATA|SSW_R`; (4) the final prefetch setup `m_aob=m_au; m_ir=m_irc; m_pc=m_au; m_au+=2; ...; m_base_ssw=SSW_PROGRAM|SSW_R`, plus between reads the commits `m_irc=m_edb; m_dbin=m_edb` and after the data read the Z computation. **`.L` inserts one extra prefetch (the abs-address-low refill) between reads 2 and 3** with setup `m_aob=m_au; set_16h(m_at,m_dbin); m_au+=2; m_base_ssw=SSW_PROGRAM|SSW_R` and the data-read setup uses `merge_16_32`. Read `m68000-sdf.cpp:20110-20346` line-by-line while emitting.

- [ ] **Step 1: Declare the opcode emitter.**

In `src/devices/cpu/m68000/m68000.h`, beside `generate_bus_step`:
```cpp
	void generate_btst_imm8_absolute(drcuml_block &block, uml::code_label lbl_delegate); // native btst #n,(xxx).W/.L (O-mem-1)
```

- [ ] **Step 2: Implement `generate_btst_imm8_absolute`.** In `m68000drc.cpp`, after `generate_bus_step`. Look up the opcode's bus-step run from the generated tables (match `m_ird` against `s_drc_bus_run_table` `{value,mask}` at *emit* time — the opword is known at runtime in I7 but the *step list* is selected at compile time by which arm of the dispatch we are in; `.W` vs `.L` is distinguished by `m_ird` bit 0 of the EA field, so emit a runtime branch on the opword OR — simpler and preferred — emit two arms, one per absolute size, each with its own compile-time step run). Between steps emit the verbatim architectural setup above; after the data read emit the Z computation. Use the descriptor's `redo_substate`/`completed_substate`/`charge`/`has_addr_error`/`byte_lane` for each `generate_bus_step` call — never hard-code them.

```cpp
//-------------------------------------------------
//  generate_btst_imm8_absolute - native UML for
//  btst #n,(xxx).W and btst #n,(xxx).L
//
//  Mirrors m68000_device::btst_imm8_adr16_df (.W, 4
//  reads, substates 1..8) and btst_imm8_adr32_df
//  (.L, 5 reads, substates 1..10) in m68000-sdf.cpp.
//  Each read is a generate_bus_step() (the charge +
//  suspend + addr-error); between reads this emits
//  the opcode-specific architectural setup (m_aob/
//  m_pc/m_au/m_base_ssw/...) and the m_irc/m_dbin
//  commits, and after the data read computes Z.
//  On a fully-granted instruction it runs all reads
//  natively then JMPs to lbl_delegate (the quantum
//  tail is a no-op once retired).  I7 holds m_ird.
//-------------------------------------------------

void m68000_device::generate_btst_imm8_absolute(drcuml_block &block, uml::code_label lbl_delegate)
{
	// Distinguish .W (0x0838) from .L (0x0839): the opword's bit 0 of the EA
	// field selects the absolute size.  Two compile-time arms, each with its own
	// bus-step run from the generator, keeps each straight-line and auditable.
	uml::code_label const lbl_absl = m_drc_labelnum++;
	uml::code_label const lbl_done = m_drc_labelnum++;
	UML_AND(block, I0, I7, 0x0001);                                  // i0 = opword & 1 (0=.W absw, 1=.L absl)
	UML_CMP(block, I0, 0);
	UML_JMPc(block, COND_NE, lbl_absl);

	// ---- .W arm: btst #n,(xxx).W  (value 0x0838, mask 0xffff) ----
	{
		const drc_bus_run &run = find_bus_run(0x0838, 0xffff);      // helper: locate the run in s_drc_bus_run_table
		// read 1 setup: m_aob=m_au; m_pc=m_au; set_16l(m_dt,m_dbin); m_au+=2; m_base_ssw=SSW_PROGRAM|SSW_R
		emit_btst_w_setup_read1(block);                             // verbatim from m68000-sdf.cpp:20113-20118
		generate_bus_step(block, s_drc_bus_step_table[run.first + 0], lbl_delegate);
		emit_btst_commit_irc_dbin(block);                          // m_irc=m_edb; m_dbin=m_edb
		// read 2 setup: ...:20137-20144
		emit_btst_w_setup_read2(block);
		generate_bus_step(block, s_drc_bus_step_table[run.first + 1], lbl_delegate);
		emit_btst_commit_irc_dbin(block);
		// DATA read setup: ...:20163-20167
		emit_btst_w_setup_dataread(block);
		generate_bus_step(block, s_drc_bus_step_table[run.first + 2], lbl_delegate); // byte_lane, no addr-error
		emit_btst_commit_dbin(block);                              // m_dbin=m_edb (data read commits only m_dbin)
		// Z computation: alu_and8(m_dbin, 1<<(m_dcr&7)); sr_z();  ...:20186-20188
		emit_btst_compute_z(block);
		// final prefetch setup + read: ...:20182-20194 (m_aob=m_au; m_ir=m_irc; m_pc=m_au; m_au+=2; m_ird=m_ir; next_state; SSW_PROGRAM)
		emit_btst_setup_final_prefetch(block);
		generate_bus_step(block, s_drc_bus_step_table[run.first + 3], lbl_delegate);
		emit_btst_commit_retire(block);                           // m_irc=m_edb; m_dbin=m_edb; set_ftu_const(); m_inst_state=...; trace
		UML_JMP(block, lbl_done);
	}

	UML_LABEL(block, lbl_absl);
	// ---- .L arm: btst #n,(xxx).L  (value 0x0839, mask 0xffff) ----
	{
		const drc_bus_run &run = find_bus_run(0x0839, 0xffff);
		emit_btst_l_setup_read1(block);                            // ...:20221-20226
		generate_bus_step(block, s_drc_bus_step_table[run.first + 0], lbl_delegate);
		emit_btst_commit_irc_dbin(block);
		emit_btst_l_setup_read2(block);                            // ...:20245-20249 (set_16h(m_at,...))
		generate_bus_step(block, s_drc_bus_step_table[run.first + 1], lbl_delegate);
		emit_btst_commit_dbin(block);                             // .L read2 commits m_dbin only
		emit_btst_l_setup_read3(block);                            // ...:20267-20274 (merge_16_32; set_16l(m_at,...))
		generate_bus_step(block, s_drc_bus_step_table[run.first + 2], lbl_delegate);
		emit_btst_commit_irc_dbin(block);
		emit_btst_l_setup_dataread(block);                         // ...:20293-20297
		generate_bus_step(block, s_drc_bus_step_table[run.first + 3], lbl_delegate); // byte_lane, no addr-error
		emit_btst_commit_dbin(block);
		emit_btst_compute_z(block);
		emit_btst_setup_final_prefetch(block);                     // ...:20312-20323
		generate_bus_step(block, s_drc_bus_step_table[run.first + 4], lbl_delegate);
		emit_btst_commit_retire(block);
		UML_JMP(block, lbl_done);
	}

	UML_LABEL(block, lbl_done);
}
```

> **Implementation note on the `emit_btst_*` helpers and `find_bus_run`:** these
> are small private static helpers in `m68000drc.cpp` (not header-declared
> emitters — they are file-local `static` functions taking `drcuml_block &` and
> emitting a handful of LOAD/STORE/ALU UML ops). Each is a *verbatim*
> transcription of the corresponding handler lines into LOAD/STORE-with-base
> UML (e.g. `set_16l(m_dt, m_dbin)` becomes: load `m_dt`, load `m_dbin`, mask
> the low 16 bits of `m_dt`, OR in `m_dbin & 0xffff`, store `m_dt`). Implement
> each by reading the exact handler line and translating to ABI-safe UML — no
> `mem(&field)`. `find_bus_run` is a trivial linear scan of
> `s_drc_bus_run_table[]` returning the matching entry (the table is tiny). Keep
> these helpers right above `generate_btst_imm8_absolute` so a reviewer reads
> setup-then-use top to bottom. **There is no shortcut here — the byte-lane,
> the `m_dcr & 7` bit select, and the `ext32`/`merge_16_32` address math must
> match the handler exactly or Leg B fails on RAM/flags (ADR 0007 §1, the
> byte-lane warning).**

- [ ] **Step 3: Add the predicate + dispatch arm.** In `is_native_opcode` (`:199`), add the btst-absolute patterns; in `generate_native_dispatch` (`:216`), add the arm after the moveq arm.

In `is_native_opcode`:
```cpp
	// btst #n,(xxx).W : 0x0838  /  btst #n,(xxx).L : 0x0839  (mask 0xfffe matches both)
	if((opword & 0xfffe) == 0x0838)
		return true;
```

In `generate_native_dispatch`, after the moveq block (`:226`, before the closing comment):
```cpp
	// btst #n,(xxx).W/.L: (m_ird & 0xfffe) == 0x0838
	{
		uml::code_label const lbl_not_btst_abs = m_drc_labelnum++;
		UML_AND(block, I0, I7, 0xfffe);                              // i0 = opword & 0xfffe
		UML_CMP(block, I0, 0x0838);
		UML_JMPc(block, COND_NE, lbl_not_btst_abs);                 // not btst-absolute -> next test / delegate
		generate_btst_imm8_absolute(block, lbl_delegate);          // full native opcode (ends in JMP lbl_delegate)
		UML_JMP(block, lbl_delegate);                               // (defensive; generate_btst already JMPs)
		UML_LABEL(block, lbl_not_btst_abs);
	}
```

- [ ] **Step 4: Build.**

Run:
```bash
MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd "$PWD"; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'
```
Expected: clean build.

- [ ] **Step 5: Oracle Leg A + Leg B (x64), local.**

Run:
```bash
./mametests "[m68000]"            # Leg A: interpreter vs corpus (decode/interp unchanged)
./mametests "[m68000][drc]"       # Leg B: interpreter == DRC, register/flag/RAM/CYCLE-exact, x64
```
Expected: both green. Leg B now exercises the native btst-absolute path under the oracle's one-bus-cycle stepping (which suspends at nearly every `−4`), so a single mis-charged cycle, a wrong substate, a wrong byte lane, or a wrong Z fails here immediately. If Leg B fails: use `superpowers:systematic-debugging`; the failure message names the file/case and whether it diverged on registers, flags, RAM, or cycles — map that back to the step. Do **not** weaken the assertion; if a case genuinely cannot be matched, the fallback is to drop btst-absolute from `is_native_opcode` (route back to `cfunc_`) and report — never approximate.

- [ ] **Step 6: Oracle Leg B (C backend), local.**

Run:
```bash
CPUORACLE_M68_DRC_C=1 ./mametests "[m68000][drc]"
```
Expected: green (the same UML, lowered by drcbec instead of drcbex64 — catches backend-divergent emission, ADR 0007 R-C).

- [ ] **Step 7: `srcclean` + commit.**

```bash
git add src/devices/cpu/m68000/m68000.h src/devices/cpu/m68000/m68000drc.cpp
git commit -m "feat(m68000drc): native btst #n,(xxx).W/.L via generate_bus_step (O-mem-1)"
```

---

## Task 5: Native-coverage assertion + cut-line doc + the authoritative Linux gate

**Goal:** Machine-assert that btst-absolute now dispatches native (not
`cfunc_`), record it in the cut-line doc, and — the load-bearing gate — get the
dual-leg oracle GREEN on the **appserver Linux `oracle` CI job**. Local Windows
green is necessary-not-sufficient.

**Files:**
- Modify: `tests/emu/cpu/m68000_drc_coverage.cpp` (the boundary-N coverage assertion — add btst-absolute to the asserted-native set; if this file does not yet exist because boundary N has not landed, add the assertion inline in the Leg-B harness instead and note the dependency)
- Modify: `src/devices/cpu/m68000/README-drc.md` (move `btst (xxx).W/.L` from the "Explicitly NOT native" list `:77` to the "Native opcodes shipped" table `:186`)

**Interfaces:**
- Consumes: `is_native_opcode(u16)` (Task 4) — the coverage test asserts `is_native_opcode(0x0838) && is_native_opcode(0x0839)` are true and that they dispatch native under instrumentation.

- [ ] **Step 1: Add the coverage assertion.** If `tests/emu/cpu/m68000_drc_coverage.cpp` exists (boundary N), add btst-absolute to its asserted-native opcode set:
```cpp
	// O-mem-1: btst #n,(xxx).W/.L are now native (ADR 0007).
	CHECK(m68000_device::is_native_opcode(0x0838)); // btst #n,(xxx).W
	CHECK(m68000_device::is_native_opcode(0x0839)); // btst #n,(xxx).L
```
If the file does not exist yet (boundary N not landed), add the same two `CHECK`s to the existing `[m68000][drc]` Leg-B case in `tests/emu/cpu/cpuoracle.cpp` (after the harness confirms `drc_engaged()`), and note in the PR that the standalone coverage file is a boundary-N deliverable.

- [ ] **Step 2: Update the cut-line doc.** In `src/devices/cpu/m68000/README-drc.md`:
  - In the "Explicitly NOT native in increment 1" list, remove `btst` from the bit-ops enumeration at `:82-83` **only for the absolute-EA forms** — reword to "`btst`/`bchg`/`bclr`/`bset` (except `btst #n,(xxx).W/.L`, native as of O-mem-1)".
  - In the "Absolute modes" bullet (`:77`), note that `btst #n,(xxx).W/.L` is the first native absolute-EA opcode (O-mem-1), the rest of absolute-EA still `cfunc_`.
  - In the "Native opcodes shipped (current)" table (`:186`), add a row:
```markdown
| `btst #n,(xxx).W` / `.L` | O-mem-1 | Full native via `generate_bus_step()` — 4-read (.W) / 5-read (.L) interruptible-read sequence with per-bus-cycle charge, suspend checkpoint, address-error branch; resume after a mid-instruction yield owned by the interpreter (ADR 0007 OQ-1). |
```

- [ ] **Step 3: Local full gate (both legs, both backends).**

Run:
```bash
MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc 'export OS=Windows_NT; cd "$PWD"; mingw32-make REGENIE=1 && mingw32-make TESTS=1 -j32'
./mametests "[m68000]"
./mametests "[m68000][drc]"
CPUORACLE_M68_DRC_C=1 ./mametests "[m68000][drc]"
./mame -validate
```
Expected: all green; `-validate` clean. This is the **necessary** local gate.

- [ ] **Step 4: Push the branch and open the PR; get the appserver Linux `oracle` job GREEN.** This is the **sufficient** gate (ADR 0006 criterion 3, ADR 0007 Testing & validation). The `oracle` CI job runs the dual-leg oracle on Linux/SysV — where boundary M twice failed on `drcbe_x64 offset_from_rbp` while passing on Windows. Confirm the job is the `oracle` job, **not** preflight (preflight is a tiny-build smoke and does not run the corpus). If the Linux `oracle` job is red while local Windows is green, the cause is almost always an ABI-safety regression (a plain `mem(&field)` slipped in) or a backend-divergent emission — audit every new UML operand for LOAD/STORE-with-base, and re-check the C-backend run. Do not merge until the Linux `oracle` job is green.

```bash
git add tests/emu/cpu/m68000_drc_coverage.cpp src/devices/cpu/m68000/README-drc.md
git commit -m "test+docs(m68000drc): assert btst-absolute native; record O-mem-1 in cut-line doc"
git push -u origin <branch>
gh pr create --fill   # PR body: Docs Impact + the gate evidence (Linux oracle job green)
```
Expected (to merge, per the auto-merge policy): the appserver Linux `oracle` job green (Leg A + Leg B, register/flag/RAM/cycle-exact, x64 + C backend), `-validate` clean, code review clean. Merge once all gates are green.

- [ ] **Step 5: Re-run the throughput benchmark (boundary N harness) and report.** Per ADR 0007 Testing & validation, O-mem-1 is where the first real speedup appears (btst-absolute is 43% of aurail cycles). Run the boundary-N `bench-methodology-m68000.md` harness `-drc 0` vs `-drc 1` on the named driver and record the number in the PR. This does not gate merge for O-mem-1 (correctness gates merge), but it is the evidence the mechanism is worth it.

---

## Self-review

**Spec coverage (ADR 0007 → tasks):**
- §1 native memory-access primitive (`UML_READ` against `SPACE_PROGRAM`, ABI-safe LOAD/STORE, byte-lane) → Task 3 (the read + lane) + Task 4 (the per-step setup).
- §2 cycle-exactness (per-bus-cycle `−N` from the generated descriptor) → Task 1 (descriptor) + Task 3 (the charge from `step.charge`).
- §3 interruptible-read / suspend (native read + native checkpoint; OQ-1 interpreter resume; OQ-5 cfunc redo) → Task 2 (cfunc) + Task 3 (checkpoint, yields to delegate).
- §4 address-error branches → Task 3 (`step.has_addr_error` arm, extra `−4`, `S_ADDRESS_ERROR`, yield).
- §5 dual-path integration + `cfunc_` fallback + gate → Task 4 (dispatch arm) + Task 5 (coverage + Linux gate).
- §6 increment strategy (O-mem-1 = mechanism + btst `.W`/`.L`, alone) → the whole plan; out-of-scope opcodes excluded.
- OQ-1…OQ-5 → resolved in ADR 0007 and baked into Tasks 2/3/4 (see the design-decision callouts).
- Definition of done (O-mem-1): btst native (Task 4), Leg-B green on the matrix (Tasks 4–5), coverage assertion includes it (Task 5), benchmark speedup (Task 5 step 5), generator round-trip additive (Task 1 step 5). ✓ all covered.

**Placeholder scan:** No `TBD`/`implement later`/`similar to Task N`. The `emit_btst_*` helpers are described as verbatim transcriptions with the exact source lines to copy (`m68000-sdf.cpp:20110-20346`) and one worked example (`set_16l`), not left abstract — the one place a "copy the handler line" instruction is unavoidable, because the helper *is* a 1:1 transcription and inventing different code would be wrong. This is a deliberate "transcribe these exact lines" instruction, not a placeholder.

**Type consistency:** `generate_bus_step(drcuml_block&, const drc_bus_step&, code_label)`, `m_drc_redo_scratch` (u8), `cfunc_take_access_to_be_redone`, `generate_btst_imm8_absolute(drcuml_block&, code_label)`, `s_drc_bus_step_table` / `s_drc_bus_run_table` / `find_bus_run` — names and signatures match across Tasks 1–5. Substate numbers are read from the descriptor (`step.redo_substate`/`completed_substate`), never hard-coded, matching the single-source rule. `SPACE_PROGRAM` (not `SPACE_OPCODES`) for both read kinds — **valid only behind the `drc_native_mem_ea_allowed()` compile-time gate added by the [ADR 0007 Addendum (2026-06-28)](../adr/0007-m68000-native-memory-ea-suspend-mechanism.md#addendum--resolution-2026-06-28--o-mem-1-pre-merge-review); §1's "one program space" was corrected there.**

**Genuinely-new open questions (design is settled; few expected):**
1. **Bus-step run selection at emit time vs. runtime.** The plan emits two compile-time arms (`.W`/`.L`) selected by a runtime `m_ird & 1` branch, each consuming its own compile-time `find_bus_run` result. An alternative is a single arm that indexes the run table at runtime — rejected as more complex for two opcodes. Not blocking; flagged for the Builder in case the runtime branch on the opword is cleaner in practice.
2. **Whether `m68000-drcdesc.ipp` is already `#include`d in the class scope.** Boundary K created it "nothing includes it yet"; the frontend (boundary L) consumes `s_drc_desc_table`. Task 3 assumes the `.ipp` (now with the bus-step tables) is visible in `m68000drc.cpp`'s translation unit via the class-scope include. If it is included only in `m68000fe.cpp`, Task 3 step 1 must add the include to the `m68000drc.cpp` TU (or move the bus-step tables to a header the translator sees). This is a 5-minute mechanical check the Builder does at Task 3 step 1 — noted so it is not a surprise.

Neither changes the architecture; both are local implementation choices the Builder resolves at the first build.
