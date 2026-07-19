#ifndef SELFHOST_H
#define SELFHOST_H

/*
 * selfhost — pixelgo develops itself.
 *
 * Prepares a dedicated workspace ("pixelgo-dev" by default) in which the AI
 * agents receive the WHOLE pixelgo codebase in shared/, can read/modify/compile
 * it, and the HUMAN is the reviewer: the flow stops after coder and shows the
 * result.
 *
 * Why it is a module of the project, not an external script: re-versioning the
 * project (pixelgo produces the next version of pixelgo) is a core capability,
 * not something glued on from outside. The command lives in the binary, next to
 * 'flow' and 'serve'.
 *
 * It fully reuses the existing infrastructure:
 *   - workspace_create()     -> creates the structure + shared/
 *   - workspace_add_agent()  -> registers the agents (architect, coder)
 *   - flow / serve           -> you run it like any other workspace, on another port
 *
 * Isolation from the production instance:
 *   - a separate workspace (it does not touch your workspaces)
 *   - you serve it on another port: pixelgo serve 8090
 */

#define SELFHOST_WS_DEFAULT   "pixelgo-dev"
#define SELFHOST_FLOW_NAME    "selfhost.flow"

/* Options for init. NULL/0 fields fall back to the defaults. */
typedef struct {
    const char *ws_name;   /* defaults to SELFHOST_WS_DEFAULT                 */
    const char *src_dir;   /* the pixelgo source to copy into shared/; default "." */
    const char *provider;  /* "anthropic" | "openai" | "gemini"; default anthropic */
    const char *model;     /* defaults to a reasonable model for the provider  */
    int force;             /* 1 = if the workspace exists, delete and rebuild it */
} selfhost_opts_t;

/*
 * Creates (or rebuilds, if force) the self-host workspace:
 *   1. workspace_create(ws)
 *   2. copies src_dir -> shared/ (without .git, *.o, workspaces/, the pixelgo binary)
 *   3. adds the architect (read-only, makes the plan) and coder (writes + make) agents
 *   4. writes the flow file (architect -> coder -> END) in the workspace root
 *
 * Returns 0 on success, non-zero on error (details on stderr + log).
 */
int selfhost_init(const selfhost_opts_t *opts);

#endif
