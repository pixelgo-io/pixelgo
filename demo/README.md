# PixelGo — Demo

Three examples that work out of the box. The agents are pre-configured, the test
project has real bugs, the graphs are written.

## Getting started (2 minutes)

```bash
# 1. Build — this also creates the agents and the test project
make

# 2. Set the Anthropic key
cp .env.example .env
nano .env                     # ANTHROPIC_API_KEY=sk-ant-...
chmod 600 .env

# 3. Run a demo
./pixelgo flow run demo demo/01-code-review.flow "Check src/calc.c"
```

`make` sets the demo up once. Later rebuilds leave it alone, so anything the
agents changed in `workspaces/demo` stays put. To start the demo over from
scratch:

```bash
make demo-reset
```

Or, in the web interface:

```bash
./pixelgo serve                 # http://127.0.0.1:8080 → "Graphs" tab
```

There you also see the **pixels circulating** through the graph, in real time.

---

## What's in the demo

**The test project** (`src/calc.c`) — a C calculator with **5 real, intentional
bugs**:

1. `add_history` — `strcpy` without a bounds check → **buffer overflow**
2. `divide` — does not check `b == 0` → **crash**
3. `format_result` — `malloc` without `free` → **memory leak**
4. `print_result` — does not check NULL
5. `sum_array` — `i <= len` → **off-by-one** (reads past the array)

Interesting detail: **`gcc -Wall -Wextra` finds none of them.** They are logic
bugs the compiler cannot see. The agent can.

---

## Demo 1 — Code review with a correction loop

```bash
./pixelgo flow run demo demo/01-code-review.flow "Check src/calc.c"
```

**What happens:**

`reviewer` reads the code → finds the bugs → writes **NEEDS_FIX**
→ `coder` fixes them **and compiles** with gcc → if gcc errors, it reads and
fixes them → sends back to `reviewer` → when it's OK, **APPROVED** → done.

**What it demonstrates:** the conditional loop. Work returns automatically until
it passes review. And `coder` does not write blind code — it compiles and sees
the errors.

⚠️ It's a loop. The reviewer's prompt explicitly tells it to approve once the
serious bugs are fixed, so it does not spin forever (the limit is 50 steps).

---

## Demo 2 — Parallel analysis (fan-out / fan-in)

```bash
./pixelgo flow run demo demo/02-parallel-audit.flow "Analyze src/calc.c"
```

**What happens:**

`coordinator` splits the work → `security`, `performance` and `style` analyze
**simultaneously**, each on its own specialty → `merger` receives all three
reports and combines them into a single, prioritized one.

**What it demonstrates:** parallelism. The analysis takes as long as the slowest
agent, not the sum of them. In the web interface you see the 3 pixels leaving
coordinator **at once**, then **converging** toward merger.

---

## Demo 3 — Write code and verify it yourself

```bash
./pixelgo flow run demo demo/03-build-verify.flow \
  "Write a program that prints the first 20 Fibonacci numbers"
```

**What happens:**

`builder` writes the code → **compiles** it → if gcc errors, it **reads** and
fixes them → recompiles → when it works, **runs the program** to verify the
output → `tester` writes separate tests and runs them.

**What it demonstrates:** the difference between a text generator and a
programmer. The agent does not declare itself done until it compiles cleanly.

---

## The agents created

| Agent | What it does | Tools |
|---|---|---|
| `reviewer` | reads code, finds problems | read, list, search |
| `coder` | fixes and **compiles** | read, write, list, search, **run_command** (gcc, make) |
| `coordinator` | splits the work (fan-out) | read, list |
| `security` | security analysis | read, list, search |
| `performance` | performance analysis | read, list, search |
| `style` | style analysis | read, list, search |
| `merger` | combines the reports | write |
| `builder` | writes code and verifies it | read, write, list, **run_command** (gcc) |
| `tester` | writes tests and runs them | read, write, list, **run_command** (gcc) |

**Least privilege:** each agent gets only what it needs. `reviewer` reads but
does not write. Only `coder`, `builder` and `tester` can run commands — and only
`gcc`/`make`, nothing else.

---

## A different model

The default is `claude-sonnet-4-5`. You can change it:

```bash
PIXELGO_MODEL=claude-opus-4-1 make demo-reset
```

Check the Anthropic console for the models available to you.

---

## Costs

After each graph, you see how much it cost:

```
--- cost per agent ---
  reviewer     anthropic  claude-sonnet-4-5  in=5200  out=800  $0.03
  coder        anthropic  claude-sonnet-4-5  in=6100  out=1200 $0.04
--- total: cost=$0.07 ---
```

The demos are small — a full review costs a few cents.
