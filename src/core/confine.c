/* O_PATH is a GNU extension, and it is the right way to name a directory for
   a Landlock rule: it opens the path without opening the file. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "confine.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

/*
 * The default read-only set: what a compiler, a linker and a build tool need
 * in order to work at all, and nothing that holds anything of the user's.
 *
 * Deliberately NOT here: $HOME. A build that needs a per-user cache -
 * ~/.gradle, ~/.cargo, ~/.m2 - is given it explicitly through
 * PIXELGO_CONFINE_READ, because granting all of $HOME would hand over
 * exactly what this is meant to protect: .env, .ssh, .aws, the browser
 * profiles.
 */
#define CONFINE_READ_DEFAULT \
    "/usr:/lib:/lib64:/bin:/sbin:/etc:/opt:/proc/self:/dev/null:/dev/zero:" \
    "/dev/urandom:/dev/random:/dev/tty:/dev/pts"

const char *confine_read_paths(void) {
    const char *env = getenv("PIXELGO_CONFINE_READ");
    return (env && env[0]) ? env : CONFINE_READ_DEFAULT;
}

confine_mode_t confine_mode(void) {
    const char *m = getenv("PIXELGO_CONFINE");
    if (!m || !m[0]) return CONFINE_TRY;
    if (strcmp(m, "off") == 0)     return CONFINE_OFF;
    if (strcmp(m, "require") == 0) return CONFINE_REQUIRE;
    if (strcmp(m, "try") == 0)     return CONFINE_TRY;
    LOG_W("confine: PIXELGO_CONFINE='%s' is not off/try/require, using try", m);
    return CONFINE_TRY;
}

#ifdef __linux__

#include <linux/landlock.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

/* glibc ships no wrappers for these three, so they go through syscall(). */
static int ll_create_ruleset(const struct landlock_ruleset_attr *attr,
                             size_t size, __u32 flags) {
    return (int)syscall(SYS_landlock_create_ruleset, attr, size, flags);
}
static int ll_add_rule(int fd, enum landlock_rule_type type,
                       const void *attr, __u32 flags) {
    return (int)syscall(SYS_landlock_add_rule, fd, type, attr, flags);
}
static int ll_restrict_self(int fd, __u32 flags) {
    return (int)syscall(SYS_landlock_restrict_self, fd, flags);
}

/*
 * Which access bits this kernel knows about.
 *
 * This matters more than it looks, and it has to be masked TWICE.
 *
 * By the running kernel's ABI: asking a ruleset to HANDLE a bit the kernel
 * does not define makes landlock_create_ruleset fail with EINVAL, so a
 * binary built against new headers would simply stop confining anything on
 * an older kernel - the worst possible outcome, since it fails in the
 * permissive direction.
 *
 * And by what the build headers define: a kernel can be newer than the
 * headers pixelgo was compiled against, and a name that does not exist at
 * compile time is a compile error rather than a runtime one. Hence the
 * #ifdefs. Each one costs one access bit we do not confine, which is a small
 * loss; getting it wrong costs the whole ruleset.
 */
static __u64 fs_mask(int abi) {
    __u64 m = LANDLOCK_ACCESS_FS_EXECUTE     |
              LANDLOCK_ACCESS_FS_WRITE_FILE  |
              LANDLOCK_ACCESS_FS_READ_FILE   |
              LANDLOCK_ACCESS_FS_READ_DIR    |
              LANDLOCK_ACCESS_FS_REMOVE_DIR  |
              LANDLOCK_ACCESS_FS_REMOVE_FILE |
              LANDLOCK_ACCESS_FS_MAKE_CHAR   |
              LANDLOCK_ACCESS_FS_MAKE_DIR    |
              LANDLOCK_ACCESS_FS_MAKE_REG    |
              LANDLOCK_ACCESS_FS_MAKE_SOCK   |
              LANDLOCK_ACCESS_FS_MAKE_FIFO   |
              LANDLOCK_ACCESS_FS_MAKE_BLOCK  |
              LANDLOCK_ACCESS_FS_MAKE_SYM;
#ifdef LANDLOCK_ACCESS_FS_REFER
    if (abi >= 2) m |= LANDLOCK_ACCESS_FS_REFER;      /* rename across dirs */
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    if (abi >= 3) m |= LANDLOCK_ACCESS_FS_TRUNCATE;
#endif
#ifdef LANDLOCK_ACCESS_FS_IOCTL_DEV
    if (abi >= 5) m |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
#endif
    return m;
}

static int landlock_abi(void) {
    int abi = ll_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
    return abi > 0 ? abi : -1;
}

int confine_available(void) {
    return landlock_abi() > 0;
}

/* Grants `access` on one path. A path that does not exist is skipped rather
   than failing the whole ruleset: the default read list names directories
   that are absent on some systems (/lib64 on a pure 64-bit layout, /dev/pts
   in a minimal container), and a missing one is not a reason to run
   unconfined. */
static int grant(int ruleset, const char *path, __u64 access) {
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) {
        if (errno != ENOENT)
            LOG_D("confine: cannot open '%s': %s", path, strerror(errno));
        return 0;
    }

    /*
     * Directory-only access bits are invalid on anything that is not a
     * directory, and the kernel rejects the whole rule with EINVAL rather
     * than ignoring the surplus. The read list names devices on purpose -
     * /dev/null, /dev/urandom, /dev/tty - so the mask has to be narrowed
     * for them instead of being passed through.
     */
    struct stat st;
    if (fstat(fd, &st) == 0 && !S_ISDIR(st.st_mode)) {
        __u64 file_bits = LANDLOCK_ACCESS_FS_EXECUTE |
                          LANDLOCK_ACCESS_FS_READ_FILE |
                          LANDLOCK_ACCESS_FS_WRITE_FILE;
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
        file_bits |= LANDLOCK_ACCESS_FS_TRUNCATE;
#endif
#ifdef LANDLOCK_ACCESS_FS_IOCTL_DEV
        file_bits |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
#endif
        access &= file_bits;
        if (access == 0) { close(fd); return 0; }
    }

    struct landlock_path_beneath_attr pb = {
        .allowed_access = access,
        .parent_fd      = fd,
    };
    int rc = ll_add_rule(ruleset, LANDLOCK_RULE_PATH_BENEATH, &pb, 0);
    close(fd);
    if (rc != 0) {
        LOG_W("confine: cannot grant access to '%s': %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

int confine_apply(const agent_t *agent) {
    confine_mode_t mode = confine_mode();
    if (mode == CONFINE_OFF) return 0;

    int abi = landlock_abi();
    if (abi < 0) {
        if (mode == CONFINE_REQUIRE) {
            LOG_E("confine: PIXELGO_CONFINE=require but this kernel has no "
                  "Landlock (needs 5.13 or newer); refusing to run the command");
            return -1;
        }
        LOG_W("confine: this kernel has no Landlock, so the command runs "
              "UNCONFINED and can read any file this user can");
        return 0;
    }

    __u64 all   = fs_mask(abi);
    /* Writing implies being allowed to read what you wrote, so the write set
       is the whole mask rather than the write bits alone. */
    __u64 ro    = LANDLOCK_ACCESS_FS_EXECUTE |
                  LANDLOCK_ACCESS_FS_READ_FILE |
                  LANDLOCK_ACCESS_FS_READ_DIR;

    struct landlock_ruleset_attr attr = { .handled_access_fs = all };
    int rs = ll_create_ruleset(&attr, sizeof(attr), 0);
    if (rs < 0) {
        if (mode == CONFINE_REQUIRE) {
            LOG_E("confine: cannot create a Landlock ruleset (%s) and "
                  "PIXELGO_CONFINE=require; refusing to run the command",
                  strerror(errno));
            return -1;
        }
        LOG_W("confine: cannot create a Landlock ruleset (%s); the command "
              "runs UNCONFINED", strerror(errno));
        return 0;
    }

    int bad = 0;

    /* Writable: where the work happens. */
    if (agent && agent->dir[0])        bad |= grant(rs, agent->dir, all);
    if (agent && agent->shared_dir[0]) bad |= grant(rs, agent->shared_dir, all);

    /*
     * /tmp is writable too. Not a concession made lightly - it is where the
     * demo's gcc puts its object file, and where most toolchains put their
     * intermediates - but confining writes to a world-writable directory
     * protects nothing that was not already shared with every process on the
     * machine. What matters is that /tmp is the ONLY writable path outside
     * the workspace.
     */
    bad |= grant(rs, "/tmp", all);

    /* Read-only: the toolchain, and nothing of the user's. */
    char paths[4096];
    snprintf(paths, sizeof(paths), "%s", confine_read_paths());
    for (char *p = strtok(paths, ":"); p; p = strtok(NULL, ":"))
        if (p[0]) bad |= grant(rs, p, ro);

    if (bad && mode == CONFINE_REQUIRE) {
        LOG_E("confine: a rule could not be added and PIXELGO_CONFINE=require; "
              "refusing to run the command");
        close(rs);
        return -1;
    }

    /*
     * no_new_privs first: without it the kernel refuses to restrict a process
     * that could still gain privileges through a setuid binary, which would
     * be a hole straight through the ruleset.
     */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        LOG_W("confine: PR_SET_NO_NEW_PRIVS failed: %s", strerror(errno));
        close(rs);
        return mode == CONFINE_REQUIRE ? -1 : 0;
    }

    int rc = ll_restrict_self(rs, 0);
    close(rs);

    if (rc != 0) {
        if (mode == CONFINE_REQUIRE) {
            LOG_E("confine: landlock_restrict_self failed (%s) and "
                  "PIXELGO_CONFINE=require; refusing to run the command",
                  strerror(errno));
            return -1;
        }
        LOG_W("confine: landlock_restrict_self failed (%s); the command runs "
              "UNCONFINED", strerror(errno));
        return 0;
    }

    LOG_D("confine: child restricted (Landlock ABI %d)", abi);
    return 0;
}

#else   /* not Linux */

int confine_available(void) { return 0; }

int confine_apply(const agent_t *agent) {
    (void)agent;
    confine_mode_t mode = confine_mode();
    if (mode == CONFINE_OFF) return 0;
    if (mode == CONFINE_REQUIRE) {
        LOG_E("confine: PIXELGO_CONFINE=require, but this platform has no "
              "kernel confinement for child processes; refusing to run the "
              "command");
        return -1;
    }
    /* Once per child process, which is once per run_command - noisy enough to
       be noticed, quiet enough not to drown the output. */
    LOG_W("confine: no kernel confinement on this platform, so the command "
          "runs UNCONFINED and can read any file this user can");
    return 0;
}

#endif
