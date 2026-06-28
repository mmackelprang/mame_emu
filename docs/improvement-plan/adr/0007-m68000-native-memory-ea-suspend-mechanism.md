# ADR 0007 — m68000 DRC native memory-EA + suspend / cycle / address-error mechanism

> **Status:** Accepted, **amended 2026-06-28** by **two** addenda at the foot of this file:
> (1) the [O-mem-1 pre-merge review](#addendum--resolution-2026-06-28--o-mem-1-pre-merge-review)
> (corrects §1's address-space claim, makes §5 an enforceable compile-time gate); and
> (2) the [O-mem-2 write-side mechanism](#addendum--resolution-2026-06-28--o-mem-2-write-side-mechanism)
> (designs the RMW write step + auto-inc/dec EA arithmetic + `Dn`/`#imm8` source forms, adds the
> fully-granted Leg-B oracle pass, resolves OQ-6…OQ-9, confirms gate continuity, and corrects the MMU
> rationale — **all O-mem-2 questions owner-decided 2026-06-28; the Planner has a fully-decided spec**).
> Each addendum is the controlling text where it conflicts with the body. Original: Accepted (all 5
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
  **→ Resolved (keep simplification under the gate) in the
  [O-mem-2 Addendum](#addendum--resolution-2026-06-28--o-mem-2-write-side-mechanism) below; the write side
  shares the identical flag mechanism, so the resolution covers reads and writes uniformly.**

---

## Addendum / Resolution (2026-06-28) — O-mem-2 write-side mechanism

> **Status:** Accepted. Extends (does not alter) the mechanism, the increment plan (§6), the
> compile-time space-topology gate, and OQ-1…OQ-5. It designs the **write side** of
> `generate_bus_step()` for the read-modify-write bit opcodes (`bchg`/`bclr`/`bset`), the
> **auto-increment/decrement EA arithmetic** that O-mem-3/4/5 reuse, **resolves OQ-6**, confirms
> **gate continuity**, and corrects a now-known-inaccurate MMU rationale (the §5 addendum's
> "plain M68000 has no MMU" claim). It is the controlling design for the **O-mem-2** batch
> ([§6](#6-increment-strategy-how-planner-should-batch-it) row 2). Written by the Architect from a
> line-by-line read of the as-built read emitter (`m68000drc.cpp`) and the interpreter handlers
> (`m68000-sdf.cpp` / `m68000-sdp.cpp`). **One decision (W4 — the oracle-coverage gap) needs owner
> sign-off before the Planner runs; see the flag at the end.**

### Scope of O-mem-2 (pin it before the Planner expands it)

The target opcodes — verified against the device's own `_df` handler table — are the bit opcodes with
the three register-indirect data EAs:

| Opcode family | `(An)` / `(An)+` / `-(An)` handlers (`m68000-sdf.cpp`) | Access shape |
|---|---|---|
| `btst #n,<ea>` | `btst_imm8_ais_df` :19582 · `_aips_df` :19668 · `_pais_df` :19759 | **read-only** (3 reads) |
| `btst Dn,<ea>` | `btst_dd_ais_df` :5654 · `_aips_df` :5717 · `_pais_df` :5785 | **read-only** (2 reads — no imm fetch) |
| `bchg #n,<ea>` | `bchg_imm8_ais_df` :20703 · `_aips_df` :20804 · `_pais_df` :20910 | **RMW** (3 reads + 1 write) |
| `bclr #n,<ea>` | `bclr_imm8_ais_df` :21681 · `_aips_df` :21786 · `_pais_df` :21896 | **RMW** (3 reads + 1 write) |
| `bset #n,<ea>` | `bset_imm8_ais_df` :22679 · `_aips_df` :22780 · `_pais_df` :22886 | **RMW** (3 reads + 1 write) |
| `bchg/bclr/bset Dn,<ea>` | `*_dd_ais/aips/pais_df` (6770/6848/6931, 7658/7740/7827, 8598/8676/8759) | **RMW** (2 reads + 1 write) |

**Scope (OQ-8 — owner-decided 2026-06-28: BOTH source forms in one batch).** O-mem-2 lands the
`#imm8`-source **and** `Dn`-source forms of all four bit-ops across the three EAs — **24 forms**
(`btst`×6 read-only, `bchg`/`bclr`/`bset`×18 RMW). The two source forms share the identical write step,
EA arithmetic, and Z/bit-modify computation; they differ only in the bit-number source and one prefetch
read (full design in **W3b**). The per-opcode step lists are single-sourced from the generator, so the
extra forms are descriptor rows + dispatch arms, not new mechanism.

### W1. Write-side `generate_bus_step()` — symmetric to the read step

The interpreter's write microcode step (the `bcsm2` state in every RMW handler, e.g.
`bchg_imm8_ais_df` `m68000-sdf.cpp:20778-20801`, partial-handler `case 7` at `m68000-sdp.cpp:20646-20656`) is:

```cpp
// (pre-write architectural setup — emitted by the OPCODE emitter, not the bus primitive)
m_aob = m_at;                 // restore the data EA saved during the read setup
m_ird = m_ir;  if(m_next_state != S_TRACE) m_next_state = m_int_next_state;
set_8xl(m_dbout, m_aluo);     // m_dbout = the MODIFIED byte, replicated into BOTH lanes
alu_and8(m_alub, 1 << (m_dcr & 7)); sr_z();   // Z from the ORIGINAL byte (m_alub), set BEFORE the write
m_base_ssw = SSW_DATA;        // NB: SSW_DATA only — the R bit is CLEAR (it is a write)
// (the bus step itself — this is what the WRITE generate_bus_step emits)
m_program.write_interruptible(m_aob & ~1, m_dbout, (m_aob & 1) ? 0x00ff : 0xff00);
m_icount -= 4;
if(m_icount <= 0) {                                  // identical two-way suspend checkpoint
    if(access_to_be_redone()) { m_icount += 4; m_inst_substate = 7; }  // redo (refund + replay)
    else                      { m_inst_substate = 8; }                 // completed
    return;
}
// NO address-error branch after the write
set_ftu_const(); m_inst_state = ...;                 // retire (interpreter `case 8`)
```

**The write is the same flag-not-longjmp shape as the read** (the load-bearing fact §"interruptible
read is a flag" established for reads). `write_interruptible(addr, data, mask)` (`emumem.h:1813`)
returns `void`, and the interpreter applies the *identical* `m_icount -= N; if(<=0){
access_to_be_redone() ? redo : completed; return; }` checkpoint after it — querying the *same*
`cpu_device::m_access_to_be_redone` flag. So a native UML write sequence can issue the write and then
emit the same checkpoint; there is nothing non-local to model. **No new suspend machinery — the
write reuses the read step's checkpoint verbatim.**

**Design — extend the one primitive, do not fork it (OQ-4 inline-first preserved).** Switch the
existing `generate_bus_step()` on the descriptor's `step.kind` (the generated enum already reserves
`DRC_BUS_DATA_WRITE = 2`, `m68000-drcdesc.ipp`). The charge + two-way suspend checkpoint (the
shared core, `m68000drc.cpp:394-417`) is emitted unchanged for all kinds; only the *access emission*
and the *fault branch* differ by kind:

| Step element | READ (`PREFETCH_READ`/`DATA_READ`) — as built | WRITE (`DATA_WRITE`) — O-mem-2 adds |
|---|---|---|
| address | `I2 = m_aob & ~1` | `I2 = m_aob & ~1` (same) |
| lane mask | post-read shift/mask select (`byte_lane`) | `mask = (m_aob & 1) ? 0x00ff : 0xff00` → operand of `UML_WRITEM` |
| value | result committed to `m_edb` | `UML_LOAD m_dbout` (caller already did `set_8xl`) |
| access op | `UML_READ(I0, I2, SIZE_WORD, SPACE_PROGRAM)` | `UML_WRITEM(I2, m_dbout, mask, SIZE_WORD, SPACE_PROGRAM)` |
| charge `−N` | shared | shared |
| suspend checkpoint | shared (redo/completed substates from descriptor) | shared (identical) |
| address-error branch | present iff `has_addr_error` | **absent** (`has_addr_error == 0` always) |

- **The UML primitive is `UML_WRITEM`** (`drcumlsh.h:67` —
  `UML_WRITEM(block, addr, src, mask, size, space)`), a word-sized masked write at the even address
  `m_aob & ~1` with mask `0x00ff`/`0xff00`. This is the exact mirror of the interpreter's
  `write_interruptible(addr, data, mask)` — a masked word write updates only the selected byte lane,
  so on flat RAM (the gate) it is byte-for-byte equal. **Do not** decompose to a `SIZE_BYTE`
  `UML_WRITE` at the computed byte address: that presents a different bus access shape than the
  interpreter's word-masked write and would diverge on any tapped word handler.
- **Value placement is the *caller's* job.** `set_8xl(m_dbout, m_aluo) = (m_aluo & 0x00ff) |
  (m_aluo << 8)` (`m68000.h:349`) replicates the modified byte into both lanes; the lane mask in
  `UML_WRITEM` selects which lane actually lands. The opcode emitter computes the modified byte and
  stores the replicated value into `m_dbout` (a `u16`, SIZE_WORD) *before* calling the write
  `generate_bus_step()` (mirroring `bcsm2`'s ordering: `set_8xl` precedes the write).
- **No address-error branch after the write.** The data write reuses the address already validated by
  the data *read* at the same EA (`m_aob = m_at`); for a byte access even/odd are both legal, so the
  interpreter has *no* `m_aob & 1 → S_ADDRESS_ERROR` after the write. `DATA_WRITE` rows therefore
  carry `has_addr_error = 0` and the primitive emits no fault branch — the write step is *simpler*
  than a prefetch read.
- **`m_aluo`/`m_alub` are scratch, not live across the yield.** The native emitter computes the
  modified byte directly in UML and stores `m_dbout`; it need not write `m_aluo`. On a
  completed-substate-8 yield the interpreter's `case 8` only retires (uses neither). On a
  redo-substate-7 yield the interpreter's `case 7` re-issues the write from `m_dbout` (not `m_aluo`).
  Both are satisfied by leaving `m_dbout` (and `m_sr`'s Z) correct before the write.

**The one asymmetry to record:** the write's *redo* arm (substate 7), if it ever fired, would have
the native path write once, refund, yield, and the interpreter re-write on resume — a double write of
the *same* value to the *same* cell (idempotent). It fires **only** on a deferring-tap bus, which the
space-topology gate excludes; under the gate the redo arm is **dormant-but-correct**, exactly as the
read's redo arm (OQ-6 below). The *completed* arm (substate 8 — `m_icount` hit 0 exactly at the
write) **is** reachable on flat RAM and **is** correct: the native `UML_WRITEM` leaves memory as
`write_interruptible` would, and `case 8` retires.

### W2. Auto-increment / decrement EA arithmetic (the layer O-mem-3/4/5 reuse)

Address-register-indirect EA computation is **opcode-emitter setup** (emitted before the data
`generate_bus_step`), not part of the bus primitive — the same separation O-mem-1 used for the
absolute-address setup. The register index is `ry = map_sp((m_irdi & 7) | 8)` where
`map_sp(r) = (r == 15 ? m_sp : r)` (`m68000.h:352`) and `m_sp ∈ {15,16}` is the active-A7 bank index
(`m68000.h:194`). Verified from the handlers:

- **`(An)` (`ais`, e.g. `:19610`):** `m_aob = m_da[ry]; m_at = m_da[ry];` — no register update. Native:
  load `m_da[ry]` into `m_aob` and `m_at`.
- **`(An)+` (`aips`, e.g. `:19696-19704`):** read at the *old* address, then post-increment:
  `m_aob = m_da[ry]; m_at = m_da[ry]; m_au = m_da[ry] + (ry < 15 ? 1 : 2); … ; m_da[ry] = m_au;` — the
  write-back to `m_da[ry]` happens *after* the address is latched (and, for RMW, after the read).
- **`-(An)` (`pais`, e.g. `:19790-19797`):** pre-decrement, read at the *new* address:
  `m_au = m_da[ry] - (ry < 15 ? 1 : 2); m_aob = m_au; m_at = m_au; m_da[ry] = m_au;` — **plus an internal
  `m_icount -= 2`** at the `pdcw1` micro-step (`:19793`) with **no** suspend checkpoint (it is not a bus
  access), emitted as a bare charge before the data read's `generate_bus_step`.
- **The A7 byte special case is `(ry < 15 ? 1 : 2)`** — a byte access to `(A7)+` / `-(A7)` adjusts the
  stack pointer by **2**, not 1, to keep it word-aligned. `ry < 15` is true for `A0–A6` (indices 8–14)
  and false for either A7 bank (`m_sp` = 15 or 16). Bit ops are **always byte**, so the delta is `1`
  except `2` for A7. The native emitter computes `ry` from the opword (`(m_ird & 7)`; if `== 7`, load
  `m_sp`; else `+8`) and selects the delta accordingly — an architectural register-index computation,
  **orthogonal to the space gate** (it touches `m_sp`/`m_da[]`, never a space selector).

> **Generalization note for O-mem-3/4/5 (record, do not implement here):** for word/long operands the
> delta becomes `2`/`4` with **no** A7 exception (only *byte* has the A7-by-2 rule). The `(An)+`/`-(An)`
> setup emitters O-mem-2 writes should take the operand size so MOVE/ALU reuse them; O-mem-2 only
> instantiates the byte case. Keep the predecrement internal `−2` single-sourced from the descriptor
> (see W5), not hand-typed.

### W3. RMW composition — where the bit is modified vs. where Z is set

The RMW sequence composes as **read-EA-prefetch(s) → data-read → compute → data-write → retire**.
The bit modification and the Z flag are set at **different** points, and getting the order wrong is the
highest-risk bug in the batch (analogous to O-mem-1's R-A):

1. **Data read** (`adrw2` / `pinw2` / `pdcw2`): the original byte arrives in `m_edb`, committed to
   `m_dbin`. `DATA_READ`, `byte_lane = 1`, `has_addr_error = 0`.
2. **Compute** (`bcsm1` for `bchg`; `bclm1`+`bclm2` for `bclr`; the `bset` analogue): in the *same*
   microcode state as the **refill prefetch read**, the interpreter saves the original byte
   (`m_alub = m_dbin`) and computes the modified byte into `m_aluo` via the per-opcode ALU op —
   **`bchg`: `alu_eor8` (toggle); `bclr`: `alu_or8` then `alu_eor8` (= AND-NOT); `bset`: `alu_or8`
   (set)** (verified: `bchg` :20759, `bclr` :21736+:21741). The bus step here is a *prefetch read*
   (`DATA`-independent), so the compute is pre-read architectural setup.
3. **Write step** (`bcsm2`): `set_8xl(m_dbout, modified)` then `alu_and8(m_alub, 1<<(m_dcr&7)); sr_z()`
   — **Z is computed from the ORIGINAL byte (the tested bit's prior value) and set BEFORE the write**,
   then the modified byte is written. This is correct 68000 semantics: Z reflects the bit *as tested*;
   memory receives the *post-op* value.

So the **only per-opcode difference across `bchg`/`bclr`/`bset` is the step-2 ALU op**; the bus-step
descriptor (kinds, charges, substates, addr-error flags, byte-lanes) is **identical** for all three at
a given EA. The native emitter therefore shares all setup/commit/write emission and varies only the
bit-modify UML (`UML_XOR` / `UML_OR` / `UML_OR`+`UML_XOR` against `1 << (m_dcr & 7)`), with Z derived
from `original & (1 << (m_dcr & 7))` exactly as O-mem-1's `compute_z` does (`m68000drc.cpp:534-548`),
but reading the original byte from `m_dbin` rather than recomputing.

### W3b. `Dn`-source forms (`btst/bchg/bclr/bset Dn,<ea>`) — same write step + EA, one fewer prefetch

Per OQ-8 (owner: include both source forms), O-mem-2 also emits the **register-source** forms. Verified
against `btst_dd_ais_df` (`:5654`, read-only) and `bchg_dd_ais_df` (`:6770`, RMW): the `Dn`-source
forms **share the identical write step (W1), EA arithmetic (W2), and Z/bit-modify computation (W3)** as
the `#imm8` forms — the only architectural differences are:

1. **No immediate-extension fetch.** The `#imm8` forms open with the `o#w1` prefetch that reads the
   bit-number extension word (substates 1/2); the `Dn` forms have **no** such read — the bit number is
   `m_dcr = m_da[rx]` (`rx = (m_irdi >> 9) & 7`), a register read, set in the EA-setup with no bus
   cycle. So the `Dn` forms have **one fewer prefetch read**, and the whole substate ladder shifts down
   by 2.
2. **Resulting bus-step runs (single-sourced from the generator; verified substates):**

   | Source · EA | bus cycles | substate ladder (kind) |
   |---|---|---|
   | `#imm8 (An)/(An)+` RMW | 4 | ext-read 1/2 · data-read 3/4 · refill 5/6 · **write 7/8** |
   | `#imm8 -(An)` RMW | 4 (+ internal −2) | ext-read 1/2 · data-read 3/4 (after `−2`) · refill 5/6 · **write 7/8** |
   | `Dn (An)/(An)+` RMW | 3 | data-read 1/2 · refill 3/4 · **write 5/6** |
   | `Dn -(An)` RMW | 3 (+ internal −2) | data-read 1/2 (after `−2`) · refill 3/4 · **write 5/6** |
   | `#imm8 <ea>` `btst` | 3 | ext-read 1/2 · data-read 3/4 · final-prefetch 5/6 (read-only, no write) |
   | `Dn <ea>` `btst` | 2 | data-read 1/2 · final-prefetch 3/4 (read-only, no write) |

   (The `(An)+`/`-(An)` rows carry the W2 auto-inc/dec EA setup and, for `-(An)`, the internal `−2`.)

**Implications for the Planner / generator (small, all additive):**

- **Dispatch arms:** the `Dn` forms are a *different opcode family* — encodings `0x01xx` mask `0xf1f8`
  (`btst_dd_ais` `0x0110`, `bchg` `0x0150`, `bclr` `0x0190`, `bset` `0x01d0`, +`0x08`/`0x10` for
  `(An)+`/`-(An)`), vs the `#imm8` `0x08xx` mask `0xfff8`. Each is its own `if (drc_native_mem_ea_allowed())`
  arm in `generate_native_dispatch`, but they call the *same* emitter helpers (EA setup, write
  `generate_bus_step`, `compute_z`, bit-modify) — only the bit-number source (`m_da[rx]` vs `m_dt`) and
  the step run differ.
- **Generator/descriptor:** no new step *kinds* — the `Dn` runs are just shorter `drc_bus_run` entries
  with renumbered substates, emitted by the same microcode walk (OQ-2). No new descriptor fields beyond
  what `#imm8` + the `DATA_WRITE` kind already introduce.
- **Net opcode count** O-mem-2 makes native: 4 bit-ops × 3 EAs × 2 source forms = **24 forms**
  (`btst`×6 read-only, `bchg`/`bclr`/`bset`×18 RMW). All share W1/W2/W3.

### W4. ⚠ Oracle coverage of the native write step — the load-bearing gate decision

**Finding (verified, applies to O-mem-1 too): under the oracle's stepping regime the native multi-step
path runs only its FIRST bus step; every later step — including the write — is executed by the
interpreter, so the native write would ship UNVALIDATED by Leg B as the harness stands.**

The Leg-B driver grants **exactly one cycle at a time**: `oracle_m68000_device::step_instruction`
ignores its `budget` and loops `*m_icountptr = 1; run();` until `m_ipc` changes
(`cpu_test_harness.cpp:374-453`), and **both** legs use it (`cpuoracle.cpp:1102,1138-1178`). With a
1-cycle grant the native path's first `generate_bus_step` charges `−4`, finds `m_icount ≤ 0`, and
yields at a non-zero substate; the boundary-M entry guard (`m_inst_substate == 0`,
`m68000drc.cpp:162-164`) then routes **every subsequent grant to the interpreter delegate**. To run a
4-bus-cycle RMW fully native the core needs `m_icount > 16` at instruction start; the oracle gives
`1`. Consequences:

- **O-mem-1 (already merged):** only native *read-1* of `btst`-absolute + the substate-2 yield is
  Leg-B-exercised; reads 2–5, the byte-lane data read, `compute_z`, and `retire` run on the
  interpreter in *both* legs (so they agree trivially). The native tail is exercised only by the
  throughput benchmark (Task 7, real driver, large quanta) and the spot-integration smoke (Task 9),
  **not** by the cycle/RAM/state-exact correctness gate. The ADR's "a native `btst`-absolute that
  miscounts a single bus cycle … fails Leg B immediately" is true **only for bus step 1** under the
  current harness.
- **O-mem-2 (the write batch):** the write is bus step **4** (substate 7/8), reached only after three
  prior steps that each yield under 1-cycle stepping. **The native write step is unreachable under
  Leg B as built** — O-mem-2's headline mechanism would merge with zero correctness-gate coverage.
  That is exactly the "oracle-blind gap" the O-mem-1 pre-merge review exists to prevent.

**Decision (RESOLVED — owner-approved: add the fully-granted Leg-B pass, "Task 0a"). This is a
gate-methodology change, designed concretely here.**

#### Task 0a — the fully-granted Leg-B pass (concrete design)

Add a **second DRC stepping mode** to the oracle stepper (`oracle_m68000_device::step_instruction`,
`cpu_test_harness.cpp:374-453`), selected per the existing backend-toggle convention (e.g. an
env flag `CPUORACLE_M68_DRC_FULLGRANT=1`, alongside `CPUORACLE_M68_DRC_C`). It changes **only** the
cycle-grant sizing of the existing single-step loop — not the comparison, the snapshot, or the
retirement detection:

- **Grant `length` on the first iteration, then drain at 1 cycle.** The current loop hard-codes
  `*m_icountptr = 1` every iteration. The full-grant mode sets `*m_icountptr = length` on the **first**
  iteration (`length` = the corpus `expected_cycles`, already in hand at `cpuoracle.cpp:1065`) and
  `1` thereafter; the cycle accumulation becomes `consumed += before − *m_icountptr` (currently
  `1 − *m_icountptr`, valid only for a unit grant). Everything else — `snapshot_retired()` each
  iteration while `m_inst_state != S_TRACE`, the `m_ipc != entry_ipc` break, the `frozen_consumed`
  trace latch — is **unchanged**.
- **Why this runs the whole native instruction including the WRITE.** With `m_inst_substate == 0`
  (fresh case) the boundary-M entry guard admits the native block (`m68000drc.cpp:162-187`); with
  `m_icount == length`, every `generate_bus_step` charges `−4` and finds `m_icount > 0` **until the
  last bus access**, so the native path runs read-EA → data-read → compute → **data-write** in one
  pass. After the final access's `−4`, `m_icount == length − length == 0`, the suspend checkpoint
  fires (`≤ 0`), the *completed* substate is stored, and the block yields — exactly as the interpreter
  leg does at the same point. The (bus-free) **retire** then runs under the interpreter in the 1-cycle
  drain, where the existing snapshot-before-the-retiring-grant logic captures instruction-1's retired
  state and the `m_ipc` change is detected.
- **Why NOT `length + headroom` in a single grant (correcting the first-pass note in W4's owner
  brief).** Granting `> length` makes the final access find `m_icount > 0`, so the native **retire**
  runs — but the block then JMPs to `lbl_delegate → cfunc_interpret_quantum` and the interpreter
  immediately runs the **next** instruction's first prefetch on the remaining cycles, advancing
  `m_ipc` and overwriting `m_pc`/`m_au`/`m_irc`/`m_ird` *before* `run()` returns. The harness cannot
  then snapshot instruction-1's retired state (the 1-cycle loop exists precisely to stop between retire
  and the next prefetch). `length` is therefore the exact sweet spot: **all native bus accesses run,
  no overshoot.**
- **SR.T deferred-trace: handled for free.** Because the native phase stops at `m_icount == 0`
  *before* retire, it never reaches the post-retire `m_next_state = S_TRACE` set or the trace dispatch;
  the retire+trace run in the 1-cycle drain under the unchanged `m_inst_state == S_TRACE` snapshot-
  freeze logic (`:424-430`). So `SR.T` cases need **no special handling** in the full-grant mode — a
  direct consequence of granting exactly `length` rather than overshooting (the overshoot variant is
  precisely what *would* have broken `SR.T` handling).

**Coverage this adds, and the one residual.** The full-grant pass exercises, natively and under the
correctness gate, every native bus access of a multi-step opcode — the headline being O-mem-2's
**data write** (its RAM byte, the SR.Z it sets, and the per-bus-cycle charge), validated because the
write's effects propagate into the retired-state comparison (the retire being interpreter-run in
*both* legs cannot mask a native-write divergence). **Residual:** the native **retire** lambda itself
(the `m_inst_state` dispatch + trace-arm in `generate_btst_imm8_absolute`'s `retire()`, and its
O-mem-2 analogue) is the *only* native code still not oracle-exercised — it runs in production when a
real driver grants `> length`, but the snapshot model cannot reach it without the overshoot above.
This residual is low-risk (it mirrors the already-validated boundary-M `moveq` handoff tail and routes
`set_ftu_const` through a cfunc) and is logged as a bounded known limitation, not a blocker. (A future
overshoot-tolerant snapshot — capturing retired state via an instrumentation hook the instant
`m_inst_substate` returns to 0 — could close it; out of scope for O-mem-2.)

- **Retroactive scope (in-scope to fix here):** Task 0a runs against the **existing native
  `btst`-absolute** too, so it **becomes the first correctness-gate coverage of O-mem-1's native tail**
  (reads 2-5, the byte-lane data read, `compute_z`) — to date validated only by the throughput
  benchmark and code review. **Any divergence Task 0a surfaces in `btst`-absolute is in-scope for the
  O-mem-2 PR to fix** (same mechanism family). The Planner should run Task 0a **first**, before adding
  any write opcode, to confirm the read tail is clean on the new pass — a clean baseline makes a
  later write-path failure unambiguous.
- **"Green" definition.** The full-grant pass is green when, over the full corpus on **x64
  (`drcbex64`)** and **C (`drcbec`)**, the DRC leg and interpreter leg are **register/flag/RAM/cycle
  identical** — the same equality the standard Leg B asserts (`cpuoracle.cpp:1198-1226`), under the
  `length`-grant regime.
- **Supplements, does not replace, the one-cycle pass.** Both run. The one-cycle pass stays primary:
  it validates native **step 1 + the suspend/redo handoff** and the `SR.T` snapshot path at microcycle
  granularity (which the full-grant pass coarsens). The full-grant pass adds the native **multi-step +
  write** coverage the one-cycle pass structurally cannot reach. A batch merges only when **both** are
  green (on x64 and C, Leg A unchanged).

### OQ-6 — interruptible / wait-state native reads **and writes** → RESOLVED: keep the gated simplification

**Decision: O-mem-2 keeps the non-interruptible `UML_READ` / `UML_WRITEM` simplification under the
existing `drc_native_mem_ea_allowed()` gate; it does NOT add a native interruptible path.** Rationale,
now confirmed for the write side:

- `write_interruptible` shares the **identical** `m_access_to_be_redone` flag mechanism as
  `read_interruptible` (the interpreter applies the same `access_to_be_redone()` checkpoint after both).
  On flat RAM/ROM — the only bus class the gate permits and the oracle runs — **neither** ever defers,
  charges a wait-state, or sets the flag, so `UML_WRITEM ≡ write_interruptible` exactly (same RAM
  effect, same cycles), and the write's redo arm is **dormant-but-correct** just like the read's.
- O-mem-2's opcodes touch only `m_program` (data read/write) and `m_opcodes` (prefetch); under the
  gate all resolve to `SPACE_PROGRAM` (W6). The only deferring taps in a plain `M68000` are the
  VPA/autovector taps on `AS_CPU_SPACE` (`m68000.cpp` `default_autovectors_map`), which these
  reads/writes never touch. No in-scope plain-`M68000` DRC driver taps the program/data path.

So OQ-6's *substance* is unchanged by the write side; it stays **DEFERRED** (option implicitly (a):
gated-by-absence), with the closure requirement intact: **before** DRC-native memory-EA is enabled on
any driver that taps the program/data path, the oracle must gain a **deferring-tap corpus config** (a
`before_time`/`defer_access` tap on the data address) to drive the redo/wait machinery — now for both
the read *and* the write redo arms. That deferring-tap config is **distinct** from W4's fully-granted
pass (W4 reaches the native steps; the deferring-tap config exercises the redo arm) and remains
deferred; recommend building it only when a tapping driver is actually targeted. **OQ-6 status:
DEFERRED, re-confirmed for writes; resolution = keep simplification under the gate.**

### Gate continuity — the existing predicate covers O-mem-2 unchanged

Confirmed against the handlers: every O-mem-2 access goes through `m_opcodes.read_interruptible`
(prefetch), `m_program.read_interruptible` (data read), or `m_program.write_interruptible` (data
write) — e.g. `bchg_imm8_ais_df` :20713 / :20739 / :20787. Under `drc_native_mem_ea_allowed()`
(`m68000.cpp:80-87`) all of `m_s_program == m_s_opcodes == m_s_uprogram == m_s_uopcodes` and
`m_mmu == nullptr`, so `m_program` and `m_opcodes` are the *same* `address_space` and `SPACE_PROGRAM`
is correct for prefetch, data read, **and data write** in both `SR_S` modes — **no new gate clause and
no `SR_S` runtime branch**. Each new O-mem-2 dispatch arm is wrapped in
`if (drc_native_mem_ea_allowed()) { … }` exactly as the `btst`-absolute arm
(`m68000drc.cpp:249-259`); when false the arm is not emitted and dispatch falls to `cfunc_`.

- **`AS_OPCODES`-native stays OUT of O-mem-2** (the deliberate, recorded later extension from the §5
  addendum). O-mem-2 only adds the **`AS_OPCODES` differential-oracle config** that the §5 addendum
  parked as "O-mem-2's opening task" — a config that builds a *separate* `AS_OPCODES` space (distinct
  contents from `AS_PROGRAM`) and runs Leg B `-drc 0` vs `-drc 1`: with the gate in place the native
  arm is not emitted there, proving the `cfunc_` fallback matches and the DRC does not mis-read the
  opcode space. This is **O-mem-2 Task 0** (it validates the gate; it does not enable native
  `AS_OPCODES`).

### Correction to the §5 addendum — the MMU rationale was inaccurate (LOW finding from PR #28 review)

The §5 addendum stated "`drc_supported_for_type()` already restricts DRC to the plain `M68000`, which
has no MMU, so `m_mmu` is `nullptr` at emit time in every supported config." **Both halves are
inaccurate** (verified live):

1. `drc_supported_for_type()` (`m68000.cpp:74-78`) checks **only** `type() == M68000`; it does **not**
   test `m_mmu`. The `m_mmu == nullptr` clause lives solely in `drc_native_mem_ea_allowed()`
   (`m68000.cpp:83`).
2. A **plain `M68000` can have an MMU attached** — Apple Lisa, Sun-1, and SGI pm2 attach a custom MMU
   to a `type() == M68000` CPU via `set_current_mmu()` (`m68000.cpp:89-95`). So `m_mmu` is **not**
   guaranteed `nullptr` for a DRC-eligible device.

**Disposition:** the `m_mmu == nullptr` clause in `drc_native_mem_ea_allowed()` is therefore
**load-bearing, not theoretical** — it actively excludes the Lisa/Sun-1/SGI class from native
memory-EA. Correct the §5-addendum prose accordingly (this addendum is the controlling text).

**Safeguard — IN SCOPE for O-mem-2 (OQ-9, owner-approved):** `set_current_mmu()` (and `enable_mmu()`,
`:129-133`) currently mutate `m_mmu` **without** setting `m_cache_dirty`. The resident block is emitted
once at the first `code_flush_cache()` after `device_start` (`m68000.cpp:318`); the gate is evaluated
**then**. If an MMU is attached/enabled **after** that emit (some MMUs toggle at runtime via a control
register), `drc_native_mem_ea_allowed()` will have already emitted the native arm against the pre-MMU
topology, and native accesses would bypass translation → mis-execution. **O-mem-2 adds the fix:** set
`m_cache_dirty = true` whenever `m_mmu` changes value in `set_current_mmu()` / `enable_mmu()`, so the
resident block regenerates and the gate re-evaluates (cheap — a single resident block) — plus a unit
test: attach an MMU after the block is emitted, assert the block regenerates and the native memory-EA
arm is no longer emitted (and, symmetrically, that detaching re-enables it). This hardens the same gate
O-mem-2 widens and discharges the safeguard the §5 addendum already called for. (Whether any current
DRC-eligible plain-`M68000` driver attaches its MMU at *runtime* vs. machine-config time is **not** a
gating question — the fix is correct either way; it is defense-in-depth if config-time-only and a
correctness fix if runtime.)

### Merge-gate guidance for O-mem-2 (what the Planner should encode)

Each O-mem-2 PR merges only on **all** of:

1. **Dual-leg flat-RAM oracle GREEN** (Leg A unchanged; Leg B register/flag/RAM/**cycle**-exact),
   per batch, on **x64 (`drcbex64`)** and **C (`CPUORACLE_M68_DRC_C=1`)**; arm64 on the CI matrix.
2. **The fully-granted Leg-B pass (W4) GREEN** — the *new* gate that actually exercises the native
   data-write/retire. **Without this the native write is unvalidated; treat it as a blocking gate, not
   a nice-to-have.**
3. **The `AS_OPCODES` differential-oracle config (Task 0) GREEN** — proves the gate keeps `AS_OPCODES`
   machines on `cfunc_` and the fallback matches.
4. **Gate-predicate + coverage assertions updated** — `drc_native_mem_ea_allowed()` unit test still
   green; the native-coverage assertion (`tests/emu/cpu/m68000_drc_coverage.cpp`, Task 6) extended so
   each newly-native `bchg`/`bclr`/`bset`/`btst` `(An)/(An)+/-(An)` form (both `#imm8` and `Dn` source —
   24 forms) asserts native-dispatched; **plus the MMU-regen test** (OQ-9: attach an MMU post-emit,
   assert the native arm drops out and dispatch falls to `cfunc_`).
5. **Generator additivity** — regenerating leaves the existing committed files byte-identical; only
   `m68000-drcdesc.ipp` grows (the new `DATA_WRITE` rows + the `(An)/(An)+/-(An)` runs, including the
   predecrement internal `−2`), single-sourced from the microcode walk (OQ-2).
6. **Code review** (pre-merge reviewer) and the **Linux `oracle` CI job** green (Windows-green is
   necessary-not-sufficient — boundary M passed Windows, failed Linux SysV twice).
7. **Throughput bench re-run on a gate-eligible, flat-topology driver** where the RMW bit ops are hot
   (not an FD1094/`AS_OPCODES` set — those run `cfunc_` and show no speedup, per the §5 addendum's
   honest-number note).

### W5 / descriptor delta (single-sourcing the write rows)

`m68000gen.py`'s bus-step walk (Task 1 of O-mem-1) extends additively to emit, for the O-mem-2 family:
the `DATA_WRITE` step (kind 2, `charge` = 4, the redo/completed substate pair, `has_addr_error = 0`,
and a `byte_lane`-equivalent flag so the emitter applies the `0x00ff`/`0xff00` mask to `UML_WRITEM`),
and the predecrement **internal `−2`** (model it as a per-step `pre_charge` field on the data-read step,
or an `INTERNAL` step kind — Planner's choice, but it MUST come from the microcode walk, never
hand-typed). The substate numbers are taken from the handler walk exactly as O-mem-1 did
(`#imm8` `(An)`/`(An)+` RMW: reads at 1/2, 3/4, refill 5/6, **write 7/8**; `-(An)` identical substates
with the internal `−2` folded before the data read; `Dn`-source forms shift down by one read).

### Consequences delta (O-mem-2)

- **Good:** the write side is a *small* extension of the proven read primitive (one `step.kind`
  branch + `UML_WRITEM`), the EA-arithmetic layer is reusable by O-mem-3/4/5, and the gate/OQ-6/space
  reasoning all carry over unchanged. `bchg`/`bclr`/`bset` differ only by one ALU op.
- **Cost / honest notes:** (1) the native write is **not** reachable by the *current* oracle — Task 0a's
  fully-granted pass is mandatory new scaffolding, and it exposes (and now closes) that O-mem-1's native
  tail was likewise only benchmark-validated; (2) the native **retire** lambda remains the one native
  residual the snapshot model cannot reach (W4); (3) the predecrement internal `−2` adds a non-bus
  charge the descriptor must carry; (4) the MMU safeguard broadens O-mem-2's blast radius slightly (two
  core mutators gain `m_cache_dirty = true`).

### Open questions — all RESOLVED for O-mem-2 (owner-decided 2026-06-28)

- **OQ-6 → RESOLVED:** keep the non-interruptible `UML_READ`/`UML_WRITEM` simplification under the
  space-topology gate; the deferring-tap oracle config remains the *deferred* closure requirement, now
  covering the read **and** write redo arms (built only when a tapping driver is targeted).
- **OQ-7 → RESOLVED (yes):** add the fully-granted Leg-B pass as **Task 0a**, designed concretely in W4
  (grant `length` then drain at 1; `SR.T` handled for free; supplements the one-cycle pass; runs against
  the existing `btst`-absolute, whose native tail it retroactively gates — any divergence there is
  in-scope to fix). **Blocking gate for every O-mem-2 PR.** Residual: native `retire` (bounded, logged).
- **OQ-8 → RESOLVED (both source forms):** O-mem-2 covers `#imm8` **and** `Dn` source for all four
  bit-ops × three EAs (24 forms; W3b). Same write step / EA arithmetic; the `Dn` forms are a separate
  dispatch family with one fewer prefetch and a shifted substate ladder — additive to the generator.
- **OQ-9 → RESOLVED (fold in):** O-mem-2 adds `m_cache_dirty = true` on `m_mmu` change in
  `set_current_mmu()`/`enable_mmu()` + a regen unit test (see the MMU section).

No open questions remain for O-mem-2; the Planner has a fully-decided spec. (The deferring-tap oracle
config under OQ-6, and the native-`AS_OPCODES` space-selection path, stay deferred to later, separately
scoped increments and are recorded as such.)
