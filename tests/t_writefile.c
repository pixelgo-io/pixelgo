/*
 * Tests for write_file creating missing parent directories.
 *
 * The feature exists because agents kept getting stuck: writing
 * "nginx/default.conf" into a fresh workspace failed, and the model would burn
 * turns inventing workarounds instead of just creating the directory.
 *
 * The risk it introduces is obvious - a tool that creates directories could
 * create them anywhere. So the cases below spend most of their effort on the
 * paths that must NOT work.
 */
#include "agent.h"
#include "tools.h"
#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static char base[256];
static agent_t agent;

static int exists(const char *rel) {
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", base, rel);
    struct stat st;
    return stat(p, &st) == 0;
}

static int is_dir(const char *rel) {
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", base, rel);
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Goes through the dispatcher, exactly as a model-issued call would. */
static tool_result_t write_at(const char *path, const char *content) {
    char json[1024];
    snprintf(json, sizeof(json),
             "{\"path\":\"%s\",\"content\":\"%s\"}", path, content);
    return tool_dispatch(&agent, "write_file", json);
}

int main(void) {
    char tmpl[] = "/tmp/pixelgo-wf-XXXXXX";
    char *d = mkdtemp(tmpl);
    if (!d) { perror("mkdtemp"); return 1; }
    snprintf(base, sizeof(base), "%s", d);

    memset(&agent, 0, sizeof(agent));
    agent.type = AGENT_TYPE_AI;
    snprintf(agent.id, sizeof(agent.id), "tester");
    snprintf(agent.dir, sizeof(agent.dir), "%s", base);
    /* No shared dir: everything must stay under the agent's own directory. */
    agent.shared_dir[0] = 0;
    snprintf(agent.cfg.ai.tools[0], MAX_STR, "write_file");
    agent.cfg.ai.tool_count = 1;

    if (chdir(base) != 0) { perror("chdir"); return 1; }

    section("writing into the agent directory still works");
    {
        tool_result_t r = write_at("plain.txt", "hello");
        ok("a file at the top level", r.ok == 1);
        ok("and it is there",         exists("plain.txt"));
    }

    section("missing parent directories are created");
    {
        tool_result_t r = write_at("nginx/default.conf", "server {}");
        ok("one level deep",        r.ok == 1);
        ok("the directory exists",  is_dir("nginx"));
        ok("the file exists",       exists("nginx/default.conf"));
    }

    section("several levels at once");
    {
        tool_result_t r = write_at("src/app/Http/Controller.php", "<?php");
        ok("three levels deep",      r.ok == 1);
        ok("each level was created", is_dir("src") && is_dir("src/app") && is_dir("src/app/Http"));
        ok("the file exists",        exists("src/app/Http/Controller.php"));
    }

    section("existing directories are reused, not disturbed");
    {
        write_at("nginx/other.conf", "second");
        ok("a second file in the same directory", exists("nginx/other.conf"));
        ok("the first one is untouched",          exists("nginx/default.conf"));
    }

    section("directories are NOT created outside the sandbox");
    {
        tool_result_t r = write_at("../escaped/file.txt", "nope");
        ok("the write is refused", r.ok == 0);

        char p[512];
        snprintf(p, sizeof(p), "%s/../escaped", base);
        struct stat st;
        ok("and no directory was created", stat(p, &st) != 0);
    }

    section("deeper escapes are refused too");
    {
        tool_result_t r = write_at("a/b/../../../escaped2/f.txt", "nope");
        ok("the write is refused", r.ok == 0);

        char p[512];
        snprintf(p, sizeof(p), "%s/../escaped2", base);
        struct stat st;
        ok("nothing was created outside", stat(p, &st) != 0);
    }

    section("an absolute path is refused");
    {
        tool_result_t r = write_at("/tmp/pixelgo-should-not-exist/f.txt", "nope");
        ok("the write is refused", r.ok == 0);
        struct stat st;
        ok("nothing was created", stat("/tmp/pixelgo-should-not-exist", &st) != 0);
    }

    section("a file where a directory should be is an error, not a crash");
    {
        write_at("blocker", "i am a file");
        tool_result_t r = write_at("blocker/inside.txt", "nope");
        ok("the write is refused", r.ok == 0);
    }

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", base);
    if (system(cmd) != 0) { /* best effort */ }

    return t_report("write_file");
}
