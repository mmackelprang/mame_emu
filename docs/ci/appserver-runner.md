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
