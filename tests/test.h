#ifndef TEST_H
#define TEST_H

/*
 * A deliberately tiny test framework: no dependencies, no runner, no magic.
 *
 * Each test file is a normal program that prints what it checked and exits
 * non-zero if anything failed. `make test` compiles and runs them all. Keeping
 * it this small means the tests never become a thing you have to learn before
 * you can add one.
 */

#include <stdio.h>
#include <string.h>

static int t_failures = 0;
static const char *t_section = "";

/* Groups the output, so a failure is easy to place. */
static void section(const char *name) {
    t_section = name;
    printf("\n  %s\n", name);
}

static void ok(const char *what, int passed) {
    if (passed) {
        printf("    ok    %s\n", what);
    } else {
        printf("    FAIL  %s\n", what);
        t_failures++;
    }
}

static void eq_int(const char *what, long got, long want) {
    if (got == want) {
        printf("    ok    %s\n", what);
    } else {
        printf("    FAIL  %s (got %ld, want %ld)\n", what, got, want);
        t_failures++;
    }
}

static void eq_str(const char *what, const char *got, const char *want) {
    if (got && want && strcmp(got, want) == 0) {
        printf("    ok    %s\n", what);
    } else {
        printf("    FAIL  %s\n      got:  %s\n      want: %s\n",
               what, got ? got : "(null)", want ? want : "(null)");
        t_failures++;
    }
}

/* Call at the end of main; use its value as the exit status. */
static int t_report(const char *suite) {
    (void)t_section;
    printf("\n  %s: %s\n\n", suite, t_failures ? "FAILED" : "all passed");
    return t_failures ? 1 : 0;
}

#endif
