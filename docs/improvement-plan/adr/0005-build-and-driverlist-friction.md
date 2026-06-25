# ADR 0005 — Build & driver-list friction reduction

> **Status:** In progress (boundaries H + I shipped; J pending) · **Phase:** P1 · **Owner:** TBD
> **Depends on:** none · **Related:** [0001](0001-differential-cpu-oracle.md) (shares the CI-feedback-speed goal)
> **Spec:** [`docs/improvement-plan/specs/2026-06-24-mame-improvements-design.md`](../specs/2026-06-24-mame-improvements-design.md)
> **Date:** 2026-06-24

## Context

Three recurring friction points slow iteration and trip contributors:

1. **GENie globs sources at generation time, so new files/dirs are invisible until a
   re-REGENIE.** `scripts/target/mame/mame.lua` enumerates manufacturer libraries by
   globbing directories and files: `linkProjects_mame_mame` iterates
   `os.matchdirs(.../src/<target>/*)` and tests `os.matchfiles(... "**.cpp")` /
   `"**.h"` / `"**.ipp")` (the `for x, dir in pairs(os.matchdirs(...))` loop, ~line 46),
   and `createProjects_mame_mame` re-globs the same way (~line 97). The glob runs at
   **project-generation** time. A newly created `.cpp` or a brand-new manufacturer
   directory is therefore **not part of the build** until `make REGENIE=1` regenerates
   the project files — a silent, confusing failure mode for newcomers ("I added the
   file, why isn't it compiling?").

2. **`src/mame/mame.lst` is a hand-maintained 49,654-line file, and `reconcilelist`
   is the most-tripped CI gate.** Every enabled system must be listed under an
   `@source:<path>.cpp` header. The CI step
   (`.github/workflows/ci-linux.yml:65-66`) runs
   `./mame -listxml | python scripts/build/makedep.py reconcilelist -l src/mame/<subtarget>.lst -`.
   The reconciler (`DriverReconciler`, `scripts/build/makedep.py:597`; dispatched at
   `:1070-1082`; `parse_list` at `:461`) compares the `.lst` against the systems the
   built binary actually exposes and sets `self.bad = True` on any mismatch
   (`makedep.py:627,630,678,682,692,698`), failing CI. It **reports** the discrepancy
   but offers **no autofix** — the contributor must hand-edit a 50k-line file to match,
   which is the single most common CI faceplant. (`tiny.lst` is the 95-line equivalent
   for `SUBTARGET=tiny`.)

3. **Full-MAME CI legs are multi-hour; the fast path exists but isn't the default
   loop.** The Linux CI builds full `mame` (clang) and `mametiny` (gcc) with `make -j3`
   (`ci-linux.yml:62`) — thousands of files, multi-hour. The iteration fast paths —
   `make SOURCES=<file>` and `SOURCEFILTER=<file.flt>` (the
   `write_sources_filter`/`sourcesfilter` machinery at `makedep.py:1044-1057`) — are
   documented but not wired as a quick pre-flight gate that gives contributors fast
   feedback before the multi-hour legs run.

## Decision

Reduce friction with three tooling changes, each independently shippable and each
shipping with tests:

### 1. Detect new sources and re-REGENIE automatically (or warn loudly)

The glob-at-generation behavior can't be made fully dynamic without restructuring
GENie, but we can **detect staleness and act**:

- Add a **`make` target / helper** (e.g. `make check-sources` or a step folded into
  the default flow) that computes a fingerprint of the source-file set GENie would
  glob (the union of `os.matchfiles` results over `src/<target>/*` and the device
  `scripts/src/*.lua` selectors) and compares it to the set baked into the last
  generated project files. On mismatch it **either** auto-runs `REGENIE=1` **or** fails
  with a clear "new sources detected — run `make REGENIE=1`" message. (Auto-REGENIE is
  the friendlier default for local dev; a warn-only mode for CI.)
- Implement the fingerprint in a small Python helper under `scripts/build/`
  (reusing `collect_sources`/glob logic already in `makedep.py`) so it's testable in
  isolation and shares the path-walking with the existing tooling.

### 2. Make `reconcilelist` an autofix tool

Extend `scripts/build/makedep.py` so the reconciler can **rewrite the `.lst` to match
the binary** instead of only reporting:

- Add a `--fix` (or a new `fixlist`) mode to the `reconcilelist` subcommand
  (`makedep.py:758,1070`). When the binary's `-listxml` and the `.lst` disagree, emit
  a corrected `.lst` (insert missing systems under the right `@source:` header in sort
  order; remove stale entries), preserving the file's grouping/formatting conventions
  that `DriverReconciler`/`parse_list` (`makedep.py:461,597`) already understand.
- CI continues to run the **check** mode (fail on mismatch) — the autofix is a
  **developer convenience** (`python scripts/build/makedep.py reconcilelist --fix -l
  src/mame/mame.lst <xml>`), turning a 50k-line hand-edit into a one-command fix the
  contributor reviews and commits.

### 3. Fast CI feedback before the long legs

- Add a **fast pre-flight CI job** that builds a tiny slice — `SUBTARGET=tiny` (already
  a CI matrix leg) or a `SOURCES=`/`SOURCEFILTER` slice of the touched drivers — and
  runs `-validate` + `reconcilelist` on it, so obvious breakage fails in minutes, not
  hours. The full legs still run, but contributors get a fast signal.
- Where a PR touches only specific driver files, drive the fast job from the changed
  file set via the existing `sourcesfilter` machinery
  (`makedep.py:1044-1057,1066`) to build just those.
- This dovetails with [0001](0001-differential-cpu-oracle.md)'s CI additions
  (`mametests`/`srcclean`), which are likewise fast and belong in the quick pre-flight.

## Integration seams (file:line)

| Seam | Location | Change |
|---|---|---|
| Source glob (link) | `scripts/target/mame/mame.lua:~46` (`os.matchdirs`/`os.matchfiles` in `linkProjects_mame_mame`) | Fingerprint source for staleness detection |
| Source glob (create) | `scripts/target/mame/mame.lua:~97` (same in `createProjects_mame_mame`) | Same |
| Hand-maintained list | `src/mame/mame.lst` (49,654 lines); `tiny.lst` (95) | Target of autofix |
| Reconciler class | `scripts/build/makedep.py:597` (`DriverReconciler`), `:461` (`parse_list`), `:1070-1082` (dispatch), `:758` (subparser) | Add `--fix`/`fixlist` mode |
| Reconciler failure flag | `scripts/build/makedep.py:627,630,678,682,692,698` (`self.bad = True`) | Check mode stays for CI |
| Sources filter machinery | `scripts/build/makedep.py:1044-1057` (`write_sources_filter`), `:1066` (`sourcesfilter`) | Drive fast per-PR slice build |
| CI reconcile step | `.github/workflows/ci-linux.yml:65-66` | Keep as check; add fast pre-flight job |
| CI full build | `.github/workflows/ci-linux.yml:62` (`make -j3`, multi-hour) | Add fast pre-flight ahead of it |
| New: staleness helper | `scripts/build/<new>.py` + `make` target | Detect new sources / auto-REGENIE |

## Alternatives considered

1. **Make GENie fully dynamic (no glob; discover at build time).** Rejected — a deep
   restructuring of the GENie target model with broad blast radius; staleness detection
   + auto-REGENIE captures most of the value at a fraction of the risk.
2. **Auto-generate `mame.lst` entirely from the code (drop the hand-maintained list).**
   Rejected for this ADR — the list also encodes maintainer intent (which clones/sets
   are enabled, ordering, grouping). An **autofix that proposes the diff** keeps human
   review while eliminating the hand-edit toil; full auto-generation is a bigger,
   separate policy decision.
3. **Drop the `reconcilelist` CI gate.** Rejected — it catches real list/code drift;
   the fix is to make compliance *easy* (autofix), not to remove the check.
4. **Only document the fast-path commands better.** Rejected as insufficient — docs
   exist; the win is wiring a fast pre-flight into CI so the fast signal is automatic.

## Consequences

**Good**
- Eliminates the "added a file, it's silently not built" trap and the most common CI
  faceplant (`reconcilelist`), the two biggest contributor friction points.
- Faster CI feedback shortens the iteration loop for everyone.
- All three changes are tooling-only — no emulation risk — and are unit-testable.

**Bad / cost**
- Auto-REGENIE-on-staleness must be careful not to surprise users mid-build or trigger
  spurious regenerations (fingerprint must be precise); a warn-only mode is the safe
  default for CI.
- The `--fix` reconciler must exactly reproduce the `.lst` formatting/ordering
  conventions or it will churn the file; this needs golden-file tests.
- A fast pre-flight job adds CI minutes (small) and a second place to keep build flags
  in sync with the full legs.

## Accuracy & determinism preservation

Entirely build/tooling-scoped. No emulation source, scheduler, timing, or save-state
code is touched. The autofix only edits `*.lst` text and project-generation metadata;
it cannot change runtime behavior. Accuracy and determinism are unaffected.

## Testing & validation

- **Reconciler autofix golden tests** (Python unit tests under `scripts/` or a
  `tests/` harness): given a known-divergent `.lst` + a fixture `-listxml`, assert
  `--fix` produces the exact expected `.lst` (formatting, ordering, `@source:`
  grouping preserved), and that re-running check mode on the fixed file reports clean
  (`self.bad == False`).
- **Staleness-detector tests:** given a fixture source tree with an added/removed
  `.cpp`, assert the fingerprint mismatch is detected and the correct action
  (warn / auto-REGENIE signal) is taken; assert no false positive on an unchanged tree.
- **Fast pre-flight validation:** the new CI job builds a `tiny`/`SOURCES=` slice and
  runs `./mame -validate` + `reconcilelist` (check mode); it must pass on a clean tree
  and fail on an intentionally-broken slice.
- **Existing gates unchanged:** the full CI legs, `-validate`, and the
  `reconcilelist` **check** continue to run exactly as today.
- **Iteration commands the tooling must keep working:** `make SOURCES=<file>`,
  `make REGENIE=1`, `make SUBTARGET=tiny`, `make TESTS=1 && ./mametests` (the spec's
  verified loop; note `make` itself is a documented prerequisite — `pacman -S make`).
- **Definition of done:** autofix + staleness helper land with green unit tests; fast
  pre-flight job green; full CI legs unaffected.

## Open questions (for the owner before Planner runs)

1. **Auto-REGENIE vs. warn-only default.** Should local `make` auto-run `REGENIE=1`
   when it detects new sources, or just warn and let the developer run it? (Recommend:
   warn-by-default to avoid surprise; an opt-in `AUTO_REGENIE=1` for those who want it;
   CI is warn/fail-only.)
2. **Reconciler autofix authority.** Is `--fix` strictly a local developer tool
   (contributor reviews + commits the diff), or do we ever want CI to auto-commit list
   fixes? (Recommend: local-only; never auto-commit — preserves maintainer review of
   which sets are enabled.)
3. **Fast pre-flight slice definition.** Drive the fast job from the PR's changed files
   (via `sourcesfilter`) or always build `SUBTARGET=tiny`? (Recommend: `tiny` for a
   stable baseline plus a changed-files slice when the diff is driver-local.)
4. **Where the new Python helper/tests live.** Under `scripts/build/` with a sibling
   test, or fold into a `mametests`-adjacent harness? (Recommend: `scripts/build/`
   with Python unit tests, matching where `makedep.py` lives.)
5. **Upstreamability.** These touch shared build infrastructure (`mame.lua`,
   `makedep.py`) that upstream cares about. Keep changes additive/opt-in so they're
   palatable upstream, or treat them as fork-local conveniences? (Recommend:
   additive/opt-in, upstream-friendly.)
