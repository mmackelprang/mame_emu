# MAME CI on the appserver self-hosted runner

GitHub-hosted Actions minutes for this account are exhausted, so the **fast
correctness gates** run on a self-hosted GitHub Actions runner on `appserver`,
mirroring the FamilyWorkspace runner pattern. The runner polls GitHub *outbound*
(no inbound webhook, **no GH minutes consumed**).

## What runs where

| Gate | Job | Runner |
|---|---|---|
| Tiny build + `-validate` + reconcile (check) | `ci-linux.yml` → `preflight` | **appserver** |
| Differential CPU oracle (z80 + m6502 + **m68000 full corpus**) + legacy unit tests | `ci-linux.yml` → `oracle` | **appserver** |
| `srcclean` changed-files gate | `srcclean.yml` → `srcclean` | **appserver** |
| Full cross-compiler mame build matrix | `ci-linux.yml` → `build-linux` | GH-hosted, **gated OFF** (`if: vars.RUN_FULL_BUILD == 'true'`) |
| macOS / Windows / docs / translations | their workflows | GH-hosted (dormant until credits / fallback policy) |

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

## Re-enabling GH-hosted heavy builds

Set repo variable `RUN_FULL_BUILD=true` (Settings → Secrets and variables →
Actions → Variables) to re-activate the `build-linux` matrix. This is the seam
for the planned **try-GH-then-fallback-to-appserver** policy.
