# ADR 0003 — Front-end usability quick wins

> **Status:** In progress — Groups A + B shipped; Group C pending · **Phase:** P1 · **Owner:** TBD
> **Depends on:** none · **Related:** [0004 (web control surface)](0004-web-control-surface.md) reuses the same data feeds
> **Spec:** [`docs/improvement-plan/specs/2026-06-24-mame-improvements-design.md`](../specs/2026-06-24-mame-improvements-design.md)
> **Date:** 2026-06-24

## Context

New-user friction in the CLI and internal UI is concentrated in a handful of places
where MAME *already has* the information a user needs but doesn't surface it. Each
item below was verified against the current tree (line numbers confirmed):

1. **`-listslots` / `-listmedia` show options but never the command syntax.**
   `clifront.cpp:836` (`listslots`) and `clifront.cpp:909` (`listmedia`) print
   columns (`"SYSTEM" "SLOT NAME" "SLOT OPTIONS" "SLOT DEVICE NAME"` at
   `clifront.cpp:845-846`; media extensions at `:919-920`) but never the literal
   `-<slot> <opt>` or `-flop1 <image>` invocation a user must type.

2. **Launch-time missing-ROM error names nothing.** `romload.cpp:671` throws
   `emu_fatalerror(EMU_ERR_MISSING_FILES, "Required files are missing, the machine
   cannot be run.")` — no file names, no audit detail, no next step.

3. **Rich per-ROM audit detail already exists but the UI discards it.**
   `audit.cpp:498-579` (`media_auditor::summarize()`) produces detailed strings —
   `"NEEDS REDUMP"`, `"NO GOOD DUMP KNOWN"`, `"INCORRECT CHECKSUM: EXPECTED %s /
   FOUND %s"`, `"INCORRECT LENGTH: %d bytes"`, `"NOT FOUND (%s)"` — when given an
   output stream. The in-UI audit at `ui/auditmenu.cpp:226-227` calls
   `auditor.audit_media(AUDIT_VALIDATE_FAST)` and keeps **only** the summary
   pass/fail (`info.available = (summary == CORRECT) || ...`), throwing the detail
   away.

4. **The simple selector has a helpful empty state the full selector lacks.**
   `ui/simpleselgame.cpp:305-319` draws a friendly "No system ROMs found. Please
   check the rompath setting…" box when `m_nomatch`. The full system-selection menu
   has no equivalent guidance.

5. **Slot menu shows codes, not descriptions, and resets silently.**
   `ui/slotopt.cpp:191-193` displays `option->name()` (the short code, e.g. `cga`)
   as the list label; the human device name is only in a bottom text box.
   `ui/slotopt.cpp:250-253` calls `machine().schedule_hard_reset()` on change with no
   confirmation/warning.

6. **Unknown CLI option gives no "did you mean", though system-name fuzzy exists.**
   `clifront.cpp:241` rethrows `emu_fatalerror(EMU_ERR_INVALID_CONFIG, "%s",
   ex.message())` with no suggestion. Contrast `clifront.cpp:304-331`, which on an
   unknown *system name* runs `drivlist.find_approximate_matches()` and prints the
   top approximate matches.

7. **Input set/append is "not very discoverable" — by its own comment.**
   `ui/inputmap.cpp:556-558`: `// flip between set and append // not very
   discoverable, but with the prompt it isn't completely opaque`.

8. **`-showusage` has no examples.** `clifront.cpp:1735-1738` prints
   `"Usage:  %s [machine] [media] [software] [options]"` then the raw option list —
   no concrete `mame asteroids` / `mame -flop1 game.img` examples.

**i18n constraint (verified):** all user-facing UI/CLI strings are wrapped in the
`_()` translation macro (`#define _(...) (::util::lang_translate(__VA_ARGS__))`,
`src/lib/util/language.h:24`), and translations load from compiled `strings.mo`
catalogs via `src/frontend/mame/language.cpp`. **Any copy change to a translated
string adds/alters a catalog entry** and should follow MAME's localization workflow.

## Decision

Ship a **grouped set of copy/UX improvements** that surface already-present
information, wrapping all new user-facing strings in `_()` and matching existing UI
patterns. No accuracy impact whatsoever. Group the changes so each is independently
reviewable and testable:

### Group A — CLI discoverability (`clifront.cpp`)
- **A1.** `listslots`/`listmedia`: append a usage hint line/column showing the exact
  syntax (`-<slot> <opt>`, `-flop1 <image>`), and/or a footer example. Keep the
  table layout; add a "to use" line per the existing `osd_printf_info` style.
- **A2.** Unknown-option fuzzy suggestion: at `clifront.cpp:241`, before/around
  rethrowing, compute approximate matches against the known option names (reuse the
  same Levenshtein-style approach already used for system names at `:304-331`) and
  print "did you mean: …". Factor the existing matcher into a shared helper if not
  already shared.
- **A3.** `-showusage` examples: at `clifront.cpp:1735-1738`, append a short,
  translated **Examples** block (run a game, mount a floppy, list a system's slots).

### Group B — Actionable ROM/audit errors
- **B1.** Launch missing-ROM error (`romload.cpp:671`): replace the bare message with
  one that names the failing system and points at the audit menu / `-verifyroms`, and
  where feasible includes the missing file list already known at that point. Keep the
  `EMU_ERR_MISSING_FILES` code.
- **B2.** In-UI audit detail (`ui/auditmenu.cpp:226-227`): pass an output stream into
  `audit_media()` / `summarize()` (the detail path at `audit.cpp:498-579` already
  exists) and surface the per-ROM reason in the audit menu instead of discarding it.

### Group C — Internal-UI guidance
- **C1.** Full selector empty state: add a `simpleselgame.cpp:305-319`-style "no ROMs
  found, check rompath" box to the full system-selection menu's empty case, reusing
  the same copy/pattern for consistency.
- **C2.** Slot menu descriptions (`ui/slotopt.cpp:191-193`): show the human device
  description alongside (or instead of) the bare code in the list label, using the
  `devtype().fullname()` already available.
- **C3.** Slot-change reset warning (`ui/slotopt.cpp:250-253`): add a brief
  translated notice that changing slots reboots the machine (match MAME's existing
  confirmation/notice patterns) rather than resetting silently.
- **C4.** Input set/append discoverability (`ui/inputmap.cpp:556-558`): make the
  set-vs-append affordance explicit in the prompt (the comment concedes it's opaque);
  add a translated hint line.

### Cross-cutting
- Every new/changed string goes through `_()`; the change set ships with the
  corresponding catalog updates and a note for translators.
- Match the surrounding brace/whitespace style per file (some UI files are K&R), run
  `srcclean` on touched files.

## Integration seams (file:line)

| Item | Location | Change |
|---|---|---|
| A1 listslots/listmedia | `clifront.cpp:836` (`listslots`), `:909` (`listmedia`), headers `:845-846` / `:919-920` | Add usage-syntax hint |
| A2 option fuzzy | `clifront.cpp:241` (no suggestion) ↔ `:304-331` (system fuzzy exists) | Reuse approximate-match helper for options |
| A3 usage examples | `clifront.cpp:1735-1738` (`CLICOMMAND_SHOWUSAGE`) | Append Examples block |
| B1 ROM error | `romload.cpp:671` (`emu_fatalerror EMU_ERR_MISSING_FILES`) | Name system + point to audit/`-verifyroms` |
| B2 audit detail | `ui/auditmenu.cpp:226-227` (discards detail) ← `audit.cpp:498-579` (`summarize()` detail) | Capture + show per-ROM reason |
| C1 empty state | `ui/simpleselgame.cpp:305-319` (pattern to copy) → full selector menu | Add empty-state box |
| C2 slot labels | `ui/slotopt.cpp:191-193` (`option->name()`) | Add `devtype().fullname()` to label |
| C3 reset warning | `ui/slotopt.cpp:250-253` (`schedule_hard_reset()`) | Add reboot notice |
| C4 input help | `ui/inputmap.cpp:556-558` ("not very discoverable") | Add prompt hint |
| i18n | `src/lib/util/language.h:24` (`_()`), `src/frontend/mame/language.cpp` (`strings.mo`) | All copy via `_()`, catalog updated |

## Alternatives considered

1. **A new "first-run wizard" UI.** Rejected for this ADR — large, design-heavy, and
   out of scope for "quick wins"; the high-leverage fixes are surfacing existing
   data, not new flows. (Could be a later Designer-led item.)
2. **Leave error strings English-only / bypass `_()`.** Rejected — breaks MAME's
   localization contract; translated strings are the project norm.
3. **Rewrite the audit subsystem to expose structured data.** Rejected as overkill —
   `summarize()` already produces the detail; B2 just stops discarding it.
4. **Add a "did you mean" only for systems (skip options).** Rejected — the option
   case is the one with no help today and the matcher already exists; reuse is cheap.

## Consequences

**Good**
- Materially better first-run experience using information MAME already has; minimal
  risk.
- Each group is small, independently shippable, and golden-output testable.
- Establishes a pattern (surface, don't discard) reused by
  [0004](0004-web-control-surface.md), which exposes the same audit/config/slot data
  over REST.

**Bad / cost**
- Copy changes touch the translation catalog; coordinating translations is overhead.
- UI menu changes must respect per-file brace/whitespace style and existing
  navigation conventions; sloppy edits risk consistency drift (a Polisher pass is
  warranted before merge).
- Golden-output tests for CLI text are sensitive to wording; they must be scoped to
  stable, structural assertions (presence of the syntax hint, the system name in the
  error) rather than exact prose, to avoid brittleness.

## Accuracy & determinism preservation

Pure UX/text. No emulation code path changes; no scheduler, timing, save-state, or
device behavior is touched. `slotopt` already reboots on slot change — C3 only adds a
*notice*, not new reset behavior. Accuracy and determinism are unaffected.

## Testing & validation

- **Golden-output CLI tests** (new Catch2 cases in `mametests`, linking via
  `scripts/src/tests.lua`): assert that `-listslots`/`-listmedia` output contains the
  usage-syntax hint; that an unknown option produces a "did you mean" line; that
  `-showusage` contains an Examples section; that the missing-ROM error contains the
  system name and an audit pointer. Assert **structural substrings**, not full prose,
  to stay robust to translation/wording.
- **Manual UI smoke** (the internal-UI items C1–C4 aren't easily golden-tested):
  exercise the audit menu on a deliberately incomplete ROM set and confirm per-ROM
  detail appears; open the slot menu and confirm descriptions + reboot notice; check
  the full-selector empty state with an empty rompath.
- **`srcclean`** on all touched files (gated in CI by [0001](0001-differential-cpu-oracle.md)).
- **`./mame -validate`** stays green (sanity; no device changes expected).
- **Definition of done:** golden CLI tests green in `mametests`; UI items
  manually verified; catalog entries added; `srcclean` clean.

## Open questions (for the owner before Planner runs)

1. **Localization workflow.** Do we add only the English source strings (and let the
   translation pipeline pick them up), or also regenerate the `.pot`/catalog in this
   change? Confirm the project's expected localization step for new strings.
2. **Golden-test brittleness vs. translation.** Golden CLI tests must run in a fixed
   locale (English) to be stable. Acceptable to force `LANG`/MAME language to a known
   value in the test fixture? (Recommend: yes — pin the test locale.)
3. **Scope of B1 missing-file list.** Is the missing-file list reliably available at
   `romload.cpp:671`, or should B1 just name the system + point to `-verifyroms`
   (which has the full detail)? Owner to confirm how much detail to inline vs. defer
   to the audit path.
4. **C3 confirmation vs. notice.** Should changing a slot require a confirmation
   prompt (an extra keypress) or just show a non-blocking "this reboots the machine"
   notice? (Recommend: non-blocking notice — minimal friction, matches current
   one-step behavior.)
