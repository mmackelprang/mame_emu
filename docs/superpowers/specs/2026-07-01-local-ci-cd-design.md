# Design: zero-cost CI/CD — public MAME + reusable self-hosted playbook

- **Date:** 2026-07-01
- **Status:** Design approved (brainstorming complete); implementation plan pending
- **Author:** Mark Mackelprang (with Claude)
- **Related:** `docs/ci/appserver-runner.md` (the existing canary/fallback prototype this supersedes for MAME)

## Problem

GitHub Actions on this **private** repo has been running up real expense. MAME's
multi-hour builds bill on every hosted minute, and the expensive legs dominate:

| Runner | Private-repo minute multiplier |
|---|---|
| Linux | 1× billed |
| Windows | 2× billed |
| **macOS** | **10× billed** |
| `windows-11-arm` (a *larger* runner) | always billed |

The 10× macOS leg and the 2×-Windows + arm64-larger-runner legs, multiplied by
hour-scale builds on a private repo, are where the money went. The account's
spending limit is now **$0**, which stops the bleeding but also breaks CI: once
included minutes are gone, hosted jobs are blocked.

An earlier prototype (`docs/ci/appserver-runner.md`) added a `probe-github`
canary + self-hosted Linux fallback on `appserver` to keep the *Linux* gates
alive at $0, but Windows/macOS legs simply **skip** on the fallback path — so
those flavors currently get no coverage while private.

## Goals

1. **Guarantee $0** ongoing GitHub Actions spend — structurally, not just capped.
2. **Cover all three build flavors** (Windows / Linux / macOS) at the best
   fidelity practical.
3. Produce a **general, reusable solution** for the owner's *other* private
   projects — not a MAME-only hack.

## Non-goals

- Native macOS builds on a Linux host (physically impossible; documented as the
  known gap for the private-project path).
- Replacing GitHub as the SCM / PR host. We keep GitHub; we only change *where
  and whether* compute runs and *whether* it can bill.
- Preserving the `probe-github` canary / `CI_FORCE_APPSERVER` machinery for
  MAME — it existed only to dodge private-repo billing and becomes dead weight
  once MAME is public.

## Key insight & decision

**For _public_ repos, standard GitHub-hosted runners (Linux, Windows, macOS at
standard sizes) are free and unlimited.** Only *larger* runners (e.g.
`windows-11-arm`) and *storage overage* can bill on a public repo. Self-hosted
runners consume **zero** hosted minutes and never bill, on public or private
repos alike.

This splits the problem cleanly into two independent halves, which is the
approved **Hybrid** direction:

- **MAME → public.** Its code is GPL/BSD MAME derivative work with no committed
  secrets (verified), so going public is *safe*. Public unlocks free, native,
  full-fidelity Win/Linux/macOS CI and makes the Linux-only-host macOS problem
  vanish entirely. (Approved.)
- **Other private projects → self-hosted playbook.** The already-prototyped
  Dockerized runner + reusable workflow templates become a documented, reusable
  kit for repos that must stay private. (Approved.)

The two halves are simpler apart than forcing one Linux box to fake macOS.

### Pre-decision safety verification (done during brainstorming)

- `gh secret list` → empty (no Actions secrets configured).
- `git grep 'secrets\.'` over `.github/**` → empty (no workflow uses secrets).
- Secret-pattern scan of tracked files → only `3rdparty/asio` **example** test
  certificates (public upstream MAME files), no real credentials.
- Repo is `PRIVATE`, `isFork:false`, Actions enabled, one self-hosted runner
  online (`appserver-mame`, labels `self-hosted,Linux,X64,appserver`).

---

## Section A — MAME public: free native CI on all three OSes

**Theme: simplify.** Most current workflow complexity exists only to dodge
private-repo billing; on a public repo it is removable.

### A1. Make the repo public
Safe per the verification above. This is the single biggest cost lever.

### A2. Strip the canary/fallback from all 9 workflows
Affected: `ci-linux.yml`, `ci-windows.yml`, `ci-macos.yml`, `docs.yml`,
`srcclean.yml`, `hash.yml`, `includeguards.yml`, `language.yml`,
`bgfxshaders.yml`.

For each: delete the `probe-github` job, the `needs: probe-github` gates, the
`if: ${{ ... gh_ok ... }}` conditions, the `fromJSON(... appserver ...)`
`runs-on` fallback arrays, and `CI_FORCE_APPSERVER` references. Every gate runs
on a plain hosted runner (`ubuntu-latest`, `windows-latest`, `macOS-latest`).

The Linux `oracle` job (currently the appserver-only correctness gate) folds
back into `build-linux`, where the differential CPU oracle + `mametests`
already run — no coverage is lost; the standalone appserver-only `oracle` job
is deleted.

### A3. Retire the appserver runner *from this repo*
Reasons: (a) no longer needed — hosted is free; (b) **self-hosted + public is a
security hole** — a fork PR from a stranger could execute code on the box.
Deregister the `appserver-mame` runner from `mame_emu` (the container keeps
running for the private projects in Section B). MAME never targets a
self-hosted label again.

### A4. Remove the one billable leg (`windows-11-arm`)
It is a *larger* runner and bills even on a public repo. Remove it from routine
CI (the `compiler: clang-arm64` matrix entry in `ci-windows.yml`). Re-add arm64
Windows coverage as an opt-in `workflow_dispatch` job if ever wanted.
**Trade-off accepted:** loss of automatic arm64-Windows (clang aarch64 MinGW)
compile coverage; x64 gcc+clang builds and the full CPU oracle are untouched.

### A5. Trim artifact retention (the one residual public-repo cost)
Actions *storage* still bills past the free tier on public repos. All
`upload-artifact` steps get `retention-days: 7`, and artifact upload is
restricted to push-to-`main` events (not PRs). Keeps storage comfortably free.

**Outcome:** native full-fidelity Win + Linux + macOS build *and* test, free and
unlimited, on structurally simpler workflows, with every money-capable knob
removed or made manual.

---

## Section B — reusable self-hosted playbook (for private projects)

Lives in a new dedicated private repo **`ci-selfhosted`** (approved) so its
reusable workflows can be `uses:`-referenced and versioned. Three components:

### B1. Runner provisioning (parameterized)
A template compose block + a one-command `add-runner.sh <repo> <labels>` script
that stamps out a repo-scoped runner for any private repo, preserving the
existing conventions:
- **Repo-scoped** (safe to share the `appserver` label across projects — a
  repo-scoped runner only accepts its own repo's jobs).
- **`EPHEMERAL: true`** (fresh runner per job — hygiene + security).
- **Resource caps** (memory/cpu `deploy.resources.limits`) so one project can't
  starve the box.
- Built on `myoung34/github-runner` (the existing prototype image).
- Canonical compose stays synced with FamilyWorkspace `infra/runner/` per the
  existing source-of-truth note.

### B2. Reusable `workflow_call` templates
Share the *plumbing*, not the build steps (which differ per project):
- A "run on self-hosted, matrix over flavors" skeleton.
- The label convention `[self-hosted, linux, x64, appserver]`.
- The $0-lock conventions baked in (no hosted `runs-on`).
- Consumed via `uses: <owner>/ci-selfhosted/.github/workflows/build.yml@<ref>`
  with the project supplying its own build command as an input.

### B3. Honest cross-OS fidelity matrix (Linux host)

| Flavor | On the Linux self-hosted host | Fidelity |
|---|---|---|
| **Linux** | native | full — build + test |
| **Windows** | cross-compile — MinGW for C/C++ (as MAME); `dotnet`/Go/Rust cross-build natively | build: high · test: partial via Wine, or skip |
| **macOS** | cross-compile only where the toolchain allows (pure-Go easy; Rust needs osxcross+SDK; C/C++ fragile) | build: low–none · **test: none** — add a Mac runner for real macOS |

The playbook documents this up front: a MinGW-style C/C++ private project gets
native Linux + solid Windows cross-compile; **macOS is the gap only Apple
hardware closes.**

---

## Section C — the $0-lock (defense in depth)

Four independent layers; each alone would prevent spend.

### L1 — Spending limit $0 (already set; keep)
The backstop. On **public** MAME, standard runners are free/unlimited, so $0
only guards *larger runners* (blocked outright) and *storage overage* (see L3).
On **private** projects, self-hosted is free regardless, so $0 has nothing to
bill against.

### L2 — Structural: no billable `runs-on` schedulable
- MAME (public): only free `ubuntu/windows/macOS-latest`; the larger-runner leg
  is removed (A4).
- Private projects: every job targets `[self-hosted, …]`; zero hosted
  `runs-on`.
- Enforced by a reusable **"no-billable-runner" lint** (shipped in
  `ci-selfhosted`): scans `.github/workflows/**` and fails on a larger-runner
  label — or, for a private repo, on *any* hosted `runs-on`. Catches accidental
  reintroduction before it runs.

### L3 — Storage retention trimmed
`retention-days: 7` on all `upload-artifact` steps; uploads on push-to-`main`
only. Removes the one cost that survives on public repos. (Same change as A5;
restated here as a cost layer.)

### L4 — Billing smoke detector (in scope, approved)
A scheduled job running **on the free self-hosted runner** that queries the
GitHub billing API and alerts (log/push/email) if **any** nonzero billable
Actions minutes or storage overage appears. Catches regressions L1–L3 might
miss. Requires a `user`-scoped token (the only added setup; noted in the
existing appserver-runner doc as the same prerequisite for quota-aware auto).

---

## Rollout order (high level; detailed steps come from the implementation plan)

1. **Section A (MAME public)** — highest value, lowest risk, immediate cost fix:
   flip visibility (A1), strip canary/fallback (A2), retire appserver from MAME
   (A3), drop arm64 leg (A4), trim retention (A5). Verify CI is green on free
   hosted runners.
2. **Section C L1–L3** — largely folded into A; add the no-billable-runner lint
   (L2) and confirm retention (L3). L1 already in place.
3. **Section B (`ci-selfhosted` repo)** — create the repo, extract the runner
   compose template + `add-runner.sh`, author the reusable `workflow_call`
   templates + the no-billable-runner lint, write the cross-OS playbook doc.
4. **Section C L4** — the billing smoke detector job, scheduled on the
   self-hosted runner, with the `user`-scoped token.

## Risks & caveats

- **Public exposure of methodology.** MAME's ADRs/architecture/improvement-plan
  become public. Accepted: it's GPL'd MAME work, no secrets.
- **arm64-Windows coverage dropped** from routine CI (A4). Mitigation: opt-in
  `workflow_dispatch`.
- **Self-hosted on a public repo is unsafe.** Explicitly avoided (A3) — MAME
  never uses self-hosted once public.
- **macOS for private projects** cannot be native on a Linux host (B3).
  Documented as the known gap; adding a Mac runner is the escape hatch.
- **Billing API token scope.** L4 needs a `user`-scoped token; keep it off the
  public repo (store on the appserver / secret store, not in `mame_emu`).
- **One box, serial CI** for private projects. Acceptable now (MAME leaves the
  box); scale by adding runner replicas if contention appears.

## Verification

- **MAME green on free hosted:** after A, `ci-linux`/`ci-windows`/`ci-macos`
  and the lighter workflows pass on `*-latest` runners with no `probe-github`.
- **No-billable-runner lint** passes on MAME (no larger-runner label) and on
  each private repo (no hosted `runs-on`).
- **Billing stays $0:** L4 monitor reports zero billable minutes + no storage
  overage across a full month; spot-check `gh api` billing endpoints.
- **Reusable kit proven:** at least one private project consumes a
  `ci-selfhosted` reusable workflow and builds Linux natively + Windows
  cross-compiled on the appserver runner.
