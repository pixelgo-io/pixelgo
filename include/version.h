#ifndef VERSION_H
#define VERSION_H

/*
 * The single source of truth for the version.
 *
 * Bump the patch number for bug fixes, the minor for new features. Keeping it
 * here rather than only in an archive name means `pixelgo --version` always
 * reports what is actually running, which matters when someone reports a bug.
 */
#define PIXELGO_VERSION "20.2"

#endif
