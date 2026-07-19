# pixelgo

**Small agents. Any system.**

pixelgo is a minimalist AI agent orchestration engine, written in C. Each agent
is a "pixel" — the smallest unit, under your control. You compose them into a
graph and build any pipeline: graphics, code, automation, robotics.

---

## Why pixelgo

- **Minimalist, under control.** Written in C, no hidden magic. You see and
  control every agent, every tool, every step. Strict file sandbox.
- **Composable.** Small agents with clear roles, wired into a flow graph. Add a
  new "pixel" without rewriting anything.
- **Visual.** You don't debug an opaque log — you watch execution move through
  the graph, with a live journal of what each agent is doing.
- **Multi-provider.** Anthropic, OpenAI, Gemini. Each agent can use a different
  model.
- **Does anything.** Graphics, code pipelines, automation, robotics. Same brick,
  different result.

## Build

```bash
make
```

Dependency: `libcurl4-openssl-dev`

```bash
apt-get install libcurl4-openssl-dev
```

## Quick start

```bash
# 1. create a workspace
./pixelgo workspace create demo

# 2. add an AI agent (provider, model, prompt, tools)
./pixelgo agent add ai demo reviewer anthropic claude-sonnet-5 \
  "You are a code review agent." "read_file,list_dir"

# 3. give it a key and run it
export ANTHROPIC_API_KEY=sk-...
./pixelgo agent run demo reviewer "Check src/main.c"
```

Keys can also live in a `.env` file (see `.env.example`):
`ANTHROPIC_API_KEY`, `OPENAI_API_KEY`, `GEMINI_API_KEY`.

## Web interface (the animated graph)

```bash
./pixelgo serve 8080          # opens http://127.0.0.1:8080
./pixelgo serve 8080 --daemon # in the background
./pixelgo serve status
./pixelgo serve stop
```

There you watch the pixels circulate through the graph, the agents' live journal,
and the run cost in real time. You can also create workspaces, agents and graphs
from the browser — the "New graph" box in the Graphs tab validates the
definition before saving, so a broken graph never lands on disk.

## Flows: composing agents into a graph

A flow describes how the agents connect. Example — a pipeline with a correction
loop:

```
entry planner

node planner
node coder
node reviewer

edge planner  -> coder
edge coder    -> reviewer

# conditional edges: the decision comes from the reviewer's output
edge reviewer -> coder  when NEEDS_FIX
edge reviewer -> END    when APPROVED
```

```bash
./pixelgo flow run demo examples/01-pipeline-review.flow "Write a CSV parser"
./pixelgo flow diagram examples/01-pipeline-review.flow   # export Mermaid
```

Flows support conditional edges (a decision based on output), **fan-out**
(several agents in parallel from the same node) and **fan-in** (a node that waits
for all of them). See more in `examples/`.

## Agent tools

Each agent gets only the tools you give it, and all of them go through a sandbox
that blocks access outside its directory:

| Tool | What it does |
|---|---|
| `read_file` | reads a file |
| `write_file` | writes a file |
| `list_dir` | lists a directory |
| `search_files` | searches through files |
| `run_command` | runs a command (from an explicitly allowed list) |

`run_command` has an allowlist: you give exactly the allowed commands, e.g.
`"make,gcc"`. Everything else is blocked.

## selfhost: pixelgo develops itself

pixelgo can improve its own code — the brick that rearranges itself. A dedicated
workspace where the agents receive the whole codebase, modify it and compile it,
while **you are the reviewer**:

```bash
./pixelgo selfhost init --src .
./pixelgo serve 8090          # on another port; the production instance is untouched
```

Pick the `pixelgo-dev` workspace, load `selfhost.flow`, type a task and press
Run. The flow is `architect → coder → END`: architect makes the plan (read-only),
coder implements and compiles until `make` passes, then stops and shows you the
summary. You decide whether to keep it.

## Running as a service

To have pixelgo start at login and restart if it crashes:

```bash
make service          # systemd on Linux, launchd on macOS
make service-uninstall
```

This installs a **user** service, not a system-wide one. That is deliberate:
the web UI has no authentication, so it binds to `127.0.0.1` only and the
agents run with your own permissions. There is nothing to gain from root.

The installer copies the binary to `~/.local/bin`, keeps data in
`~/.local/share/pixelgo`, and reads API keys from `~/.config/pixelgo/env`
(seeded from your `.env` if you have one). Pick a different port with
`PIXELGO_PORT=9000 make service`.

```bash
# Linux
systemctl --user status pixelgo
journalctl --user -u pixelgo -f

# macOS
launchctl print gui/$(id -u)/io.pixelgo.serve | head
tail -f ~/.local/share/pixelgo/pixelgo.log
```

On Linux, a user service stops when you log out. To keep it running across
reboots: `sudo loginctl enable-linger $USER`.

**Do not expose this to a network as is.** If you need remote access, put it
behind a reverse proxy that handles authentication.

## Approving what an agent does

By default an agent runs unsupervised until it finishes. You can require your
approval per agent, per tool:

```bash
./pixelgo agent add ai demo coder anthropic claude-sonnet-5 \
  "You are a C programmer." \
  "read_file,write_file,run_command" "gcc,make" \
  --approve "write_file,run_command"
```

Now the agent reads files freely, but stops and asks before writing or running
anything:

```
  ┌─ approval needed ─────────────────────────────
  │ agent: coder
  │ tool:  run_command
  │ gcc -Wall -c src/calc.c
  └───────────────────────────────────────────────
  allow? [y/N]
```

In the web interface the same thing appears as a dialog, and the graph pauses
until you decide.

Two things worth knowing. Approving does not bypass the `run_command`
allowlist — that stays the last line of defence, so a distracted `y` cannot
undo your configuration. And if nobody can be asked (no terminal, no browser
attached), the answer is **no**: an agent set up to need approval never acts
unsupervised just because nobody was watching.

## Tests

```bash
make test
```

Unit tests for the parts where a mistake is expensive and hard to notice: the
sandbox, the permission gates, and the flow transition rules. They link against
the compiled objects, so what is tested is what ships.

Writing them found a real hole: a symlink whose target was outside the sandbox
was accepted, because only the parent directory was canonicalised while the
final component was appended as text. `read_file` would have followed it out.

## Commands

```
workspace create <name>
agent add worker <ws> <id> <command> [args...]
agent add ai     <ws> <id> <provider> <model> <prompt> <tool1,tool2,...> [cmd1,cmd2,...]
agent run  <ws> <id> [task]
agent chat <ws> <id> [--reset]      # conversation, remembers
agent list <ws>
flow run     <ws> <file.flow> [task]
flow diagram <file.flow>
serve [port] [--daemon] | serve stop | serve status
selfhost init [--ws <name>] [--src <dir>] [--provider <p>] [--model <m>] [--force]
keys
```

## Architecture at a glance

```
include/   the contract of each module (agent, workspace, sandbox, flow, tools...)
src/
  core/    workspace, sandbox, running agents, the flow graph, selfhost
  tools/   read/write/list/search/run — all through the sandbox
  runtime/ the AI loop + provider clients (anthropic, openai, gemini)
  web/     HTTP server, API, the animated graph, daemon, jobs, cost tracking
  cli/     pixelgo
```

The sandbox is the security foundation: every file access is validated with
`realpath` and checked to stay inside the agent's directory or in `shared/`.

## License

[Apache License 2.0](LICENSE) © 2026 pixelgo

You are free to use, modify, and distribute pixelgo, including in commercial
products. The license includes an explicit patent grant. See the [`LICENSE`](LICENSE)
file for the full text.
