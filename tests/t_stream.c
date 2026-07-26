#include "test.h"
#include "events.h"
#include "tools.h"
#include "agent.h"
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

/*
 * Does run_command publish output WHILE the command runs, rather than only at
 * the end?
 *
 * The regression this guards against is silent: without streaming everything
 * still works, the journal just goes quiet for however long the command takes.
 * Nothing errors, nothing crashes - you simply cannot see what is happening.
 * So the test has to assert on the journal's contents, not on the return value.
 */

static int count_lines_containing(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[2048];
    int n = 0;
    while (fgets(line, sizeof(line), f))
        if (strstr(line, needle)) n++;
    fclose(f);
    return n;
}

static agent_t make_agent(void) {
    agent_t a;
    memset(&a, 0, sizeof(a));
    snprintf(a.id, sizeof(a.id), "streamer");
    a.type = AGENT_TYPE_AI;
    snprintf(a.cfg.ai.tools[0], MAX_STR, "run_command");
    a.cfg.ai.tool_count = 1;
    snprintf(a.cfg.ai.run_command_allowlist[0], MAX_STR, "sh");
    a.cfg.ai.allowlist_count = 1;
    return a;
}

int main(void) {
    const char *jpath = "/tmp/pixelgo_stream_test.events";
    remove(jpath);
    setenv("PIXELGO_EVENTS_FILE", jpath, 1);

    if (!events_open_from_env()) {
        printf("\n  FAIL: could not open the test journal\n\n");
        return 1;
    }
    events_set_node("streamer");

    agent_t agent = make_agent();

    section("a command's output reaches the journal as it runs");
    tool_result_t r = tool_dispatch(&agent, "run_command",
        "{\"command\":\"sh\",\"args\":[\"-c\","
        "\"printf 'alpha\\\\nbeta\\\\ngamma\\\\n'\"]}");

    ok("the command succeeded", r.ok);

    int chunks = count_lines_containing(jpath, "\"t\":\"tool_output\"");
    char msg[128];
    snprintf(msg, sizeof(msg), "journal got %d tool_output events (want 3)", chunks);
    ok(msg, chunks == 3);

    ok("each line was emitted separately, not as one blob",
       count_lines_containing(jpath, "alpha") == 1 &&
       count_lines_containing(jpath, "beta")  == 1 &&
       count_lines_containing(jpath, "gamma") == 1);

    ok("the model still receives the whole output",
       strstr(r.output, "alpha") && strstr(r.output, "gamma"));

    section("a line with no trailing newline is still published");
    remove(jpath);
    events_close();
    events_open_from_env();

    r = tool_dispatch(&agent, "run_command",
        "{\"command\":\"sh\",\"args\":[\"-c\",\"printf 'dangling'\"]}");
    ok("the unterminated final line reached the journal",
       count_lines_containing(jpath, "dangling") == 1);

    section("carriage returns split lines (docker progress bars)");
    remove(jpath);
    events_close();
    events_open_from_env();

    r = tool_dispatch(&agent, "run_command",
        "{\"command\":\"sh\",\"args\":[\"-c\",\"printf 'step1\\\\rstep2\\\\rstep3\\\\n'\"]}");
    ok("a \\r-redrawn progress line becomes 3 journal lines",
       count_lines_containing(jpath, "\"t\":\"tool_output\"") == 3);

    section("a failing command still streams what it printed");
    remove(jpath);
    events_close();
    events_open_from_env();

    r = tool_dispatch(&agent, "run_command",
        "{\"command\":\"sh\",\"args\":[\"-c\",\"printf 'before failure\\\\n'; exit 3\"]}");
    ok("the command reported failure", !r.ok);
    ok("its output was streamed anyway",
       count_lines_containing(jpath, "before failure") == 1);

    section("streaming is silent when there is no journal (CLI runs)");
    events_close();
    unsetenv("PIXELGO_EVENTS_FILE");
    r = tool_dispatch(&agent, "run_command",
        "{\"command\":\"sh\",\"args\":[\"-c\",\"printf 'cli output\\\\n'\"]}");
    ok("the command still works without a journal", r.ok);
    ok("and the model still gets its output", strstr(r.output, "cli output") != NULL);


    section("a command with huge output runs to completion, keeping head and tail");
    events_close();
    unsetenv("PIXELGO_EVENTS_FILE");

    /*
     * The regression: the old code broke out of the read loop when the buffer
     * filled and then SIGTERMed the command. A docker build was killed for
     * being verbose, leaving containers half-created, and the model was handed
     * the FIRST 8 KB - apt package chatter - while the verdict at the end was
     * thrown away.
     */
    r = tool_dispatch(&agent, "run_command",
        "{\"command\":\"sh\",\"args\":[\"-c\","
        "\"echo BEGIN_MARKER; i=0; while [ $i -lt 4000 ]; do "
        "echo 'padding line to fill the buffer with plausible build output'; "
        "i=$((i+1)); done; echo END_MARKER; exit 0\"]}");

    ok("the command reported success, not a kill", r.ok);
    ok("the START of the output survived", strstr(r.output, "BEGIN_MARKER") != NULL);
    ok("the END of the output survived - where the verdict lives",
       strstr(r.output, "END_MARKER") != NULL);
    ok("the result says what was dropped", strstr(r.output, "omitted") != NULL);

    section("a failing command with huge output still reports failure");
    r = tool_dispatch(&agent, "run_command",
        "{\"command\":\"sh\",\"args\":[\"-c\","
        "\"i=0; while [ $i -lt 4000 ]; do echo 'noise noise noise noise noise'; "
        "i=$((i+1)); done; echo FATAL_ERROR_HERE; exit 1\"]}");

    ok("failure is reported as failure", !r.ok);
    ok("the final error line survived the truncation",
       strstr(r.output, "FATAL_ERROR_HERE") != NULL);

    events_close();
    remove(jpath);
    return t_report("streaming");
}
