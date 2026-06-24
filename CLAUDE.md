# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

MAME is a multi-purpose emulation framework that documents vintage hardware (arcade machines, computers, consoles, calculators) by recreating it in software. The source code *is* the documentation; the running emulator validates its accuracy. The codebase is large (~360 manufacturer directories of drivers) but highly regular — almost everything is a `device_t`.

## Build system

The build is **GENie-based** (Lua project generator). Running `make` first runs GENie to generate platform-native makefiles/project files under `build/`, then invokes the real build. You rarely call GENie directly.

```sh
make                              # full build of the default `mame` target (very long: hours, thousands of files)
make SUBTARGET=tiny               # small driver subset -> produces `mametiny` (fast; what CI uses for smoke tests)
make SOURCES=src/mame/atari/asteroid.cpp   # build ONLY these driver(s) -> custom exe; the fast way to iterate on one driver
make -j3                          # parallelize (CI uses -j2/-j3)
make TOOLS=1                      # also build chdman, floptool, unidasm, romcmp, srcclean, etc.
make TESTS=1                      # also build `mametests` (Catch2 unit tests)
make REGENIE=1                    # force-regenerate project files (REQUIRED after adding/removing source files)
make vs2022 [MSBUILD=1]           # generate Visual Studio solution (and optionally build it)
make OSD=sdl                      # choose the SDL OSD instead of the platform default
```

Key `make` variables (full list is documented at the top of `makefile`): `TARGET`, `SUBTARGET`, `OSD`, `DEBUG=1`, `SYMBOLS=1`, `OPTIMIZE=`, `NOWERROR=1`, `SANITIZE=`, `IGNORE_GIT=1`, `SOURCEFILTER=<file.flt>`.

**Platform note:** On Windows, builds use the MinGW-w64 build environment from mamedev.org/tools (MSYS2 UCRT64/CLANG64), not MSVC directly — even VS project generation needs that toolchain present.

**Adding source files:** because GENie caches the file list, after creating a new `.cpp`/`.h` you must rebuild project files (`make REGENIE=1` or `make` re-runs GENie when scripts change). New drivers must also be registered in a driver list (see below).

## Validation & tests

There is no single "run the test suite" command. Correctness is enforced by three distinct gates (all run in `.github/workflows/ci-*.yml`):

```sh
./mame -validate                  # PRIMARY gate: internal self-validation of every driver/device/memory map. Run this after any driver/device change.
make TESTS=1 && ./mametests        # Catch2 unit tests for core utilities only (tests/ dir: corestr, options, attotime, rgbutil)
./mame -listxml | python scripts/build/makedep.py reconcilelist -l src/mame/mame.lst -   # checks the driver list matches the code
```

`mame -validate` is the most important check — it catches the vast majority of driver/device mistakes. The Catch2 suite (`tests/`) only covers a handful of `src/lib/util` and `src/emu` primitives.

Run `srcclean` (built via `TOOLS=1`) on touched files before committing — it normalizes whitespace to project conventions.

## Architecture

The codebase is layered: a portable core, reusable hardware-component devices, and per-machine drivers that compose those devices. The OSD layer isolates everything portable from the host OS.

- **`src/emu/`** — the emulation core. Defines `device_t` (the base of nearly everything) and the **device interfaces** (mix-ins named `di*.cpp/.h`: `diexec` = executable/CPU, `dimemory` = address spaces, `digfx`, `disound`, `diimage`, `dinvram`, etc.). Also: address maps (`addrmap`), timing (`attotime`, scheduler), save-state, config, and the debugger. Read `device.h` and the `di*.h` files to understand how components plug together.
- **`src/devices/`** — reusable hardware building blocks, each a device: `cpu/` (one dir per CPU family, with disassemblers and often DRC recompilers), `sound/`, `video/`, `machine/` (peripheral chips), `bus/` (slot devices / connectors), `imagedev/` (media: floppy, cassette, cartridge).
  - **Each CPU is its own class.** `src/devices/cpu/` holds ~210 family directories; every concrete CPU is a `<cpu>_device` class (e.g. `z80_device` in `z80/z80.h`) deriving from **`cpu_device`** (`src/emu/devcpu.h`), which is `device_t` plus the execute / memory / state / disasm interfaces. Chip variants subclass their parent CPU rather than copying it (e.g. `nsc800_device`, `r800_device`, `z80n_device` all `: public z80_device`). Types are exposed via `DECLARE_DEVICE_TYPE`/`DEFINE_DEVICE_TYPE`, and disassemblers are split into separate `*d.cpp/.h` files so the standalone `unidasm` tool can reuse them without the emulation core.
- **`src/mame/<manufacturer>/`** — the actual machine drivers, grouped by manufacturer (e.g. `atari/`, `sega/`, `nintendo/`). A driver is a `*_state` class that wires devices together in a machine-config method, plus ROM definitions, plus one or more **system registration macros** at the bottom of the file: `GAME(...)`, `GAMEL(...)` (arcade), `COMP(...)` (computer), `CONS(...)` (console), `SYST(...)`. GENie auto-discovers each manufacturer subdirectory as a build library (see `linkProjects_mame_mame` in `scripts/target/mame/mame.lua`) — no per-driver build edits needed, but the system must be listed in a `.lst`.
- **`src/osd/`** — OS-dependent layer: `windows/`, `sdl/`, `mac/`, shared `modules/` (input, sound, video, debugger backends), and the inline-asm/atomics headers (`ei*.h`, `eminline.h`). This is the boundary that keeps the core portable.
- **`src/frontend/mame/`** — the built-in UI, internal menus, and command-line frontend.
- **`src/lib/`** — `util/` (core utilities, container/string helpers, file/zip I/O), `formats/` (disk/tape/cartridge image formats), `netlist/` (analog/digital netlist simulation).
- **`src/tools/`** — standalone utilities (`chdman`, `floptool`, `castool`, `imgtool/`, `unidasm`, `romcmp`, `srcclean`).

### Driver / build lists

- **`scripts/src/*.lua`** declare every CPU, sound chip, machine device, bus, and format with `--@` selector comments that the target gathers. To enable a *new device* in the full build, add it here.
- **`src/mame/mame.lst`** is the manually-maintained master list of every enabled system, grouped under `@source:<path>.cpp` headers. **A new driver must be added here** (and `tiny.lst` is the equivalent for `SUBTARGET=tiny`). The `reconcilelist` CI step fails if `.lst` and code disagree.

## Coding conventions

(From `README.md` "Coding standard", `.editorconfig`, and docs.mamedev.org/contributing/cxx.html.)

- **Indentation: tabs, 4-space tab width**, one tab per level. **Spaces for intra-line alignment.** (Python/RST files use spaces — see `.editorconfig`.) Trailing whitespace trimmed; final newline required.
- **Brace style:** new code generally prefers **Allman**, but parts of the tree use K&R. *Match the file you are editing* and keep whitespace diffs minimal.
- **License header is mandatory** on every source file — two comment lines at the very top:
  ```cpp
  // license:BSD-3-Clause
  // copyright-holders:Your Name
  ```
  New contributions are encouraged under BSD-3-Clause (LGPL-2.1 / GPL-2.0 are also accepted). The project as a whole is GPL-2.0+.
- Header include guards are checked in CI (`includeguards.yml`).

## Workflow note for this repo

This is the upstream MAME tree (`master` branch, remote `mamedev/mame`). Per the user's global workflow rules, do implementation work on a short-lived branch and open a PR rather than committing emulation/source changes directly to `master`.
