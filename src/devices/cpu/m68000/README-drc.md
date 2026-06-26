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
- Absolute modes `(xxx).W` / `(xxx).L` (`adr16` / `adr32`) — deferred to a later increment.
- All `movem` register lists (`list` / `listp`), `movep`, `link` / `unlk`, `pea` / `lea`,
  `exg`, `ext`, `swap`, `jmp` / `jsr`, `dbcc`, `chk`, shifts/rotates
  (`asl`/`asr`/`lsl`/`lsr`/`rol`/`ror`/`roxl`/`roxr`), multiply/divide
  (`muls`/`mulu`/`divs`/`divu`), BCD (`abcd`/`sbcd`/`nbcd`), extended ALU
  (`addx`/`subx`/`negx`), bit ops (`btst`/`bchg`/`bclr`/`bset`), `tas`, and the
  `ccr` / `sr` / `usp` operand forms — all `cfunc_` in increment 1.

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
| **K (this doc)** | 1–2 | Cut-line doc + generated DRC descriptor table. **No behavior change.** |
| L | 3–4 | DRC frontend skeleton + dual-path plumbing, 100% `cfunc_` fallback. |
| M | 5 | First native UML emission for the opcode set above. |

This document is updated when the native set changes (each coverage-widening increment, ADR
0002 §4 / Phase-2 Task 10).
