/*
 * Tests for the two independent gates on what an agent may do:
 *
 *   tools[]              - which tools it can call at all
 *   run_command_allowlist - which commands run_command will execute
 *   approval_tools[]     - which tools stop and ask the human first
 *
 * Both matching rules are exact string comparison, and that is deliberate: a
 * prefix or substring match would be easy to slip past ("gcc" matching
 * "gcc-evil"). The tests below pin that down, because it is the kind of thing a
 * well-meaning refactor loosens.
 */
#include "agent.h"
#include "tools.h"
#include "test.h"
#include <string.h>

static agent_t make_agent(void) {
    agent_t a;
    memset(&a, 0, sizeof(a));
    a.type = AGENT_TYPE_AI;
    snprintf(a.id, sizeof(a.id), "coder");
    return a;
}

static void add_tool(agent_t *a, const char *name) {
    snprintf(a->cfg.ai.tools[a->cfg.ai.tool_count++], MAX_STR, "%s", name);
}

static void add_approval(agent_t *a, const char *name) {
    snprintf(a->cfg.ai.approval_tools[a->cfg.ai.approval_count++], MAX_STR, "%s", name);
}

int main(void) {
    section("tool permissions");
    {
        agent_t a = make_agent();
        add_tool(&a, "read_file");
        add_tool(&a, "write_file");

        ok("a granted tool is allowed",      agent_has_tool(&a, "read_file") == 1);
        ok("another granted tool",           agent_has_tool(&a, "write_file") == 1);
        ok("a tool never granted",           agent_has_tool(&a, "run_command") == 0);
        ok("a prefix of a granted tool",     agent_has_tool(&a, "read") == 0);
        ok("a granted tool plus a suffix",   agent_has_tool(&a, "read_file2") == 0);
        ok("the empty string",               agent_has_tool(&a, "") == 0);
    }

    section("an agent with no tools can do nothing");
    {
        agent_t a = make_agent();
        ok("read_file is refused",   agent_has_tool(&a, "read_file") == 0);
        ok("run_command is refused", agent_has_tool(&a, "run_command") == 0);
    }

    section("worker agents have no tools at all");
    {
        agent_t a = make_agent();
        a.type = AGENT_TYPE_WORKER;
        add_tool(&a, "read_file");   /* even if the field is somehow populated */
        ok("a worker is refused regardless", agent_has_tool(&a, "read_file") == 0);
    }

    section("approval is per tool");
    {
        agent_t a = make_agent();
        add_tool(&a, "read_file");
        add_tool(&a, "write_file");
        add_tool(&a, "run_command");
        add_approval(&a, "write_file");
        add_approval(&a, "run_command");

        ok("a listed tool needs approval",      tool_needs_approval(&a, "write_file") == 1);
        ok("another listed tool",               tool_needs_approval(&a, "run_command") == 1);
        ok("an unlisted tool runs freely",      tool_needs_approval(&a, "read_file") == 0);
        ok("a prefix does not match",           tool_needs_approval(&a, "write_fil") == 0);
        ok("a suffix does not match",           tool_needs_approval(&a, "write_file2") == 0);
        ok("the empty string",                  tool_needs_approval(&a, "") == 0);
    }

    section("no approval configured means unsupervised");
    {
        agent_t a = make_agent();
        add_tool(&a, "run_command");
        ok("nothing needs approval", tool_needs_approval(&a, "run_command") == 0);
    }

    section("approval and permission are independent");
    {
        /* A tool can be marked for approval without being granted. The agent
           still cannot call it - approval never widens what is permitted. */
        agent_t a = make_agent();
        add_tool(&a, "read_file");
        add_approval(&a, "run_command");

        ok("run_command is still not permitted", agent_has_tool(&a, "run_command") == 0);
        ok("but it is marked for approval",      tool_needs_approval(&a, "run_command") == 1);
    }

    return t_report("permissions");
}
