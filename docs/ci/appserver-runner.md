# MAME CI on the appserver self-hosted runner

GitHub-hosted Actions minutes for this account are exhausted, so the **fast
correctness gates** run on a self-hosted GitHub Actions runner on `appserver`,
mirroring the FamilyWorkspace runner pattern. The runner polls GitHub *outbound*
(no inbound webhook, **no GH minutes consumed**).

## What runs where

| Gate | Job | Runner |
|---|---|---|
| Tiny build + `-validate` + reconcile (check) | `ci-linux.yml` → `preflight` | GH if available, else **appserver** |
| Differential CPU oracle (z80 + m6502 + **m68000 full corpus**) + legacy unit tests | `ci-linux.yml` → `oracle` | **appserver** (fallback path only) |
| `srcclean` changed-files gate | `srcclean.yml` → `srcclean` | GH if available, else **appserver** |
| Full cross-compiler mame build matrix | `ci-linux.yml` → `build-linux` | GitHub-hosted; runs only when GH backend is active |
| macOS / Windows / docs / translations | their workflows | GitHub-hosted (run when GH is available) |

The `oracle` job builds only the **tiny slice + the `mametests` Catch2 binary**
(`SUBTARGET=tiny TESTS=1`) — minutes, not the multi-hour full-mame build — then
runs the full corpus with no cap (ADR 0006 owner decision #3).

## The runner (on appserver)

A repo-scoped [`myoung34/github-runner`](https://github.com/myoung34/docker-github-actions-runner)
container, defined in `/srv/gha-runners/compose.runner.yml` alongside the
FamilyWorkspace and RTest runners (same shared `${FW_RUNNER_PAT}` from
`/srv/gha-runners/.env` — a fine-grained token scoped to all of Mark's repos):

```yaml
  mame-runner:
    image: myoung34/github-runner:ubuntu-noble   # 24.04, matches ubuntu-latest
    container_name: gha-runner-mame
    restart: unless-stopped
    environment:
      RUNNER_NAME: appserver-mame
      RUNNER_SCOPE: repo
      REPO_URL: https://github.com/mmackelprang/mame_emu
      ACCESS_TOKEN: ${FW_RUNNER_PAT}
      LABELS: self-hosted,linux,x64,appserver
      EPHEMERAL: "true"
      DISABLE_AUTOMATIC_DEREGISTRATION: "false"
    volumes:
      - gha-runner-mame-work:/_work
    deploy: { resources: { limits: { memory: 24G, cpus: "14" } } }
```

Repo-scoped runners only accept jobs from their own repo, so sharing the
`appserver` label with the FW/RTest runners is safe.

### Operate

```sh
ssh appserver
cd /srv/gha-runners
docker compose -f compose.runner.yml up -d mame-runner   # create / start
docker logs gha-runner-mame                              # registration / job logs
docker compose -f compose.runner.yml restart mame-runner # bounce
```

Online status: <https://github.com/mmackelprang/mame_emu/settings/actions/runners>
or `gh api repos/mmackelprang/mame_emu/actions/runners`.

> **Source-of-truth note:** the canonical `compose.runner.yml` lives in the
> FamilyWorkspace repo (`infra/runner/`); the `mame-runner` block above should be
> synced there for parity (a backup of the pre-edit live file is at
> `/srv/gha-runners/compose.runner.yml.bak-pre-mame-2026-06-26`).

## Fallback policy — try GitHub-hosted, fall back to appserver (canary)

Each Linux workflow starts with a tiny **`probe-github`** job on `ubuntu-latest`.
If GitHub-hosted is available the probe runs and emits `gh_ok=true`, so the Linux
gates run on GitHub (`ubuntu-latest`) and the heavy `build-linux` matrix +
macOS/Windows legs run too. If GitHub Actions minutes are exhausted the probe
cannot run, `gh_ok` is unset, and the gates **fall back to the appserver runner**
(free; never consumes minutes). `continue-on-error` keeps the run green on
fallback. When on appserver the standalone `oracle` job is the correctness gate
(the `build-linux` matrix is skipped — the single box can't replicate the
cross-compiler matrix, and it would otherwise run the oracle twice).

**GitHub-hosted-only legs** (`ci-windows`, `ci-macos`, `docs`, `language`,
`includeguards`, `bgfxshaders`, `hash`) have **no appserver fallback** — they
can't run on the Linux self-hosted runner. They carry the same `probe-github`
gate and simply **skip** when GitHub isn't active (forced-appserver or
quota-out), so they never burn credits while CI is on appserver. They resume
automatically when the probe succeeds again (GitHub minutes available and
`CI_FORCE_APPSERVER` unset).

### Kill-switch / manual override
Set repo variable **`CI_FORCE_APPSERVER=true`** to skip the probe and force every
gate onto appserver — the guaranteed escape hatch. Use it when you *know* GH is
out (avoids each run's probe attempt and any heavy GH matrix), or if the
auto-detection ever misbehaves (e.g. transient GH saturation, where GitHub queues
rather than fails — `timeout-minutes` does not tick while a job is queued).

```sh
gh variable set CI_FORCE_APPSERVER --body true    # force appserver
gh variable delete CI_FORCE_APPSERVER             # re-enable the auto-canary
```

### Verification status (honest)
The **forced-appserver** path (kill-switch on) and the **GitHub-available** path
(probe ✓ → `ubuntu-latest`) are deterministically verifiable. The **quota-out
auto-detect** path only exercises when the account is actually out of minutes and
relies on GitHub running the self-hosted gates while failing the hosted probe in
the same run — *probably* fine but unverified here; the kill-switch bounds the
risk (one variable forces appserver). If quota-aware auto becomes worth the
robustness, a scheduled job reading the billing API (needs a `user`-scoped token)
replaces the probe cleanly.
