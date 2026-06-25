# Build & driver-list friction reducers (ADR 0005 / Pick 5)

## Overview

This directory ships three small, additive, opt-in tools that reduce the build
and driver-list friction described in
[ADR 0005](../../docs/improvement-plan/adr/0005-build-and-driverlist-friction.md):
a source-set staleness detector (`make check-sources`), a driver-list autofix
(`reconcilelist --fix`), and a fast CI pre-flight job. None of them change the
default build path, the existing CI gates, or any emulation behavior — they are
purely upstream-friendly conveniences a developer (or CI, in check-only mode)
can opt into.

## `make check-sources` — source-set staleness detector (boundary H)

**The problem.** GENie globs the source tree at *project-generation* time (when
project files are produced), not at build time. A `.cpp`, `.h`, or `.ipp` you
add after the last `make REGENIE=1` is therefore silently left out of the build:
the file exists on disk, but the generated project files never learned about it,
so it never compiles. This is a confusing failure mode ("I added the file — why
isn't it building?").

**The detector.** `make check-sources` fingerprints the source set GENie would
glob *now* and compares it to the set baked into the last generated project
files, reporting which files were added or removed. It has three modes:

- `make check-sources` — **warn-only default**. Prints a warning if the source
  set is stale and **exits 0**. Safe to fold into other flows without surprising
  anyone.
- `make check-sources AUTO_REGENIE=1` — **opt-in auto-regenerate**. When the
  detector reports staleness, it re-runs the build with `REGENIE=1` to pick the
  new files up. (Because the fallback invokes `make` with no explicit goal, this
  runs the default `all` target — a full regenerate-and-rebuild, not project-file
  regeneration alone.)
- `make check-sources FAIL_ON_STALE=1` — **CI fail mode**. Exits non-zero on
  staleness so a CI step can hard-fail.

The target is purely additive: it is **never** pulled into the default
`all`/`generate` build path and does nothing unless you invoke it explicitly.
The underlying helper is
[`scripts/build/sourcestale.py`](sourcestale.py), with pytest tests at
[`scripts/build/tests/test_sourcestale.py`](tests/test_sourcestale.py).

## `reconcilelist --fix` — driver-list autofix (boundary I)

**The problem.** `src/mame/mame.lst` is a hand-maintained file of roughly
50,000 lines: every enabled system must be listed under the right `@source:`
header. The `reconcilelist` CI gate compares that list against the systems the
built binary actually exposes and fails on any mismatch — historically the
single most-tripped CI gate, because fixing it meant hand-editing a 50k-line
file to match the binary.

**The check form CI runs (unchanged).** CI continues to run only the check form,
which reports a mismatch and fails — it never rewrites anything:

```sh
./mame -listxml | python scripts/build/makedep.py reconcilelist -l src/mame/mame.lst -
```

**The new local autofix.** `--fix` rewrites the `.lst` in place to match the
binary instead of only reporting the mismatch:

```sh
./mame -listxml | python scripts/build/makedep.py reconcilelist --fix -l src/mame/mame.lst -
```

You can also pass an XML file path instead of `-` to read from a file rather
than stdin:

```sh
./mame -listxml > systems.xml
python scripts/build/makedep.py reconcilelist --fix -l src/mame/mame.lst systems.xml
```

The autofix inserts missing systems under the correct `@source:` header in sort
order and removes stale entries, while preserving the file's conventions: the
header block, the order of `@source:` groups, intra-group ordering, the
blank-line convention between groups, CRLF line endings, and the trailing
newline. It aims for minimal churn (it only touches what actually diverged) and
is idempotent (re-running it on an already-fixed file is a no-op).

**Upstreamability / safety note.** `--fix` is a **local developer tool only.**
**CI never runs `--fix`** — it only ever runs the check form above. The
contributor reviews the resulting diff and commits it, which preserves
maintainer review of *which* sets/clones are enabled (the list encodes intent,
not just facts). The golden tests that pin the exact output formatting live at
[`scripts/build/tests/test_reconcile_fix.py`](tests/test_reconcile_fix.py).

`--fix` also **errors out** (rather than silently mangling the file) if it is
given a list file that contains `#include` directives, since it cannot safely
rewrite recursive/multi-file lists — fix the referenced file directly instead.

## Fast CI pre-flight (boundary J)

**The problem.** The full-MAME Linux CI legs build thousands of files with
`make -j3` and take multiple hours. Obvious breakage — a driver that fails
`-validate`, or a `.lst` that has drifted from the code — shouldn't have to wait
hours to surface.

**The pre-flight job.** `.github/workflows/ci-linux.yml` now has a `preflight`
job that runs *before* the multi-hour matrix legs. It:

- builds only the `SUBTARGET=tiny` slice (`mametiny`, ~95 drivers — minutes, not
  hours, with `TOOLS=1`),
- runs `./mametiny -validate`, and
- runs the reconcile **check** form on `tiny.lst`:
  `./mametiny -listxml | python scripts/build/makedep.py reconcilelist -l src/mame/tiny.lst -`.

The multi-hour `build-linux` matrix legs (gcc → `mametiny`, clang → `mame`) are
gated on it via `needs: preflight`, so they only start once the fast pre-flight
passes — fail-fast feedback in minutes. The pre-flight uses the reconcile
**CHECK form only**; it never runs `--fix` (consistent with the safety note
above).

**Future enhancement — changed-files slice.** Today the pre-flight uses
`SUBTARGET=tiny` as a stable, upstream-friendly baseline. A PR that touches only
driver files could instead drive a `SOURCES=`/`SOURCEFILTER` slice built from
just the changed set, via `scripts/build/makedep.py sourcesfilter` (the
sources-filter machinery: subparser at `makedep.py:1035`, implementation
`write_sources_filter` at `makedep.py:1330`, dispatch at `makedep.py:1352`).
That would build only
the touched drivers for an even faster signal on driver-local PRs; it is noted
here as a documented future enhancement, not wired into the YAML yet.

## Upstreamability

All three tools are additive and opt-in. They do not change the default build
path or any existing CI gate: the full CI legs, `-validate`, and the
`reconcilelist` **check** continue to run exactly as before. `make
check-sources` does nothing unless invoked; the `reconcilelist` autofix never
auto-commits and CI never runs it; the fast pre-flight only adds an earlier,
faster signal ahead of the unchanged full legs. That additive, review-preserving
posture is what makes them palatable upstream.
