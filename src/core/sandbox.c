#include "sandbox.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <unistd.h>
#include <sys/stat.h>

/*
 * The golden rule: NOBODY touches a file without going through this function.
 *
 * Strategy:
 *   1. Canonicalize the agent's root (it must exist).
 *   2. Build the candidate absolute path (relative -> appended to the root).
 *   3. realpath() requires the file to exist, but write_file must be able to
 *      CREATE new files. Solution: canonicalize the PARENT DIRECTORY (which must
 *      exist and resolves any '..' or symlink in the path), then append the final
 *      file name.
 *   4. Check that the resolved path is strictly inside the root - not just a
 *      textual prefix (root="/a/b" must not accept "/a/bcd").
 *
 * Canonicalizing the parent via realpath also resolves symlinks in the
 * intermediate components, so a symlink escaping the sandbox is caught:
 * realpath follows it to the real destination, and the prefix check fails.
 */

static int resolve_canonical(const char *path, char out[PATH_MAX]) {
    return realpath(path, out) != NULL;
}

/*
 * If the final component exists AND is a symlink, replace the assembled path
 * with what it actually points to.
 *
 * This matters because we canonicalize only the PARENT directory - the last
 * component is appended as text so that write_file can create files that do not
 * exist yet. But a symlink IS that last component, so without this step it is
 * never resolved: "link_out" passes the check as an innocent name inside the
 * sandbox, while open() quietly follows it outside.
 *
 * Returns 0 if the link cannot be resolved (dangling), 1 otherwise. A path that
 * is not a symlink is left untouched.
 */
static int resolve_final_symlink(char *resolved, size_t size) {
    struct stat st;
    if (lstat(resolved, &st) != 0) return 1;      /* does not exist yet: fine */
    if (!S_ISLNK(st.st_mode))      return 1;      /* not a symlink: fine */

    char real[PATH_MAX];
    if (!realpath(resolved, real)) return 0;      /* dangling link: refuse */

    if (snprintf(resolved, size, "%s", real) >= (int)size) return 0;
    return 1;
}

/* Checks: `child` is the root itself or lies strictly under `root`. */
static int is_within(const char *root, const char *child) {
    size_t root_len = strlen(root);
    if (strncmp(child, root, root_len) != 0) return 0;
    /* either exactly the root, or followed by a separator */
    return child[root_len] == '\0' || child[root_len] == '/';
}

static int resolve_within(const char *agent_root, const char *requested, char out[PATH_MAX]) {
    if (!agent_root || !requested || !out) return 0;
    if (requested[0] == '\0') return 0;

    char root_canonical[PATH_MAX];
    if (!resolve_canonical(agent_root, root_canonical)) {
        /* the agent's root does not exist -> misconfiguration, we refuse */
        return 0;
    }

    /* Build the candidate absolute path, checking lengths so we do not truncate
       silently (a truncation could produce a different, unsafe path). */
    char candidate[PATH_MAX];
    int n;
    if (requested[0] == '/') {
        n = snprintf(candidate, sizeof(candidate), "%s", requested);
    } else {
        n = snprintf(candidate, sizeof(candidate), "%s/%s", root_canonical, requested);
    }
    if (n < 0 || n >= (int)sizeof(candidate)) return 0; /* too long -> refuse */

    /* Split into parent-directory + name, so we canonicalize the parent even if
       the final file does not exist yet (useful for write_file). dirname/basename
       may modify the buffer they receive, so we work on copies. */
    char copy_dir[PATH_MAX], copy_base[PATH_MAX];
    snprintf(copy_dir, sizeof(copy_dir), "%s", candidate);
    snprintf(copy_base, sizeof(copy_base), "%s", candidate);
    char *dir_part  = dirname(copy_dir);
    char *base_part = basename(copy_base);

    /* We forbid "." and ".." as the final name - ambiguous and useless here. */
    if (strcmp(base_part, "..") == 0) return 0;

    char dir_canonical[PATH_MAX];
    if (!resolve_canonical(dir_part, dir_canonical)) {
        /* the parent directory does not exist -> we refuse (we do not create directories on the sly) */
        return 0;
    }

    /* The parent must itself be inside the sandbox (catches symlinks that take
       the parent directory outside). */
    if (!is_within(root_canonical, dir_canonical)) return 0;

    char resolved[PATH_MAX];
    /* Special case: base_part == "." means we were asking for the directory itself. */
    if (strcmp(base_part, ".") == 0) {
        n = snprintf(resolved, sizeof(resolved), "%s", dir_canonical);
    } else {
        n = snprintf(resolved, sizeof(resolved), "%s/%s", dir_canonical, base_part);
    }
    if (n < 0 || n >= (int)sizeof(resolved)) return 0;

    /* A symlink as the final component must be followed before we judge it. */
    if (!resolve_final_symlink(resolved, sizeof(resolved))) return 0;

    /* The final golden check. */
    if (!is_within(root_canonical, resolved)) return 0;

    snprintf(out, PATH_MAX, "%s", resolved);
    return 1;
}


/*
 * The variant with TWO allowed roots: the agent's directory AND the workspace's
 * shared directory.
 *
 * Order matters: we try the agent's own root first (most paths are relative to
 * it), then the shared one. If the path is absolute or escapes both, it is
 * refused - so the isolation stays strict: an agent can NOT touch another agent's
 * directory, only its own and the shared one.
 */
/* Checks whether an already-resolved ABSOLUTE path is inside a root. */
static int abs_within_root(const char *root, const char *abs_path) {
    if (!root || !root[0]) return 0;

    char root_canon[PATH_MAX];
    if (!resolve_canonical(root, root_canon)) return 0;

    return is_within(root_canon, abs_path);
}

/*
 * The correct resolution for an agent working in the SHARED directory.
 *
 * BUG FIXED: the old variant appended the relative path to agent_root. But the
 * agent chdirs into shared/, so "src/calc.c" meant cwd/src/calc.c, NOT
 * agent_dir/src/calc.c. The result: the agent "saw" its own (empty) directory
 * instead of the code in shared.
 *
 * Correct: we resolve the relative path against the CWD (where the agent actually
 * works), then check that the result is in ONE of the two allowed roots.
 */
int sandbox_resolve_path_ex(const char *agent_root, const char *shared_root,
                            const char *requested, char out[PATH_MAX]) {
    if (!requested || !requested[0] || !out) return 0;

    /* 1. Build the candidate absolute path, against the cwd (not against root!). */
    char candidate[PATH_MAX];
    if (requested[0] == '/') {
        if (snprintf(candidate, sizeof(candidate), "%s", requested) >= (int)sizeof(candidate))
            return 0;
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) return 0;
        if (snprintf(candidate, sizeof(candidate), "%s/%s", cwd, requested)
            >= (int)sizeof(candidate)) return 0;
    }

    /* 2. Canonicalize (resolves '..', symlinks), allowing the LAST component
          not to exist yet (write_file creates new files). */
    char copy_dir[PATH_MAX], copy_base[PATH_MAX];
    snprintf(copy_dir, sizeof(copy_dir), "%s", candidate);
    snprintf(copy_base, sizeof(copy_base), "%s", candidate);

    char *dir_part = dirname(copy_dir);
    char *base_part = basename(copy_base);

    if (strcmp(base_part, "..") == 0) return 0;

    char dir_canon[PATH_MAX];
    if (!resolve_canonical(dir_part, dir_canon)) return 0;

    char resolved[PATH_MAX];
    int n;
    if (strcmp(base_part, ".") == 0)
        n = snprintf(resolved, sizeof(resolved), "%s", dir_canon);
    else
        n = snprintf(resolved, sizeof(resolved), "%s/%s", dir_canon, base_part);
    if (n < 0 || n >= (int)sizeof(resolved)) return 0;

    /* A symlink as the final component must be followed before we judge it. */
    if (!resolve_final_symlink(resolved, sizeof(resolved))) return 0;

    /* 3. The golden check: the result must be in ONE of the roots. */
    if (!abs_within_root(agent_root, resolved) &&
        !abs_within_root(shared_root, resolved))
        return 0;

    snprintf(out, PATH_MAX, "%s", resolved);
    return 1;
}

/* The single-root variant (compatibility). */
int sandbox_resolve_path(const char *agent_root, const char *requested, char out[PATH_MAX]) {
    return resolve_within(agent_root, requested, out);
}
