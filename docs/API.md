# pixelgo HTTP API — contract

**Contract version: 1**

This is the written contract for pixelgo's HTTP API: every route, what it
accepts, what it returns, and how large the answer can be. It exists for two
readers:

- **the embedded web UI**, which ships in the same binary and changes with it;
- **pixelgo-ops**, which talks to pixelgo inside a VM and has to decide, for
  every request, whether the route is allowed and whether the response is
  acceptable. Everything coming out of the VM is treated as hostile, so ops
  checks responses against this document rather than trusting them.

The routes are defined once in code, in the table at the end of
`src/web/web_api.c` (see `include/web_api.h`). `tests/t_api_contract.c` checks
that the index below and that table list exactly the same routes, with the same
content type and maximum size, so a route cannot be added, removed or changed
on one side only.

## Rules that apply to every route

- **Exact paths.** A path is served only if it matches a route below exactly.
  Anything else is `404 {"error":"no such route"}`. A path that matches a route
  but with another method is `405 {"error":"method not allowed"}`. There is no
  prefix or substring matching.
- **`{id}`** in a path is one job id: 1 to 31 letters or digits. Generated ids
  are lowercase hex.
- **Query strings** are not part of the path; parameters are listed per route.
- **Request bodies** are JSON, UTF-8, and at most `http_max_body()` bytes:
  64 KB by default, configurable with `PIXELGO_HTTP_MAX_BODY` up to 4 MB
  (`GET /api/limits` reports the value in effect). A larger body is currently
  truncated at the cap, not refused: the route then usually sees invalid JSON
  and answers `400`. Clients should not send more than the cap; a proper `413`
  is a separate change.
- **Errors** are always `application/json; charset=utf-8`, shaped
  `{"error":"<message>"}`, with a 4xx or 5xx status — including on routes whose
  successful response is not JSON. An error body is at most about 1.2 KB and is
  always valid JSON.
- **No redirects, ever.** No route answers with a 3xx status. A client must not
  follow a redirect from this API: a proxy that does (pixelgo-ops, standing
  between a browser on the host and pixelgo inside a VM) would be made to fetch
  whatever URL the guest names, which is an outbound request from the host on
  the guest's behalf. A 3xx is a contract violation and must be refused, not
  passed on.
- **The status codes listed per route are exhaustive.** Any other status is a
  contract violation. Together with the rule above this means: 2xx as
  documented, or 4xx/5xx with a JSON error body, and nothing else.
- **Maximum size** is a hard cap on the response body, errors included. The
  server enforces it for every route in one place; a response that would exceed
  it is replaced by `500 {"error":"response exceeds the contract maximum"}`.
  A client may treat anything larger as a protocol violation.

## Index

Status `implemented` means the route exists in this version. `planned` means it
is defined here so both sides can build against it, but is not implemented yet;
a client must not rely on it until its status changes.

<!-- routes:begin -->
| Method | Path | Success content type | Max bytes | Status |
| --- | --- | --- | --- | --- |
| GET | `/` | `text/html; charset=utf-8` | 1048576 | implemented |
| GET | `/index.html` | `text/html; charset=utf-8` | 1048576 | implemented |
| GET | `/api/workspaces` | `application/json; charset=utf-8` | 16384 | implemented |
| POST | `/api/workspaces` | `application/json; charset=utf-8` | 4096 | implemented |
| GET | `/api/agents` | `application/json; charset=utf-8` | 32768 | implemented |
| POST | `/api/agents` | `application/json; charset=utf-8` | 4096 | implemented |
| POST | `/api/chat` | `application/json; charset=utf-8` | 4096 | implemented |
| GET | `/api/history` | `application/json; charset=utf-8` | 131072 | implemented |
| POST | `/api/history/reset` | `application/json; charset=utf-8` | 4096 | implemented |
| GET | `/api/limits` | `application/json; charset=utf-8` | 4096 | implemented |
| GET | `/api/flows` | `application/json; charset=utf-8` | 8192 | implemented |
| POST | `/api/flows` | `application/json; charset=utf-8` | 4096 | implemented |
| POST | `/api/flow/diagram` | `application/json; charset=utf-8` | 16384 | implemented |
| POST | `/api/flow/graph` | `application/json; charset=utf-8` | 32768 | implemented |
| POST | `/api/flow/run` | `application/json; charset=utf-8` | 4096 | implemented |
| GET | `/api/jobs/{id}` | `application/json; charset=utf-8` | 67584 | implemented |
| GET | `/api/jobs/{id}/events` | `application/json; charset=utf-8` | 131072 | implemented |
| GET | `/api/jobs/{id}/data-edit-source` | `text/plain; charset=utf-8` | 4194304 | implemented |
| POST | `/api/jobs/{id}/approve` | `application/json; charset=utf-8` | 4096 | implemented |
| POST | `/api/jobs/{id}/data-edit` | `application/json; charset=utf-8` | 4096 | implemented |
| GET | `/healthz` | `application/json; charset=utf-8` | 4096 | planned |
| GET | `/readyz` | `application/json; charset=utf-8` | 4096 | planned |
| GET | `/api/logs` | `application/json; charset=utf-8` | 131072 | planned |
<!-- routes:end -->

Callers: every implemented route is used by the embedded UI, and pixelgo-ops
forwards all of them, since it serves the same UI. The planned routes are for
pixelgo-ops.

## Routes

### `GET /` and `GET /index.html`

The embedded web UI, a single self-contained HTML page. No parameters.

- `200` — the page.

### `GET /api/workspaces`

The workspaces under `workspaces/` that have a `workspace.conf`.

- `200` — array of `{"name": string, "agent_count": number}`.

### `POST /api/workspaces`

Creates a workspace.

- Body: `{"name": string}` (required, non-empty).
- `201` — `{"ok": true}`.
- `400` — missing or invalid name. `500` — could not be created.

### `GET /api/agents`

The agents in a workspace.

- Query: `ws` (required).
- `200` — array of agents. Every agent has `"id": string` and
  `"type": "ai" | "worker"`. An `ai` agent also has `"provider": string`,
  `"model": string`, `"system_prompt": string`, `"tools": [string]`,
  `"allowlist": [string]` (commands `run_command` may run) and
  `"approval": [string]` (tools that ask before running). A `worker` agent has
  `"command": string`.
- `400` — missing `ws`. `404` — no such workspace.

### `POST /api/agents`

Creates an agent in a workspace.

- Body: `{"ws": string, "id": string, "type": "ai" | "worker", ...}`.
  For `ai`: `provider` and `model` (required), `system_prompt`, `tools`,
  `allowlist`, `approval` (optional; arrays of strings). For `worker`:
  `command` (required).
- `201` — `{"ok": true}`.
- `400` — invalid body or fields. `404` — no such workspace. `500` — could not
  be saved.

### `POST /api/chat`

Sends a message to an agent. The reply is produced by a background job.

- Body: `{"ws": string, "agent": string, "message": string}` (all required).
- `202` — `{"ok": true, "job_id": string}`. Poll `GET /api/jobs/{id}`.
- `400` — missing fields, or the agent is not an `ai` agent. `404` — no such
  workspace or agent. `500` — the job could not be started.

### `GET /api/history`

An agent's saved conversation history.

- Query: `ws`, `agent` (both required).
- `200` — array of messages in the provider-neutral format used internally.
  If the history does not fit the maximum size, `[]`.
- `400` — missing parameters. `404` — no such workspace or agent.

### `POST /api/history/reset`

Clears an agent's history.

- Body: `{"ws": string, "agent": string}` (both required).
- `200` — `{"ok": true}`.
- `400` — missing fields. `404` — no such workspace or agent.

### `GET /api/limits`

Limits in effect for this server.

- `200` — `{"http_max_body": number}`: the effective request body cap, in bytes.

### `GET /api/flows`

The `.flow` files under `demo/` and `examples/`.

- `200` — array of relative paths (strings).

### `POST /api/flows`

Saves a graph. A plain `name` is written as `demo/<name>.flow`; a name with a
`/` is used as a path, and must be under `demo/` or `examples/` (no `..`).
`.flow` is appended when missing. The content is parsed before it is written,
so a broken graph never replaces a working one.

- Body: `{"name": string, "content": string}` (both required).
- `200` — `{"saved": string}`, the path written.
- `400` — missing fields, invalid name or path, or invalid graph. `500` — could
  not be written.

### `POST /api/flow/diagram`

A graph rendered as Mermaid text.

- Body: `{"flow": string}`, a path under `demo/` or `examples/` (no `..`).
- `200` — `{"mermaid": string}`.
- `400` — missing or invalid path, or the graph cannot be loaded. `500` —
  rendering failed.

### `POST /api/flow/graph`

A graph's structure, for drawing it.

- Body: `{"flow": string}`, as above.
- `200` — `{"entry": string, "nodes": [string],
  "edges": [{"from": string, "to": string, "condition": string}]}`.
- `400` — missing or invalid path, or the graph cannot be loaded.

### `POST /api/flow/run`

Runs a graph in a workspace, as a background job.

- Body: `{"ws": string, "flow": string, "task": string}`; `ws` and `flow`
  required, `task` optional.
- `202` — `{"ok": true, "job_id": string}`.
- `400` — invalid body, path or graph, or the graph does not validate against
  the workspace. `404` — no such workspace. `500` — the job could not be
  started.

### `GET /api/jobs/{id}`

A job's state.

- `200` — `{"id": string, "kind": "chat" | "flow",
  "status": "running" | "done" | "failed", ...}`. When `done`: a `chat` job has
  `"reply": string`; a `flow` job has `"outputs": [...]` (one entry per node
  run) and the totals `total_input_tokens`, `total_output_tokens`,
  `total_cache_write_tokens`, `total_cache_read_tokens`,
  `total_cost_micro_usd`, `total_cost_no_cache_micro_usd` (numbers). When
  `failed`: `"error": string`.
- `400` — invalid id. `404` — no such job.

### `GET /api/jobs/{id}/events`

A batch of a job's live events, from its journal.

- Query: `from` (optional, default 0): index of the first event wanted.
- `200` — `{"events": [object], "next": number}`. Ask again with
  `from=next`. A batch is capped in count and size; an empty batch with the
  same `next` means nothing new yet. The event objects are those written by
  `events.c` (see `include/events.h`).
- `400` — invalid id.

### `GET /api/jobs/{id}/data-edit-source`

The text of a pending editable-approval request, for the data-edit dialog.

- `200` — `text/plain; charset=utf-8`, the text exactly as the agent sent it,
  at most 4194304 bytes (4 MB, the same ceiling as a request body, since the
  edited text has to come back through `POST .../data-edit`).
- `404` — no pending request. `400` — invalid id. `500` — the text cannot be
  read, or is larger than the maximum (the message says so).
- Clients must display this text as text, never interpret it as HTML.

### `POST /api/jobs/{id}/approve`

The human's decision for a pending tool approval.

- Body: `{"allow": boolean}`. Anything other than `true` denies.
- `200` — `{"allowed": boolean}`, the decision recorded.
- `400` — invalid id or body. `500` — the decision could not be recorded.

### `POST /api/jobs/{id}/data-edit`

The human's decision for a pending editable-approval request.

- Body: `{"allow": true, "text": string}` approves with the edited text;
  `{"allow": true}` without `text` approves unchanged; `{"allow": false}`
  denies.
- `200` — `{"allowed": boolean}`.
- `400` — invalid id or body. `500` — the decision could not be recorded.

## Planned routes

Defined now so pixelgo and pixelgo-ops can build against them. Not implemented
in this version.

### `GET /healthz` (planned)

Liveness, for `pixelgo-ops status` and for the version check ops performs
before serving its own copy of the UI.

- `200` — `{"status": "ok", "version": string, "contract": number}`, where
  `contract` is this document's contract version.

### `GET /readyz` (planned)

Whether the server takes new work.

- `200` — `{"ready": boolean, "reason": string}`. `ready` is false while the
  server is shutting down and draining running work (see the graceful shutdown
  work), with `reason` saying why.

### `GET /api/logs` (planned)

The end of `pixelgo.log`, for `pixelgo-ops logs`.

- Query: `lines` (optional, default 200, at most 1000).
- `200` — `{"lines": [string]}`, oldest first. Lines are returned as logged;
  the client is responsible for stripping control characters before printing
  them to a terminal.
