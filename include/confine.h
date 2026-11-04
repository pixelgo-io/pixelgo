#ifndef CONFINE_H
#define CONFINE_H

#include "agent.h"

/*
 * Confining the processes run_command starts.
 *
 * sandbox.c governs pixelgo's OWN file tools: every read_file and write_file
 * goes through sandbox_resolve_path and cannot leave the agent's roots. It
 * says nothing about a process the agent starts. A gcc, a make, a gradle
 * inherits pixelgo's view of the filesystem and can read $HOME/.env as
 * easily as its own source file.
 *
 * The command allowlist is not a substitute, and it is important to be
 * honest about why: every build tool executes arbitrary code by design.
 * `make` runs what the Makefile says, `gradle` runs Groovy from
 * build.gradle, `npm install` runs postinstall scripts, `cargo build` runs
 * build.rs - and the agent writes those files. An allowlist of build tools
 * stops typos and accidents. It is not a boundary.
 *
 * So the boundary has to be below the process, in the kernel. On Linux that
 * is Landlock: the child declares, before exec, which paths it may touch,
 * and the restriction is irreversible and inherited by every descendant.
 * A confined `make` can spawn `gcc`, and neither escapes.
 *
 * On platforms without Landlock this is a no-op that says so once. macOS
 * would need Seatbelt (sandbox-exec), which Apple has deprecated while
 * still shipping it; that is separate work and needs a Mac to write on.
 *
 * WHAT THIS DOES NOT DO: it confines the filesystem only. A confined child
 * can still open sockets, so a build that fetches dependencies keeps
 * working - and so would one that exfiltrates what it read. Network
 * confinement is a later ABI and a separate decision, because most real
 * builds need the network.
 */

/*
 * PIXELGO_CONFINE:
 *   try      (default) confine when the kernel supports it, warn when not
 *   require  refuse to run the command when confinement is unavailable
 *   off      do not confine at all
 *
 * "try" is the default because today there is NO confinement: confining
 * where possible is a strict improvement and breaks nobody, while refusing
 * to run would break every user on a kernel older than 5.13 the moment they
 * upgraded pixelgo. Anyone who wants the stronger promise asks for it.
 */
typedef enum {
    CONFINE_OFF = 0,
    CONFINE_TRY,
    CONFINE_REQUIRE
} confine_mode_t;

confine_mode_t confine_mode(void);

/* Is kernel-level confinement available on this machine? Cheap, and safe to
   call from the parent before forking. */
int confine_available(void);

/*
 * Applies the restriction to the CALLING process. Call it in the child,
 * after fork and before exec - never in the parent, because it cannot be
 * undone.
 *
 * The child may read, write, create and delete inside the agent's own
 * directory, the workspace's shared directory, and the temporary directory.
 * It may read and execute the paths a toolchain needs - /usr, /lib, /etc and
 * friends - and nothing else at all.
 *
 * Returns 0 when the child is confined, or when the mode is "off"; -1 when
 * confinement was required and could not be applied, in which case the
 * caller must not exec.
 */
int confine_apply(const agent_t *agent);

/*
 * The read-only paths a child is given, as a colon-separated list.
 * PIXELGO_CONFINE_READ replaces the default when set.
 *
 * Exposed so a test can assert the default rather than restate it, and so
 * the error message for "your build cannot find its toolchain" can print
 * what the child was actually allowed to see.
 */
const char *confine_read_paths(void);

#endif
