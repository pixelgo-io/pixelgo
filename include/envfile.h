#ifndef ENVFILE_H
#define ENVFILE_H

/*
 * Loading API keys from a file, so you do not need `export` on every start
 * (and so it also works with --daemon).
 *
 * We look, in order, for:
 *   1. the path in the PIXELGO_ENV_FILE variable, if set
 *   2. ./.env              (in the current directory)
 *   3. ~/.config/pixelgo/env
 *
 * The first one found wins. If none exists, it is not an error - the keys can
 * come directly from the environment (`export`), as before.
 *
 * Format (simple, like any .env):
 *
 *   # comments start with #
 *   ANTHROPIC_API_KEY=sk-ant-...
 *   OPENAI_API_KEY="sk-..."       # the quotes are optional
 *   GEMINI_API_KEY='AIza...'
 *   PIXELGO_LOG_LEVEL=debug
 *
 * IMPORTANT: variables already set in the environment are NOT overwritten.
 * An `export ANTHROPIC_API_KEY=...` takes priority over the file - this way you
 * can temporarily override a key without editing the file.
 */

/* Loads the config file (if it exists) into the environment.
   Returns the number of variables set, or -1 if it found no file. */
int envfile_load(void);

/* Loads a specific file. Returns the number of variables set, -1 on error. */
int envfile_load_path(const char *path);

/* The path of the file that was actually loaded (or "" if none).
   Useful for diagnostic messages. */
const char *envfile_loaded_path(void);

#endif
