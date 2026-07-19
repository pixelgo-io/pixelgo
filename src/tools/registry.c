#include "tools.h"
#include "log.h"
#include <string.h>
#include <stdio.h>

/* Implementations - in file_tools.c */
tool_result_t tool_read_file(agent_t *agent, const char *input_json);
tool_result_t tool_write_file(agent_t *agent, const char *input_json);
tool_result_t tool_list_dir(agent_t *agent, const char *input_json);

/* Implementations - in exec_tools.c */
tool_result_t tool_run_command(agent_t *agent, const char *input_json);
tool_result_t tool_search_files(agent_t *agent, const char *input_json);

/*
 * A table-driven registry. Adding a new tool = a single line here (plus its
 * definition in tool_defs.c so the model "sees" it). There is no need to modify
 * the dispatch logic.
 */
static const tool_entry_t tool_registry[] = {
    { "read_file",    tool_read_file    },
    { "write_file",   tool_write_file   },
    { "list_dir",     tool_list_dir     },
    { "run_command",  tool_run_command  },
    { "search_files", tool_search_files },
};
static const int tool_registry_count =
    (int)(sizeof(tool_registry) / sizeof(tool_registry[0]));

static const tool_entry_t *registry_find(const char *name) {
    for (int i = 0; i < tool_registry_count; i++) {
        if (strcmp(tool_registry[i].name, name) == 0) return &tool_registry[i];
    }
    return NULL;
}

int agent_has_tool(agent_t *agent, const char *tool_name) {
    if (agent->type != AGENT_TYPE_AI) return 0;
    for (int i = 0; i < agent->cfg.ai.tool_count; i++) {
        if (strcmp(agent->cfg.ai.tools[i], tool_name) == 0) return 1;
    }
    return 0;
}

int tool_needs_approval(agent_t *agent, const char *tool_name) {
    if (agent->type != AGENT_TYPE_AI) return 0;
    for (int i = 0; i < agent->cfg.ai.approval_count; i++) {
        if (strcmp(agent->cfg.ai.approval_tools[i], tool_name) == 0) return 1;
    }
    return 0;
}

tool_result_t tool_dispatch(agent_t *agent, const char *tool_name, const char *input_json) {
    /* STEP 1: is the agent actually allowed to use this tool? */
    if (!agent_has_tool(agent, tool_name)) {
        LOG_W("dispatch: tool '%s' NOT permitted for agent '%s'", tool_name, agent->id);
        return tool_err("tool '%s' not permitted for agent '%s'", tool_name, agent->id);
    }

    /* STEP 2: does the tool exist in the registry? */
    const tool_entry_t *entry = registry_find(tool_name);
    if (!entry) {
        LOG_W("dispatch: unknown tool '%s' (not in the registry)", tool_name);
        return tool_err("unknown tool '%s'", tool_name);
    }

    /* STEP 3: execute. Each implementation validates its own arguments. */
    return entry->fn(agent, input_json);
}
