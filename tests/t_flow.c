/*
 * Tests for the .flow parser and the transition rules.
 *
 * The interesting part is not parsing - it is flow_next_all(), which decides
 * what runs after a node finishes. That function is where fan-out, conditional
 * edges and loops all meet, and getting the precedence wrong there means graphs
 * that silently take the wrong branch.
 */
#include "flow.h"
#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char tmpdir[256];

static const char *write_flow(const char *name, const char *content) {
    static char path[512];
    snprintf(path, sizeof(path), "%s/%s", tmpdir, name);
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    fputs(content, f);
    fclose(f);
    return path;
}

/* Does out[] contain this target? Order is not guaranteed, so search. */
static int has_target(char out[][MAX_STR], int n, const char *want) {
    for (int i = 0; i < n; i++)
        if (strcmp(out[i], want) == 0) return 1;
    return 0;
}

int main(void) {
    char tmpl[] = "/tmp/pixelgo-flow-XXXXXX";
    char *d = mkdtemp(tmpl);
    if (!d) { perror("mkdtemp"); return 1; }
    snprintf(tmpdir, sizeof(tmpdir), "%s", d);

    flow_graph_t g;
    char out[FLOW_MAX_NODES][MAX_STR];

    section("parsing a simple graph");
    {
        const char *p = write_flow("simple.flow",
            "# a comment\n"
            "\n"
            "entry planner\n"
            "node planner\n"
            "node coder\n"
            "edge planner -> coder\n"
            "edge coder -> END\n");

        ok("loads",                 flow_load(&g, p) == 1);
        eq_str("entry is recorded", g.entry, "planner");
        eq_int("two nodes",         g.node_count, 2);
        eq_int("two edges",         g.edge_count, 2);
    }

    section("malformed files are refused at load time");
    {
        /* flow_load only parses. Whether the nodes correspond to real agents is
           flow_validate's job, against a workspace - so only syntax errors are
           caught here. */
        ok("missing entry",
           flow_load(&g, write_flow("no_entry.flow",
               "node a\nedge a -> END\n")) == 0);

        ok("an edge without an arrow",
           flow_load(&g, write_flow("no_arrow.flow",
               "entry a\nnode a\nedge a b\n")) == 0);

        ok("an edge with an empty endpoint",
           flow_load(&g, write_flow("empty_end.flow",
               "entry a\nnode a\nedge a -> \n")) == 0);

        ok("a file that does not exist",
           flow_load(&g, "/tmp/definitely-not-here.flow") == 0);
    }

    section("unconditional edges: every target runs (fan-out)");
    {
        flow_load(&g, write_flow("fanout.flow",
            "entry coord\n"
            "node coord\nnode a\nnode b\nnode c\n"
            "edge coord -> a\nedge coord -> b\nedge coord -> c\n"
            "edge a -> END\nedge b -> END\nedge c -> END\n"));

        int n = flow_next_all(&g, "coord", "any output", out, FLOW_MAX_NODES);
        eq_int("three targets at once", n, 3);
        ok("a is among them", has_target(out, n, "a"));
        ok("b is among them", has_target(out, n, "b"));
        ok("c is among them", has_target(out, n, "c"));
    }

    section("conditional edges depend on the output");
    {
        flow_load(&g, write_flow("cond.flow",
            "entry reviewer\n"
            "node reviewer\nnode coder\n"
            "edge reviewer -> coder when NEEDS_FIX\n"
            "edge reviewer -> END   when APPROVED\n"));

        int n = flow_next_all(&g, "reviewer", "...NEEDS_FIX...", out, FLOW_MAX_NODES);
        eq_int("one target when the marker matches", n, 1);
        eq_str("and it is the right one", out[0], "coder");

        n = flow_next_all(&g, "reviewer", "all good, APPROVED", out, FLOW_MAX_NODES);
        eq_int("the other marker", n, 1);
        eq_str("goes to END", out[0], FLOW_END);

        n = flow_next_all(&g, "reviewer", "no marker at all", out, FLOW_MAX_NODES);
        eq_int("no marker, no applicable edge", n, 0);
    }

    section("an unconditional edge acts as a fallback");
    {
        flow_load(&g, write_flow("fallback.flow",
            "entry reviewer\n"
            "node reviewer\nnode coder\n"
            "edge reviewer -> coder when NEEDS_FIX\n"
            "edge reviewer -> END\n"));

        int n = flow_next_all(&g, "reviewer", "NEEDS_FIX please", out, FLOW_MAX_NODES);
        ok("the matching conditional edge is taken", has_target(out, n, "coder"));

        n = flow_next_all(&g, "reviewer", "nothing to see", out, FLOW_MAX_NODES);
        eq_int("without a marker, the fallback runs", n, 1);
        eq_str("which is END", out[0], FLOW_END);
    }

    section("loops are allowed");
    {
        ok("a graph with a cycle loads",
           flow_load(&g, write_flow("loop.flow",
               "entry coder\n"
               "node coder\nnode reviewer\n"
               "edge coder -> reviewer\n"
               "edge reviewer -> coder when NEEDS_FIX\n"
               "edge reviewer -> END when APPROVED\n")) == 1);

        int n = flow_next_all(&g, "reviewer", "NEEDS_FIX", out, FLOW_MAX_NODES);
        eq_str("the loop edge is taken", out[0], "coder");
    }

    section("fan-in: counting predecessors");
    {
        flow_load(&g, write_flow("fanin.flow",
            "entry coord\n"
            "node coord\nnode a\nnode b\nnode merger\n"
            "edge coord -> a\nedge coord -> b\n"
            "edge a -> merger\nedge b -> merger\n"
            "edge merger -> END\n"));

        eq_int("merger has two predecessors", flow_predecessor_count(&g, "merger"), 2);
        eq_int("a has one",                   flow_predecessor_count(&g, "a"), 1);
        eq_int("the entry node has none",     flow_predecessor_count(&g, "coord"), 0);
    }

    section("a dead end reports no targets");
    {
        flow_load(&g, write_flow("dead.flow",
            "entry a\nnode a\nnode b\n"
            "edge a -> b\n"));   /* b has no outgoing edge */

        int n = flow_next_all(&g, "b", "whatever", out, FLOW_MAX_NODES);
        eq_int("no transition from b", n, 0);
    }

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
    if (system(cmd) != 0) { /* best effort */ }

    return t_report("flow");
}
