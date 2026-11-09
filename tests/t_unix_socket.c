#include "test.h"
#include "http_server.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>

/*
 * The unix-socket listener (PIXELGO_UNIX_SOCKET).
 *
 * Why it exists: pixelgo-ops runs outside the VM and needs pixelgo's API
 * without a network in between. A unix socket is a file, so ops can be built
 * and tested against a real pixelgo today, with no hypervisor and no VM - the
 * vsock listener is later and is the same code with a different address family.
 *
 * What is worth testing is not "does a socket appear". It is the handful of
 * things that make the difference between a channel you can rely on and one
 * that eats a running server's socket or hands the API to the wrong user:
 *
 *   - the mode is 0600, because on this channel the filesystem IS the access
 *     control; there is no loopback bind to hide behind
 *   - a socket file held by a LIVE server is never taken away
 *   - a socket file left by a dead one is cleaned up, since it looks identical
 *   - the file does not outlive the server
 *   - the Host/Origin rule, which exists for browsers, is not applied to a
 *     channel no browser can open
 *
 * The server is started as a child process and spoken to over the socket, so
 * this exercises the real listener rather than a copy of its logic.
 */

#define TEST_PORT 18771

static char g_sock[256];
static char g_dir[128];

/* --- a minimal HTTP client over a unix socket ------------------------- */

static int sock_connect(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);

    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) { close(fd); return -1; }
    return fd;
}

/*
 * Sends a request and returns the status code, or -1. `host` goes into the Host
 * header as given, which is the point of two of the tests below.
 */
static int request_status(const char *path, const char *host, const char *route) {
    int fd = sock_connect(path);
    if (fd < 0) return -1;

    char req[512];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                     route, host);
    if (write(fd, req, (size_t)n) != n) { close(fd); return -1; }

    char buf[256];
    ssize_t got = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (got <= 0) return -1;
    buf[got] = 0;

    int status = 0;
    if (sscanf(buf, "HTTP/1.%*d %d", &status) != 1) return -1;
    return status;
}

/* --- the server under test ------------------------------------------- */

static pid_t g_server = 0;

/* Waits until the socket accepts a connection, up to ~3s. */
static int server_ready(const char *path) {
    for (int i = 0; i < 300; i++) {
        int fd = sock_connect(path);
        if (fd >= 0) { close(fd); return 1; }
        usleep(10 * 1000);
    }
    return 0;
}

static int start_server(void) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        /* Quiet, and out of the way of the repository's own files. */
        setenv("PIXELGO_UNIX_SOCKET", g_sock, 1);
        setenv("PIXELGO_LOG_LEVEL", "error", 1);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); close(devnull); }

        char port[16];
        snprintf(port, sizeof(port), "%d", TEST_PORT);
        execl("./pixelgo", "./pixelgo", "serve", port, (char *)NULL);
        _exit(127);
    }
    g_server = pid;
    return server_ready(g_sock);
}

static void stop_server(void) {
    if (g_server <= 0) return;
    kill(g_server, SIGTERM);
    for (int i = 0; i < 200; i++) {
        if (waitpid(g_server, NULL, WNOHANG) == g_server) { g_server = 0; return; }
        usleep(10 * 1000);
    }
    kill(g_server, SIGKILL);
    waitpid(g_server, NULL, 0);
    g_server = 0;
}

/* --- the tests ------------------------------------------------------- */

static void test_serves_the_api(void) {
    section("the API answers over the socket, with the mode locked down");

    if (!start_server()) { ok("the server came up on the socket", 0); return; }
    ok("the server came up on the socket", 1);

    struct stat st;
    ok("the socket file exists", stat(g_sock, &st) == 0);
    ok("and it is a socket", S_ISSOCK(st.st_mode));
    eq_int("mode 0600 - on this channel the filesystem is the access control",
           (long)(st.st_mode & 07777), 0600);

    eq_int("a known route answers", request_status(g_sock, "localhost", "/api/workspaces"), 200);
    eq_int("an unknown route still 404s rather than being refused",
           request_status(g_sock, "localhost", "/api/nope"), 404);
}

/*
 * The Host/Origin check defends a browser against being talked into reaching
 * the API - a rebound hostname, a form on another page. It recognises our own
 * authority in Host and Origin, and over a unix socket there is no authority to
 * recognise and no browser that could open one. Applying it here would mean
 * guessing what Host a non-browser client ought to have sent, so it is not
 * applied; the 0600 socket file is the check instead.
 *
 * Asserted both ways round, so that a change which starts enforcing it on this
 * channel - or stops enforcing it on TCP - fails here.
 */
static void test_host_rule_is_per_channel(void) {
    section("the browser rule applies to the browser channel, not this one");

    eq_int("a foreign Host is served over the socket",
           request_status(g_sock, "evil.example.com", "/api/workspaces"), 200);

    /* The same request over TCP, which is where the rule belongs. */
    http_req_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.host, sizeof(req.host), "evil.example.com");
    ok("and refused over TCP", !http_origin_allowed(&req, TEST_PORT));

    snprintf(req.host, sizeof(req.host), "127.0.0.1:%d", TEST_PORT);
    ok("while our own authority is still allowed over TCP",
       http_origin_allowed(&req, TEST_PORT));
}

/*
 * Starts a server with the given socket path, in a directory of its own so that
 * the PID file does not decide the outcome, and reports whether it refused to
 * start. Returns 1 if it exited non-zero, 0 if it came up or hung.
 */
static int server_refuses(const char *sock_path, const char *port) {
    char cwd[512];
    if (!getcwd(cwd, sizeof(cwd))) return 0;
    char bin[600];
    snprintf(bin, sizeof(bin), "%s/pixelgo", cwd);

    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        setenv("PIXELGO_UNIX_SOCKET", sock_path, 1);
        setenv("PIXELGO_LOG_LEVEL", "error", 1);
        if (chdir(g_dir) != 0) _exit(126);

        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
        execl(bin, bin, "serve", port, (char *)NULL);
        _exit(127);
    }

    int status = 0, exited = 0;
    for (int i = 0; i < 300; i++) {          /* it must not sit there serving */
        if (waitpid(pid, &status, WNOHANG) == pid) { exited = 1; break; }
        usleep(10 * 1000);
    }
    if (!exited) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); return 0; }

    return WIFEXITED(status) && WEXITSTATUS(status) != 0;
}

static void test_live_socket_is_not_stolen(void) {
    section("a socket held by a live server is left alone");

    ok("a second server on the same socket refuses to start",
       server_refuses(g_sock, "18772"));
    eq_int("and the first one is still answering",
           request_status(g_sock, "localhost", "/api/workspaces"), 200);
}

static void test_socket_does_not_outlive_the_server(void) {
    section("the socket file is removed when the server stops");

    stop_server();

    struct stat st;
    ok("the file is gone", stat(g_sock, &st) != 0 && errno == ENOENT);
}

/*
 * A socket file left behind by a crash is indistinguishable from a live one by
 * looking at it, which is why the listener asks before removing it. Here
 * nothing is listening, so it must be cleaned up and the server must come up.
 */
static void test_stale_socket_is_cleaned_up(void) {
    section("a socket left by a dead server is cleaned up, not fatal");

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ok("a leftover socket file was created", fd >= 0);
    if (fd < 0) return;

    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", g_sock);
    ok("bound", bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0);
    close(fd);                               /* never listened: stale */

    struct stat st;
    ok("the stale file is there before we start", stat(g_sock, &st) == 0);

    ok("the server starts anyway", start_server());
    eq_int("and serves", request_status(g_sock, "localhost", "/api/workspaces"), 200);

    stop_server();
}

/*
 * A path that cannot fit in sun_path is a configuration error. Truncating it
 * would bind a DIFFERENT socket than the one that was asked for, and ops would
 * then connect to a path nothing is listening on - a failure that looks like a
 * network problem and is not one.
 */
static void test_overlong_path_is_refused(void) {
    section("a path too long for sun_path is refused, not truncated");

    struct sockaddr_un a;
    char longpath[sizeof(a.sun_path) + 32];
    memset(longpath, 'a', sizeof(longpath) - 1);
    longpath[0] = '/';
    longpath[sizeof(longpath) - 1] = 0;

    ok("the path really is longer than sun_path",
       strlen(longpath) >= sizeof(a.sun_path));
    ok("and the server refuses to start with it",
       server_refuses(longpath, "18773"));
}

/* A regular file where the socket should go is not ours to delete: it is far
   more likely to be something of the user's than a leftover of ours. */
static void test_non_socket_path_is_refused(void) {
    section("an existing regular file at that path is not replaced");

    char path[sizeof(g_dir) + 32];
    snprintf(path, sizeof(path), "%s/not-a-socket", g_dir);

    FILE *f = fopen(path, "w");
    ok("a regular file is in the way", f != NULL);
    if (!f) return;
    fprintf(f, "something of the user's\n");
    fclose(f);

    ok("the server refuses to start", server_refuses(path, "18774"));

    struct stat st;
    ok("and the file is still there", stat(path, &st) == 0 && S_ISREG(st.st_mode));
    unlink(path);
}

int main(void) {
    printf("\n=== unix socket ===\n");

    snprintf(g_dir, sizeof(g_dir), "/tmp/px_usock_%d", (int)getpid());
    if (mkdir(g_dir, 0700) != 0 && errno != EEXIST) {
        printf("  cannot create %s\n", g_dir);
        return 1;
    }
    snprintf(g_sock, sizeof(g_sock), "%s/api.sock", g_dir);

    test_serves_the_api();
    test_host_rule_is_per_channel();
    test_live_socket_is_not_stolen();
    test_socket_does_not_outlive_the_server();
    test_stale_socket_is_cleaned_up();
    test_overlong_path_is_refused();
    test_non_socket_path_is_refused();

    stop_server();
    unlink(g_sock);
    rmdir(g_dir);

    return t_report("unix socket");
}
