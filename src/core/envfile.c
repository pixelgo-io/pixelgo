#include "envfile.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

static char g_loaded[512] = "";

const char *envfile_loaded_path(void) {
    return g_loaded;
}

/* trims whitespace at both ends, in-place */
static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = 0;
    return s;
}

/* Strips the quotes around the value, if present (single or double). */
static char *unquote(char *s) {
    size_t len = strlen(s);
    if (len >= 2 &&
        ((s[0] == '"' && s[len - 1] == '"') ||
         (s[0] == '\'' && s[len - 1] == '\''))) {
        s[len - 1] = 0;
        return s + 1;
    }
    return s;
}

static int file_exists(const char *path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int envfile_load_path(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    /*
     * Security warning: the file contains API keys. If it is readable by anyone,
     * we say so - but we do not refuse (the user may have their reasons).
     */
    struct stat st;
    if (stat(path, &st) == 0 && (st.st_mode & 0077))
        LOG_W("envfile: '%s' is readable by other users (chmod 600 would be safer)",
              path);

    char line[2048];
    int count = 0, lineno = 0;

    while (fgets(line, sizeof(line), f)) {
        lineno++;
        line[strcspn(line, "\n")] = 0;

        char *s = trim(line);
        if (s[0] == 0 || s[0] == '#') continue;   /* empty or a comment */

        /* the optional form "export KEY=VALUE" - we accept that too */
        if (strncmp(s, "export ", 7) == 0) s = trim(s + 7);

        char *eq = strchr(s, '=');
        if (!eq) {
            LOG_W("envfile: %s:%d ignored (no '=')", path, lineno);
            continue;
        }
        *eq = 0;
        char *key = trim(s);
        char *val = unquote(trim(eq + 1));

        if (key[0] == 0) continue;

        /*
         * We do NOT overwrite what is already in the environment. This way, an
         * `export KEY=...` done by the user takes priority over the file - they can
         * override temporarily without editing anything.
         */
        if (getenv(key) != NULL) {
            LOG_D("envfile: '%s' is already in the environment, keeping the existing value", key);
            continue;
        }

        if (setenv(key, val, 0) == 0)
            count++;
        else
            LOG_W("envfile: could not set '%s'", key);
    }
    fclose(f);

    snprintf(g_loaded, sizeof(g_loaded), "%s", path);
    LOG_I("envfile: loaded '%s' (%d variables)", path, count);
    return count;
}

int envfile_load(void) {
    /* 1. the explicit path from the environment */
    const char *explicit_path = getenv("PIXELGO_ENV_FILE");
    if (explicit_path && explicit_path[0]) {
        if (file_exists(explicit_path))
            return envfile_load_path(explicit_path);
        LOG_W("envfile: PIXELGO_ENV_FILE='%s' does not exist", explicit_path);
        return -1;
    }

    /* 2. ./.env in the current directory */
    if (file_exists(".env"))
        return envfile_load_path(".env");

    /* 3. ~/.config/pixelgo/env */
    const char *home = getenv("HOME");
    if (home && home[0]) {
        char path[512];
        snprintf(path, sizeof(path), "%s/.config/pixelgo/env", home);
        if (file_exists(path))
            return envfile_load_path(path);
    }

    return -1;   /* no file - not an error, the keys can come from export */
}
