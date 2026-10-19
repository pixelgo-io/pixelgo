/*
 * Confining the processes run_command starts.
 *
 * sandbox.c governs pixelgo's own file tools. It says nothing about a gcc, a
 * make or a gradle the agent starts, and the command allowlist cannot stand
 * in for it: every build tool runs arbitrary code by design, and the agent
 * writes the Makefile. So the boundary has to be in the kernel.
 *
 * These tests run real child processes, because that is the only way to know
 * whether the kernel actually refused. A mock would test the arithmetic of
 * the ruleset, which is not the part that can be wrong.
 *
 * On a kernel without Landlock the confinement checks SKIP rather than fail:
 * reporting a security hole on a machine that cannot close it would train
 * people to ignore the suite. The configuration checks still run everywhere.
 */

#include "test.h"
#include "confine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

static char root[256];     /* a stand-in for the user's project directory */
static char agent_dir[320];
static char shared_dir[320];
static char secret[320];   /* a stand-in for .env */

static void fixture(void) {
    snprintf(root, sizeof(root), "%s/.pixelgo-confine-%d",
             getenv("HOME") ? getenv("HOME") : "/var/tmp", (int)getpid());

    snprintf(agent_dir,  sizeof(agent_dir),  "%s/agent",  root);
    snprintf(shared_dir, sizeof(shared_dir), "%s/shared", root);
    snprintf(secret,     sizeof(secret),     "%s/.env",   root);

    mkdir(root, 0755);
    mkdir(agent_dir, 0755);
    mkdir(shared_dir, 0755);

    FILE *f = fopen(secret, "w");
    if (f) { fputs("ANTHROPIC_API_KEY=sk-ant-must-not-leak\n", f); fclose(f); }

    char p[640];
    snprintf(p, sizeof(p), "%s/ok.c", shared_dir);
    f = fopen(p, "w");
    if (f) { fputs("int main(void){return 0;}\n", f); fclose(f); }
}

static void fixture_clean(void) {
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0) { /* best effort */ }
}

static agent_t mk_agent(void) {
    agent_t a;
    memset(&a, 0, sizeof(a));
    snprintf(a.id,         sizeof(a.id),         "confine-test");
    snprintf(a.dir,        sizeof(a.dir),        "%s", agent_dir);
    snprintf(a.shared_dir, sizeof(a.shared_dir), "%s", shared_dir);
    return a;
}

/* Runs argv in a confined child, with cwd set to the shared directory, and
   returns its exit status. 126 means confinement was required and refused. */
static int run_confined(const agent_t *a, char *const argv[]) {
    fflush(NULL);          /* or the child flushes OUR buffered output on exit */
    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) {
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
        if (confine_apply(a) != 0) _exit(126);
        if (chdir(a->shared_dir) != 0) _exit(125);
        execvp(argv[0], argv);
        _exit(127);
    }
    int st = 0;
    waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int can_read(const agent_t *a, const char *path) {
    char *argv[] = { (char *)"cat", (char *)path, NULL };
    return run_confined(a, argv) == 0;
}

static int have_cmd(const char *cmd) {
    char probe[256];
    snprintf(probe, sizeof(probe), "command -v %s >/dev/null 2>&1", cmd);
    return system(probe) == 0;
}

int main(void) {
    fixture();
    agent_t agent = mk_agent();

    section("the mode is read from the environment");
    unsetenv("PIXELGO_CONFINE");
    ok("nothing set means try - confine where possible, never refuse to run",
       confine_mode() == CONFINE_TRY);
    setenv("PIXELGO_CONFINE", "off", 1);
    ok("off", confine_mode() == CONFINE_OFF);
    setenv("PIXELGO_CONFINE", "require", 1);
    ok("require", confine_mode() == CONFINE_REQUIRE);
    setenv("PIXELGO_CONFINE", "nonsense", 1);
    ok("an unrecognised value falls back to try, not to off",
       confine_mode() == CONFINE_TRY);
    unsetenv("PIXELGO_CONFINE");

    section("the read-only set names a toolchain and nothing of the user's");
    {
        const char *d = confine_read_paths();
        ok("/usr is readable - without it nothing compiles", strstr(d, "/usr") != NULL);
        ok("/etc is readable - ld.so.cache, locale",         strstr(d, "/etc") != NULL);
        ok("/dev/null is readable",                          strstr(d, "/dev/null") != NULL);
        ok("$HOME is NOT in the default set: that is where .env, .ssh and .aws live",
           strstr(d, "$HOME") == NULL && strstr(d, "/home") == NULL);
        setenv("PIXELGO_CONFINE_READ", "/usr:/opt/android-sdk", 1);
        ok("PIXELGO_CONFINE_READ replaces the default, for a toolchain elsewhere",
           strcmp(confine_read_paths(), "/usr:/opt/android-sdk") == 0);
        unsetenv("PIXELGO_CONFINE_READ");
    }

    if (!confine_available()) {
        section("kernel confinement");
        printf("    skip  no Landlock on this kernel (needs 5.13 or newer)\n");
        printf("          the behaviour checks below cannot run here\n");
        fixture_clean();
        return t_report("confine");
    }

    section("a confined child reaches its work and nothing else");
    {
        char p[640];
        snprintf(p, sizeof(p), "%s/ok.c", shared_dir);
        ok("the source in the shared directory is readable", can_read(&agent, p));

        snprintf(p, sizeof(p), "%s/own.txt", agent_dir);
        FILE *f = fopen(p, "w");
        if (f) { fputs("mine\n", f); fclose(f); }
        ok("a file in the agent's own directory is readable", can_read(&agent, p));

        ok("/etc/passwd is readable, because /etc is granted on purpose",
           can_read(&agent, "/etc/passwd"));
    }

    section("the keys are out of reach - this is the whole point");
    {
        ok(".env beside the workspace is NOT readable", !can_read(&agent, secret));

        char p[640];
        snprintf(p, sizeof(p), "%s/.ssh/id_rsa", getenv("HOME") ? getenv("HOME") : "/root");
        ok("a private key in $HOME is not readable", !can_read(&agent, p));

        snprintf(p, sizeof(p), "%s/.bashrc", getenv("HOME") ? getenv("HOME") : "/root");
        ok("nor anything else in $HOME", !can_read(&agent, p));
    }

    section("writes land only where they should");
    {
        char arg[700];
        snprintf(arg, sizeof(arg), "%s/written.txt", shared_dir);
        char *ok_argv[] = { (char *)"touch", arg, NULL };
        ok("a file can be created in the shared directory",
           run_confined(&agent, ok_argv) == 0);

        snprintf(arg, sizeof(arg), "%s/escaped.txt", root);
        char *bad_argv[] = { (char *)"touch", arg, NULL };
        ok("but not one directory up, outside the workspace",
           run_confined(&agent, bad_argv) != 0);

        char *home_argv[] = { (char *)"touch",
                              (char *)"/tmp/../etc/pixelgo-should-not-exist", NULL };
        ok("and not through a path that climbs out",
           run_confined(&agent, home_argv) != 0);
    }

    section("a real build still works - confinement that breaks gcc is useless");
    if (!have_cmd("gcc")) {
        printf("    skip  no gcc on this machine\n");
    } else {
        char *gcc_argv[] = { (char *)"gcc", (char *)"-Wall", (char *)"-c",
                             (char *)"ok.c", (char *)"-o", (char *)"/tmp/ok.o", NULL };
        ok("gcc compiles a file from the shared directory",
           run_confined(&agent, gcc_argv) == 0);

        char *link_argv[] = { (char *)"gcc", (char *)"-o", (char *)"ok",
                              (char *)"ok.c", NULL };
        ok("and links a binary into the shared directory",
           run_confined(&agent, link_argv) == 0);
    }

    section("and the attack it exists to stop: make runs arbitrary code");
    if (!have_cmd("make") || !have_cmd("gcc")) {
        printf("    skip  no make or no gcc on this machine\n");
    } else {
        char p[700];
        snprintf(p, sizeof(p), "%s/Makefile", shared_dir);

        /* an honest Makefile: the agent's real work */
        FILE *f = fopen(p, "w");
        if (f) { fputs("all:\n\tgcc -c ok.c -o ok.o\n", f); fclose(f); }
        char *make_argv[] = { (char *)"make", NULL };
        ok("make builds the project - serious work is possible",
           run_confined(&agent, make_argv) == 0);

        /* the same Makefile, written by an agent that was steered into
           exfiltrating the key. The allowlist cannot tell these apart: both
           are "make". */
        f = fopen(p, "w");
        if (f) { fprintf(f, "all:\n\tcat %s\n", secret); fclose(f); }
        ok("but make cannot read the key, even though it runs arbitrary code",
           run_confined(&agent, make_argv) != 0);
    }

    section("PIXELGO_CONFINE=off does what it says, and is therefore dangerous");
    {
        setenv("PIXELGO_CONFINE", "off", 1);
        ok("with confinement off, the key IS readable - the flag is not cosmetic",
           can_read(&agent, secret));
        unsetenv("PIXELGO_CONFINE");
        ok("and refusing again once it is back on", !can_read(&agent, secret));
    }

    fixture_clean();
    return t_report("confine");
}
