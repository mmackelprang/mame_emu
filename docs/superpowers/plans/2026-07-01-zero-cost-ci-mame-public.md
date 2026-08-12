# Zero-Cost CI (MAME public) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move `mame_emu` to a public repo so all Win/Linux/macOS CI runs free on GitHub-hosted runners, delete the private-repo billing-avoidance machinery, and lock spend structurally at $0.

**Architecture:** Public repos get free/unlimited *standard* hosted runners; only *larger* runners and *storage overage* can bill. So we (1) strip the `probe-github` canary + appserver-fallback from all 9 workflows and run each on plain hosted runners, (2) delete the one larger-runner leg (`windows-11-arm`), (3) trim artifact retention, (4) add a cost-guard lint that fails if any billable runner label reappears, then (5) flip the repo public and retire the self-hosted runner from it (self-hosted + public = code-exec risk).

**Tech Stack:** GitHub Actions YAML, `gh` CLI, `python -c "import yaml"` for local workflow validation (no `actionlint` available), `grep` structural asserts.

## Global Constraints

- **Repo:** `mmackelprang/mame_emu` — currently `PRIVATE`, spending limit `$0`, one self-hosted runner `appserver-mame` (`id=100`).
- **Implementation branch:** `ci/zero-cost-mame` (branch off `main`; open a PR per the owner's branch-per-change rule). This plan and its spec live on the separate `docs/plan-local-ci-cd` branch.
- **No `actionlint`** in this environment — validate each edited workflow with `python -c "import yaml,sys; yaml.safe_load(open(sys.argv[1]))" <file>` (pyyaml is present).
- **Free standard runners only** after this plan: `ubuntu-latest`, `windows-latest`, `macOS-latest`. **Never** a larger runner (`windows-11-arm`, `*-large`, `*-xlarge`) and **never** a self-hosted label in this repo once public.
- **Artifact uploads:** `retention-days: 7` and gated `if: ${{ github.event_name == 'push' }}` (no PR uploads) on every `upload-artifact` step.
- **Ordering rule:** All workflow edits (Tasks 1–6) land on `main` *before* the repo is flipped public (Task 7), so the first public CI run already uses clean workflows. Task 7 is **owner-gated** — pause for explicit confirmation before changing visibility.
- **Coverage note (documented, accepted):** dropping `windows-11-arm` removes automatic arm64-Windows (clang aarch64 MinGW) compile coverage. x64 gcc+clang builds and the full CPU oracle are unchanged.

## File Structure

**Modified workflows** (`.github/workflows/`):
- `ci-linux.yml` — remove `probe-github`; `preflight` + `build-linux` run on `ubuntu-latest`; delete the standalone appserver `oracle` job (its passes already run inside `build-linux`).
- `ci-windows.yml` — remove `probe-github`; drop the `clang-arm64` matrix leg; run on `windows-latest`.
- `ci-macos.yml` — remove `probe-github`; run on `macOS-latest`.
- `docs.yml`, `srcclean.yml`, `hash.yml`, `includeguards.yml`, `language.yml`, `bgfxshaders.yml` — remove `probe-github` + `needs`/`if` gates; run directly on their hosted runner (`srcclean` loses its `fromJSON` appserver fallback → plain `ubuntu-latest`).

**Created:**
- `.github/workflows/costguard.yml` — L2 lint: fails if a billable runner label appears in any workflow.

**Doc updated:**
- `docs/ci/appserver-runner.md` — rewritten to reflect: MAME is public, canary retired, appserver retired from this repo, pointer to the forthcoming `ci-selfhosted` playbook for private projects.

**Repo-level actions (Task 7, no files):** flip visibility public, deregister runner `id=100`, delete `CI_FORCE_APPSERVER` variable, stop/remove the `mame-runner` container on appserver, confirm spending limit `$0`.

---

### Task 1: Simplify `ci-linux.yml`

**Files:**
- Modify (full replace): `.github/workflows/ci-linux.yml`

**Interfaces:**
- Consumes: nothing (entry workflow).
- Produces: a `preflight` gate + a `build-linux` matrix (`gcc`/`clang`) that both run the differential CPU oracle; consumed by no other workflow.

- [ ] **Step 1: Replace the whole file with the simplified version**

```yaml
name: CI (Linux)

on:
  push:
    branches:
    - main
    paths:
    - '.github/workflows/**'
    - '3rdparty/**'
    - 'hash/**'
    - 'scripts/**'
    - 'src/**'
    - 'tests/**'
    - 'COPYING'
    - 'makefile'
  pull_request:
    paths:
    - '.github/workflows/**'
    - '3rdparty/**'
    - 'hash/**'
    - 'scripts/**'
    - 'src/**'
    - 'tests/**'
    - 'COPYING'
    - 'makefile'

permissions:
  contents: read

jobs:
  # Fast pre-flight that gates the multi-hour matrix legs (ADR 0005 decision #3).
  # Builds only the tiny slice (SUBTARGET=tiny -> mametiny), runs -validate and
  # the reconcile CHECK form (never --fix) so obvious breakage fails in minutes.
  preflight:
    runs-on: ubuntu-latest
    steps:
    - uses: actions/checkout@main
      with:
        fetch-depth: 0
    - name: Install dependencies
      run: |
        sudo apt-get update
        sudo apt-get install -y libsdl2-dev libsdl2-ttf-dev libfontconfig-dev libasound2-dev libxinerama-dev libxi-dev qt6-base-dev qt6-base-dev-tools
    - name: Build tiny slice
      env:
        SUBTARGET: tiny
        TOOLS: 1
      run: make -j"$(nproc)"
    - name: Validate
      run: ./mametiny -validate
    - name: Reconcile driver list (check only)
      run: ./mametiny -listxml | python3 scripts/build/makedep.py reconcilelist -l src/mame/tiny.lst -

  # Full cross-compiler mame build matrix. Each leg builds + runs the
  # differential CPU oracle incl. the m68000 DRC Leg B passes (ADR 0001/0006/0007).
  # The clang/mame leg is the canonical full-corpus gate (oracle_cap empty == all);
  # the gcc/tiny leg runs a 64-file subset to avoid replaying the full corpus twice.
  build-linux:
    needs: preflight
    strategy:
      matrix:
        compiler: [gcc, clang]
        include:
          - compiler: gcc
            cc: gcc
            cxx: g++
            archopts: -U_FORTIFY_SOURCE
            subtarget: tiny
            executable: mametiny
            oracle_cap: "64"
          - compiler: clang
            cc: clang
            cxx: clang++
            subtarget: mame
            executable: mame
            oracle_cap: ""
    runs-on: ubuntu-latest
    steps:
    - uses: actions/checkout@main
      with:
        fetch-depth: 0
    - name: Install dependencies
      run: |
        sudo apt-get update
        sudo apt-get install -y libsdl2-dev libsdl2-ttf-dev libfontconfig-dev libasound2-dev libxinerama-dev libxi-dev qt6-base-dev qt6-base-dev-tools
    - name: Install clang
      if: matrix.compiler == 'clang'
      run: sudo apt-get install -y clang
    - name: Build
      env:
        OVERRIDE_CC: ${{ matrix.cc }}
        OVERRIDE_CXX: ${{ matrix.cxx }}
        ARCHOPTS: ${{ matrix.archopts }}
        SUBTARGET: ${{ matrix.subtarget }}
        TOOLS: 1
        TESTS: 1
      run: make -j3
    - name: Validate
      run: ./${{ matrix.executable }} -validate
    - name: Reconcile driver list
      run: ./${{ matrix.executable }} -listxml | python3 scripts/build/makedep.py reconcilelist -l src/mame/${{ matrix.subtarget }}.lst -
    - name: Fetch CPU oracle vectors
      run: python3 tests/cpuoracle/fetch_vectors.py --cores z80,m6502,m68000
    - name: Run mametests (oracle + legacy unit tests)
      env:
        CPUORACLE_MAX_FILES: ${{ matrix.oracle_cap }}
      run: ./mametests
    - name: Run m68000 DRC Leg B -- fully-granted pass (native write, ADR 0007 W4)
      env:
        CPUORACLE_MAX_FILES: ${{ matrix.oracle_cap }}
        CPUORACLE_M68_DRC_FULLGRANT: "1"
      run: ./mametests "[m68000][drc]"
    - name: Run m68000 DRC Leg B -- C backend (drcbec)
      env:
        CPUORACLE_MAX_FILES: ${{ matrix.oracle_cap }}
        CPUORACLE_M68_DRC_C: "1"
      run: ./mametests "[m68000][drc]"
    - name: Run m68000 DRC Leg B -- fully-granted C backend
      env:
        CPUORACLE_MAX_FILES: ${{ matrix.oracle_cap }}
        CPUORACLE_M68_DRC_FULLGRANT: "1"
        CPUORACLE_M68_DRC_C: "1"
      run: ./mametests "[m68000][drc]"
    - name: Run m68000 DRC Leg B -- partial-grant pass (mid-instruction resume)
      env:
        CPUORACLE_MAX_FILES: ${{ matrix.oracle_cap }}
        CPUORACLE_M68_DRC_PARTGRANT: "1"
      run: ./mametests "[m68000][drc]"
    - name: Run m68000 DRC Leg B -- partial-grant C backend
      env:
        CPUORACLE_MAX_FILES: ${{ matrix.oracle_cap }}
        CPUORACLE_M68_DRC_PARTGRANT: "1"
        CPUORACLE_M68_DRC_C: "1"
      run: ./mametests "[m68000][drc]"
    - name: ORM check
      run: python3 scripts/minimaws/minimaws.py load --executable ./${{ matrix.executable }} --softwarepath hash
    - uses: actions/upload-artifact@main
      if: ${{ github.event_name == 'push' }}
      with:
        name: ${{ matrix.executable }}-linux-${{ matrix.compiler }}-${{ github.sha }}
        path: |
          ${{ matrix.executable }}
          chdman
          unidasm
        if-no-files-found: error
        retention-days: 7
```

- [ ] **Step 2: Validate YAML + assert the removals**

Run:
```bash
python -c "import yaml,sys; yaml.safe_load(open('.github/workflows/ci-linux.yml'))" && echo "YAML OK"
grep -nE 'probe-github|gh_ok|CI_FORCE_APPSERVER|self-hosted|appserver|fromJSON' .github/workflows/ci-linux.yml && echo "FOUND (should be none)" || echo "clean: no canary/fallback refs"
grep -c 'CPUORACLE_M68_DRC' .github/workflows/ci-linux.yml
```
Expected: `YAML OK`; `clean: no canary/fallback refs`; the DRC-pass count prints `10` (5 passes × 2 legs) — confirming the oracle passes survive inside `build-linux`.

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/ci-linux.yml
git commit -m "ci(linux): drop probe-github canary + appserver fallback; fold oracle into build-linux; trim artifact retention"
```

---

### Task 2: Simplify `ci-windows.yml` and drop the arm64 larger-runner leg

**Files:**
- Modify (full replace): `.github/workflows/ci-windows.yml`

**Interfaces:**
- Consumes: nothing.
- Produces: a `build-windows` matrix with exactly two legs — `gcc-x64` (UCRT64) and `clang-x64` (CLANG64). No arm64 leg.

- [ ] **Step 1: Replace the whole file with the simplified version (arm64 leg removed)**

```yaml
name: CI (Windows)

on:
  push:
    branches:
    - main
    paths:
    - '.github/workflows/**'
    - '3rdparty/**'
    - 'scripts/**'
    - 'src/**'
    - 'tests/**'
    - 'COPYING'
    - 'makefile'
  pull_request:
    paths:
    - '.github/workflows/**'
    - '3rdparty/**'
    - 'scripts/**'
    - 'src/**'
    - 'tests/**'
    - 'COPYING'
    - 'makefile'

permissions:
  contents: read

jobs:
  build-windows:
    strategy:
      matrix:
        compiler: [gcc-x64, clang-x64]
        include:
          - compiler: gcc-x64
            os: windows-latest
            msys: UCRT64
            slug: mingw-w64-ucrt-x86_64
            cc: gcc
            cxx: g++
            subtarget: mame
            executable: mame
          - compiler: clang-x64
            os: windows-latest
            msys: CLANG64
            slug: mingw-w64-clang-x86_64
            extrapkg: mingw-w64-clang-x86_64-gcc-compat
            cc: clang
            cxx: clang++
            subtarget: tiny
            executable: mametiny
    runs-on: ${{ matrix.os }}
    defaults:
      run:
        shell: msys2 {0}
    steps:
    - uses: msys2/setup-msys2@e9898307ac31d1a803454791be09ab9973336e1c # v2.31.1
      with:
        msystem: ${{ matrix.msys }}
        install: git make ${{ matrix.slug }}-${{ matrix.cc }} ${{ matrix.slug }}-python ${{ matrix.slug }}-lld ${{ matrix.slug }}-llvm ${{ matrix.slug }}-libc++ ${{ matrix.extrapkg }}
    - uses: actions/checkout@main
      with:
        fetch-depth: 0
    - name: Build
      env:
        OVERRIDE_AR: "llvm-ar"
        OVERRIDE_CC: ${{ matrix.cc }}
        OVERRIDE_CXX: ${{ matrix.cxx }}
        ARCHOPTS: "-fuse-ld=lld"
        SUBTARGET: ${{ matrix.subtarget }}
        TOOLS: 1
        TESTS: 1
      run: make -j3
    - name: Validate
      run: ./${{ matrix.executable }}.exe -validate
    - name: Fetch CPU oracle vectors
      run: python tests/cpuoracle/fetch_vectors.py --cores z80,m6502,m68000
    - name: Run mametests (oracle subset + legacy unit tests)
      env:
        CPUORACLE_MAX_FILES: 64
      run: ./mametests.exe
    - uses: actions/upload-artifact@main
      if: ${{ github.event_name == 'push' }}
      with:
        name: ${{ matrix.executable }}-windows-${{ matrix.compiler }}-${{ github.sha }}
        path: |
          ${{ matrix.executable }}.exe
          chdman.exe
          unidasm.exe
        if-no-files-found: error
        retention-days: 7
```

- [ ] **Step 2: Validate YAML + assert arm64 and canary are gone**

Run:
```bash
python -c "import yaml,sys; yaml.safe_load(open('.github/workflows/ci-windows.yml'))" && echo "YAML OK"
grep -nE 'probe-github|gh_ok|arm64|windows-11-arm|CLANGARM64' .github/workflows/ci-windows.yml && echo "FOUND (should be none)" || echo "clean: no arm64/canary refs"
```
Expected: `YAML OK`; `clean: no arm64/canary refs`.

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/ci-windows.yml
git commit -m "ci(windows): drop probe-github + billable windows-11-arm leg; trim artifact retention"
```

---

### Task 3: Simplify `ci-macos.yml`

**Files:**
- Modify (full replace): `.github/workflows/ci-macos.yml`

**Interfaces:**
- Consumes: nothing.
- Produces: a single `build-macos` job on `macOS-latest`.

- [ ] **Step 1: Replace the whole file with the simplified version**

```yaml
name: CI (macOS)

on:
  push:
    branches:
    - main
    paths:
    - '.github/workflows/**'
    - '3rdparty/**'
    - 'scripts/**'
    - 'src/**'
    - 'tests/**'
    - 'COPYING'
    - 'makefile'
  pull_request:
    paths:
    - '.github/workflows/**'
    - '3rdparty/**'
    - 'scripts/**'
    - 'src/**'
    - 'tests/**'
    - 'COPYING'
    - 'makefile'

permissions:
  contents: read

jobs:
  build-macos:
    runs-on: macOS-latest
    steps:
    - uses: actions/checkout@main
      with:
        fetch-depth: 0
    - name: Install dependencies
      run: brew install python3 sdl3
    - name: Build
      env:
        USE_LIBSDL: 1
        TOOLS: 1
        TESTS: 1
      run: make -j2
    - name: Validate
      run: ./mame -validate
    - name: Fetch CPU oracle vectors
      run: python3 tests/cpuoracle/fetch_vectors.py --cores z80,m6502,m68000
    - name: Run mametests (oracle subset + legacy unit tests)
      env:
        CPUORACLE_MAX_FILES: 64
      run: ./mametests
    - uses: actions/upload-artifact@main
      if: ${{ github.event_name == 'push' }}
      with:
        name: mame-macos-${{ github.sha }}
        path: |
          mame
          chdman
          unidasm
        if-no-files-found: error
        retention-days: 7
```

- [ ] **Step 2: Validate YAML + assert canary gone**

Run:
```bash
python -c "import yaml,sys; yaml.safe_load(open('.github/workflows/ci-macos.yml'))" && echo "YAML OK"
grep -nE 'probe-github|gh_ok' .github/workflows/ci-macos.yml && echo "FOUND (should be none)" || echo "clean"
```
Expected: `YAML OK`; `clean`.

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/ci-macos.yml
git commit -m "ci(macos): drop probe-github canary; trim artifact retention"
```

---

### Task 4: Simplify the six lightweight workflows

Same mechanical transform for each: delete the entire `probe-github:` job, delete `needs: probe-github`, delete the `if: ${{ !cancelled() && needs.probe-github.outputs.gh_ok == 'true' }}` line (and for `srcclean`, the `if: ${{ !cancelled() }}` line), keep the job's own `runs-on`. Add retention/push-gating on any `upload-artifact`.

**Files:**
- Modify: `.github/workflows/docs.yml`
- Modify: `.github/workflows/srcclean.yml`
- Modify: `.github/workflows/hash.yml`
- Modify: `.github/workflows/includeguards.yml`
- Modify: `.github/workflows/language.yml`
- Modify: `.github/workflows/bgfxshaders.yml`

**Interfaces:**
- Consumes: nothing.
- Produces: each workflow's single job runs directly on its hosted runner.

- [ ] **Step 1: `docs.yml`** — delete the `probe-github` job; on `build-docs` remove `needs: probe-github` and the `if:` line so it reads `runs-on: ubuntu-latest`; on the `upload-artifact` step add `if: ${{ github.event_name == 'push' }}` and `retention-days: 7`. Resulting `jobs:` block:

```yaml
jobs:
  build-docs:
    runs-on: ubuntu-latest
    steps:
    - uses: actions/checkout@main
      with:
        fetch-depth: 0
    - name: Install dependencies
      run: |
        sudo apt-get update
        sudo apt-get install -y librsvg2-bin latexmk python3-pip python3-sphinx texlive texlive-formats-extra texlive-science
        pip3 install sphinxcontrib-svg2pdfconverter
    - name: Build HTML
      run: make -C docs html
    - name: Build PDF
      run: make -C docs PAPER=a4 latexpdf
    - uses: actions/upload-artifact@main
      if: ${{ github.event_name == 'push' }}
      with:
        name: mame-docs-${{ github.sha }}
        path: |
          docs/build/html
          docs/build/latex/MAME.pdf
        if-no-files-found: error
        retention-days: 7
```

- [ ] **Step 2: `srcclean.yml`** — delete the `probe-github` job; on the `srcclean` job remove `needs: probe-github` and the `if: ${{ !cancelled() }}` line, and change the `runs-on:` from the `fromJSON(...)` fallback expression to plain `runs-on: ubuntu-latest`. Leave every other step (build srcclean, resolve changed files, assert no diff) exactly as-is. The job header must read:

```yaml
  srcclean:
    runs-on: ubuntu-latest
    steps:
    - uses: actions/checkout@main
      with:
        fetch-depth: 0
```

- [ ] **Step 3: `hash.yml`** — delete the `probe-github` job; on `validate` remove `needs: probe-github` and the `if:` line, keep `runs-on: ubuntu-latest`. Header:

```yaml
  validate:
    runs-on: ubuntu-latest
    steps:
    - uses: actions/checkout@main
      with:
        fetch-depth: 0
```

- [ ] **Step 4: `includeguards.yml`** — identical transform to Step 3 (`validate` job on `ubuntu-latest`, `probe-github` deleted, `needs`/`if` removed). Header is the same three lines as Step 3.

- [ ] **Step 5: `language.yml`** — delete the `probe-github` job; on `build-language` remove `needs`/`if`, keep `runs-on: ubuntu-latest`; add `if: ${{ github.event_name == 'push' }}` + `retention-days: 7` to the upload. Resulting `jobs:` block:

```yaml
jobs:
  build-language:
    runs-on: ubuntu-latest
    steps:
    - uses: actions/checkout@main
      with:
        fetch-depth: 0
    - name: Compile message catalogs
      run: for x in language/*/*.po ; do python scripts/build/msgfmt.py --output-file "`dirname "$x"`/`basename "$x" .po`.mo" "$x" ; done
    - uses: actions/upload-artifact@main
      if: ${{ github.event_name == 'push' }}
      with:
        name: mame-language-${{ github.sha }}
        path: language/*/*.mo
        if-no-files-found: error
        retention-days: 7
```

- [ ] **Step 6: `bgfxshaders.yml`** — delete the `probe-github` job; on `rebuild` remove `needs`/`if`, keep `runs-on: windows-latest`; add `if: ${{ github.event_name == 'push' }}` + `retention-days: 7` to the upload. Header:

```yaml
  rebuild:
    runs-on: windows-latest
    defaults:
      run:
        shell: msys2 {0}
```
and the upload step:
```yaml
    - uses: actions/upload-artifact@main
      if: ${{ github.event_name == 'push' }}
      with:
        name: mame-bgfx-${{ github.sha }}
        path: bgfx/shaders
        if-no-files-found: error
        retention-days: 7
```

- [ ] **Step 7: Validate all six + assert canary fully gone**

Run:
```bash
for f in docs srcclean hash includeguards language bgfxshaders; do
  python -c "import yaml,sys; yaml.safe_load(open('.github/workflows/$f.yml'))" && echo "$f: YAML OK"
done
grep -rlnE 'probe-github|gh_ok|CI_FORCE_APPSERVER|fromJSON' .github/workflows/ && echo "FOUND (should be none)" || echo "clean: canary fully removed from all workflows"
```
Expected: six `YAML OK` lines, then `clean: canary fully removed from all workflows`.

- [ ] **Step 8: Commit**

```bash
git add .github/workflows/docs.yml .github/workflows/srcclean.yml .github/workflows/hash.yml .github/workflows/includeguards.yml .github/workflows/language.yml .github/workflows/bgfxshaders.yml
git commit -m "ci: drop probe-github canary from the six lightweight workflows; trim artifact retention"
```

---

### Task 5: Add the cost-guard lint (L2)

**Files:**
- Create: `.github/workflows/costguard.yml`

**Interfaces:**
- Consumes: the `.github/workflows/` tree.
- Produces: a `no-billable-runners` job that fails CI if any workflow references a larger/billable runner.

- [ ] **Step 1: Write a local failing test — a temporary billable label the guard must reject**

Create a throwaway file to prove the detector works:
```bash
printf 'jobs:\n  x:\n    runs-on: windows-11-arm\n' > .github/workflows/_costguard_probe.yml
```

- [ ] **Step 2: Create `.github/workflows/costguard.yml`**

```yaml
name: Cost guard (no billable runners)

# This repo is PUBLIC: standard ubuntu/windows/macOS-latest runners are free and
# unlimited. The only Actions cost vectors are (a) *larger* runners (billed even
# on public repos) and (b) storage overage (handled by retention-days elsewhere).
# This gate fails if any workflow reintroduces a larger/billable runner label.

on:
  push:
    branches:
    - main
    paths:
    - '.github/workflows/**'
  pull_request:
    paths:
    - '.github/workflows/**'

permissions:
  contents: read

jobs:
  no-billable-runners:
    runs-on: ubuntu-latest
    steps:
    - uses: actions/checkout@main
    - name: Assert no larger/billable runner labels are referenced
      run: |
        set -euo pipefail
        # Known GitHub larger-runner labels that bill on public repos, plus the
        # generic *-large / *-xlarge suffix convention. Standard *-latest labels
        # do not match. Exclude this guard file's own comments from the scan.
        if grep -rInE 'runs-on:[^#]*(windows-11-arm|[a-z0-9_-]+-(large|xlarge))\b' .github/workflows/ ; then
          echo "::error::A billable/larger runner label is referenced above. Public-repo CI must use only free standard runners (ubuntu/windows/macOS-latest)."
          exit 1
        fi
        echo "cost guard: no billable runner labels found."
```

- [ ] **Step 3: Run the guard's grep locally against the probe — verify it FAILS**

Run:
```bash
grep -rInE 'runs-on:[^#]*(windows-11-arm|[a-z0-9_-]+-(large|xlarge))\b' .github/workflows/ ; echo "exit=$?"
```
Expected: the line from `_costguard_probe.yml` prints and `exit=0` (grep found a match → the guard step would `exit 1`). This confirms the detector catches a billable label.

- [ ] **Step 4: Remove the probe — verify the guard now PASSES**

Run:
```bash
rm .github/workflows/_costguard_probe.yml
grep -rInE 'runs-on:[^#]*(windows-11-arm|[a-z0-9_-]+-(large|xlarge))\b' .github/workflows/ ; echo "exit=$?"
python -c "import yaml,sys; yaml.safe_load(open('.github/workflows/costguard.yml'))" && echo "YAML OK"
```
Expected: no matches, `exit=1` (grep found nothing → guard step passes); `YAML OK`.

- [ ] **Step 5: Commit**

```bash
git add .github/workflows/costguard.yml
git commit -m "ci: add cost-guard lint failing on any billable/larger runner label"
```

---

### Task 6: Rewrite `docs/ci/appserver-runner.md` for the new reality

**Files:**
- Modify (full replace): `docs/ci/appserver-runner.md`

**Interfaces:**
- Consumes: nothing.
- Produces: accurate operator docs; points private projects to the forthcoming `ci-selfhosted` playbook.

- [ ] **Step 1: Replace the file contents**

```markdown
# MAME CI — public repo, free hosted runners

`mame_emu` is a **public** repo, so all CI runs on **free, unlimited** standard
GitHub-hosted runners (`ubuntu-latest`, `windows-latest`, `macOS-latest`). There
is no self-hosted runner and no billing-avoidance machinery in this repo.

## What runs where

| Gate | Workflow | Runner |
|---|---|---|
| Tiny build + `-validate` + reconcile (check) | `ci-linux.yml` → `preflight` | `ubuntu-latest` |
| Full cross-compiler build + differential CPU oracle (z80 + m6502 + m68000 full corpus) + m68000 DRC Leg B | `ci-linux.yml` → `build-linux` | `ubuntu-latest` |
| Windows x64 build + oracle subset (gcc UCRT64, clang CLANG64) | `ci-windows.yml` | `windows-latest` |
| macOS build + oracle subset | `ci-macos.yml` | `macOS-latest` |
| `srcclean` / docs / translations / include-guards / XML+JSON / BGFX shaders | their workflows | hosted |
| No billable runner reintroduced | `costguard.yml` | `ubuntu-latest` |

## Why there is no self-hosted runner here

Standard hosted runners are free on public repos, so a self-hosted runner buys
nothing — and self-hosted + public is a security hole (a fork PR could execute
code on the box). The `appserver-mame` runner was deregistered from this repo on
going public.

## Cost controls (defense in depth)

- **Spending limit `$0`** — blocks the only paid vectors (larger runners; storage
  overage).
- **No billable `runs-on`** — enforced by `costguard.yml`; the `windows-11-arm`
  larger-runner leg was removed (re-add arm64 as a manual `workflow_dispatch`
  job only if needed).
- **Artifact retention `7` days, push-to-main only** — keeps Actions storage in
  the free tier.

## Private projects

The reusable self-hosted-runner playbook for repos that must stay private (the
Dockerized runner, reusable `workflow_call` templates, the cross-OS matrix, and
the billing smoke-detector) lives in the separate **`ci-selfhosted`** repo. See
its README.
```

- [ ] **Step 2: Validate + commit**

Run:
```bash
grep -niE 'canary|probe-github|CI_FORCE_APPSERVER|fallback' docs/ci/appserver-runner.md && echo "stale refs remain" || echo "doc clean"
```
Expected: `doc clean`.

```bash
git add docs/ci/appserver-runner.md
git commit -m "docs(ci): rewrite appserver-runner.md for public MAME + free hosted CI"
```

---

### Task 7: Open the PR, merge, then flip the repo public — OWNER-GATED

> **Do not run the visibility change without explicit owner confirmation at this step.** Everything before this point is reversible; going public is a deliberate, owner-approved action.

**Files:** none (repo settings + appserver container).

**Interfaces:**
- Consumes: Tasks 1–6 merged to `main`.
- Produces: a public repo whose first CI run uses the clean workflows; the self-hosted runner retired from this repo.

- [ ] **Step 1: Push the branch and open the PR**

```bash
git push -u origin ci/zero-cost-mame
gh pr create --base main --head ci/zero-cost-mame \
  --title "Zero-cost CI: public MAME on free hosted runners" \
  --body "Strips the probe-github canary + appserver fallback from all workflows, drops the billable windows-11-arm leg, trims artifact retention, and adds a cost-guard lint. Precedes flipping the repo public. Spec: docs/superpowers/specs/2026-07-01-local-ci-cd-design.md"
```

- [ ] **Step 2: Merge to `main`**

While the repo is still private at `$0`, hosted CI is blocked, so the PR checks will not run — that is expected. The per-task local YAML/grep asserts above are the validation. Merge with admin override:
```bash
gh pr merge ci/zero-cost-mame --merge --admin
git checkout main && git pull
```

- [ ] **Step 3: Confirm main is clean before going public**

Run:
```bash
grep -rlnE 'probe-github|gh_ok|windows-11-arm|CI_FORCE_APPSERVER|self-hosted|appserver' .github/workflows/ && echo "STOP: billing/canary refs still on main" || echo "main clean — safe to go public"
```
Expected: `main clean — safe to go public`. If anything prints, STOP and fix before Step 4.

- [ ] **Step 4: Flip the repo public (owner confirms first)**

```bash
gh repo edit mmackelprang/mame_emu --visibility public --accept-visibility-change-consequences
gh repo view mmackelprang/mame_emu --json visibility
```
Expected: `{"visibility":"PUBLIC"}`.

- [ ] **Step 5: Retire the self-hosted runner from this repo**

Deregister from the repo (security — no fork PR can target it):
```bash
gh api -X DELETE repos/mmackelprang/mame_emu/actions/runners/100
gh api repos/mmackelprang/mame_emu/actions/runners --jq '.total_count'
```
Expected: `0`.

Then stop/remove the container on the appserver and drop its compose block (sync the canonical copy in FamilyWorkspace `infra/runner/`):
```bash
ssh appserver 'cd /srv/gha-runners && docker compose -f compose.runner.yml stop mame-runner && docker compose -f compose.runner.yml rm -f mame-runner'
```
Then edit `/srv/gha-runners/compose.runner.yml` to delete the `mame-runner:` service block, and mirror the deletion into the FamilyWorkspace `infra/runner/compose.runner.yml` source of truth.

- [ ] **Step 6: Delete the now-unused kill-switch variable**

```bash
gh variable delete CI_FORCE_APPSERVER
gh variable list
```
Expected: `CI_FORCE_APPSERVER` no longer listed.

- [ ] **Step 7: Trigger a run and verify it is green on free hosted runners**

```bash
gh workflow run ci-linux.yml --ref main 2>/dev/null || git commit --allow-empty -m "ci: trigger first public run" && git push
gh run watch "$(gh run list --workflow=ci-linux.yml -L1 --json databaseId --jq '.[0].databaseId')" --exit-status
```
Expected: the run completes green, and `gh run view` shows every job's `runs-on` resolved to `ubuntu-latest` (no `appserver`/self-hosted).

- [ ] **Step 8: Confirm spending stays $0**

- In the GitHub billing UI (Settings → Billing → Spending limit), confirm the Actions spending limit is `$0`.
- Confirm no billable usage appears after the first public run:
```bash
gh api /repos/mmackelprang/mame_emu/actions/runners --jq '.total_count'   # expect 0 self-hosted
```
Expected: `0`. Billable-minutes verification (account-level) is handled by the L4 monitor in the follow-on `ci-selfhosted` plan.

---

## Self-Review

**1. Spec coverage (Section A + C-L1/L2/L3 — the mame_emu subsystem):**
- A1 make public → Task 7 Step 4. ✓
- A2 strip canary/fallback from all 9 workflows → Tasks 1–4. ✓
- A3 retire appserver runner from repo → Task 7 Step 5. ✓
- A4 drop `windows-11-arm` → Task 2. ✓
- A5 trim artifact retention → Tasks 1–4 (every upload gets `retention-days: 7` + push-only). ✓
- C-L1 spending limit $0 verified → Task 7 Step 8. ✓
- C-L2 no-billable-runner lint → Task 5. ✓
- C-L3 storage retention → same as A5. ✓
- Doc reflecting new reality → Task 6. ✓
- **Deferred to the follow-on `ci-selfhosted` plan (by design):** Section B (runner kit + reusable templates + cross-OS matrix) and C-L4 (billing smoke detector — must live in a private repo, never in public MAME). Flagged in the plan header.

**2. Placeholder scan:** No TBD/TODO; every step has concrete YAML or commands with expected output. ✓

**3. Type/label consistency:** runner labels used consistently (`ubuntu-latest`/`windows-latest`/`macOS-latest`); the `probe-github`/`gh_ok`/`CI_FORCE_APPSERVER`/`fromJSON`/`self-hosted`/`appserver` tokens are asserted-absent by the same grep in Tasks 1–4, 6 and Task 7 Step 3; runner `id=100` and variable name `CI_FORCE_APPSERVER` match the live values gathered before writing. ✓
