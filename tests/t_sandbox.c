/*
 * Tests for the sandbox: the one thing that keeps an agent inside its own
 * directory. Every file operation goes through here, so a hole in this function
 * is a hole in the whole system.
 *
 * The cases below are the ways out that actually get tried: relative escapes,
 * absolute paths, symlinks pointing outside, and prefix collisions where a
 * sibling directory happens to start with the same characters as the root.
 */
#include "sandbox.h"
#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>

static char root[PATH_MAX/2];
static char shared[PATH_MAX/2];

/* A scratch tree:
 *   base/
 *     agent/            <- the sandbox root
 *       file.txt
 *       sub/nested.txt
 *       link_out    -> ../outside/secret.txt
 *       link_in     -> file.txt
 *     agentx/           <- prefix collision with "agent"
 *     shared/           <- the workspace's shared dir
 *     outside/secret.txt
 */
static char base[256];   /* mkdtemp under /tmp: short by construction */

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    fputs(content, f);
    fclose(f);
}

static void setup(void) {
    char tmpl[] = "/tmp/pixelgo-sbox-XXXXXX";
    char *d = mkdtemp(tmpl);
    if (!d) { perror("mkdtemp"); exit(1); }
    snprintf(base, sizeof(base), "%s", d);

    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/agent", base);      mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/agent/sub", base);  mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/agentx", base);     mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/shared", base);     mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/outside", base);    mkdir(p, 0755);

    snprintf(p, sizeof(p), "%s/agent/file.txt", base);       write_file(p, "hello\n");
    snprintf(p, sizeof(p), "%s/agent/sub/nested.txt", base); write_file(p, "nested\n");
    snprintf(p, sizeof(p), "%s/outside/secret.txt", base);   write_file(p, "secret\n");
    snprintf(p, sizeof(p), "%s/agentx/other.txt", base);     write_file(p, "other\n");
    snprintf(p, sizeof(p), "%s/shared/code.c", base);        write_file(p, "int main(){}\n");

    /* symlink escaping the sandbox */
    char target[PATH_MAX], link[PATH_MAX];
    snprintf(target, sizeof(target), "%s/outside/secret.txt", base);
    snprintf(link, sizeof(link), "%s/agent/link_out", base);
    if (symlink(target, link) != 0) perror("symlink");

    /* symlink staying inside */
    snprintf(target, sizeof(target), "%s/agent/file.txt", base);
    snprintf(link, sizeof(link), "%s/agent/link_in", base);
    if (symlink(target, link) != 0) perror("symlink");

    snprintf(root, sizeof(root), "%s/agent", base);
    snprintf(shared, sizeof(shared), "%s/shared", base);
}

static void teardown(void) {
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", base);
    if (system(cmd) != 0) { /* best effort */ }
}

/* Convenience: does this path resolve inside the sandbox? */
static int allowed(const char *requested) {
    char out[PATH_MAX];
    return sandbox_resolve_path(root, requested, out);
}

int main(void) {
    setup();

    section("paths inside the sandbox are allowed");
    ok("a plain file",            allowed("file.txt") == 1);
    ok("a file in a subdirectory", allowed("sub/nested.txt") == 1);
    ok("the directory itself",     allowed(".") == 1);
    ok("a file that does not exist yet (write_file needs this)",
                                   allowed("new.txt") == 1);
    ok("a new file in an existing subdirectory",
                                   allowed("sub/new.txt") == 1);

    section("escapes are refused");
    ok("parent directory",         allowed("../outside/secret.txt") == 0);
    ok("several levels up",        allowed("../../etc/passwd") == 0);
    ok("absolute path",            allowed("/etc/passwd") == 0);
    ok("hidden in the middle",     allowed("sub/../../outside/secret.txt") == 0);
    /* A trailing ".." is refused outright rather than resolved. It would land
       back on the root and be harmless, but allowing it means reasoning about
       where every ".." ends up - cheaper to say no. */
    ok("trailing dot-dot",         allowed("sub/..") == 0);

    section("symlinks are followed, not trusted");
    ok("a symlink pointing outside is refused", allowed("link_out") == 0);
    ok("a symlink staying inside is allowed",   allowed("link_in") == 1);

    section("prefix collisions do not fool the check");
    /* "/tmp/x/agent" must not accept "/tmp/x/agentx/..." just because the
       string starts the same way. */
    char sibling[PATH_MAX];
    snprintf(sibling, sizeof(sibling), "%s/agentx/other.txt", base);
    ok("a sibling directory sharing the prefix", allowed(sibling) == 0);
    ok("via a relative path",                    allowed("../agentx/other.txt") == 0);

    section("odd input is handled, not crashed on");
    ok("empty string",             allowed("") == 0);
    ok("just a slash",             allowed("/") == 0);
    ok("a very long path",         allowed(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == 0);

    section("the resolved path is canonical");
    char out[PATH_MAX];
    if (sandbox_resolve_path(root, "sub/../file.txt", out)) {
        char want[PATH_MAX];
        snprintf(want, sizeof(want), "%s/file.txt", root);
        eq_str("dot-dot inside is collapsed", out, want);
    } else {
        ok("dot-dot inside is collapsed", 0);
    }

    section("two roots: the agent's own and the shared one");
    /* The _ex variant resolves relative paths against the CURRENT DIRECTORY,
       not against the root - because a real agent chdirs into shared/ before
       working. So the test has to stand where the agent stands. */
    char cwd_before[PATH_MAX];
    if (!getcwd(cwd_before, sizeof(cwd_before))) { perror("getcwd"); return 1; }

    if (chdir(shared) != 0) { perror("chdir shared"); return 1; }
    ok("a file in the shared dir is allowed",
       sandbox_resolve_path_ex(root, shared, "code.c", out) == 1);
    ok("escaping both is refused from there",
       sandbox_resolve_path_ex(root, shared, "../outside/secret.txt", out) == 0);

    if (chdir(root) != 0) { perror("chdir root"); return 1; }
    ok("a file in the agent dir is allowed",
       sandbox_resolve_path_ex(root, shared, "file.txt", out) == 1);
    ok("the shared dir is reachable from the agent dir",
       sandbox_resolve_path_ex(root, shared, "../shared/code.c", out) == 1);
    ok("a NULL shared root still allows the agent's own files",
       sandbox_resolve_path_ex(root, NULL, "file.txt", out) == 1);
    ok("with a NULL shared root, the shared dir is out of bounds",
       sandbox_resolve_path_ex(root, NULL, "../shared/code.c", out) == 0);

    if (chdir(cwd_before) != 0) { /* best effort */ }

    teardown();
    return t_report("sandbox");
}
