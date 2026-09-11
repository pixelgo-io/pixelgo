#include "data_request_audit.h"
#include "provider.h"
#include "usage.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

long data_audit_estimate_tokens(size_t payload_bytes) {
    /* ~4 bytes per token, the usual rough estimate. Never zero once there is
       any payload, so a tiny non-empty prompt still shows as "~1 token"
       rather than "0". */
    long tok = (long)(payload_bytes / 4);
    if (payload_bytes > 0 && tok == 0) tok = 1;
    return tok;
}

int data_threshold_parse(const char *str, long *out_bytes) {
    if (!str || !str[0] || !out_bytes) return 0;

    if (strcasecmp(str, "off") == 0) {
        *out_bytes = 0;
        return 1;
    }

    long amount = 0;
    char unit[8] = {0};
    if (sscanf(str, "%ld%7s", &amount, unit) < 1) return 0;
    if (amount < 0) return 0;

    if (strcasecmp(unit, "KB") == 0) amount *= 1024;
    else if (strcasecmp(unit, "MB") == 0) amount *= 1024 * 1024;
    else if (strcasecmp(unit, "GB") == 0) amount *= 1024 * 1024 * 1024;
    /* no unit, or an unrecognized one -> treated as raw bytes, matching the
       original behavior of this parser before it was factored out here */

    *out_bytes = amount;
    return 1;
}

long data_threshold_global(void) {
    const char *env = getenv("PIXELGO_DATA_THRESHOLD");
    if (!env || !env[0]) return 0;

    long bytes = 0;
    if (!data_threshold_parse(env, &bytes)) {
        LOG_W("PIXELGO_DATA_THRESHOLD='%s' could not be parsed (expected e.g. "
              "\"500KB\", \"2MB\", or \"off\"), disabling the global default", env);
        return 0;
    }
    return bytes;   /* 0 if explicitly "off", otherwise the byte count */
}

void data_audit_build_summary(llm_provider_id_t provider, const char *model,
                              size_t payload_bytes, long tokens_estimated,
                              long cost_micro_usd,
                              char *out, size_t out_size) {
    char cost_str[16];
    usage_format_cost(cost_micro_usd, cost_str, sizeof(cost_str));

    double mb = payload_bytes / 1024.0 / 1024.0;

    snprintf(out, out_size,
             "%.2f MB payload (~%ld tokens est.), estimated cost %s to %s/%s",
             mb, tokens_estimated, cost_str,
             provider_to_string(provider), model ? model : "?");
}

void data_audit_log_decision(const char *workspace_id, const char *agent_id,
                             size_t payload_bytes, long tokens_estimated,
                             long cost_micro_usd, const char *decision) {
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";

    char dir[400];
    snprintf(dir, sizeof(dir), "%s/.local/share/pixelgo", home);

    /*
     * Best-effort directory creation, without a shell. `run_command`/exec_tools.c
     * deliberately never uses system() to avoid shell interpretation of agent
     * data - the audit log path is built from getenv("HOME") and a fixed
     * suffix, so it is not attacker-controlled the way a payload could be,
     * but a direct mkdir() keeps this file consistent with that policy
     * instead of being the one exception. EEXIST is expected and fine (the
     * directory is usually already there after the first run); any other
     * failure is left for fopen() below to report.
     */
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        /* Non-fatal: fopen below will fail cleanly if the directory truly
           could not be created, and we log that. */
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/data_audit.log", dir);

    FILE *f = fopen(path, "a");
    if (!f) {
        LOG_W("data_audit: could not open %s for the audit log", path);
        return;
    }

    time_t now = time(NULL);
    char cost_str[16];
    usage_format_cost(cost_micro_usd, cost_str, sizeof(cost_str));

    fprintf(f, "%ld | ws=%s | agent=%s | payload=%zu bytes | tokens=~%ld | cost=%s | decision=%s\n",
            (long)now, workspace_id ? workspace_id : "-",
            agent_id ? agent_id : "-", payload_bytes, tokens_estimated,
            cost_str, decision ? decision : "-");

    fclose(f);
}
