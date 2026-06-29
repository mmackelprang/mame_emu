<!--
license:BSD-3-Clause
copyright-holders:Mark Mackelprang
-->

# m68000 DRCUML port — increment-1 cut line and cycle-adapter notes

> **Status:** Increment 1 (plain 68000, user-mode common path). See
> [ADR 0002](../../../../docs/improvement-plan/adr/0002-m68000-drcuml-port.md) (architecture),
> [ADR 0006](../../../../docs/improvement-plan/adr/0006-m68000-oracle-gate-definition.md)
> (the oracle gate), and the
> [Phase-2 plan](../../../../docs/improvement-plan/plan/phase-2-m68000-drc.md).
>
> This document is the **single, normative statement** of (1) which opcodes increment 1
> emits as native UML versus routes to the interpreter via `cfunc_`, and (2) how the DRC
> obtains per-instruction cycle counts. It is written at **PR boundary K** (Tasks 1–2);
> the native emission itself lands at boundary M (Task 5). At boundary K there is **no
> behavior change** — the only code artifact is the generated DRC decode-descriptor table
> (Task 2), proven additive by an empty `git diff` on the existing generated decode files.

## Why this document exists

The 68000 DRC is **oracle-gated cycle-for-cycle**: every DRC PR must hold the interpreter
(`m68000.cpp` under `-drc 0`) and the DRC (`-drc 1`) register-, flag-, memory-, and
**cycle**-exact across the full corpus (ADR 0006 Leg B). The only safe way to grow native
coverage under that gate is to make the **native opcode set explicit and minimal**, and to
route everything else to the interpreter unchanged. This file is that explicit set. Anything
not listed here is, by definition, a `cfunc_` fallback in increment 1.

## The increment-1 NATIVE opcode cut line

Increment 1 emits **native UML** only for the following opcode classes, and only in the
listed addressing modes. Everything outside this set routes to `cfunc_` (the interpreter
handler).

### Native opcode classes

The resolved common-path set (ADR 0002 open-question 2, owner-confirmed):

- **ALU** — `add` / `adda` / `sub` / `suba` / `and` / `or` / `eor` / `cmp` / `cmpa`
  (`.b` / `.w` / `.l` where the size exists), plus the immediate forms
  `addi` / `subi` / `andi` / `ori` / `eori` / `cmpi` (to a register / simple-EA
  destination), and `neg` / `not` / `clr` / `tst`.
- **MOVE** — `move` / `movea` (`.b` / `.w` / `.l`), and `moveq`.
- **branch (unconditional)** — `bra`, `bsr` (PC-relative, `rel8` / `rel16`).
- **Bcc (conditional)** — `bcc` / `bcs` / `beq` / `bge` / `bgt` / `bhi` / `ble` / `bls` /
  `blt` / `bmi` / `bne` / `bpl` / `bvc` / `bvs` (`rel8` / `rel16`).
- **Scc** — `scc` / `scs` / `seq` / `sf` / `sge` / `sgt` / `shi` / `sle` / `sls` / `slt` /
  `smi` / `sne` / `spl` / `st` / `svc` / `svs` (register / simple-EA destination).
- **addq / subq** — `addq` / `subq` (`.b` / `.w` / `.l`), register / simple-EA destination.

These are the dynamically-hottest, lowest-risk, non-faulting user-mode instructions on the
plain 68000 — the bulk of the Sega System 16 / CPS / Neo-Geo instruction mix.

### Native addressing modes (the "register + simple-EA" cut)

Within the native opcode classes, native emission is restricted to the **register and
simple-EA** modes — the modes whose effective-address computation has a fixed, prefetch-
insensitive cycle cost and no fault surface in user mode:

| Native EA mode | `m68000.lst` token(s) | Notes |
|---|---|---|
| Data register direct `Dn` | `ds`, `dd` | source and destination |
| Address register direct `An` | `as`, `ad` | source and destination (where the opcode allows) |
| Address register indirect `(An)` | `ais` | |
| Postincrement `(An)+` | `aips`, `aipd` | |
| Predecrement `-(An)` | `pais`, `paid` | |
| Displacement `(d16,An)` | `das`, `daid`, `dad` | 16-bit displacement only |
| Immediate | `imm`, `imm3`, `imm4`, `imm8`, `imm8o`, `imm12`, `imm16`, `imm32`, `i16u` | source operand |
| PC-relative displacement `(d16,PC)` | `dpc` | source only |
| Branch displacement | `rel8`, `rel16` | bra/bsr/Bcc/Scc targets |

**Explicitly NOT native in increment 1 (always `cfunc_`):**

- Indexed / brief-extension modes `(d8,An,Xn)` (`dais`) and `(d8,PC,Xn)` (`dpci`) — the
  index extension word's timing and the scaled-index path are deferred.
- Absolute modes `(xxx).W` / `(xxx).L` (`adr16` / `adr32`) — `btst #n,(xxx).W/.L` is the
  first native absolute-EA opcode (O-mem-1, via `generate_bus_step()`), **on a flat-topology
  non-MMU bus only** (`drc_native_mem_ea_allowed()`); all other absolute-EA opcodes remain
  `cfunc_`, deferred to later O-mem increments.
- All `movem` register lists (`list` / `listp`), `movep`, `link` / `unlk`, `pea` / `lea`,
  `exg`, `ext`, `swap`, `jmp` / `jsr`, `dbcc`, `chk`, shifts/rotates
  (`asl`/`asr`/`lsl`/`lsr`/`rol`/`ror`/`roxl`/`roxr`), multiply/divide
  (`muls`/`mulu`/`divs`/`divu`), BCD (`abcd`/`sbcd`/`nbcd`), extended ALU
  (`addx`/`subx`/`negx`), bit ops `btst`/`bchg`/`bclr`/`bset` (except
  `btst #n,(xxx).W/.L`, native as of O-mem-1, and `btst`/`bchg`/`bclr`/`bset` `#n`\|`Dn`,`(An)`/`(An)+`/`-(An)`,
  native as of O-mem-2 — all **on a flat-topology non-MMU bus only**), `tas`,
  and the `ccr` / `sr` / `usp` operand forms — all `cfunc_` in increment 1.
- **Memory-EA `MOVE` / `MOVEA`** is `cfunc_` in increment 1 **except** (a) MOVEA `(An)`/`(An)+`/`-(An)`→`An`
  (`.w`+`.l`, 6 forms), native as of **O-mem-3a**, and (b) single-access (`.b`+`.w`) `MOVE` **reg↔mem**
  `(An)`/`(An)+`/`-(An)` (mem→Dn load, Dn→mem store, An→mem store; **15 forms**), native as of **O-mem-3b**
  — both **on a flat-topology non-MMU bus only** (`drc_native_mem_ea_allowed()`). `(d16,An)` MOVE/MOVEA,
  long `.l` reg↔mem, and mem→mem `MOVE` remain `cfunc_`, deferred to O-mem-3c (mem→mem) / O-mem-3d (`.l`) /
  O-mem-3e (`(d16,An)`).

### Hard `cfunc_` boundaries (never native in increment 1, by category)

Independent of opcode/EA, the following **always** route to the interpreter, because they
touch behavior the oracle gate forbids the DRC from approximating:

- **Faulting paths** — anything that can raise an address error, bus error, illegal
  instruction, privilege violation, divide-by-zero, CHK, or TRAP/TRAPV.
- **Privileged / supervisor state** — any access to `sr` (whole), `usp`, the `S`/`T` bits,
  `stop`, `reset`, `rte`/`rtr` — privilege checks and exception frames stay interpreted.
- **Prefetch-visible / bus-timing-subtle** — anything whose cycle count depends on the
  prefetch pipeline state in a way the fixed per-instruction cost cannot capture
  (`tas` RMW, `movep`, `movem` bus bursts).
- **FPU / MMU / MCU** — out of scope for the base 68000 entirely.
- **Mid-instruction suspend** — any opcode that the microcode core can suspend partway
  (`m_post_run` / `do_post_run`) stays on the interpreter arm.

These categories are exactly the ones flagged by ADR 0002 §3 and the ADR 0006 allowlist
({TAS, TRAPV, address-error}); for the allowlisted opcodes the DRC `cfunc_`s, so Leg B
holds there by construction (including cycles).

## Inherited Phase-1 cycle-adapter note

**The DRC mirrors the interpreter's per-instruction cycle counts; it never re-estimates
them.** This is a hard invariant, inherited from the Phase-1 oracle work and ADR 0002 §3 /
§A1:

1. **The interpreter is the authority for cycles.** The shipping reference is `m68000.cpp`
   under `-drc 0`. ADR 0006 establishes that the in-tree interpreter — *not* the
   MAME-derived corpus — is the m68000 behavioral authority. The DRC must match **the
   interpreter's** consumed-cycle count, which is what ships, not the corpus's and not an
   independently-derived 68k timing model.

2. **Per-instruction cycle cost comes from the same source the interpreter uses.** The new
   microcode core's cycle counts originate in the microcode list (`m68000.lst` /
   `m68k_in.lst`) that `m68000gen.py` consumes. The DRC takes each native opcode's cycle
   cost from that **same generated decode truth** (the Task-2 descriptor table) and emits it
   as a UML `icount` decrement — never an ad-hoc estimate. This is why Task 2 single-sources
   the descriptors from the generator: the DRC frontend reuses the interpreter's decode and
   timing facts rather than re-deriving them and risking drift.

3. **Divergence is always a DRC bug.** If a native opcode's cycle count does not match the
   interpreter's, the resolution is to **route that opcode to `cfunc_`** (or fix the DRC
   emission), **never** to modify the interpreter to match the DRC. We never edit the live
   core to suit the recompiler.

4. **The corpus cycle adapter (`m_au` / PC read-back) is a Leg-A harness concern, not a DRC
   concern.** Phase-1 close-out (ADR 0006, close-out path) added a documented per-core
   adapter so the oracle reads the retired PC from `m_au` (start + 4, what the corpus
   encodes) rather than `STATE_GENPC` (start + 2), and applies RAM-before-PC ordering. That
   adapter lives in the test harness (`cpuoracle.cpp`) and bounds **Leg A** (interpreter vs
   corpus). **Leg B** (interpreter ≡ DRC) — the load-bearing Phase-2 gate — is
   corpus-drift-immune: it compares two MAME execution paths and never consults the corpus,
   so the DRC need only match the interpreter's exported state and icount.

## Regeneration workflow (Task 2 — the generated DRC descriptor table)

The DRC decode descriptors are emitted by `m68000gen.py` alongside the existing committed
generated files, and are **checked in** (the maintainer runs the generator by hand; this is
**not** a build-time `custombuildtask`). To regenerate after editing the generator or the
instruction list, run from `src/devices/cpu/m68000/`:

```sh
# existing generated outputs (must remain byte-identical when only the
# descriptor extension changed — this is the additive-only proof)
python m68000gen.py decode m68000.lst m68000-decode.cpp
python m68000gen.py header m68000.lst m68000-head.h
python m68000gen.py sdf    m68000.lst m68000-sdf.cpp
python m68000gen.py sif    m68000.lst m68000-sif.cpp
python m68000gen.py sdp    m68000.lst m68000-sdp.cpp
python m68000gen.py sip    m68000.lst m68000-sip.cpp

# the new DRC decode-descriptor table (Task 2)
python m68000gen.py drcdesc m68000.lst m68000-drcdesc.ipp
```

After regenerating, `git diff` on `m68000-decode.cpp`, `m68000-head.h`, and the four
`m68000-s{d,i}{f,p}.cpp` files **must be empty** — only `m68000-drcdesc.ipp` may change.
A non-empty diff on the existing files means the generator change was mis-scoped (not
additive) and must be fixed before commit.

> **Byte-identity note (Python 3.11+).** Python 3.11 changed `IntEnum.__str__` to return the
> bare integer value instead of the historical `ClassName.member` form. The committed
> handler files (`m68000-s{d,i}{f,p}.cpp`) embed enum names in their debug comments, so on
> Python ≥ 3.11 a naïve regeneration would churn those comments. `m68000gen.py` therefore
> formats enum members through an explicit `enum_str()` helper that reproduces the pre-3.11
> `ClassName.member` form, keeping regeneration byte-identical across Python versions. Do not
> remove that helper; it is what makes the additive-only proof hold on a modern toolchain.

## What ships at each boundary (for reference)

| Boundary | Tasks | What changes here |
|---|---|---|
| **K** | 1–2 | Cut-line doc + generated DRC descriptor table. **No behavior change.** ✅ shipped. |
| **L (lit Leg B)** | 3–4 | DRC frontend skeleton (`m68000fe.{cpp,h}`, consumes the K table, no UML emit) + dual-path `execute_run()` with a **100% `cfunc_`** dispatcher (entry block = `UML_CALLC` interpreter-quantum → `UML_EXIT`; no native emission). Lights up oracle **Leg B** (interpreter ≡ DRC, register/flag/RAM/**cycle** exact) on the x64 (`drcbex64`) + C (`drcbec`) backends. Interpreter arm byte-unchanged; `m_isdrc` scoped to plain M68000. ✅ shipped. |
| M | 5 | Native **dispatch** via a **single resident UML block** (`m68000drc.cpp`: the entry block decodes the current opword `m_ird` in-line and runs a native fast-path or delegates the granted quantum to the interpreter — no per-PC compilation, no HASHJMP, so nothing is cached per PC; a per-PC scheme overran the 8 MiB code cache across the corpus's ~310 k distinct PCs and forced flushes that were fragile across UML backends, and went stale when the harness rewrote program RAM per case) **+ the first native opcode, `moveq`** (decoded from `m_ird` at runtime), via a hybrid handoff (native CASE 0 = the `Dn`/CCR write + prefetch-pipe advance; the interpreter `cfunc_`s CASE 1+2 = the interruptible prefetch, the `m_icount-=4` cycle charge, and the dispatch — so the cycle cost comes from the interpreter, never re-estimated, and it is cycle-exact by construction). The native fast-path runs ONLY at a genuine instruction-fetch boundary, gated by three guards (`m_inst_substate==0`, `m_ipc==m_pc-2`, `m_inst_state==m_decode_table[m_ird]`). Everything except `moveq` delegates to the interpreter. Leg B 15.7M assertions cycle-exact on `drcbex64` + `drcbec`. ✅ shipped. |
| O… | 10 | The **rest of the increment-1 native set** above (ALU/MOVE/branch/Bcc/Scc/addq/subq) widens opcode-by-opcode on top of the boundary-M dispatch, each step gated by Leg B. |

This document is updated when the native set changes (each coverage-widening increment, ADR
0002 §4 / Phase-2 Task 10).

### Native opcodes shipped (current)

| Opcode | Boundary | Native emission |
|---|---|---|
| `moveq #imm,Dn` | M | CASE 0 native (register/flag write + prefetch-pipe advance); timing tail `cfunc_`'d to the interpreter via the hybrid handoff. |
| `btst #n,(xxx).W` / `.L` | O-mem-1 | Full native via `generate_bus_step()` — 4-read (.W) / 5-read (.L) interruptible-read sequence with per-bus-cycle charge, suspend checkpoint, and address-error branch; resume after a mid-instruction yield owned by the interpreter's partial handler (ADR 0007 OQ-1). **Native ONLY on a flat-topology, non-MMU bus (`drc_native_mem_ea_allowed()`): when a driver configures `AS_OPCODES` (decrypted opcodes, e.g. FD1094) / user spaces / an MMU the opcode stays `cfunc_` (ADR 0007 Addendum 2026-06-28).** |
| `btst`/`bchg`/`bclr`/`bset` `#n`\|`Dn`,`(An)`/`(An)+`/`-(An)` | O-mem-2 | 24 forms (`btst`×6 read-only, `bchg`/`bclr`/`bset`×18 RMW). RMW via the **write-side** `generate_bus_step()` (`UML_WRITEM`, byte-lane mask, value from `m_dbout`); `btst` read-only. EA arithmetic with the A7-byte-by-2 rule and the `-(An)` predecrement internal `−2` (single-sourced as the descriptor's `pre_charge`); the bit is modified at the refill-prefetch state but **Z is set from the ORIGINAL byte** before the write. Substates/charges single-sourced from the generator. **Native ONLY on a flat-topology, non-MMU bus (`drc_native_mem_ea_allowed()`)** — `AS_OPCODES`/user-space/MMU machines stay `cfunc_` (ADR 0007 Addendum). The native data-write is validated by the fully-granted Leg-B oracle pass (ADR 0007 W4). |
| `movea.w`/`movea.l` `(An)`/`(An)+`/`-(An)`,`An` | O-mem-3a | 6 forms. Source-EA read via the frozen `generate_bus_step()` read path (`byte_lane=0` word read, `has_addr_error=1`); **long = two-word read** (high then low) with the high word latched in `m_alue`. Dest is an **address register**: write-back is `ext32(m_dbin)` (`.w`) / `set_16h`+`set_16l` (`.l`) at the final-prefetch state. **No data-write step, no flags, no `generate_bus_step()` edit.** Decodes `rx`/`ry` from `m_irdi` (the emitter latches `m_irdi = m_ird` first so the interpreter's partial handler resumes correctly). The `-(An)` predecrement internal `−2` is single-sourced as the first read's `pre_charge`. **Native ONLY on a flat-topology, non-MMU bus (`drc_native_mem_ea_allowed()`)** — `AS_OPCODES`/user-space/MMU machines stay `cfunc_`. Validated by the 1-cycle (incl. the `m_irdi` resume), full-grant (the native latch + register write-back), and single-offset partial-grant Leg-B passes (ADR 0007 O-mem-3 addendum). `(d16,An)` MOVEA is deferred to O-mem-3e. |
| `move.b`/`move.w` `(An)`/`(An)+`/`-(An)` reg↔mem | O-mem-3b | 15 forms (mem→Dn load 6, Dn→mem store 6, An→mem store 3 word). **Load:** source-EA read via the frozen `generate_bus_step()` (`.b` `byte_lane=1`/no-fault, `.w` `byte_lane=0`/fault); dest is a **data register** — write-back `set_8`/`set_16l` **preserves the unwritten half** (distinct from MOVEA's `ext32`); no data-write step. **Store:** the source register feeds `m_dbout` (`.w` low word; `.b` `set_8xl` replicate); the **word DATA WRITE** uses the one O-mem-3 `generate_bus_step()` primitive edit — `byte_lane==0` full-word `UML_WRITE` (byte store reuses O-mem-2's `byte_lane==1` masked path); the word write carries `has_addr_error=1` via the shared kind-agnostic fault branch. The **`-(An)` store is PREFETCH-first then DATA WRITE** (the reversed microcode order; `m_dbout` reconstructed from `m_aluo` in the write-setup so a mid-grant resume is exact). **MOVE flags** (`sr_nzvc`: N=MSB, Z=(value==0), V=C=0, X untouched) from the moved value (load: after the read; store: before/with the write). Decodes `rx`/`ry` from `m_irdi` (latched `m_irdi=m_ird`). Per-mode EA arithmetic incl. the byte `(A7)`=2 delta exception. **Native ONLY on a flat-topology, non-MMU bus (`drc_native_mem_ea_allowed()`)** — `AS_OPCODES`/user-space/MMU stay `cfunc_`. Validated by the 1-cycle, full-grant (the native word write + flags), and single-offset partial-grant Leg-B passes (x64 + C). `(d16,An)`→3e, `.l`→3d, mem→mem→3c. |

### Known limitations

- **Native memory-EA address space (`drc_native_mem_ea_allowed()`).** `generate_bus_step()` reads via
  `UML_READ(SPACE_PROGRAM)`. That is correct for every access only when the bound bus has no separate
  `AS_OPCODES` (decrypted opcodes), no `AS_USER_PROGRAM`/`AS_USER_OPCODES`, and no MMU — i.e. all of
  `m_s_program`/`m_s_opcodes`/`m_s_uprogram`/`m_s_uopcodes` resolve to the same `address_space` and
  `m_mmu == nullptr`. The native memory-EA dispatch arms are gated on this compile-time predicate; on
  any other topology the opcode falls through to `cfunc_` (ADR 0007 Addendum 2026-06-28).
- **Non-interruptible reads AND writes (OQ-6, stays deferred).** Native memory-EA reads use
  non-interruptible `UML_READ` and writes use `UML_WRITEM`, exact only on non-deferring buses
  (`UML_READ ≡ read_interruptible` / `UML_WRITEM ≡ write_interruptible` on plain RAM/ROM, which never
  defers, charges no wait-states, and never sets `m_access_to_be_redone`); interruptible/wait-state native
  accesses on deferring-tap buses remain deferred (ADR 0007 **OQ-6**, re-confirmed for writes by the
  O-mem-2 addendum). Within the gate, no in-scope plain-M68000 DRC driver taps the program/data path, so
  the descriptor's redo path (read AND write redo arms) is gated-by-absence (dormant-but-correct). Closing
  it requires a deferring-tap oracle corpus config, distinct from the W4 fully-granted pass, built only
  when a tapping driver is actually targeted.
- **Native retire not oracle-exercised (W4 residual).** The fully-granted Leg-B pass yields at the last
  bus access (`m_icount == 0`), so the bus-free native **retire** lambda (the `m_inst_state` dispatch +
  `set_ftu_const` cfunc + trace arm) runs only in production under a `> length` grant; the snapshot model
  cannot reach it without overshooting (which would advance into the next instruction). Bounded, low-risk
  (it mirrors the validated boundary-M `moveq` tail and routes `set_ftu_const` through the interpreter's
  own cfunc), logged as a known limitation, not a blocker.
- **Long-MOVEA read-high→read-low intermediate resume boundary (O-mem-3a, OQ-10 by-construction).** The
  single-offset (`length-4`) partial-grant pass resumes the long forms at the **final prefetch** — after
  both word reads have run natively — so the interpreter-side resume *between* read-high and read-low
  (substate 2→3, where the native must already have latched `m_alue`) is not directly oracle-exercised.
  It is covered **by construction**: the full-grant pass runs the whole native latch + register write-back
  end-to-end, the 1-cycle pass exercises the interpreter-side handoff after read-high, and the `m_alue`
  latch is emitted *before* the read-low bus step (so a completed-suspend at read-low leaves it correct).
  OQ-10 is owner-decided (2026-06-28): accept this residual — **no parameterized partial-grant sweep**.
- **The word `DATA WRITE` `byte_lane==0` full-word `UML_WRITE` is the single `generate_bus_step()` primitive
  edit of the entire MOVE arc (O-mem-3b).** O-mem-3a touched `generate_bus_step()` not at all; O-mem-3c/3d/3e
  reuse it frozen. The edit is reviewed in isolation (its own commit) with an oracle re-run proving the
  `byte_lane==1` byte-write path is **byte-identical** (the O-mem-2 bit-op RMW oracle stays green under all
  three grant modes). The word write carries `has_addr_error=1` via the **already kind-agnostic** shared
  address-error branch — no write-specific fault branch was added.
- **O-mem-3b `-(An)` store residual + retire (by-construction).** The `-(An)` store is PREFETCH-first then
  DATA WRITE; its `m_dbout` is reconstructed from `m_aluo` in the post-prefetch write-setup, so `m_aluo` is
  set *before* the prefetch bus step (a completed-suspend at the prefetch leaves the interpreter's `mmmw2`
  resume able to recompute `m_dbout` identically). The single-offset partial-grant pass resumes at the
  *second* access for every form; combined with the full-grant (whole instruction native) and 1-cycle
  (interpreter-side handoff) passes this covers the between-access boundary by construction. The native
  **retire** tail remains the one native residual not oracle-exercised (the snapshot model cannot reach an
  overshoot; mirrors the validated `moveq`/`btst`/MOVEA tail).
