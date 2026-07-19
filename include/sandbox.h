#ifndef SANDBOX_H
#define SANDBOX_H

#include <limits.h>

/*
 * sandbox_resolve_path:
 *   agent_root  - the agent's root directory (absolute, already canonical)
 *   requested   - the path requested by the agent/tool (relative or absolute, UNTRUSTED)
 *   out         - buffer where we put the resolved, canonical path, if valid
 *
 * Returns 1 if the path is valid AND lies inside agent_root.
 * Returns 0 if the path escapes the sandbox (e.g. "../../etc/passwd") or cannot be resolved.
 *
 * The golden rule: NOBODY touches a file without going through this function.
 */
int sandbox_resolve_path(const char *agent_root, const char *requested, char out[PATH_MAX]);

/*
 * Like sandbox_resolve_path, but accepts TWO allowed roots: the agent's own
 * directory AND the workspace's shared directory.
 * shared_root may be NULL/"" (then it behaves like the single-root variant).
 * Returns 1 if the path is inside ONE of them.
 */
int sandbox_resolve_path_ex(const char *agent_root, const char *shared_root,
                            const char *requested, char out[PATH_MAX]);

#endif
