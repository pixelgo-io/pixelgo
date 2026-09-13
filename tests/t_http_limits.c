#include "test.h"
#include "http_server.h"
#include <stdlib.h>

/*
 * http_max_body() reads PIXELGO_HTTP_MAX_BODY fresh from the environment
 * each call and clamps it into [HTTP_MAX_BODY_DEFAULT, HTTP_MAX_BODY_CEILING].
 * This is the only guard between a misconfigured env var and either (a) a
 * silently-too-strict server (below the original 64KB every other route
 * already relies on) or (b) a static socket-read buffer overrun (the
 * ceiling exists specifically so this can never ask for more than the
 * buffer in http_server.c was sized for).
 */

static void test_unset_uses_default(void) {
    unsetenv("PIXELGO_HTTP_MAX_BODY");
    eq_int("unset env: falls back to the 64KB default",
           (long)http_max_body(), HTTP_MAX_BODY_DEFAULT);
}

static void test_within_range_is_used_as_is(void) {
    setenv("PIXELGO_HTTP_MAX_BODY", "1048576", 1);   /* 1MB: comfortably inside the range */
    eq_int("1MB (within range): used exactly as given",
           (long)http_max_body(), 1048576);
    unsetenv("PIXELGO_HTTP_MAX_BODY");
}

static void test_kb_mb_suffixes_are_accepted(void) {
    /* Same format as PIXELGO_DATA_THRESHOLD/--data-threshold - one shared
       parser, not a second bytes-only one for this variable. */
    setenv("PIXELGO_HTTP_MAX_BODY", "500KB", 1);
    eq_int("\"500KB\" suffix: parsed the same way --data-threshold parses it",
           (long)http_max_body(), 500 * 1024);
    unsetenv("PIXELGO_HTTP_MAX_BODY");

    setenv("PIXELGO_HTTP_MAX_BODY", "2MB", 1);
    eq_int("\"2MB\" suffix: parsed correctly",
           (long)http_max_body(), 2L * 1024 * 1024);
    unsetenv("PIXELGO_HTTP_MAX_BODY");
}

static void test_off_is_treated_as_too_small_not_an_error(void) {
    /* "off" is meaningful for --data-threshold (disable the check) but not
       for a body-size cap (the server always needs SOME buffer) - it
       parses successfully to 0 bytes via the shared parser, then falls
       into the same "too small, raise to default" path any other
       too-small value would, rather than being special-cased as invalid. */
    setenv("PIXELGO_HTTP_MAX_BODY", "off", 1);
    eq_int("\"off\": falls back to the default (0 bytes isn't a usable cap)",
           (long)http_max_body(), HTTP_MAX_BODY_DEFAULT);
    unsetenv("PIXELGO_HTTP_MAX_BODY");
}

static void test_above_ceiling_is_clamped(void) {
    setenv("PIXELGO_HTTP_MAX_BODY", "999999999", 1);
    eq_int("absurdly large value: clamped to the ceiling, not accepted as-is",
           (long)http_max_body(), HTTP_MAX_BODY_CEILING);
    unsetenv("PIXELGO_HTTP_MAX_BODY");
}

static void test_below_default_is_raised_to_default(void) {
    setenv("PIXELGO_HTTP_MAX_BODY", "100", 1);
    eq_int("below the default: raised to the default, not honored as a smaller cap",
           (long)http_max_body(), HTTP_MAX_BODY_DEFAULT);
    unsetenv("PIXELGO_HTTP_MAX_BODY");
}

static void test_garbage_falls_back_to_default(void) {
    setenv("PIXELGO_HTTP_MAX_BODY", "not-a-number", 1);
    eq_int("garbage value: falls back to the default rather than erroring out",
           (long)http_max_body(), HTTP_MAX_BODY_DEFAULT);
    unsetenv("PIXELGO_HTTP_MAX_BODY");
}

static void test_zero_and_negative_fall_back_to_default(void) {
    setenv("PIXELGO_HTTP_MAX_BODY", "0", 1);
    eq_int("zero: falls back to the default", (long)http_max_body(), HTTP_MAX_BODY_DEFAULT);

    setenv("PIXELGO_HTTP_MAX_BODY", "-5", 1);
    eq_int("negative: falls back to the default", (long)http_max_body(), HTTP_MAX_BODY_DEFAULT);

    unsetenv("PIXELGO_HTTP_MAX_BODY");
}

static void test_exactly_at_the_ceiling_is_honored(void) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", HTTP_MAX_BODY_CEILING);
    setenv("PIXELGO_HTTP_MAX_BODY", buf, 1);
    eq_int("exactly the ceiling: honored, not treated as 'over'",
           (long)http_max_body(), HTTP_MAX_BODY_CEILING);
    unsetenv("PIXELGO_HTTP_MAX_BODY");
}

int main(void) {
    section("http_max_body: unset / normal range");
    test_unset_uses_default();
    test_within_range_is_used_as_is();
    test_kb_mb_suffixes_are_accepted();
    test_off_is_treated_as_too_small_not_an_error();

    section("http_max_body: clamping at both ends");
    test_above_ceiling_is_clamped();
    test_below_default_is_raised_to_default();
    test_exactly_at_the_ceiling_is_honored();

    section("http_max_body: malformed input never crashes or under-protects");
    test_garbage_falls_back_to_default();
    test_zero_and_negative_fall_back_to_default();

    return t_report("http_limits");
}
