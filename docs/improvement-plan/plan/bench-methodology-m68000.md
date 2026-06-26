# m68000 DRC — benchmark methodology & interpreter baseline

*Phase-2 Task 7 artifact. Baseline captured 2026-06-26.*

This is the **before** measurement for ADR 0002 (m68000 → DRCUML). The post-DRC
run must use the **same host, build flags, game, and parameters** to be
comparable. The acceptance bar (below) gates "done" for DRC Increment 1, in
addition to correctness (the oracle's Leg B, interpreter ≡ DRC).

## Workload

- **Game:** `aurail` (Aurail, Sega System 16B) — main CPU **Motorola MC68000**,
  **unencrypted** (zero FD1094/FD1089 references in `-listxml`), so the number
  reflects plain-68000 execution, not a decryption layer. ROMs verified `good`
  against the MAME 0.288 merged set.
- **Why System 16B:** a representative real-game 68000 workload (the realistic,
  user-facing metric). Note this measures the **whole machine** (68000 + Z80
  sound + video/sprites), so the DRC's whole-machine speedup is **Amdahl-bounded**
  by the non-68000 share of emulation time. A m68000-*isolated* metric (the
  oracle corpus replayed at scale) can be added to show the raw core speedup.

## Metric & command

MAME `-bench N` runs N emulated seconds headless, unthrottled, no audio/video
output, and reports **Average speed** = emulated-time / wall-clock × 100%.
Higher = faster. The DRC toggle is the boolean flag `-drc` / `-nodrc` (NOT
`-drc 0/1`).

```sh
mame aurail -rompath "<rompath>" -bench 120 -nodrc   # interpreter
mame aurail -rompath "<rompath>" -bench 120 -drc     # DRC
```

## Environment (must match for the post-DRC run)

| | |
|---|---|
| Host CPU | AMD Ryzen 9 7950X (16C/32T) |
| OS / build | Windows, MSYS2 MINGW64, gcc 14.2.0 |
| MAME | 0.288, `SOURCES=src/mame/sega/segas16b.cpp` subset, default (release) optimization |
| Build env note | makefile needs `MSYSTEM=MINGW64` **and** `OS=Windows_NT` (the MSYS2 login shell blanks `OS`) |
| Bench | `-bench 120`, 3 runs averaged |
| ROMs | `D:\Down\MAME 0.288 ROMs (merged)` (version-matched) |

## Baseline results (interpreter, no m68000 DRC exists yet)

| Config | Run 1 | Run 2 | Run 3 | Mean |
|---|---|---|---|---|
| `-nodrc` (interpreter) | 1615.40% | 1608.54% | 1620.58% | **1614.8%** |
| `-drc` | 1629.38% | 1618.14% | 1616.43% | **1621.3%** |

`-drc` ≈ `-nodrc` (Δ ≈ +0.4%, within run-to-run noise) — confirms the m68000
core is interpreter-only today; the `-drc` flag has no m68000 effect. **Baseline
= ~1615% of realtime on aurail.**

## Acceptance bar (owner to confirm)

The bar is the **overall aurail `-drc` speedup vs this interpreter baseline**:

> **Proposed: `-drc` ≥ 1.3× the interpreter baseline on aurail** (≈ **≥ 2100%**)
> for DRC Increment 1's native-opcode common-path workload, with **monotonic
> non-regression** as native coverage widens.

Rationale: MAME's mature DRCUML backends typically give large *raw-core* m68000
speedups, but the whole-machine gain on a real game is Amdahl-limited by the
Z80/video/sprite share. 1.3× is a meaningful first target that proves the DRC is
carrying real load without over-promising before native coverage is known;
refine X upward once Increment 1's opcode coverage is measured. **The cycle-exact
correctness guarantee comes from the oracle (Leg B), independent of this bar.**

## Reproduce

```sh
cd <repo> && MSYSTEM=MINGW64 /c/msys64/usr/bin/bash -lc \
  'export OS=Windows_NT; mingw32-make SOURCES=src/mame/sega/segas16b.cpp -j32 REGENIE=1'
for i in 1 2 3; do ./mame.exe aurail -rompath "<rompath>" -bench 120 -nodrc; done
for i in 1 2 3; do ./mame.exe aurail -rompath "<rompath>" -bench 120 -drc;   done
```
