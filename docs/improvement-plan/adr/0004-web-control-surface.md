# ADR 0004 — Web control surface (REST + SPA over the HTTP server)

> **Status:** Proposed · **Phase:** P3 · **Owner:** TBD
> **Depends on:** none (independent of [0002](0002-m68000-drcuml-port.md)); benefits from [0003](0003-frontend-usability-quick-wins.md)'s data-surfacing work
> **Spec:** [`docs/improvement-plan/specs/2026-06-24-mame-improvements-design.md`](../specs/2026-06-24-mame-improvements-design.md)
> **Date:** 2026-06-24

## Context

MAME ships an asio-based HTTP + WebSocket server (`src/emu/http.cpp`) that is almost
entirely unused. The current state (verified):

- **It is opt-in via options** defined in `src/emu/emuopts.h:201-203` and defaulted in
  `src/emu/emuopts.cpp:225-227`: `OPTION_HTTP` (`"http"`, default `"0"`),
  `OPTION_HTTP_PORT` (`"http_port"`, default `"8080"`), `OPTION_HTTP_ROOT`
  (`"http_root"`, default `"web"`).
- **Exactly one generic REST endpoint exists:** `GET /api/machine`, registered in
  `src/emu/machine.cpp:1246-1267`, returning JSON `{ name, devices: [tag…] }` via
  rapidjson.
- **The static file handler is crude** (`src/emu/http.cpp:310-313`: on a missing file
  it just does `response->status(400).send("Error")`), there is a **request-body TODO**
  (`src/emu/http.cpp:185-187`: `get_body()` returns `""` because the asio request's
  content field "is never filled in"), and there is **no authentication anywhere** in
  `http.cpp`.
- **`web/` is nearly empty:** `web/LICENSE`, `web/README.md`, `web/layout.xsl`, and an
  unrelated ESQ panel demo (`web/esqpanel/vfx/FrontPanel.{html,js}`).

**Lifecycle — the crux, and the spec's framing is now corrected by evidence.** The
HTTP server is **owned at the frontend level, not by a running machine**:

- `machine_manager` holds `std::unique_ptr<http_manager> m_http`
  (`src/emu/main.h:99`).
- It is created in `machine_manager::start_http_server()`
  (`src/emu/main.cpp:31-34`: `m_http = std::make_unique<http_manager>(options().http(),
  options().http_port(), options().http_root())`).
- That is called from `cli_frontend::start_execution()` at
  `src/frontend/mame/clifront.cpp:265` — **before** a system is selected
  (`mame_options::system(...)` is consulted later at `:273`).
- The `http_manager` constructor (`src/emu/http.cpp:274-277`) early-returns when
  inactive; when `-http` is on it stands up the asio server immediately.
- The per-machine `/api/machine` endpoint is added by `export_http_api()`
  (`src/emu/machine.cpp:341`) **only while a machine is alive** and only if
  `http()->is_active()` (`src/emu/http.h:159-161`).

**Therefore the server already outlives any single machine and can serve with no
machine loaded** — it just has nothing useful registered in that state today (only
static files and the machine-scoped endpoint). This is the key enabler for a launcher
surface and it already exists; we do not need to relocate the server's ownership.

**Data feeds already exist** to back a useful REST surface:
- `src/frontend/mame/infoxml.cpp` — full machine/system metadata export (the `-listxml`
  source).
- `src/frontend/mame/audit.cpp` — ROM-set audit/verify results (the same detail
  [0003](0003-frontend-usability-quick-wins.md) surfaces in the UI).
- `src/emu/emuopts.cpp` — the full options model (read/serialize config).
- `src/emu/config.cpp` — per-system configuration file I/O.

## Decision

Build a **read-mostly REST control surface + a static single-page app**, served by the
existing frontend-level `http_manager`, scoped to **launcher / audit / config / input
inspection — explicitly NOT a live pixel stream** (that's the Emscripten build's job;
excluded by the spec).

### 1. Lifecycle: a frontend-scoped API, registered independent of a machine

Add an **API registration that runs at frontend level**, alongside or just after
`machine_manager::start_http_server()` (`src/emu/main.cpp:31-34`), so the
launcher/audit/config endpoints are available **with no machine loaded**. Concretely:

- A new `web_api` (or `frontend_http_api`) module — e.g.
  `src/frontend/mame/webapi.{cpp,h}` — owns the frontend-scoped handlers and is given
  the `http_manager*` and the `emu_options`/driver enumerator it needs. It registers
  `/api/systems`, `/api/audit/<sys>`, `/api/config`, `/api/input/<sys>` once, at
  startup, on the long-lived server.
- The existing machine-scoped `/api/machine` (`machine.cpp:1246`) and any new
  machine-live endpoints continue to register/unregister with the machine via
  `export_http_api()` (`machine.cpp:341`). The two scopes coexist: frontend endpoints
  always present; machine endpoints present only while a system runs.

This directly resolves the spec's "launcher needs a server with no machine" problem —
the server is already frontend-owned (`main.h:99`); we add the **frontend-scoped
handler registration** that today is missing.

### 2. REST surface (read-mostly)

| Method · Path | Backed by | Returns |
|---|---|---|
| `GET /api/systems` | `infoxml.cpp` / driver enumerator | List of systems (name, description, year, manufacturer, status), paginated/filterable |
| `GET /api/systems/<sys>` | `infoxml.cpp` | One system's full metadata (slots, media, inputs) |
| `GET /api/audit/<sys>` | `audit.cpp` (`media_auditor::summarize`) | Per-ROM audit detail (the rich strings from `audit.cpp:498-579`) + summary |
| `GET /api/config` | `emuopts.cpp` | Current effective options (secrets/paths gated; see security) |
| `GET /api/input/<sys>` | input ports / `infoxml` | The system's input definitions (read-only first cut) |
| `GET /api/machine` | existing `machine.cpp:1246` | Unchanged; present only while a machine runs |

Writes (e.g. launching a system, mutating config, remapping input) are **deferred to a
later increment** behind explicit auth + CSRF-safe handling; the first surface is
read-only inspection, which is low-risk and immediately useful. JSON via the in-tree
rapidjson already used at `machine.cpp:1246`.

### 3. Static SPA in `web/`

A minimal SPA under `web/` (served as `http_root`): a systems browser, a per-system
detail/audit view, and a config viewer, consuming the REST endpoints above. Plain
static assets (no build toolchain dependency baked into the MAME build) to keep it
buildable and shippable from the tree.

### 4. Security model (must precede exposing anything)

- **Bind localhost by default.** The server must default to listening on `127.0.0.1`
  only; remote exposure is an explicit opt-in. (Confirm/extend the asio acceptor bind
  in `http.cpp`.)
- **Harden the static file handler** (`http.cpp:310-313`): reject path traversal
  (`..`, absolute paths, symlink escape) so `http_root` cannot serve arbitrary files;
  return proper 404 (not a 400 "Error") and correct content types.
- **Fix the request-body TODO** (`http.cpp:185-187`) before any write/POST endpoint —
  a handler that can't read its body cannot safely accept input. Read-only GET surface
  ships first precisely because this is unresolved.
- **Add an auth gate for non-localhost / any write path:** a token (`-http_token` or
  config) checked in a small middleware; no writes and no remote bind without it. The
  read-only localhost surface can ship before full auth, but the **bind-localhost +
  traversal-hardening** are non-negotiable prerequisites even for read-only.
- **Config redaction:** `/api/config` must not leak filesystem paths or anything
  sensitive beyond what `-showconfig` already prints; redact/allowlist fields.

### 5. Explicitly out of scope

- Live framebuffer/pixel streaming (bandwidth/latency — Emscripten build's domain).
- Driving an in-progress emulation from the web (input injection into a running
  machine) beyond read-only inspection in this increment.

## Integration seams (file:line)

| Seam | Location | Role |
|---|---|---|
| HTTP options | `src/emu/emuopts.h:201-203`, `emuopts.cpp:225-227` (`http`/`http_port` 8080/`http_root` "web") | Gating + config |
| Server ownership (frontend-level) | `src/emu/main.h:99` (`unique_ptr<http_manager> m_http`) | Long-lived, machine-independent |
| Server creation | `src/emu/main.cpp:31-34` (`start_http_server`) | Where frontend-scoped API registration is added |
| Created before machine | `src/frontend/mame/clifront.cpp:265` (`start_http_server`) vs `:273` (system lookup) | Proves server outlives/precedes any machine |
| Inactive early-return | `src/emu/http.cpp:274-277`; `http.h:159-161` (`is_active`) | Opt-in behavior |
| Only generic endpoint today | `src/emu/machine.cpp:1246-1267` (`/api/machine`) | Pattern + the machine-scoped registration |
| Machine-scoped registration | `src/emu/machine.cpp:341` (`export_http_api`) | Where machine-live endpoints attach/detach |
| Crude static handler | `src/emu/http.cpp:310-313` (`status(400).send("Error")`) | Harden (traversal, 404, content-type) |
| Body-read TODO | `src/emu/http.cpp:185-187` (`get_body()` returns "") | Must fix before any write endpoint |
| Data: systems | `src/frontend/mame/infoxml.cpp` | `/api/systems`, `/api/systems/<sys>` |
| Data: audit | `src/frontend/mame/audit.cpp:498-579` | `/api/audit/<sys>` |
| Data: config | `src/emu/emuopts.cpp`; `src/emu/config.cpp` | `/api/config` |
| Static SPA root | `web/` (currently `LICENSE`,`README.md`,`layout.xsl`,esqpanel demo) | New SPA assets |
| New: frontend API module | `src/frontend/mame/webapi.{cpp,h}` | Frontend-scoped handler registration |

## Alternatives considered

1. **Relocate the HTTP server to a new frontend lifetime.** Rejected — unnecessary; it
   is *already* frontend-owned (`main.h:99`) and created before any machine
   (`clifront.cpp:265`). We only need to register frontend-scoped handlers, not move
   the server.
2. **Register launcher endpoints inside `running_machine`.** Rejected — that ties them
   to a machine's lifetime (`export_http_api` at `machine.cpp:341`), so they'd vanish
   when no system is loaded — exactly the failure mode a launcher must avoid.
3. **Ship a live pixel stream.** Rejected — excluded by the spec (bandwidth/latency);
   the Emscripten build is the right vehicle for in-browser emulation.
4. **Expose write/launch endpoints in the first increment.** Rejected — the body-read
   TODO (`http.cpp:185`) and absent auth make writes unsafe today; read-only inspection
   is the safe, useful first surface.
5. **Add a heavyweight web framework / build toolchain.** Rejected — static assets +
   the existing asio server keep the SPA buildable from the tree with no new build
   dependency.

## Consequences

**Good**
- Turns a dormant subsystem into a useful, browseable control surface for
  launch/audit/config inspection, reusing existing data feeds.
- The launcher-with-no-machine problem is solved cleanly because the server is already
  frontend-scoped; the missing piece (frontend handler registration) is small.
- Read-only-first keeps risk low while the body-read/auth gaps are closed properly.

**Bad / cost**
- Net-new surface area (REST handlers + SPA + security middleware) — the largest new
  code of the usability picks; sequenced last for that reason.
- The static handler, body-read, bind, and auth gaps are real security debt that
  **must** be paid before exposure; cutting corners here is a remote-exploit risk.
- `/api/config` redaction is fiddly — must not leak paths/secrets.
- A second consumer of the audit/infoxml data feeds (alongside the UI) raises the
  stability bar on those internal APIs.

## Accuracy & determinism preservation

The control surface is **read-mostly inspection**; the first increment touches no
emulation code path, scheduler, or save-state. The server already runs on its own asio
io_context off the emulation thread *for I/O* — but it only *reads* exported state and
does not feed the save-state timeline, so determinism is preserved (consistent with
how `/api/machine` works today). Any future write/launch endpoints must marshal onto
the emulation thread and are explicitly out of this increment; this ADR forbids
off-thread mutation of timeline-feeding state.

## Testing & validation

- **API handler unit tests** (new Catch2 cases in `mametests` via
  `scripts/src/tests.lua`): drive each frontend-scoped handler with a fixture
  options/driver-enumerator and assert the JSON shape/content for `/api/systems`,
  `/api/audit/<sys>`, `/api/config` (redaction asserted), `/api/input/<sys>`. These
  test the handler logic without a live socket.
- **Static-handler security tests:** assert path-traversal attempts
  (`../`, absolute, encoded) are rejected and that a missing file returns 404 with a
  correct content type — directly covering the `http.cpp:310-313` hardening.
- **Lifecycle test:** assert frontend-scoped endpoints respond with **no machine
  loaded**, and that `/api/machine` is absent until a machine runs (covers the crux).
- **Integration smoke:** run `mame -http -http_port 8080` with no system, curl
  `/api/systems`; load a system, curl `/api/machine`; confirm bind is localhost by
  default.
- **`./mame -validate`** stays green; **`srcclean`** on touched files.
- **Definition of done:** read-only REST surface + SPA browse working against a real
  build; handler + security unit tests green in `mametests`; localhost-default bind and
  traversal hardening verified.

## Open questions (for the owner before Planner runs)

1. **Auth model for read-only localhost.** Is bind-to-localhost sufficient for the
   read-only first increment, with token auth required only for remote bind / writes?
   (Recommend: yes — localhost-only + traversal hardening for v1; token for remote.)
2. **Write scope.** Confirm writes (launch a system, mutate config, remap input) are
   out of scope for this increment and deferred behind the body-read fix + auth.
   (Recommend: defer all writes.)
3. **SPA tooling.** Hand-authored static assets only (no Node build step in the MAME
   tree), or is a separate, pre-built SPA bundle acceptable? (Recommend: committed
   static assets, no build dependency.)
4. **`/api/config` redaction policy.** Mirror exactly what `-showconfig` exposes, or a
   tighter allowlist? (Recommend: start at the `-showconfig` set, redact path-like
   values.)
5. **Endpoint naming/versioning.** Prefix with `/api/v1/` from the start to allow
   evolution? (Recommend: yes — `/api/v1/...`.)
