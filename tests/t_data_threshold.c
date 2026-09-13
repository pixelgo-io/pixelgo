#include "test.h"
#include "data_request_audit.h"
#include <stdlib.h>
#include <string.h>

/*
 * approve_data_threshold_bytes is three-state (see agent.h): -1 = not
 * specified (inherits PIXELGO_DATA_THRESHOLD), 0 = explicitly off
 * (overrides the global default), >0 = explicit value (overrides the
 * global default). This exercises the shared parser (data_threshold_parse,
 * used identically by the CLI flag, workspace.conf, and the env var) and
 * the global-default resolver (data_threshold_global) that ai_loop.c
 * consults whenever an agent's own field is -1.
 */

static void test_parse_off(void) {
    long bytes = -999;
    int parsed = data_threshold_parse("off", &bytes);
    ok("\"off\": parses successfully", parsed);
    eq_int("\"off\": yields 0 bytes", bytes, 0);
}

static void test_parse_off_case_insensitive(void) {
    long bytes = -999;
    int parsed = data_threshold_parse("OFF", &bytes);
    ok("\"OFF\": parses successfully (case-insensitive)", parsed);
    eq_int("\"OFF\": yields 0 bytes", bytes, 0);
}

static void test_parse_kb_mb_gb(void) {
    long bytes = 0;
    data_threshold_parse("500KB", &bytes);
    eq_int("500KB -> 512000 bytes", bytes, 500 * 1024);

    data_threshold_parse("2MB", &bytes);
    eq_int("2MB -> bytes", bytes, 2L * 1024 * 1024);

    data_threshold_parse("1GB", &bytes);
    eq_int("1GB -> bytes", bytes, 1L * 1024 * 1024 * 1024);
}

static void test_parse_raw_bytes_and_unknown_unit(void) {
    long bytes = 0;
    data_threshold_parse("12345", &bytes);
    eq_int("raw number, no unit -> treated as bytes", bytes, 12345);

    data_threshold_parse("99XX", &bytes);
    eq_int("unrecognized unit -> still treated as raw bytes (lenient, matches original behavior)",
           bytes, 99);
}

static void test_parse_rejects_garbage(void) {
    long bytes = 777;
    int parsed = data_threshold_parse("not-a-size", &bytes);
    ok("garbage input: parse fails", !parsed);
    eq_int("garbage input: out_bytes left untouched", bytes, 777);

    parsed = data_threshold_parse("", &bytes);
    ok("empty string: parse fails", !parsed);

    parsed = data_threshold_parse(NULL, &bytes);
    ok("NULL: parse fails, does not crash", !parsed);
}

static void test_parse_rejects_negative(void) {
    long bytes = 777;
    int parsed = data_threshold_parse("-5", &bytes);
    ok("negative number: parse fails rather than yielding a negative threshold", !parsed);
    eq_int("negative number: out_bytes left untouched", bytes, 777);
}

static void test_global_unset(void) {
    unsetenv("PIXELGO_DATA_THRESHOLD");
    eq_int("PIXELGO_DATA_THRESHOLD unset: global default is 0 (disabled)",
           data_threshold_global(), 0);
}

static void test_global_set_to_size(void) {
    setenv("PIXELGO_DATA_THRESHOLD", "1MB", 1);
    eq_int("PIXELGO_DATA_THRESHOLD=1MB: global default resolves to bytes",
           data_threshold_global(), 1024 * 1024);
    unsetenv("PIXELGO_DATA_THRESHOLD");
}

static void test_global_set_to_off(void) {
    setenv("PIXELGO_DATA_THRESHOLD", "off", 1);
    eq_int("PIXELGO_DATA_THRESHOLD=off: global default is 0",
           data_threshold_global(), 0);
    unsetenv("PIXELGO_DATA_THRESHOLD");
}

static void test_global_garbage_falls_back_to_disabled(void) {
    setenv("PIXELGO_DATA_THRESHOLD", "garbage", 1);
    eq_int("PIXELGO_DATA_THRESHOLD=garbage: falls back to 0 (disabled), not a crash",
           data_threshold_global(), 0);
    unsetenv("PIXELGO_DATA_THRESHOLD");
}

/*
 * The precedence rule ai_loop.c implements inline:
 *   effective = (agent_field >= 0) ? agent_field : data_threshold_global()
 * Exercised here directly against the three agent-field states, so a
 * future refactor of that inline expression has something to break against.
 */
static void test_precedence_not_specified_inherits_global(void) {
    setenv("PIXELGO_DATA_THRESHOLD", "2MB", 1);
    long agent_field = -1;   /* not specified */
    long effective = (agent_field >= 0) ? agent_field : data_threshold_global();
    eq_int("agent field -1 (not specified): inherits the global 2MB default",
           effective, 2L * 1024 * 1024);
    unsetenv("PIXELGO_DATA_THRESHOLD");
}

static void test_precedence_explicit_off_wins_over_global(void) {
    setenv("PIXELGO_DATA_THRESHOLD", "2MB", 1);
    long agent_field = 0;   /* explicitly off */
    long effective = (agent_field >= 0) ? agent_field : data_threshold_global();
    eq_int("agent field 0 (explicitly off): stays 0 even though a global default is set",
           effective, 0);
    unsetenv("PIXELGO_DATA_THRESHOLD");
}

static void test_precedence_explicit_value_wins_over_global(void) {
    setenv("PIXELGO_DATA_THRESHOLD", "2MB", 1);
    long agent_field = 500000;   /* explicit, different from the global */
    long effective = (agent_field >= 0) ? agent_field : data_threshold_global();
    eq_int("agent field 500000 (explicit): overrides the global 2MB default",
           effective, 500000);
    unsetenv("PIXELGO_DATA_THRESHOLD");
}

int main(void) {
    section("data_threshold_parse: \"off\"");
    test_parse_off();
    test_parse_off_case_insensitive();

    section("data_threshold_parse: sizes with units");
    test_parse_kb_mb_gb();
    test_parse_raw_bytes_and_unknown_unit();

    section("data_threshold_parse: rejects invalid input");
    test_parse_rejects_garbage();
    test_parse_rejects_negative();

    section("data_threshold_global: PIXELGO_DATA_THRESHOLD resolution");
    test_global_unset();
    test_global_set_to_size();
    test_global_set_to_off();
    test_global_garbage_falls_back_to_disabled();

    section("precedence: per-agent field vs. the global default");
    test_precedence_not_specified_inherits_global();
    test_precedence_explicit_off_wins_over_global();
    test_precedence_explicit_value_wins_over_global();

    return t_report("data_threshold");
}
