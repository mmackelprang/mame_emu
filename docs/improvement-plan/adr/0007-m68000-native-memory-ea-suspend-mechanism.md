# ADR 0007 — m68000 DRC native memory-EA + suspend / cycle / address-error mechanism

> **Status:** Accepted, **amended 2026-06-28** by the
> [Addendum / Resolution](#addendum--resolution-2026-06-28--o-mem-1-pre-merge-review) at the
> foot of this file (a pre-merge review of O-mem-1 found §1's address-space claim inaccurate and
> §5's `cfunc_` boundary unenforced; the addendum corrects §1, makes §5 an enforceable compile-time
> gate, and is the controlling text where it conflicts with §1/§5 below). Original: Accepted (all 5
> open questions resolved by the owner; O-mem-1 planned —
> see [`plan/phase-2-o-mem-1-btst-absolute.md`](../plan/phase-2-o-mem-1-btst-absolute.md)) ·
> **Phase:** P2 · **Owner:** the boundary-M owner (committed to building it)
> **Type:** Addendum to **[0002](0002-m68000-drcuml-port.md)** (it implements 0002 §3's
> "cycle-accuracy strategy" for the *memory-addressing* half of the ISA, which 0002 deferred
> wholesale to `cfunc_`).
> **Depends on:** **[0002](0002-m68000-drcuml-port.md)** (DRC architecture, dual-path, hybrid
> handoff) · **[0006](0006-m68000-oracle-gate-definition.md)** (the oracle gate — Leg B is the
> acceptance test for everything here) · the shipped **boundary M** (single-resident-block
> dispatch + the `moveq` hybrid handoff + the ABI-safe LOAD/STORE state pattern)
> **Depended on by:** Phase-2 **Task 10 / boundary O** (widening native coverage into the
> memory-EA opcodes — `btst`-absolute first, then memory-EA MOVE/ALU). Until this mechanism
> exists, every memory-touching opcode is permanently `cfunc_`, capping native coverage below
> ~44% of dynamic cycles and below the 0002 throughput bar.
> **Spec:** [`docs/improvement-plan/specs/2026-06-24-mame-improvements-design.md`](../specs/2026-06-24-mame-improvements-design.md)
> **Date:** 2026-06-27

> **Why this is its own ADR and not just a boundary-O PR.** The memory-EA path is a *foundation*:
> the same suspend / cycle-charge / address-error state machine is shared by every memory-touching
> opcode (the entire `(xxx).W`/`(xxx).L`/`(An)`/`(An)+`/`-(An)`/`(d16,An)` half of the ISA), and the
> mechanism chosen here constrains many subsequent boundary-O batches. ADR 0002 §3 explicitly routed
> *all* of this to the interpreter `cfunc_` ("the mid-instruction suspend path **routes to the
> interpreter via `cfunc_`** until the oracle proves a DRC fast-path matches"). This ADR is that
> proof-of-mechanism. It decides *one* thing precisely — how the DRC natively issues a 68000 bus
> access while staying cycle-exact under the per-bus-cycle suspend model — so that 2+ later PRs can
> share it.

## TL;DR

The hot path on real 68000 workloads is **memory-addressing**, not register-only ALU. Boundary M
made `moveq` native via a *hybrid handoff* (emit the register-only CASE 0 natively; hand the
interruptible prefetch + cycle charge to the interpreter). That handoff **cannot** carry the
memory-EA opcodes: their hot work *is* the interruptible bus read, and the read sits in the middle
of a multi-substate fault/suspend state machine. The headline case — aurail's `btst #n,(xxx).W`
(43% of cycles) — does **four** interruptible reads (3 prefetch + 1 data), each charging −4, each
with a suspend checkpoint and an `if(m_aob&1) → S_ADDRESS_ERROR` branch, and computes Z only after
the data read. Register-only opcodes top out below 44% of dynamic cycles and cannot reach the 0002
throughput bar.

**Decision.** Add a single, generated, native **bus-access primitive** to the DRC — a reusable UML
emitter, `generate_bus_step()`, that reproduces the interpreter's *exact* per-bus-cycle microcode
shape in UML:

1. emit the address setup + `m_base_ssw` write (architectural pre-read state) natively;
2. emit the access itself with **`UML_READ` / `UML_WRITE` against `SPACE_PROGRAM`** (the m68000's
   only program space), wired automatically through the device's `device_memory_interface` — never
   a re-implemented bus;
3. emit the interpreter's own **post-read checkpoint** in UML — the `m_icount -= N` charge, the
   `m_icount <= 0 && access_to_be_redone()` two-way suspend (refund-and-replay vs. keep-and-advance,
   setting `m_inst_substate`), and the `m_aob & 1 → S_ADDRESS_ERROR` fault branch — so the DRC
   *suspends and faults at the same bus cycle, with the same cycle count, as the interpreter*;
4. take the cycle constant **N and the substate numbers from the generated descriptor table**
   (single-sourced from `m68000gen.py`, the same microcode truth the interpreter uses) — never
   re-estimated.

The crux realization that makes this tractable: **the 68000 "interruptible read" is not a longjmp or
a host exception. It is a cooperative flag** (`cpu_device::m_access_to_be_redone`, queried via the
public `access_to_be_redone()`); `read_interruptible()` always returns a value. So a native UML
sequence *can* issue the read and then replicate the caller-side checkpoint — there is no
non-local control flow to model. The mechanism is "native read + native checkpoint," not "native
read that magically suspends."

**Increment plan.** Land it on **`btst #n,(xxx).W/.L`** first (the profiled hot opcode, and the
simplest memory-EA shape — a read-only data access with the canonical 4-read prefetch/fault
skeleton), gated by Leg B; then reuse the *same* `generate_bus_step()` to widen into memory-EA
`MOVE`/`ALU` (read EA, then for writes the symmetric `UML_WRITE` checkpoint). Each batch is one
boundary-O PR, each gated cycle-for-cycle by oracle Leg B.

## Context (verified against the tree)

### What boundary M shipped, and why it does not generalize

Boundary M (`src/devices/cpu/m68000/m68000drc.cpp`) is a **single resident UML block** that decodes
the live opword `m_ird` at runtime and runs a native fast-path *only* at a genuine instruction-fetch
boundary, gated by three guards (`m_inst_substate==0`; `m_ipc==m_pc-2`;
`m_inst_state==m_decode_table[m_ird]`), else delegating the granted quantum to the interpreter via
`cfunc_interpret_quantum`. The one native opcode, `moveq`, uses a **hybrid handoff**
(`generate_moveq`, `m68000drc.cpp:260-314`): it emits the interpreter's CASE 0 (the `Dn` write, the
`N/Z`-with-`V=C=0` CCR update, the prefetch-pipe pointer advance) natively, sets `m_inst_substate=1`,
and **returns to the interpreter delegate**, which runs CASE 1+2 (the interruptible prefetch, the
`m_icount-=4` charge, the suspend/dispatch). This is cycle-exact by construction because the
interpreter resumes at substate 1 and the −4 is its own.

The handoff works for `moveq` precisely because `moveq`'s *interesting* work (the register/flag
write) is the part **before** the first memory access — CASE 0 is register-only and non-faulting.
For a memory-EA opcode the interesting work is the memory access *itself*, which lives in CASE 1+2.
Handing CASE 1+2 to the interpreter means handing the entire opcode to the interpreter — i.e. it
stays `cfunc_`. The hybrid handoff therefore caps native coverage at the register-only opcodes,
which is exactly the cut line `README-drc.md` documents (it lists `btst` and the absolute/indexed
EA modes as **"Explicitly NOT native in increment 1 (always `cfunc_`)"**, deferred to "a later
increment"). **This ADR defines that later increment's mechanism.**

### The profiled hot path (verify it): `btst #n,(xxx).W`

Profiling puts aurail's hot path at `btst #n,(xxx).W` (**43% of cycles**) + `bne` (28%). The
new-core handler is `m68000_device::btst_imm8_adr16_df` (`src/devices/cpu/m68000/m68000-sdf.cpp:20110`).
Its shape (verified) is **four interruptible reads**:

```
read_interruptible(opcodes, m_aob)  // prefetch the extension word (the absolute addr.W)
  m_icount -= 4; if(<=0){ refund+redo? substate 1 : substate 2; return; }
  if(m_aob & 1){ m_icount -= 4; m_inst_state = S_ADDRESS_ERROR; return; }
read_interruptible(opcodes, m_aob)  // refill prefetch
  m_icount -= 4; if(<=0){ substate 3/4; return; }
  if(m_aob & 1){ ... S_ADDRESS_ERROR ... }
read_interruptible(program, m_aob, byte-lane mask)  // the DATA read at (xxx).W
  m_icount -= 4; if(<=0){ substate 5/6; return; }
  // (no address-error branch here in this handler -- byte read, even/odd both legal)
  alu_and8(m_dbin, 1 << (m_dcr & 7)); sr_z();   // compute Z from the read byte
read_interruptible(opcodes, m_aob)  // final prefetch
  m_icount -= 4; if(<=0){ substate 7/8; return; }
  if(m_aob & 1){ ... S_ADDRESS_ERROR ... }
  m_inst_state = m_next_state ? m_next_state : m_decode_table[m_ird];
```

Every load-bearing memory-EA capability is in this one handler: an **interruptible operand read**
(`m_program.read_interruptible` / `m_opcodes.read_interruptible`), **per-read cycle charges** of −4,
**per-substate suspend checkpoints**, **address-error fault branches** per prefetch substate, the
**byte-lane select** (`read_interruptible(m_aob & ~1, m_aob&1 ? 0x00ff : 0xff00)` then `m_edb >>= 8`
when even), and the **Z computed only after the data read**. `can_fault = 1`. If the DRC can emit
this handler natively and stay Leg-B green, it can emit the rest of the memory-EA ISA.

### The interruptible-read mechanism is a flag, not a longjmp (the key fact)

`m_program` / `m_opcodes` are `memory_access<24, 1, 0, ENDIANNESS_BIG>::specific`
(`m68000.h:178-181`). `read_interruptible(address, mask)` (`src/emu/emumem.h:1809`) is an ordinary
value-returning call. The "interruptible" part is **out of band**: `cpu_device` carries a private
`bool m_access_to_be_redone` (`src/emu/devcpu.h:65`), set by `defer_access()` / `access_before_delay()`
/ `access_before_time()` / `retry_access()` (`src/emu/devcpu.cpp:75-110`) when a tapped handler
(e.g. the VPA/autovector `before_time` taps, `m68000.cpp:326-332`) cannot complete in the remaining
quantum, and read-and-cleared by the public `access_to_be_redone()` (`src/emu/devcpu.h:44`). For a
plain RAM/ROM access with no tap the flag is **never** set and the read completes normally even if
`m_icount` went negative.

**There is no host-level suspension.** The "suspend" is entirely the caller's doing: the generated
microcode tests `m_icount <= 0`, then `access_to_be_redone()`, then sets `m_inst_substate` and
`return`s; the next `execute_run_interpreter()` iteration re-enters the **partial** handler
(`m_handlers_p[m_inst_state]`, a `switch(m_inst_substate)` with `[[fallthrough]]` labels) and resumes
mid-state. This is decisive for the ADR: **a native UML sequence can issue `UML_READ` and then emit
the same checkpoint logic, because the checkpoint is plain data manipulation (an `int` compare, a
`bool` read-and-clear, a `u16` store, an exit) — there is nothing non-local to reproduce.**

### The cycle-charge model differs fundamentally from mips3/ppc (do not copy their idiom blindly)

mips3/ppc charge cycles **per instruction, batched**: `compiler.cycles += desc->cycles`, mirrored to
`MAPVAR_CYCLES`, with a deferred `UML_SUB(icount, icount, MAPVAR_CYCLES)` + `UML_EXHc(COND_S,
out_of_cycles, pc)` at block/branch boundaries (`mips3drc.cpp:1126-1135`). That model assumes an
instruction either completes in the quantum or restarts cleanly from its PC.

The 68000 model is **per-bus-cycle, in-line**: each bus access charges −4 (or −2 for internal
micro-steps) *at the access*, can suspend *between* accesses, and resumes *mid-instruction* at a
substate — not at the PC. The DRC must therefore charge cycles **the way the interpreter does, at
each bus step, with the suspend checkpoint inline** — not in a batched per-instruction `MAPVAR_CYCLES`
sweep. We take the UML *primitives* from mips3/ppc (`UML_READ`/`UML_WRITE`, `UML_EXH`/`UML_GETEXP`,
the auto-resolved `m_space[]` accessor wiring) but **not** their cycle-accounting idiom. (Cycle
constants still come single-sourced from the descriptor table, per 0002 §3.)

### How the gate sees this (Leg B, verified)

Oracle Leg B (`tests/emu/cpu/cpuoracle.cpp:1022`, `"[cpu][m68000][drc]"`) feeds each corpus case's
**inputs** through `-drc 0` and `-drc 1` and `REQUIRE`s register/flag/RAM/**cycle** equality between
the two MAME runs (it never consults the corpus "final" values — corpus-immune). The harness drives
the core **one bus cycle at a time** (`*m_icountptr = 1`, run, sum `1 − icount`) and retires on the
`m_ipc` change (`cpuoracle.cpp:619-622`). So the native path is exercised under exactly the
suspend-at-every-−4 regime — a native `btst`-absolute that miscounts a single bus cycle, or suspends
at the wrong substate, fails Leg B immediately. That is the whole point: the gate makes this
mechanism reviewable.

## Decision — the seven sub-decisions

### 1. Native memory-access primitive

> ⚠ **Corrected by the [Addendum (2026-06-28)](#addendum--resolution-2026-06-28--o-mem-1-pre-merge-review).**
> The claim below that "the 68000 has one program space" is **inaccurate**: when a driver configures
> `AS_OPCODES` (decrypted opcodes, e.g. FD1094) and/or `AS_USER_PROGRAM`/`AS_USER_OPCODES`, the
> interpreter's prefetch reads use `m_opcodes` and data reads use `m_program` — *SR_S-swapped, distinct*
> spaces — so blanket `SPACE_PROGRAM` reads the wrong memory. `SPACE_PROGRAM` for every read is correct
> **only under a flat topology**, which the addendum makes a hard compile-time gate. Read the addendum
> before implementing this section.

Emit native accesses with the existing UML opcodes, against the m68000's single program space:

```cpp
// data / prefetch read of the EA word (address in Ix, result in Iy)
UML_READ (block, Iy, Ix, SIZE_WORD, SPACE_PROGRAM);   // 68000 program space == AS_PROGRAM
// write side (for memory-EA MOVE/ALU destinations)
UML_WRITE(block, Ix, Iv, SIZE_WORD, SPACE_PROGRAM);
```

- **Space.** The 68000 has one logical program space (`AS_PROGRAM`); `m_opcodes`/`m_program` are
  *handles into the same space distinguished by FC*, not separate UML spaces. The DRC uses
  `SPACE_PROGRAM` for both prefetch and data reads. The bus-status difference (`m_base_ssw =
  SSW_PROGRAM` vs `SSW_DATA`) is an architectural field write, emitted natively as part of the step
  (it matters only on a fault), **not** a UML space selection. *(The user/super and MMU `m_uprogram`
  / indirect-handler variants are out of scope — they stay `cfunc_`; see §5.)*
- **Wiring (nothing new to register).** `drcuml_state` was already constructed with `*this`
  (`m68000.cpp:496`); the backend's `drcbe_interface` walks the device's `device_memory_interface`
  and caches `m_space[AS_PROGRAM]`, resolving native specific-accessors at reset
  (`drcuml.cpp:120-132`; `drcbex64.cpp:1091-1099`). `UML_READ(..., SPACE_PROGRAM)` therefore
  dispatches through `space(AS_PROGRAM)` and the device's installed handlers/fastram automatically.
  **No per-handler `set_delegate`, no manual accessor table.**
- **ABI-safety (preserve the boundary-M invariant).** Every device-field access stays
  `UML_LOAD`/`UML_STORE` with a pointer base (never plain `mem(&field)`), exactly as
  `static_generate_entry_point` / `generate_moveq` already do (`m68000drc.cpp:154-313`). Plain
  `mem(&m_field)` is lowered RBP-relative by drcbe_x64 and throws `offset_from_rbp` when the heap
  device is >2 GiB from the high-mmap'd RWX cache on Linux/SysV — which silently fataled the oracle
  there while passing on Windows. `UML_READ`/`UML_WRITE` operate on UML integer registers (the
  address and result), so they are inherently ABI-safe; the field plumbing around them
  (`m_aob`, `m_edb`, `m_icount`, `m_inst_substate`, `m_sr`, …) all goes through LOAD/STORE.
- **Byte-lane convention (do not drop it).** The interpreter's byte data read is
  `m_edb = read_interruptible(m_aob & ~1, m_aob&1 ? 0x00ff : 0xff00)` then `if(!(m_aob&1)) m_edb >>= 8`
  (`m68000-sdf.cpp:20168-20170`). The native emission reproduces this exactly: read the *word* at
  `m_aob & ~1` (via `UML_READM` with the lane mask, or `UML_READ` of the word then mask/shift in UML),
  and select the high or low byte per `m_aob & 1`. Getting the lane wrong is a Leg-B RAM/flag
  failure, not a silent error.

### 2. Cycle-exactness — the crux

**Cycles come from the same microcode truth the interpreter uses, charged at the same bus cycles, and
never re-estimated.** Concretely:

- The descriptor table (`m68000-drcdesc.ipp`, generated by `m68000gen.py`, Task 2) is extended to
  carry, per native memory-EA opcode, the **ordered list of bus steps** — for each step: the access
  kind (prefetch-read / data-read / data-write), the size, the cycle charge (the `−N` the interpreter
  applies: 4 for a normal bus cycle, the documented internal `−2`/`−4` micro-steps where they occur),
  and the **two substate numbers** (the *redo* substate and the *completed* substate) that the
  interpreter's generated handler uses at that step. These are not invented — they are emitted from
  the *same* python microcode model that generates `btst_imm8_adr16_df`, so the DRC's `−4` and its
  `m_inst_substate=1/2/3/…` are byte-identical to the interpreter's by construction. *(Single-source
  rule, 0002 §3: the generator emits both the interpreter handler and the DRC step list; they cannot
  drift because they are one source.)*
- `generate_bus_step(block, step_desc)` emits, for one bus step, the exact interpreter sequence in
  UML order:
  1. native address/`m_base_ssw` setup (the pre-read architectural writes for this step);
  2. `UML_READ`/`UML_WRITE` (§1);
  3. `UML_SUB(m_icount, m_icount, N)` via LOAD/STORE on `m_icount` (an `int`);
  4. the **suspend checkpoint** (§3);
  5. the **address-error branch** (§4), where the interpreter has one;
  6. the native commit (`m_dbin = m_edb`, prefetch-pipe advance, etc.).
- Because each step charges `−N` *inline at the access* and the suspend checkpoint is emitted between
  steps, the DRC consumes cycles **bus-cycle-for-bus-cycle** as the interpreter does — including the
  case where a single one-cycle grant lands the core mid-instruction. The summed icount across grants
  equals the interpreter's, which is the Leg-B cycle assertion.
- **We never use the mips3 `MAPVAR_CYCLES` batched idiom** (see Context). The 68000 cannot defer the
  charge to a block boundary because it suspends *between* the charges.

**The cycle-exactness argument (why this is provably exact, not "close").** The DRC's bus-step list
is generated from the same microcode list as the interpreter handler; therefore (a) the *number* of
bus steps, (b) the `−N` charged at each, and (c) the substate set at each suspend are identical token-
for-token. The only freedom is the *encoding* (UML vs C++), and the encoding manipulates the same
fields (`m_icount`, `m_inst_substate`, `m_aob`, `m_edb`, `m_inst_state`) to the same values. Leg B
then checks the *result* (register/flag/RAM/cycle) across 317,500 cases including the per-bus-cycle
single-step regime. Any divergence is a translation bug, caught by the gate, and the fallback is
always available (route that opcode back to `cfunc_`). Cycle-exactness is thus *enforced by
construction-plus-gate*, identical in spirit to the boundary-M `moveq` argument, extended across the
memory access.

### 3. Interruptible-read / mid-instruction suspend

**Chosen: native read + native checkpoint (full native step), NOT checkpoint-then-handoff.** Because
the suspend is a flag (Context), the DRC emits the interpreter's post-access checkpoint directly:

```
// after UML_READ into Iedb and UML_SUB(m_icount, m_icount, N):
UML_LOAD (block, Ic, &m_icount, 0, SIZE_DWORD, SCALE_x1);
UML_CMP  (block, Ic, 0);
UML_JMPc (block, COND_G, lbl_not_suspended);          // m_icount > 0 -> no suspend
    // out of budget this bus cycle: query the redo flag via the PUBLIC accessor
    UML_CALLC(block, &cfunc_take_access_to_be_redone, this);  // returns 0/1 in a scratch field
    UML_LOAD (block, Ir, &m_drc_redo_scratch, 0, SIZE_BYTE, SCALE_x1);
    UML_CMP  (block, Ir, 0);
    UML_JMPc (block, COND_E, lbl_completed);          // !redo -> read completed, advance substate
        // redo: refund the charge and replay THIS read on resume
        UML_LOAD (block, Ic, &m_icount, 0, SIZE_DWORD, SCALE_x1);
        UML_ADD  (block, Ic, Ic, N);
        UML_STORE(block, &m_icount, 0, Ic, SIZE_DWORD, SCALE_x1);
        UML_MOV  (block, Is, <redo_substate>);
        UML_STORE(block, &m_inst_substate, 0, Is, SIZE_WORD, SCALE_x1);
        UML_JMP  (block, lbl_delegate_return);        // yield: exit to dispatcher, interpreter resumes
    UML_LABEL(block, lbl_completed);
        UML_MOV  (block, Is, <completed_substate>);
        UML_STORE(block, &m_inst_substate, 0, Is, SIZE_WORD, SCALE_x1);
        UML_JMP  (block, lbl_delegate_return);        // yield: read DID happen, resume after it
UML_LABEL(block, lbl_not_suspended);
    // commit m_edb, continue to next step
```

Two precise points:

- **Reading `m_access_to_be_redone`.** The flag is a `private` member of `cpu_device`; its address is
  not cleanly exposed and (more importantly) the read **must clear** it (`std::exchange`, `devcpu.h:44`).
  The DRC therefore does **not** `UML_LOAD` the private field — it calls the public
  `access_to_be_redone()` through a tiny `cfunc_` (`cfunc_take_access_to_be_redone`) that stores the
  result into a new DRC-owned scratch byte (`m_drc_redo_scratch`). This is one `UML_CALLC` on the
  *cold* suspend path only (taken once per quantum boundary, not per instruction), so it costs nothing
  on the hot path. *(Alternative: add a public, non-clearing `access_to_be_redone_noclear()` exists
  already (`devcpu.h:45`); but the interpreter semantics read-and-clear, so the DRC must clear too —
  the `cfunc_` route preserves the exact semantics and is the safe choice.)*
- **Yield = return to the dispatcher.** "Suspend" in UML is just storing the substate and exiting the
  block back to the scheduler (the resident-block dispatcher exits `EXECUTE_OUT_OF_CYCLES`). On the
  next grant, the **interpreter** resumes at the stored substate via its partial handler — the DRC
  does **not** need to emit the resume path. This is the one place the design still leans on the
  interpreter, and it is safe: the partial-handler resume is the interpreter's own CASE-N code,
  identical to what runs under `-drc 0`. The DRC owns the *first* pass through each bus step (the hot
  case: a fully-granted instruction completes in native UML in one go); the interpreter owns *resumes
  after a mid-instruction yield* (the rare case at a quantum boundary). Both produce identical state
  because they set the identical substate.

  > **Design note / open question OQ-1:** in the common case the harness grants the *whole*
  > instruction's cycles (a real driver grants a quantum of hundreds of cycles), so the native path
  > runs all four reads without ever suspending — the suspend arms are taken only when a grant
  > boundary falls mid-instruction. Under the *oracle's* one-cycle-at-a-time stepping, however, the
  > core suspends at almost every bus step. We must decide whether the DRC native step also handles
  > *resume* (re-entering the native step at the redo/completed substate) or whether, after the first
  > yield, the rest of that instruction is finished by the interpreter (the guard
  > `m_inst_substate==0` in `static_generate_entry_point` already routes a *resuming* instruction to
  > the interpreter delegate — so today, post-yield, the interpreter finishes it). The latter is
  > simpler and **already Leg-B-correct** (the boundary-M guards guarantee the native path only
  > starts at substate 0). Recommendation: **ship the simpler "native first pass, interpreter
  > resumes" model** — the native path emits the suspend checkpoint and yields; it does *not* emit
  > the resume. This keeps the native emitter to a single straight-line pass and still makes the
  > hot, fully-granted case 100% native. Confirm with the owner (see Open Questions).

### 4. Address-error fault branches

At each prefetch step where the interpreter has `if(m_aob & 1){ m_icount -= 4; m_inst_state =
S_ADDRESS_ERROR; return; }`, the DRC emits the identical branch natively:

```
UML_LOAD (block, Ia, &m_aob, 0, SIZE_DWORD, SCALE_x1);
UML_TEST (block, Ia, 1);
UML_JMPc (block, COND_Z, lbl_no_fault);               // even address -> no fault
    UML_LOAD (block, Ic, &m_icount, 0, SIZE_DWORD, SCALE_x1);
    UML_SUB  (block, Ic, Ic, 4);                       // the interpreter's extra -4 on fault
    UML_STORE(block, &m_icount, 0, Ic, SIZE_DWORD, SCALE_x1);
    UML_MOV  (block, Is, S_ADDRESS_ERROR);
    UML_STORE(block, &m_inst_state, 0, Is, SIZE_WORD, SCALE_x1);
    UML_JMP  (block, lbl_delegate_return);            // route the group-0 frame to the interpreter
UML_LABEL(block, lbl_no_fault);
```

- The DRC sets `m_inst_state = S_ADDRESS_ERROR` (and charges the interpreter's extra `−4`) **exactly**
  as the handler does, then **routes the group-0 frame itself to the interpreter** by exiting the
  block. The address-error *handler* (`state_address_error_df`, `m68000-sdf.cpp:454`) — which builds
  and pushes the 14-byte group-0 stack frame via its own interruptible writes, forces supervisor,
  clears trace, and vectors — stays `cfunc_` (it is a fault path, exactly the kind 0002 §3 and the
  `README-drc.md` "hard `cfunc_` boundaries" keep interpreted). The DRC's job is only to *detect the
  fault and transition the state* identically; the interpreter runs the frame.
- This matches ADR 0006's allowlist construction: address-error cases are case-level cycle-exempt in
  Leg A (`m68000_cycle_exempt`, `cpuoracle.cpp:651`), and in Leg B the DRC `cfunc_`s the frame, so
  interpreter ≡ DRC holds there by construction — the allowlist "never punches a hole in the DRC
  safety net" (0006). The native path only has to reach `S_ADDRESS_ERROR` with the right state and the
  right cycle charge, which the gate verifies on the non-exempt portion (the detection cycles) and the
  state equality.
- **Bus error (`PR_BERR`) and double fault** (`abort_access` / `do_post_run`, `m68000.cpp:124-143`;
  `S_DOUBLE_FAULT`) likewise stay interpreter-owned — the DRC never emits them, it only ever *enters*
  `S_ADDRESS_ERROR` from the even/odd test and otherwise yields.

### 5. Integration with the dual-path, the `cfunc_` fallback, and the gate

> ⚠ **Made enforceable by the [Addendum (2026-06-28)](#addendum--resolution-2026-06-28--o-mem-1-pre-merge-review).**
> The "stays `cfunc_`" intent below was stated but **not enforced** — the as-built O-mem-1 dispatch had
> no space/MMU gate, so it emitted native `btst`-absolute even when `AS_OPCODES`/user spaces were
> configured (a silent regression vs the prior all-`cfunc_` behaviour). The addendum defines the exact
> compile-time predicate `drc_native_mem_ea_allowed()` that every native memory-EA arm must be guarded
> by, and resolves that `SR_S` is **not** a runtime branch. Read the addendum before implementing this
> section.

- **`m_isdrc` dual-path unchanged.** This mechanism lives entirely inside the resident block's native
  dispatch (`generate_native_dispatch`, `m68000drc.cpp:216`); `execute_run()` / `execute_run_drc()`
  and the boundary-M three-guard entry are untouched. A native memory-EA opcode is added the same way
  `moveq` was: a predicate arm in `generate_native_dispatch` that, on match, calls the opcode's native
  emitter and then either completes (fully-granted) or yields at a suspend/fault checkpoint.
- **What stays `cfunc_` forever (or until much later, separate ADRs):**
  - **MMU / indirect handlers** — when `m_mmu` is set the core uses `m_handlers_if/ip` and the
    `m_uprogram`/translate path; out of scope, `cfunc_`.
  - **User/supervisor space switches, privileged operand forms** (`sr` whole, `usp`, `S`/`T`, `stop`,
    `reset`, `rte`/`rtr`) — `cfunc_` (`README-drc.md` hard boundaries).
  - **The fault *handlers* themselves** — address-error frame, bus-error, double-fault, illegal,
    privilege, TRAP/TRAPV/CHK/divide-by-zero — `cfunc_`. The DRC detects and transitions; it does not
    build frames.
  - **Indexed `(d8,An,Xn)` / `(d8,PC,Xn)`, `movem`, `movep`, `tas` RMW, BCD** — deferred timing-subtle
    EA/RMW shapes; later increments or permanent `cfunc_`.
  - **FPU** — out of scope (not on plain 68000).
  - **The mid-instruction *resume*** (post-yield partial handler) — owned by the interpreter (§3,
    OQ-1).
- **The gate.** Every PR built on this mechanism merges only on **ADR 0006 acceptance criterion 3
  (Leg B)**: `./mametests "[m68000][drc]"` green, register/flag/RAM/cycle-exact between `-drc 0` and
  `-drc 1`, on the backend matrix (`drcbex64`; `drcbec` via `CPUORACLE_M68_DRC_C=1`; `drcbearm64`
  where available). Leg A stays green (interpreter byte-unchanged). `mametiny -validate` clean. The
  native-coverage assertion (Task 6) is updated to include each newly-native opcode; the throughput
  benchmark (Task 7) is re-run.

### 6. Increment strategy (how Planner should batch it)

Ordered, each a single boundary-O PR, each gated by Leg B:

| Batch | Opcodes | What the batch proves |
|---|---|---|
| **O-mem-1** | `generate_bus_step()` infra + **`btst #n,(xxx).W` and `btst #n,(xxx).L`** (data-read EA, the profiled hot path) | The whole mechanism: native interruptible read, per-bus-cycle charge, suspend checkpoint, address-error branch, Z-after-read. **This is the load-bearing PR** — it unlocks everything below. Pairs with native `bne` (28%) if not already native, for the full aurail hot loop. |
| **O-mem-2** | `btst`/`bchg`/`bclr`/`bset` with **`(An)` / `(An)+` / `-(An)`** data EAs (read, and for bchg/bclr/bset the symmetric **`UML_WRITE`** + write checkpoint) | The write side of `generate_bus_step()` (the RMW data path) and the auto-inc/dec EA arithmetic, reusing O-mem-1's read step. |
| **O-mem-3** | **memory-EA `MOVE` / `MOVEA`** (`.b`/`.w`/`.l`) with `(An)`/`(An)+`/`-(An)`/`(d16,An)` source and dest | The general read-EA → write-EA path; the broadest cycle win after `btst`. Largest single coverage jump. |
| **O-mem-4** | **memory-EA `ALU`** (`add`/`sub`/`and`/`or`/`eor`/`cmp` and the `addi`/… immediate forms) with the same EAs | Reuses the read step + the existing register-only ALU emitters; mostly EA plumbing. |
| **O-mem-5+** | `(d16,PC)` source, `addq`/`subq`/`Scc`/`clr`/`tst`/`neg`/`not` memory-EA forms | Long tail; each reuses the infra. |

**Sequencing rule for Planner:** O-mem-1 must land first and alone (it is the mechanism PR; review it
in isolation against Leg B, on the full backend matrix, before any reuse). O-mem-2…5 are independent
of each other given O-mem-1 and may be ordered by profiled cycle weight. Each batch: extend the
generator's bus-step descriptor coverage (if a new step shape appears), add the predicate arm in
`generate_native_dispatch`, add the emitter, regenerate (empty diff on existing generated files —
additive-only, per Task 2 / R3), run Leg B + native-coverage + benchmark. Indexed/absolute-`.L`
edge timings that don't match go straight back to `cfunc_` (never approximated).

### 7. Effort / risk estimate and alternatives

**Effort (honest).**
- **O-mem-1 (the mechanism + `btst`-absolute):** the bulk of the work is the `generate_bus_step()`
  emitter and the generator extension to emit the per-step descriptor (cycle `−N`, substate pair,
  access kind/size/lane). Estimate **2–4 focused weeks** for a first Leg-B-green `btst #n,(xxx).W/.L`
  on x64 + C backends, dominated by (a) getting the byte-lane + Z-after-read exactly right, (b) the
  suspend-checkpoint substate bookkeeping matching the interpreter under one-cycle stepping, and (c)
  the generator round-trip staying additive (empty diff). High uncertainty on the generator side
  until the descriptor shape is pinned (OQ-2).
- **O-mem-2…4 (widening):** **1–2 weeks each** once O-mem-1's infra is proven — mostly EA-arithmetic
  and the write checkpoint, reusing the read step.
- This is consistent with 0002's "multi-month effort" honesty note; this ADR scopes the *memory-EA
  foundation*, not the whole tail.

**Risk.**
- **R-A — substate mismatch under single-cycle stepping (highest).** The oracle suspends at nearly
  every −4; an off-by-one substate or a missed refund fails Leg B. Mitigated by single-sourcing the
  substate numbers from the generator (not hand-transcribed) and by the gate catching it immediately.
- **R-B — generator descriptor shape.** Emitting a faithful per-step list from the python microcode
  model may be more invasive than the boundary-K descriptor extension. Mitigated by the additive-only
  proof (empty diff on existing files) and by being able to fall back to a hand-written step table for
  *just the btst-absolute family* in O-mem-1 if the generator extension slips (documented as a
  temporary, single-sourced-later deviation — but the long-run rule remains generator-single-sourced).
- **R-C — backend divergence.** A UML step correct on x64 may differ on C/arm64. Mitigated by the
  Task-8 matrix on every batch (boundary M already deferred arm64 to CI; same here).
- **R-D — the `access_to_be_redone()` cfunc on the suspend path.** A `UML_CALLC` mid-block must
  preserve UML register state correctly; mitigated by it being on the cold path only and by following
  the existing `cfunc_interpret_quantum` calling convention (`m68000drc.cpp:185`).
- **R-E — fastram/handler timing.** `UML_READ` against `SPACE_PROGRAM` hits whatever handler/fastram
  is installed; a tapped handler (VPA/autovector) sets the redo flag mid-DRC-read. Mitigated because
  the native checkpoint *handles* the redo flag (§3) — the same way the interpreter does — and the
  fault/tapped opcodes are `cfunc_` anyway.

**Alternatives considered.**

1. **Extend the boundary-M hybrid handoff to memory-EA opcodes (checkpoint-then-handoff for the
   read).** *Rejected as the primary mechanism.* For a memory-EA opcode the read *is* the work, so
   handing the read to the interpreter hands the whole opcode to the interpreter — it is just `cfunc_`
   with extra steps and yields **zero** native cycles on the hot path. (It remains the correct model
   for register-only opcodes, which boundary M already covers.)
2. **A generic UML "interruptible-read helper" subroutine** (one shared `UML_CALLH` handle that does
   read + charge + checkpoint, parameterized by size/space/cycle). *Considered, deferred.* Attractive
   for code size, but the per-opcode step *shapes* differ (which architectural fields are written
   before/after each read, where the address-error branch sits, the byte-lane vs word, the
   Z-after-read), so a single helper accretes parameters until it is harder to keep cycle-exact than
   straight-line emission. Recommendation: **start with inline `generate_bus_step()` emission**
   (straight-line, trivially auditable against the interpreter handler), and *factor* a shared helper
   only after 2–3 opcodes reveal the genuinely common sub-shape. (This mirrors mips3/ppc, which
   factor `static_generate_memory_accessor` subroutines *after* the shape stabilized.)
3. **`cfunc_`-the-read / native-the-rest hybrid** (interpreter issues the bus access via a cfunc;
   native UML does the surrounding ALU/flags). *Rejected.* The bus access dominates the cycle cost of
   these opcodes; cfunc-ing it leaves the hot work interpreted and adds a C-call per read on the hot
   path — slower than today's full `cfunc_` and pointless. (It also re-introduces the very
   per-read C dispatch the DRC exists to remove.)
4. **Re-implement a private DRC bus path** (skip `UML_READ`, read RAM directly from a cached fastram
   pointer in UML). *Rejected.* Throws away the auto-resolved, handler-correct, save-state-safe
   `m_space[]` accessor wiring; would silently bypass installed handlers/watchpoints and diverge from
   the interpreter on anything but plain RAM. `UML_READ`/`UML_WRITE` already give the fastram fast
   path *and* handler correctness.
5. **Adopt the mips3 `MAPVAR_CYCLES` batched cycle model.** *Rejected* (see Context §cycle-charge):
   the 68000 suspends *between* charges, so cycles cannot be deferred to a block boundary without
   losing the mid-instruction suspend semantics. The per-bus-step inline charge is mandatory.

## Consequences

**Good**
- Unlocks the **memory-addressing half of the ISA** for native emission — the dominant share of real
  68000 cycles (aurail `btst`-absolute alone is 43%). This is the capability that lets the DRC clear
  the 0002 throughput bar; register-only opcodes structurally cannot.
- The mechanism is a **single reusable primitive** (`generate_bus_step()` + the generated step
  descriptor); every later memory-EA batch reuses it, so coverage widens with shrinking marginal
  effort.
- **Cycle-exact by construction-plus-gate**: cycles and substates are single-sourced from the same
  microcode model as the interpreter, and Leg B verifies the result per bus cycle. No new accuracy
  risk that the gate doesn't catch.
- Preserves every boundary-M invariant: ABI-safe LOAD/STORE state access, single resident block, the
  three fetch-boundary guards, interpreter-owned resume and fault frames.

**Bad / cost**
- New, intricate UML emission (the suspend/fault checkpoint) that must match the interpreter
  *per bus cycle*; the highest-precision code in the port. Bugs are caught by Leg B but the
  authoring/debug loop is exacting.
- A generator extension (per-step descriptor) touches `m68000gen.py` and the committed generated
  table again; the additive-only discipline (empty diff on existing files) must hold (R-B/R3).
- One `UML_CALLC` on the suspend path (the `access_to_be_redone()` read-and-clear); cold-path only,
  but a place where UML calling-convention correctness matters.
- The "native first pass, interpreter resumes" model (OQ-1) means the *resume* after a mid-instruction
  yield is still interpreted — fine for accuracy (it's the interpreter's own code) and for the
  fully-granted hot case, but it means a workload that constantly yields mid-instruction (only the
  oracle's pathological one-cycle stepping) sees less native benefit. Real drivers grant large quanta,
  so this is a non-issue for throughput; flagged for completeness.

## Accuracy & determinism preservation

- **Interpreter remains the oracle.** `-drc 0` runs the unchanged microcode core; the DRC must match
  it register/flag/RAM/cycle-exact (Leg B). Cycles and substates are taken from the interpreter's own
  generated microcode, never re-estimated. Any unmatched case routes to `cfunc_`. We never modify the
  interpreter to suit the DRC.
- **Determinism preserved.** Native reads/writes go through the same single-threaded `space(AS_PROGRAM)`
  accessors, the same `m_icount` accounting, and the same scheduler timeline as the interpreter; the
  block cache is not part of save state. No work moves off the timeline-feeding thread (the spec's hard
  constraint). The suspend mechanism reproduces the interpreter's exact mid-instruction save points
  (`m_inst_substate`), so save-state taken mid-instruction under DRC restores identically.

## Testing & validation

- **Merge gate (every PR):** ADR 0006 criterion 3 — `./mametests "[m68000][drc]"` green, register/
  flag/RAM/**cycle** equal between `-drc 0` and `-drc 1`, **full corpus**, on `drcbex64` and `drcbec`
  (`CPUORACLE_M68_DRC_C=1`), arm64 on the CI matrix. Leg A unchanged/green.
- **Native-coverage assertion (Task 6):** updated so each newly-native memory-EA opcode is asserted
  emitted-native (not `cfunc_`); the fallback set shrinks to the documented remainder.
- **Throughput (Task 7):** re-run the named-driver benchmark after O-mem-1 (expect the first real
  speedup, since `btst`-absolute is now native) and after each widening batch.
- **Structural:** `mametiny -validate` clean (m68000drc links in tiny); z80/m6502 oracles green.
- **Spot integration (Task 9):** boot a Sega System 16 driver under `-drc 0`/`-drc 1`, confirm
  identical behavior — especially aurail (the profiled workload) once `btst`-absolute is native.
- **Definition of done (O-mem-1):** `btst #n,(xxx).W/.L` emitted native, Leg-B green on the matrix,
  native-coverage assertion includes it, benchmark shows the expected speedup on the named driver,
  generator round-trip additive-only.

## Open questions — RESOLVED (owner-locked; baked into the O-mem-1 plan)

All five are decided. The O-mem-1 plan
([`plan/phase-2-o-mem-1-btst-absolute.md`](../plan/phase-2-o-mem-1-btst-absolute.md)) implements
these answers; they are not re-litigated.

1. **OQ-1 — native resume, or interpreter resume after a mid-instruction yield? → RESOLVED:
   interpreter resume.** The native path emits the suspend checkpoint and **yields** (`UML_JMP` to the
   interpreter delegate); it does **not** emit a native resume. This is already Leg-B-correct via the
   boundary-M `m_inst_substate==0` guard (a resuming instruction is routed to the interpreter delegate),
   and it is fully native on the dominant fully-granted case (a real driver grants a large quantum, so all
   four/five reads run native in one pass). Native resume is not needed for the throughput bar (real quanta
   are large; only the oracle's pathological one-cycle stepping yields mid-instruction). *Plan: Task 3 (the
   emitter yields, never resumes).*
2. **OQ-2 — generator step-descriptor shape? → RESOLVED: extend `m68000gen.py` (generator-first,
   single source).** The generator emits the per-opcode ordered bus-step list (access kind/size/lane, `−N`,
   redo/completed substate pair) from the **same microcode walk** that generates the interpreter handler, so
   the DRC's charges and substates cannot drift from the interpreter's. A hand-written btst-only step table
   is a **documented temporary fallback only** (if the generator extension slips the schedule, R-B above),
   never the long-run state. *Plan: Task 1 (additive generator extension; empty diff on existing generated
   files).*
3. **OQ-3 — `(xxx).L` vs `(xxx).W` in the first batch? → RESOLVED: both in O-mem-1.** They differ
   only by one extra absolute-address prefetch step, so the mechanism is identical; the extra `.L` read
   exercises the multi-read suspend path the mechanism must handle anyway, and `.W`-alone would leave a
   trivially-adjacent opcode `cfunc_`. *Plan: Task 4 (two compile-time arms, `.W` substates 1–8, `.L`
   substates 1–10).*
4. **OQ-4 — factor `generate_bus_step()` into a shared `UML_CALLH` subroutine, or inline? →
   RESOLVED: inline straight-line emission first.** `generate_bus_step()` is emitted inline (trivially
   auditable against the interpreter handler) for O-mem-1; factoring a shared helper is deferred to a later
   batch once the common sub-shape is empirically stable (mirrors mips3/ppc's post-hoc factoring of
   `static_generate_memory_accessor`). The temporary code-size cost is accepted. *Plan: Task 3 (inline
   emitter, not a `UML_CALLH`).*
5. **OQ-5 — `access_to_be_redone()` access method? → RESOLVED: cold-path `cfunc_` read-and-clear.**
   The redo flag is read via a tiny `cfunc_take_access_to_be_redone` that calls the public
   `access_to_be_redone()` (preserving the interpreter's exact read-and-clear `std::exchange` semantics) and
   parks the boolean in a DRC-owned scratch byte (`m_drc_redo_scratch`). One `UML_CALLC` on the **cold**
   suspend path only (zero hot-path cost); no shared `cpu_device` change. `UML_LOAD`-ing the private field
   (semantically wrong — would not clear) and adding a new clearing accessor to `cpu_device` (broader blast
   radius) are both rejected. *Plan: Task 2 (the cfunc + scratch field), consumed by Task 3.*

---

## Addendum / Resolution (2026-06-28) — O-mem-1 pre-merge review

> **Status:** Accepted. This addendum is the controlling text where it conflicts with §1 or §5 above.
> It does **not** change the mechanism (native read + native checkpoint), the increment plan (§6), or
> any of OQ-1…OQ-5. It corrects one factual error (§1) and converts one stated-but-unenforced boundary
> (§5) into a hard, testable gate. Triggered by a pre-merge review of the O-mem-1 branch
> (`feat/o-mem-1-btst-absolute`, unmerged) against the as-built `m68000drc.cpp`.

### What the review found (both verified live against the tree — DRC is default-on for `type()==M68000`)

1. **Critical — wrong address space (correctness).** `generate_bus_step()` emits
   `UML_READ(..., SPACE_PROGRAM)` for *every* read (`m68000drc.cpp:367`). The interpreter handler
   `btst_imm8_adr16_df` reads prefetch words via **`m_opcodes.read_interruptible`**
   (`m68000-sdf.cpp:20119, 20145, 20194`) and the data word via **`m_program.read_interruptible`**
   (`:20168`). `m_program`/`m_opcodes` are the **dynamic, SR_S-swapped** accessors
   (`update_user_super()`, `m68000.cpp:614-628`): in supervisor they are `m_r_program`/`m_r_opcodes`,
   in user they are `m_r_uprogram`/`m_r_uopcodes`. Those bind at `device_start` (`m68000.cpp:373-376`)
   to **distinct address spaces** whenever a driver configures `AS_OPCODES` (decrypted opcodes — the
   FD1094/FD1089 Sega System 16/18 class, which includes the profiled target's family), or
   `AS_USER_PROGRAM`/`AS_USER_OPCODES`. On such a machine the native prefetch would fetch from
   `AS_PROGRAM` (raw/encrypted bytes) instead of `AS_OPCODES` (the decrypted stream) → wrong extension
   word → wrong absolute address → wrong data read → mis-execution. This is a **regression vs the prior
   all-`cfunc_` behaviour**, where those reads went through the correct dynamic accessor. §1's
   "the 68000 has one program space" is the root inaccuracy.

2. **High — non-interruptible read (accuracy on tapped/wait-state buses).** `UML_READ` lowers to the
   plain space accessor, **not** `read_interruptible`. The two differ *only* when a `before_time` /
   `defer_access` tap on the accessed address defers the access (setting `cpu_device::m_access_to_be_redone`
   and charging wait time). On such a tap the interpreter redoes the access and charges the wait; the
   native path does neither, and the redo arm of the descriptor checkpoint (`cfunc_take_access_to_be_redone`,
   `m68000drc.cpp:395`) is **dead under `UML_READ`** because the flag is never set. On plain RAM/ROM
   (no tap) `UML_READ ≡ read_interruptible` exactly — same value, zero extra cycles, flag never set.

3. **Why the oracle is blind to both.** Leg A/B run flat RAM, a single program space, supervisor mode,
   no wait-states. That bus *satisfies the gate this addendum adds* (so the native path is always taken
   and is exact there) and *never configures the topologies finding 1 breaks* (so the bug is unexercised).
   "Oracle green" therefore certifies the native path **only for the flat bus class** — it is necessary
   and sufficient for that class, and says nothing about the gated-out topologies. This is weighted
   explicitly in the merge-gate section below.

### Decision

**Adopt option (a): a compile-time space-topology gate.** Native memory-EA opcodes (`btst`-absolute now,
and every O-mem-2…5 opcode that follows) are emitted into the resident dispatch **only when the bound bus
topology makes `SPACE_PROGRAM` the correct target for every access**; otherwise the opcode is not added to
the native dispatch and falls through to `cfunc_interpret_quantum` — exactly the pre-O-mem-1 behaviour.
This *realizes* §5's stated "stays `cfunc_`" intent instead of merely asserting it. Option (b) (route every
read through `m_program`/`m_opcodes.read_interruptible` via a `cfunc_`) is rejected as the mechanism: it is
ADR Alternative 3 under a new name — it concedes the native-read speedup that is O-mem-1's entire reason to
exist (a C call per bus cycle, slower than today's full `cfunc_`), and the read *is* the hot work for these
opcodes.

**The gate is purely compile-time; `SR_S` is NOT a runtime branch.** This resolves the open question the
review posed. The space configuration is fixed at `device_start` and the resident block is emitted once
(in `code_flush_cache`), after start — so the topology is a constant at emit time. The key reason `SR_S`
need not be a runtime branch: the gate requires all four specific accessors to resolve to the *same*
`address_space`, and when they do, `update_user_super()` makes `m_program == m_opcodes == space(AS_PROGRAM)`
in **both** supervisor and user mode. `SPACE_PROGRAM` is therefore correct irrespective of `SR_S`, and the
fast path stays **100% native** with no per-read space selection and no mode branch. (This does not
relitigate §5's lock on user/supervisor *space switches*: under the gate there is no switch — the user and
supervisor spaces are physically the same space.)

### Correction to §1 (address space)

The 68000 core does **not** present a single program space. It presents up to five: `AS_PROGRAM`,
`AS_OPCODES`, `AS_USER_PROGRAM`, `AS_USER_OPCODES`, `AS_CPU_SPACE` (`memory_logical_space_config`,
`m68000.cpp:326-339`). Instruction/prefetch fetches go through `m_opcodes`; operand/data accesses through
`m_program`; both are swapped between the supervisor (`m_r_*`) and user (`m_r_u*`) specific accessors on
every `SR_S` change. `m_base_ssw = SSW_PROGRAM | SSW_R` vs `SSW_DATA | SSW_R` is an architectural
bus-status field (consumed only when building a fault frame) and remains an emitted field write — it is
**not** the space selector and must not be confused with one. The native primitive may use `SPACE_PROGRAM`
for all reads **only because the gate guarantees every one of `m_s_program`/`m_s_opcodes`/`m_s_uprogram`/
`m_s_uopcodes` is the identical `address_space`** (and `m_mmu == nullptr`). Outside that gate, prefetch and
data reads target different UML spaces and/or the user-space set — which is precisely the work §5 keeps in
the interpreter.

### Enforceable §5 boundary — the exact predicate

Add an instance predicate, evaluated once at resident-block emit time, and guard the native memory-EA
dispatch arm(s) with it (the register-only `moveq` arm is unaffected — it performs no data access):

```cpp
// True iff the bound bus topology lets a native memory-EA access use SPACE_PROGRAM
// for every read with no opcode/user-space distinction and no MMU translation.
// Evaluated at resident-block emit time (post device_start: the space bindings and
// m_mmu are fixed by then).  All four m_s_* are address_space* set in device_start
// (m68000.cpp:373-376); m_mmu is the MMU hook (nullptr on a plain M68000).
bool m68000_device::drc_native_mem_ea_allowed() const
{
    return !m_disable_spaces
        && (m_mmu == nullptr)            // no MMU / indirect-handler path (ADR §5)
        && (m_s_program == m_s_opcodes)  // no separate AS_OPCODES (decrypted opcodes)
        && (m_s_program == m_s_uprogram) // no separate AS_USER_PROGRAM
        && (m_s_program == m_s_uopcodes);// no separate AS_USER_OPCODES
}
```

- **`SR_S` is deliberately absent** — see the Decision: identical `m_s_*` ⇒ `SPACE_PROGRAM` correct in both
  modes ⇒ no runtime mode branch.
- **Where it is applied:** in `generate_native_dispatch` (`m68000drc.cpp:229`), wrap the `btst`-absolute
  arm (and every future memory-EA arm) in `if (drc_native_mem_ea_allowed()) { …emit arm… }`. When false,
  the arm is simply not emitted and dispatch falls through to `lbl_delegate`. This is a C++ `if` around UML
  emission — zero runtime cost on the chosen path.
- **MMU-attachment safety (belt-and-suspenders):** `drc_supported_for_type()` already restricts DRC to the
  plain `M68000`, which has no MMU, so `m_mmu` is `nullptr` at emit time in every supported config. If a
  future change lets an MMU attach after the block is emitted, `set_current_mmu()` must set
  `m_cache_dirty = true` so the resident block regenerates and the gate re-evaluates. Note it; do not rely
  on it implicitly.

### Interruptibility / wait-states (finding 2) — **DEFERRED, explicitly, not dropped**

O-mem-1 keeps `generate_bus_step()`'s `UML_READ` (non-interruptible). The justification is bounded and
recorded:

- **Within the gate, on the bus the oracle validates, `UML_READ ≡ read_interruptible`** — plain RAM/ROM
  never defers, never charges wait-states, and never sets `m_access_to_be_redone`. So Leg B is exact for
  the right reason, and the descriptor's redo arm is *dormant but correct*: it would fire identically to
  the interpreter only if the flag were set, which this bus class never does. No code change is required
  for O-mem-1 beyond the space gate.
- **The residual divergence** is a flat, un-MMU'd bus that nonetheless installs a `before_time`/`defer_access`
  tap on a *program or data address actually read by a native memory-EA opcode*. The space gate does **not**
  exclude that case. No in-scope plain-`M68000` DRC driver does it on the program/data path — the core's
  only deferring taps are the VPA/autovector taps on `AS_CPU_SPACE` (`default_autovectors_map`,
  `m68000.cpp:350-362`), which these reads never touch.
- **Closure requirement (tracked, must not be silently dropped):** before DRC-native memory-EA is enabled
  on any driver that taps the program/data path — and as the proper exercise of the redo/wait machinery the
  descriptor models — extend the oracle with a deferring-tap corpus config (a `before_time` tap on the data
  address) so Leg B drives the redo/wait path. Until that exists, the redo path is **gated-by-absence**.
  This is logged as a new Open Question (OQ-6) below so it survives into O-mem-2 planning.

### Builder directive (bounded — O-mem-1 only)

1. **Add the predicate.** Declare `bool drc_native_mem_ea_allowed() const;` in `m68000.h` (near
   `drc_supported_for_type()`, `:274`) and define it in `m68000.cpp` exactly as above.
2. **Gate the dispatch arm.** In `generate_native_dispatch` (`m68000drc.cpp:229`), wrap **only** the
   `btst`-absolute arm (`:242-251`) in `if (drc_native_mem_ea_allowed()) { … }`. Leave the `moveq` arm
   unguarded. Do **not** change `generate_bus_step` or `generate_btst_imm8_absolute` — they remain correct
   *given* the gate (`SPACE_PROGRAM` is now provably the right space).
3. **Keep `is_native_opcode(u16)` as the opcode-eligibility predicate** (it stays topology-independent:
   `btst`-absolute is eligible). The topology gate is the separate `drc_native_mem_ea_allowed()`. The
   coverage test asserts both facets (next section).
4. **Do not add an `SR_S` runtime branch, a per-read space parameter, or a `read_interruptible` cfunc.**
   Those belong to later, separately-scoped increments (AS_OPCODES-native; interruptible-native), not to
   the mechanism PR.
5. **Cut-line doc.** In `README-drc.md`, qualify the O-mem-1 row: `btst #n,(xxx).W/.L` is native **only on
   a flat-topology, non-MMU bus** (`drc_native_mem_ea_allowed()`); on `AS_OPCODES`/user-space/MMU machines
   it remains `cfunc_`. Record finding-2 deferral (interruptible/wait-state) as a known limitation.

### Merge-gate implication

- **The existing flat-RAM oracle (Leg A/B) stays the correctness gate for the native path** and is
  unchanged: it runs exactly the flat-topology bus the gate permits, so `btst`-absolute native correctness
  is certified as before, on the full backend matrix (`drcbex64`, `drcbec` via `CPUORACLE_M68_DRC_C=1`,
  arm64 on CI). Necessary and sufficient *for the gated bus class*.
- **It is NOT sufficient to prove the gate itself.** The flat oracle never configures `AS_OPCODES`/user
  spaces/MMU, so it never exercises the gated-*out* fallback. O-mem-1 must therefore add — and this is the
  merge-gate delta — a **gate-predicate unit test**: construct `m68000_device` instances with (i) a separate
  `AS_OPCODES` map, (ii) a user-space map, and (iii) an attached MMU, and assert
  `drc_native_mem_ea_allowed()` is `false` and the resident block does **not** emit the native `btst` arm
  (dispatch reaches the `cfunc_` delegate). This is cheap (device construction + a block-emission probe;
  no corpus run) and directly tests the fix.
- **Strongly recommended (O-mem-1 if the harness allows, else the first task of O-mem-2):** a second
  *differential-oracle* config that actually configures a separate `AS_OPCODES` space (distinct contents
  from `AS_PROGRAM`) and runs Leg B `-drc 0` vs `-drc 1`. With the gate in place the native arm is not
  emitted there, so this proves the `cfunc_` fallback matches *and* that the DRC does not mis-read the
  opcode space — i.e. it is the test that would have caught this bug. If it exceeds O-mem-1's scope lock,
  it is recorded as O-mem-2's opening task, not dropped.
- **Finding 2's deferring-tap oracle config** is OQ-6 (deferred), as above.

### Consequences delta

- **Good:** the gate is correct-by-construction for O-mem-1 *and* every later memory-EA batch — each new
  native opcode inherits the single `drc_native_mem_ea_allowed()` guard for free, so widening coverage never
  re-opens the space-correctness question. The fast path stays fully native (no mode branch).
- **Cost / honest note on the headline number:** the gate **excludes `AS_OPCODES` (FD1094-class) drivers
  from the native path**, so if the throughput benchmark names an FD1094 set its `btst`-absolute runs
  `cfunc_` and shows **no** speedup. O-mem-1's speedup evidence must be measured on a **gate-eligible,
  flat-topology** 68000 driver (or a decrypted/bootleg set) where `btst`-absolute is hot; the "43% of
  aurail cycles" figure remains valid as *opcode-selection* justification but does not imply a speedup on
  aurail-under-FD1094. **Native `btst`-absolute on `AS_OPCODES` machines (supervisor, prefetch →
  `m_s_opcodes` UML space index, plus its required `AS_OPCODES` oracle config) is a deliberate, recorded
  O-mem-2+ extension** — deferred precisely because it needs the oracle-harness work above to be gate-
  validated, and shipping an un-gated space-selection path is exactly the "oracle-blind gap" this review
  exists to prevent.

### New open question (logged so O-mem-2 planning inherits it)

- **OQ-6 — interruptible/wait-state native reads.** The native path uses non-interruptible `UML_READ`,
  exact only on non-deferring buses. Decide, before DRC-native memory-EA reaches any deferring-tap driver,
  whether to (a) gate those buses out (extend `drc_native_mem_ea_allowed()` with a no-deferring-taps
  condition — needs a clean queryable signal), (b) route the read through a `read_interruptible` cfunc on
  the affected opcodes only, or (c) add a true interruptible-read UML path. Requires the deferring-tap
  oracle config to validate whichever is chosen. *Owner decision deferred to O-mem-2.*
