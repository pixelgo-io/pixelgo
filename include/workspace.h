#ifndef WORKSPACE_H
#define WORKSPACE_H

#include "agent.h"

#define MAX_AGENTS 32

typedef struct {
    char name[MAX_STR];
    char root_dir[MAX_PATH_LEN];   /* e.g. workspaces/project_x */
    agent_t agents[MAX_AGENTS];
    int agent_count;
} workspace_t;

/* creates the directory structure on disk for a new workspace */
int workspace_create(workspace_t *ws, const char *name, const char *base_dir);

/* adds a new agent to the workspace (creates its directory + registers it) */
int workspace_add_agent(workspace_t *ws, agent_t *agent);

/* finds an agent by id, NULL if it does not exist */
agent_t *workspace_find_agent(workspace_t *ws, const char *id);

/* saves / loads the workspace from/to the config file (workspace.conf) */
int workspace_save(workspace_t *ws);
int workspace_load(workspace_t *ws, const char *root_dir);

#endif
