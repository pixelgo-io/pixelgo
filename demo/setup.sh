#!/bin/bash
#
# PixelGo - demo setup
#
# Creates a workspace with ready-configured agents (all on Claude) and a small
# C project with intentional bugs, so you have something for the agents to work on.
#
# Run automatically by `make`. To rebuild the demo from scratch:
#   make demo-reset
#
# After that:
#   ./pixelgo serve      -> web interface, "Graphs" tab
# or from the command line:
#   ./pixelgo flow run demo demo/01-code-review.flow "Check src/calc.c"

set -e

WS=demo
MODEL="${PIXELGO_MODEL:-claude-sonnet-4-5}"

cd "$(dirname "$0")/.."   # project root

if [ ! -x ./pixelgo ]; then
    echo "Cannot find ./pixelgo. Run 'make' first."
    exit 1
fi

# Check the key - without it, the demo cannot run
if ! ./pixelgo keys 2>/dev/null | grep -q "anthropic  OK"; then
    echo "WARNING: ANTHROPIC_API_KEY is not configured."
    echo "  Create a .env file with:  ANTHROPIC_API_KEY=sk-ant-..."
    echo "  Then:  chmod 600 .env"
    echo ""
    echo "Continuing anyway (agents are created, but will not be able to run)."
    echo ""
fi

echo "=== Creating workspace '$WS' ==="
rm -rf "workspaces/$WS"
./pixelgo workspace create "$WS" >/dev/null

# ---------------------------------------------------------------------------
# THE AGENTS
#
# All on Claude. The model can be changed via PIXELGO_MODEL.
#
# Note on tools: each agent gets ONLY what it needs.
# The reviewer reads but does not write. The coder writes and compiles. This way
# an agent cannot do damage by accident.
# ---------------------------------------------------------------------------

echo "=== Creating agents (model: $MODEL) ==="

# --- REVIEWER: reads the code, finds problems ---
./pixelgo agent add ai "$WS" reviewer anthropic "$MODEL" \
"You are a C code reviewer, demanding but constructive.

The FIRST THING you do, ALWAYS, without waiting for other instructions:
1. list_dir on '.' to see what is in the working directory
2. list_dir on subdirectories (e.g. 'src')
3. read_file on every .c or .h file found

Do not ask which code to review. Look for it yourself and find it.
The working directory already contains the project code.

Then look for REAL problems: buffer overflows, memory leaks,
missing NULL checks, off-by-one, undefined behavior.

Report each problem with: file, line, what is wrong, how to fix it.
Do not invent problems.

IMPORTANT - how you decide:
- If you receive a message from the coder saying the problems are fixed,
  RE-READ the code. If the problems really are fixed, approve.
- Do not seek perfection. If the serious bugs (crash, overflow, leak) are
  fixed and the code compiles, that is enough.
- Do not reject the same file more than twice.

FORMAT RULE - MANDATORY:

BEGIN your response with the verdict, on the FIRST line, alone, in uppercase:

  NEEDS_FIX   - if there are still SERIOUS problems (crash, overflow, leak)
  APPROVED    - if the serious bugs are fixed

Only AFTER that do you write the explanations.

Example of a correct response:

APPROVED

I checked all 5 bugs. All are fixed:
- Buffer overflow: the check is present
- ...

REPEAT the verdict at the end too, on the last line.

If you checked and the bugs are fixed, write APPROVED. Do not look for new reasons
to reject - the code does not need to be perfect, just free of serious bugs." \
"read_file,list_dir,search_files" >/dev/null

# --- CODER: fixes what the reviewer found, and VERIFIES it works ---
./pixelgo agent add ai "$WS" coder anthropic "$MODEL" \
"You are a C programmer.

You receive a review report with problems found. Fix them ALL.

HOW YOU WORK:
1. list_dir '.' and 'src' to see the files
2. read_file on the file to fix
3. write_file with the fixed version (write the ENTIRE file, not just parts)
4. COMPILE to verify:
   run_command: gcc, args ['-Wall','-Wextra','-c','src/calc.c','-o','/tmp/calc.o']

If gcc reports 'command failed', READ the errors and fix them.
If it says 'Exit code: 0 (success)', it worked - do not try other variants.

LIMITS - you may run ONLY 'gcc' and 'make'. Nothing else.
Do NOT try to run the compiled binary (/tmp/calc) - not allowed.
Do NOT run 'pwd', 'ls', 'cat' - use the list_dir / read_file tools.
Do NOT look for a Makefile.

If gcc says 'Exit code: 0 (success)', you are done. STOP.
Do not try other commands. Briefly say what you fixed and stop." \
"read_file,write_file,list_dir,search_files,run_command" \
"gcc,make" >/dev/null

# --- COORDINATOR: splits the work among the 3 specialists (fan-out) ---
./pixelgo agent add ai "$WS" coordinator anthropic "$MODEL" \
"You are a code-audit coordinator.

Look at the project files and briefly say what needs to be analyzed.
Do not do the analysis yourself - just describe what is to be done, so the
specialists (security, performance, style) know where to start.

Maximum 5 lines." \
"read_file,list_dir" >/dev/null

# --- SECURITY: specialized analysis (for the fan-out demo) ---
./pixelgo agent add ai "$WS" security anthropic "$MODEL" \
"You analyze ONLY the security of the C code.

You look for: buffer overflows, format strings, injections, missing input checks,
use-after-free, integer overflow.

Report briefly and concretely. Security only - do not comment on style." \
"read_file,list_dir,search_files" >/dev/null

# --- PERFORMANCE: specialized analysis ---
./pixelgo agent add ai "$WS" performance anthropic "$MODEL" \
"You analyze ONLY the performance of the C code.

You look for: unnecessary allocations, redundant copies, inefficient algorithms,
repeated calls that could be cached.

Report briefly and concretely. Performance only." \
"read_file,list_dir,search_files" >/dev/null

# --- STYLE: specialized analysis ---
./pixelgo agent add ai "$WS" style anthropic "$MODEL" \
"You analyze ONLY the style and readability of the C code.

You look for: unclear names, overly long functions, duplicated code, missing
or misleading comments, formatting inconsistencies.

Report briefly and concretely. Style only." \
"read_file,list_dir,search_files" >/dev/null

# --- MERGER: combines the 3 reports ---
./pixelgo agent add ai "$WS" merger anthropic "$MODEL" \
"You receive three separate reports (security, performance, style) about the same code.

Combine them into a SINGLE, prioritized report:
1. Critical problems (security, crashes)
2. Important problems (performance, bugs)
3. Improvements (style)

Remove duplicates. Be concise." \
"write_file" >/dev/null

# --- BUILDER: writes code from scratch and verifies it ---
./pixelgo agent add ai "$WS" builder anthropic "$MODEL" \
"You are a C programmer. You receive a requirement and write the code.

RULE: do not declare yourself done until it COMPILES.
After writing the file, compile it:
  run_command: gcc, args ['-Wall','-Wextra','-o','/tmp/prog','<the file>']

If there are errors, read them and fix. Repeat until it compiles cleanly.
Then RUN the program (run_command: /tmp/prog) to verify it does what it should.

At the end, say what you did and show the output." \
"read_file,write_file,list_dir,run_command" \
"gcc" >/dev/null

# --- TESTER: writes tests for existing code ---
./pixelgo agent add ai "$WS" tester anthropic "$MODEL" \
"You write tests for existing C code.

Read the code, understand what each function does, and write a test file
(test_calc.c) with asserts that cover:
- the normal case
- the edge cases (0, negative, NULL, full buffer)
- the error cases

Compile and run the tests:
  run_command: gcc, args ['-o','/tmp/test','test_calc.c','src/calc.c']
  run_command: /tmp/test

If a test FAILS, that is good - you found a bug. Report it.
Do not modify the source code, just write the tests." \
"read_file,write_file,list_dir,run_command" \
"gcc" >/dev/null

echo "    coordinator  - splits the work (fan-out)"
echo "    reviewer     - reads code, finds problems"
echo "    coder        - fixes and compiles"
echo "    security     - security analysis"
echo "    performance  - performance analysis"
echo "    style        - style analysis"
echo "    merger       - combines the reports"
echo "    builder      - writes code and verifies it"
echo "    tester       - writes tests"

# ---------------------------------------------------------------------------
# THE TEST PROJECT
#
# A C file with REAL, intentional bugs. The agents have something to find.
# We place it in the shared workspace directory.
# ---------------------------------------------------------------------------

echo ""
echo "=== Creating the test project (src/calc.c, with intentional bugs) ==="

# The code goes into the SHARED workspace directory, not into each agent.
# Otherwise each would have its own copy: coder would fix its own, and reviewer
# would read its own (stale) one and report forever that nothing was fixed.
mk_project() {
    local dir="workspaces/$WS/shared/src"
    mkdir -p "$dir"
    cat > "$dir/calc.c" << 'CEOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Simple calculator with history. */

#define MAX_HISTORY 10

struct history {
    char entries[MAX_HISTORY][32];
    int count;
};

/* BUG 1: does not check whether count exceeded MAX_HISTORY -> buffer overflow */
void add_history(struct history *h, const char *entry) {
    strcpy(h->entries[h->count], entry);
    h->count++;
}

/* BUG 2: divides without checking b == 0 -> crash */
int divide(int a, int b) {
    return a / b;
}

/* BUG 3: allocates but never frees -> memory leak */
char *format_result(int value) {
    char *buf = malloc(64);
    sprintf(buf, "Result: %d", value);
    return buf;
}

/* BUG 4: does not check NULL -> crash if malloc fails */
void print_result(int value) {
    char *s = format_result(value);
    printf("%s\n", s);
}

/* BUG 5: off-by-one (<=  instead of <) */
int sum_array(int *arr, int len) {
    int total = 0;
    for (int i = 0; i <= len; i++) {
        total += arr[i];
    }
    return total;
}

int main(void) {
    struct history h;
    h.count = 0;

    add_history(&h, "start");
    print_result(divide(10, 2));

    int nums[] = {1, 2, 3, 4, 5};
    printf("Sum: %d\n", sum_array(nums, 5));

    return 0;
}
CEOF
}

# once - all agents see the same code via the shared directory
mk_project

echo "    in workspaces/$WS/shared/src/calc.c (SHARED between agents)"
echo "    5 intentional bugs: buffer overflow, div/0, memory leak,"
echo "    missing NULL check, off-by-one"

echo ""
echo "=== DONE ==="
echo ""
echo "Run a demo:"
echo ""
echo "  # 1. Code review with correction loop (reviewer <-> coder)"
echo "  ./pixelgo flow run $WS demo/01-code-review.flow \"Check src/calc.c\""
echo ""
echo "  # 2. Parallel analysis (3 agents at once) + combined report"
echo "  ./pixelgo flow run $WS demo/02-parallel-audit.flow \"Analyze src/calc.c\""
echo ""
echo "  # 3. Write code from scratch and verify it"
echo "  ./pixelgo flow run $WS demo/03-build-verify.flow \"Write a program that prints the first 20 Fibonacci numbers\""
echo ""
echo "Or, in the web interface:"
echo "  ./pixelgo serve      -> http://127.0.0.1:8080 -> \"Graphs\" tab"
echo ""
