/*
 * Reading the verdict out of an agent's output.
 *
 * The graph used to follow a plain substring search, so an agent reviewing
 * code steered the graph by QUOTING the code it was reviewing. A file with
 * the words NEEDS_FIX anywhere in it - a comment, a test fixture, a string
 * literal - put them in the agent's output, and a verdict nobody gave was
 * acted on.
 *
 * The demo compensated for this in English: "BEGIN your response with the
 * verdict, on the FIRST line, alone" and "REPEAT the verdict at the end
 * too". Those were a workaround in a prompt for a bug in C. After this they
 * describe a convention rather than hold the thing together.
 */

#include "test.h"
#include "flow.h"
#include <string.h>

static void yes(const char *what, const char *output, const char *marker) {
    ok(what, flow_verdict_matches(output, marker) == 1);
}

static void no(const char *what, const char *output, const char *marker) {
    ok(what, flow_verdict_matches(output, marker) == 0);
}

int main(void) {
    section("the verdict on its own first line - the convention agents follow");
    yes("the marker alone",                       "APPROVED",            "APPROVED");
    yes("with a trailing newline",                "APPROVED\n",          "APPROVED");
    yes("followed by the explanation",            "APPROVED\n\nI checked all 5 bugs.\n", "APPROVED");
    yes("indented",                               "  APPROVED\n\nwhy...", "APPROVED");
    yes("with a carriage return, as a model on Windows might send",
        "APPROVED\r\nwhy...", "APPROVED");
    yes("after a blank line or two",              "\n\nAPPROVED\nwhy...", "APPROVED");
    yes("with trailing spaces on the line",       "APPROVED   \nwhy...",  "APPROVED");

    section("repeated at the end, which models do anyway");
    yes("explanation first, verdict last",
        "I checked all 5 bugs. All fixed.\n\nAPPROVED\n", "APPROVED");
    yes("last line, no trailing newline",
        "the overflow check is present\nAPPROVED", "APPROVED");
    yes("both ends, as the demo prompt asks for",
        "APPROVED\n\nAll 5 bugs are fixed.\n\nAPPROVED\n", "APPROVED");

    section("the bug this replaces: a quoted file no longer steers the graph");
    no("the marker inside a sentence",
       "The code is fine now, no NEEDS_FIX from me.\n", "NEEDS_FIX");
    no("the marker in a quoted source line",
       "APPROVED\n\nI note line 12 reads:\n  /* NEEDS_FIX: check the bounds */\n",
       "NEEDS_FIX");
    no("the marker in a quoted test fixture",
       "APPROVED\n\nThe test asserts the string \"NEEDS_FIX\" is handled.\n",
       "NEEDS_FIX");
    no("the marker as part of a longer word",
       "NEEDS_FIXING\n", "NEEDS_FIX");
    no("the marker with something else on its line",
       "NEEDS_FIX maybe\n", "NEEDS_FIX");
    no("the marker on a line in the middle, where no verdict belongs",
       "APPROVED\n\nsome prose\nNEEDS_FIX\nmore prose\nAPPROVED\n", "NEEDS_FIX");

    section("the ambiguity that used to be decided by edge order");
    /* Both markers appear. Before, whichever edge was declared first in the
       .flow file won - the graph followed the file, not the agent. */
    {
        const char *out =
            "APPROVED\n\n"
            "I checked all 5 bugs. I would have said NEEDS_FIX if the\n"
            "overflow were still there, but it is fixed.\n\n"
            "APPROVED\n";
        yes("the declared verdict is read", out, "APPROVED");
        no("the one merely mentioned is not", out, "NEEDS_FIX");
    }

    section("case matters: the markers are uppercase by convention");
    no("lowercase in prose",   "approved\n", "APPROVED");
    no("mixed case",           "Approved\n", "APPROVED");
    yes("uppercase",           "APPROVED\n", "APPROVED");

    section("degenerate input is answered, not crashed on");
    no("empty output",              "",        "APPROVED");
    no("only whitespace",           "  \n\t\n", "APPROVED");
    no("only newlines",             "\n\n\n",  "APPROVED");
    ok("NULL output",      flow_verdict_matches(NULL, "APPROVED") == 0);
    ok("NULL marker",      flow_verdict_matches("APPROVED", NULL) == 0);
    ok("empty marker matches nothing, so an unconditional edge stays the only "
       "fallback", flow_verdict_matches("APPROVED", "") == 0);

    section("a marker longer than the line it is compared against");
    no("the line is shorter than the marker", "AP\n", "APPROVED");
    no("the output is shorter than the marker", "A", "APPROVED");

    return t_report("flow_verdict");
}
