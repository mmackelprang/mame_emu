# Phase 3 — Web control surface (Pick 4)

> **Status:** Planned · **Consumes:** ADR [0004](../adr/0004-web-control-surface.md)
> **Related:** reuses [0003](../adr/0003-frontend-usability-quick-wins.md)'s audit/config data feeds
> **Spec:** [`../specs/2026-06-24-mame-improvements-design.md`](../specs/2026-06-24-mame-improvements-design.md)
> **Date:** 2026-06-24

## Goal

Build a **REST control surface + static SPA** over MAME's existing frontend-level
`http_manager`, scoped to launcher / audit / config / input — explicitly **not** a live
pixel stream. Per the resolved decision, **v1 is READ-WRITE**:

- GET **and write** endpoints for `/api/{systems,audit,config,input}` (under a `/api/v1/`
  prefix).
- Because writes ship in v1, the **`http.cpp:185` body-read fix** and an **auth model** are
  **first-class v1 tasks**, not deferred.
- **Static-handler path-traversal hardening** and **localhost-default bind** are
  non-negotiable v1 prerequisites.

The server is **already frontend-owned** (`machine_manager` holds
`unique_ptr<http_manager> m_http`, `src/emu/main.h:99`) and created **before** any machine
(`clifront.cpp:265`), so it can serve with no system loaded — the missing piece is
**frontend-scoped handler registration**.

## ADR implemented

[0004](../adr/0004-web-control-surface.md) — the entire phase, upgraded to read-write per
the resolved decision.

## Prerequisites

1. **Phase 1 complete** — GNU `make` installed; `mametests`/`srcclean` gated in CI (Phase 3
   adds its handler + security tests to that suite); the iteration loop verified.
   Phase 3 is **independent of Phase 2**.
2. The existing HTTP subsystem: opt-in via `OPTION_HTTP`/`OPTION_HTTP_PORT`/
   `OPTION_HTTP_ROOT` (`emuopts.h:201-203`, `emuopts.cpp:225-227`); server stood up in
   `machine_manager::start_http_server()` (`main.cpp:31-34`); the one existing endpoint is
   `GET /api/machine` (`machine.cpp:1246-1267`), registered only while a machine runs via
   `export_http_api()` (`machine.cpp:341`).
3. Data feeds available: `infoxml.cpp` (systems metadata), `audit.cpp:498-579` (per-ROM
   detail), `emuopts.cpp` / `config.cpp` (options/config), input ports.

## Security gating order (governs task sequencing)

Per ADR 0004 §4, **bind-localhost-default + traversal-hardening are non-negotiable before
exposing anything**, and **the body-read fix + auth must precede any write endpoint**.
Because v1 is read-write, the order is: harden the server (Tasks 1–3) → read endpoints
(Tasks 4–6) → body-read + auth (Tasks 7–8) → write endpoints (Task 9) → SPA (Task 10).

## Conventions

- New files (`webapi.{cpp,h}`, SPA assets) carry `// license:BSD-3-Clause` /
  `// copyright-holders:<name>` (SPA assets follow `web/`'s existing licensing — note
  `web/LICENSE`).
- **New `.cpp`/`.h` require `make REGENIE=1`** and registration in the appropriate
  `scripts/src/*.lua` (frontend); test TUs register in `scripts/src/tests.lua`.
- All endpoints under `/api/v1/` (resolved: version from the start).
- JSON via the in-tree `rapidjson` already used at `machine.cpp:1246`.
- Match edited files' brace/whitespace style; `srcclean` on touched files.

---

## Task 1 — Localhost-default bind

- **Files (edit):** `src/emu/http.cpp` (the asio acceptor bind in the `http_manager`
  setup, around the active-server construction ~:274-277), and if a new option is needed,
  `src/emu/emuopts.{h,cpp}` (a `-http_bind` / address option defaulting to `127.0.0.1`).
- **Change:** Make the server **bind `127.0.0.1` by default**; remote exposure is an
  explicit opt-in (a bind-address option). This is the first security prerequisite and must
  land before any endpoint is exposed.
- **Test/Validation:** Build (`make SOURCES=src/emu/http.cpp` after `make REGENIE=1` if the
  option is new). Run `mame -http` with no system; confirm the listener is on `127.0.0.1`
  only (a connection to the host's external IP is refused). `./mame -validate` green.
  `srcclean`. **Green:** default bind is localhost; opt-in remote bind works.

### Task 2 — Harden the static file handler (traversal + 404 + content-type)

- **Files (edit):** `src/emu/http.cpp` (~:300-313 — the `doc_root + path` open and the
  `status(400).send("Error")` miss path).
- **Change:** Reject path traversal (`..`, absolute paths, symlink escape) so `http_root`
  cannot serve arbitrary files; canonicalize and confirm the resolved path stays within
  `doc_root`; return a proper **404** (not a 400 "Error") for a genuine miss; set correct
  content types. This is the second non-negotiable prerequisite.
- **Test/Validation:** Security unit tests (Task 3) are the gate. Manual: `curl
  'http://127.0.0.1:8080/../../etc/passwd'` (and URL-encoded `%2e%2e%2f` variants) returns
  404/blocked, never file contents; a real missing file returns 404 with a sane
  content-type. `./mame -validate` green. `srcclean`.

### Task 3 — Static-handler security unit tests

- **Files (new):** `tests/emu/http_security.cpp` (Catch2, tagged `[http][security]`).
- **Files (edit):** `scripts/src/tests.lua` (`files {}` + include/link for the http unit
  under test).
- **Change:** Drive the path-resolution/static-handler logic **without a live socket**:
  assert that `../`, absolute, and URL-encoded traversal attempts are rejected; that a
  missing file yields 404 with the correct content type; that a legitimate in-root file is
  served. Directly covers the `http.cpp:300-313` hardening.
- **Test/Validation:** `make REGENIE=1 && make TESTS=1 && ./mametests "[http][security]"`.
  **Green:** all traversal vectors rejected; 404 + content-type correct; legit file served.

> **PR boundary P** (Tasks 1–3): server hardening + security tests. **Lands first; nothing
> is exposed until traversal hardening and localhost bind are proven.**

### Task 4 — Frontend-scoped API module + lifecycle registration

- **Files (new):** `src/frontend/mame/webapi.cpp`, `src/frontend/mame/webapi.h`.
- **Files (edit):** `src/emu/main.cpp` (~:31-34 `start_http_server`) or
  `src/frontend/mame/clifront.cpp` (~:265) — register the frontend-scoped API on the
  long-lived server at startup; `scripts/src/*.lua` (frontend) to build the new module.
- **Change:** A `web_api`/`frontend_http_api` module that owns the **frontend-scoped
  handlers** and is handed the `http_manager*` and the `emu_options`/driver enumerator it
  needs. It registers the `/api/v1/...` routes **once, at startup, on the long-lived
  server** so they are available **with no machine loaded** — resolving the
  "launcher needs a server with no machine" problem (the server is already frontend-owned;
  this adds the missing registration). The machine-scoped `/api/machine`
  (`machine.cpp:1246`, via `export_http_api` at `:341`) is untouched and continues to
  attach/detach with the machine. For this task, register a single trivial
  `GET /api/v1/ping` to prove the lifecycle.
- **Test/Validation:** **Lifecycle test** (Catch2, with a fixture options/server, no live
  socket): the frontend-scoped route is registered with **no machine loaded**; assert
  `/api/machine` is **absent** until a machine runs. Integration: `mame -http` with no
  system, `curl /api/v1/ping` returns 200. `make REGENIE=1`; `./mame -validate` green.
  `srcclean`. **Green:** frontend route responds with no machine; machine route absent
  until a system loads.

### Task 5 — Read endpoints: `/api/v1/systems` + `/api/v1/systems/<sys>`

- **Files (edit):** `src/frontend/mame/webapi.cpp` (handlers); backed by
  `src/frontend/mame/infoxml.cpp` / the driver enumerator.
- **Change:** `GET /api/v1/systems` → list of systems (name, description, year,
  manufacturer, status), paginated/filterable. `GET /api/v1/systems/<sys>` → one system's
  full metadata (slots, media, inputs) from `infoxml`. JSON via rapidjson.
- **Test/Validation:** **Handler unit tests** (Task 11) drive these with a fixture driver
  enumerator and assert JSON shape/content **without a live socket**. Integration: `curl
  /api/v1/systems` and `/api/v1/systems/<known-sys>` return the expected JSON. `./mame
  -validate` green. `srcclean`.

### Task 6 — Read endpoints: `/api/v1/audit/<sys>`, `/api/v1/config`, `/api/v1/input/<sys>`

- **Files (edit):** `src/frontend/mame/webapi.cpp`; backed by `audit.cpp:498-579`
  (`media_auditor::summarize`), `emuopts.cpp` (effective options), input ports / `infoxml`.
- **Change:** `GET /api/v1/audit/<sys>` → per-ROM audit detail (the rich strings from
  `audit.cpp:498-579`) + summary — the same data [0003](../adr/0003-frontend-usability-quick-wins.md)
  surfaces in the UI. `GET /api/v1/config` → current **effective** options, with
  **redaction**: redact/allowlist path-like and sensitive values; resolved policy — start
  at the `-showconfig` exposure set and redact path-like values. `GET /api/v1/input/<sys>`
  → the system's input definitions (read view).
- **Test/Validation:** Handler unit tests assert JSON shape; the `/api/v1/config` test
  **asserts redaction** (no filesystem paths/secrets leak beyond `-showconfig`).
  Integration: `curl` each endpoint. `./mame -validate` green. `srcclean`.

> **PR boundary Q** (Tasks 4–6): frontend-scoped read surface (`systems`/`audit`/`config`/
> `input`). The largest read-only slice; ships once hardening (PR P) is in.

### Task 7 — Fix the request-body read (`http.cpp:185`)

- **Files (edit):** `src/emu/http.cpp` (~:184-188 — `get_body()` currently returns `""`
  because the asio request's content field "is never filled in"); the request-parsing path
  that should populate the body.
- **Change:** Populate the request content so `get_body()` returns the actual POST/PUT
  body. This is the resolved **first-class v1 task** that unblocks writes — a handler that
  can't read its body cannot safely accept input.
- **Test/Validation:** Unit test (extend `tests/emu/http_security.cpp` or a new
  `tests/emu/http_body.cpp`): a request with a known body yields that body from
  `get_body()`. Integration: `curl -X POST --data '{"k":"v"}' /api/v1/ping-echo` round-trips
  the body. `make REGENIE=1` if a new TU; `./mame -validate` green. `srcclean`. **Green:**
  body round-trips through `get_body()`.

### Task 8 — Auth model + middleware (token; required for writes & remote)

- **Files (edit):** `src/emu/http.cpp` (a small auth-check middleware on registered routes),
  `src/emu/emuopts.{h,cpp}` (a `-http_token` option), `src/frontend/mame/webapi.cpp` (apply
  the gate to write routes).
- **Change:** Add a **token auth gate** (`-http_token` / config), checked in a small
  middleware. Resolved rule: **no writes and no remote bind without it**. The read-only
  localhost surface may remain open on loopback, but **every write route and any
  non-localhost bind requires a valid token** (constant-time comparison; reject with 401).
  Keep it CSRF-safe for browser use (token in a header, not a cookie-only scheme).
- **Test/Validation:** Auth unit tests (Catch2): a write route without a token → 401; with
  a valid token → allowed; a wrong token → 401; remote bind without a token → refused.
  `./mame -validate` green. `srcclean`. **Green:** writes/remote gated; reads on loopback
  unaffected.

> **PR boundary R** (Tasks 7–8): body-read fix + auth. **Both are prerequisites for any
> write endpoint** and ship together so writes never land ahead of their safety controls.

### Task 9 — Write endpoints for `/api/v1/{config,input}` (+ launch), thread-marshalled

- **Files (edit):** `src/frontend/mame/webapi.cpp` (write handlers); backed by
  `config.cpp` (per-system config I/O), `emuopts.cpp` (option mutation), input ports;
  launch via the frontend's system-start path.
- **Change:** Add the **write** half of the read-write v1 surface, all behind the Task 8
  auth gate and using the Task 7 body read:
  - `POST/PUT /api/v1/config` — mutate effective options / write per-system config
    (`config.cpp`), validated and redaction-aware (never accept a write to a redacted/
    disallowed field).
  - `POST/PUT /api/v1/input/<sys>` — write input definitions/remaps for a system.
  - (Launcher write, e.g. `POST /api/v1/systems/<sys>/launch`) — request a system start.
  **Determinism rule (ADR 0004):** any write that touches **timeline-feeding state of a
  running machine must be marshalled onto the emulation thread** — the web handler enqueues
  the mutation; it never mutates running-machine state off-thread. Frontend-level config/
  launcher writes (no running machine, or pre-start) are handled on the frontend side.
- **Test/Validation:** Handler unit tests: a config write changes the effective option and
  round-trips on read-back; an input write persists; a launch request is accepted (or
  correctly rejected when a machine is already running). **Auth asserted** (write without
  token → 401). **Redaction asserted** (write to a redacted field rejected). For
  running-machine writes, assert the mutation is **enqueued to the emulation thread**, not
  applied inline. `./mame -validate` green. `srcclean`. **Green:** writes succeed under
  auth, are rejected without it, respect redaction, and marshal onto the emu thread.

> **PR boundary S** (Task 9): the write surface. The capstone of the read-write v1 decision;
> ships only on green body-read + auth (PR R).

### Task 10 — Static SPA in `web/`

- **Files (new):** SPA assets under `web/` (e.g. `web/index.html`, `web/app.js`,
  `web/app.css`) consuming the `/api/v1/...` endpoints — a systems browser, a per-system
  detail/audit view, a config viewer, and (read-write) config/input edit forms gated by the
  auth token.
- **Change:** A **minimal hand-authored static SPA** (resolved: committed static assets, no
  Node/build-toolchain dependency in the MAME tree) served as `http_root`. It exercises the
  full read-write surface: browse systems, view audit detail, view/edit config, view/edit
  input — sending the auth token in a header for writes.
- **Test/Validation:** Integration smoke: `mame -http -http_port 8080` with no system,
  open the SPA in a browser, browse `/api/v1/systems`, drill into a system's audit/config,
  perform a token-authenticated config write and confirm it round-trips. Confirm bind is
  localhost by default. `srcclean` on any tracked text assets. **Green:** SPA browses and
  performs an authenticated write against a real build.

> **PR boundary T** (Task 10): the SPA. Static assets only; lands last, on top of the full
> read-write API.

### Task 11 — API handler + lifecycle test suite (consolidated)

- **Files (new):** `tests/frontend/webapi.cpp` (Catch2, tagged `[webapi]`).
- **Files (edit):** `scripts/src/tests.lua` (`files {}` + frontend include/link).
- **Change:** Consolidate the per-endpoint handler unit tests referenced by Tasks 5, 6, 9:
  drive each frontend-scoped handler with a **fixture options/driver enumerator** and
  assert JSON shape/content for `/api/v1/systems`, `/api/v1/audit/<sys>`, `/api/v1/config`
  (redaction asserted), `/api/v1/input/<sys>`, plus the write paths (auth + redaction +
  thread-marshal asserted) — **all without a live socket**. Include the **lifecycle**
  assertion (frontend routes present with no machine; `/api/machine` absent until a machine
  runs).
- **Test/Validation:** `make REGENIE=1 && make TESTS=1 && ./mametests "[webapi]"`.
  **Green:** all handler, redaction, auth, and lifecycle assertions pass.

> **Note:** Tasks 3 and 11 may be authored incrementally alongside the endpoints they cover
> (TDD-style) and consolidated here; the table below lists the consolidation PR, but each
> endpoint PR should land **with its tests**, not after.

---

## Suggested PR sequence (Phase 3)

| PR | Tasks | Theme | Gate |
|---|---|---|---|
| P | 1–3 | Localhost bind + traversal hardening + security tests | `mametests "[http][security]"` |
| Q | 4–6 | Frontend-scoped read API + lifecycle | `mametests "[webapi]"` reads + lifecycle |
| R | 7–8 | Body-read fix + auth middleware | `mametests` body + auth |
| S | 9 | Write endpoints (config/input/launch), thread-marshalled | `mametests` writes (auth+redaction+marshal) |
| T | 10 | Static SPA | integration smoke + authenticated write |
| (11) | 11 | Consolidated handler/lifecycle suite | folds tests from Q/R/S |

Strictly ordered by the security gate: **P (harden) → Q (reads) → R (body+auth) → S
(writes) → T (SPA).** Nothing exposed before P; no writes before R. Phase 3 is independent
of Phase 2.

## Test deliverables (Phase 3)

- **Static-handler security tests** (`tests/emu/http_security.cpp`): traversal rejection,
  404 + content-type, legit-file serving.
- **Body-read test** (extends security suite / `http_body.cpp`): `get_body()` round-trips.
- **Auth tests:** write/remote require a valid token (401 otherwise); reads on loopback
  unaffected; constant-time comparison.
- **API handler + lifecycle suite** (`tests/frontend/webapi.cpp`): JSON shape for all read
  endpoints; **config redaction** asserted; write endpoints assert auth + redaction +
  **emulation-thread marshalling**; frontend routes present with no machine, `/api/machine`
  absent until a machine runs.
- **Integration smoke:** localhost-default bind verified; SPA browses and performs an
  authenticated write against a real build.
- **`./mame -validate`** stays green; **`srcclean`** on touched files.

## Definition of Done (Phase 3, read-write v1)

1. **Security prerequisites met first:** localhost-default bind and static-handler
   traversal hardening land and are test-proven before any endpoint is exposed.
2. **Read surface** (`GET /api/v1/{systems,audit,config,input}`) works against a real
   build, available with **no machine loaded**; `/api/config` redaction asserted.
3. **Write surface** ships in v1: the **body-read fix** and **token auth** land first; write
   endpoints for config/input (+ launch) are gated by auth, respect redaction, and
   **marshal running-machine mutations onto the emulation thread**.
4. Handler + security + auth + lifecycle unit tests green in `mametests`.
5. Static SPA browses and performs an authenticated write; bind is localhost by default.
6. `./mame -validate` clean; `srcclean` clean; all endpoints under `/api/v1/`.

## Risks & assumptions

- **R1 — write surface raises the security bar (highest).** Read-write v1 means a remote-
  exploit surface if any control is skipped. Mitigation: the hard gating order (harden →
  read → body+auth → write); no write PR (S) merges before body-read + auth (R); localhost
  default + traversal hardening (P) precede everything.
- **R2 — off-thread mutation of timeline-feeding state.** A naive write handler could mutate
  a running machine off the emulation thread, breaking determinism (forbidden by ADR 0004).
  Mitigation: Task 9 marshals running-machine writes onto the emu thread; the handler test
  asserts enqueue-not-inline.
- **R3 — `/api/config` redaction leakage.** Fiddly; a missed field leaks paths/secrets.
  Mitigation: start at the `-showconfig` set, allowlist + redact path-like values, and
  **assert redaction in the handler test** (both read and write paths).
- **R4 — body-read fix scope.** The asio request-content plumbing may be deeper than a
  one-liner. Mitigation: Task 7 is its own task with a dedicated round-trip test before any
  write depends on it.
- **R5 — second consumer of audit/infoxml feeds.** The SPA plus the UI (0003) raises the
  stability bar on those internal APIs. Mitigation: handler tests pin the JSON shape so feed
  changes surface as test failures.
- **A1 — assumption:** committed static SPA assets, no Node/build-toolchain dependency in
  the MAME tree (resolved).
- **A2 — assumption:** bind-to-localhost is sufficient for the read surface on loopback;
  token auth is required for any write and any remote bind (resolved auth model).
