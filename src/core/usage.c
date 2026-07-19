#include "usage.h"
#include "provider.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * THE PRICING TABLE.
 *
 * CAREFUL: model prices CHANGE often. The values here are a reasonable starting
 * point, but they are NOT guaranteed to be current. Check them on the providers'
 * pricing pages.
 *
 * You can override any price from a config file (pricing.conf), without
 * recompiling - see load_pricing_overrides(). This way you are not stuck with old
 * hardcoded prices.
 *
 * The prices are in MICRO-DOLLARS PER MILLION TOKENS.
 *   E.g. $3.00 / 1M tokens  ->  3000000 micro-USD / 1M tokens
 *
 * Model matching is done on PREFIX, so we also catch the dated variants
 * ("claude-sonnet-4-5-20250929" matches "claude-sonnet-4-5").
 */

typedef struct {
    llm_provider_id_t provider;
    char model_prefix[64];
    long input_per_mtok;    /* micro-USD per 1M input tokens   */
    long output_per_mtok;   /* micro-USD per 1M output tokens  */
} price_t;

#define MAX_PRICES 64

static price_t g_prices[MAX_PRICES] = {
    /* --- Anthropic --- */
    { LLM_PROVIDER_ANTHROPIC, "claude-opus",    15000000, 75000000 },
    { LLM_PROVIDER_ANTHROPIC, "claude-sonnet",   3000000, 15000000 },
    { LLM_PROVIDER_ANTHROPIC, "claude-haiku",    1000000,  5000000 },

    /* --- OpenAI --- */
    { LLM_PROVIDER_OPENAI,    "gpt-4o-mini",      150000,   600000 },
    { LLM_PROVIDER_OPENAI,    "gpt-4o",          2500000, 10000000 },
    { LLM_PROVIDER_OPENAI,    "gpt-4",          30000000, 60000000 },

    /* --- Gemini --- */
    { LLM_PROVIDER_GEMINI,    "gemini-2.0-flash", 100000,   400000 },
    { LLM_PROVIDER_GEMINI,    "gemini-1.5-flash",  75000,   300000 },
    { LLM_PROVIDER_GEMINI,    "gemini-1.5-pro",  1250000,  5000000 },
    { LLM_PROVIDER_GEMINI,    "gemini",           100000,   400000 },  /* fallback */
};
static int g_price_count = 10;
static int g_pricing_loaded = 0;

/*
 * Loads price overrides from pricing.conf, if it exists.
 * Format (one line per model):
 *   provider model_prefix input_per_mtok_usd output_per_mtok_usd
 * Example:
 *   anthropic claude-sonnet 3.00 15.00
 *   gemini gemini-2.0-flash 0.10 0.40
 *
 * This way you can update the prices without recompiling - important, because
 * they change.
 */
static void load_pricing_overrides(void) {
    if (g_pricing_loaded) return;
    g_pricing_loaded = 1;

    FILE *f = fopen("pricing.conf", "r");
    if (!f) return;

    char line[256];
    int loaded = 0;

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;

        char prov[32], model[64];
        double in_usd, out_usd;
        if (sscanf(line, "%31s %63s %lf %lf", prov, model, &in_usd, &out_usd) != 4)
            continue;

        llm_provider_id_t pid = provider_from_string(prov);
        if (pid == LLM_PROVIDER_UNKNOWN) continue;

        /* we overwrite if it exists, otherwise we add */
        int found = -1;
        for (int i = 0; i < g_price_count; i++)
            if (g_prices[i].provider == pid &&
                strcmp(g_prices[i].model_prefix, model) == 0) { found = i; break; }

        int idx = (found >= 0) ? found : g_price_count;
        if (idx >= MAX_PRICES) break;

        g_prices[idx].provider = pid;
        snprintf(g_prices[idx].model_prefix, sizeof(g_prices[idx].model_prefix), "%s", model);
        g_prices[idx].input_per_mtok  = (long)(in_usd * 1000000.0);
        g_prices[idx].output_per_mtok = (long)(out_usd * 1000000.0);

        if (found < 0) g_price_count++;
        loaded++;
    }
    fclose(f);

    if (loaded > 0)
        LOG_I("usage: loaded %d prices from pricing.conf", loaded);
}

/* Looks up the price for a model. Prefix matching (the LONGEST prefix wins, so
   "gpt-4o-mini" is not caught by "gpt-4o"). */
static const price_t *find_price(llm_provider_id_t provider, const char *model) {
    load_pricing_overrides();

    const price_t *best = NULL;
    size_t best_len = 0;

    for (int i = 0; i < g_price_count; i++) {
        if (g_prices[i].provider != provider) continue;

        size_t plen = strlen(g_prices[i].model_prefix);
        if (strncmp(model, g_prices[i].model_prefix, plen) != 0) continue;

        if (plen > best_len) {
            best = &g_prices[i];
            best_len = plen;
        }
    }
    return best;
}

long usage_cost_micro(llm_provider_id_t provider, const char *model,
                      long input_tokens, long output_tokens) {
    if (!model || !model[0]) return 0;

    const price_t *p = find_price(provider, model);
    if (!p) {
        /* We do NOT invent a price - better 0 than a false number. */
        LOG_W("usage: I do not know the price for '%s' (%s). Add it to pricing.conf.",
              model, provider_to_string(provider));
        return 0;
    }

    /* cost = tokens * price_per_million / 1,000,000 */
    long cost = 0;
    cost += (input_tokens  * p->input_per_mtok)  / 1000000;
    cost += (output_tokens * p->output_per_mtok) / 1000000;
    return cost;
}

long usage_cost_if_all_on(llm_provider_id_t provider, const char *model,
                          long input_tokens, long output_tokens) {
    return usage_cost_micro(provider, model, input_tokens, output_tokens);
}

void usage_add(usage_t *total, const usage_t *add) {
    if (!total || !add) return;
    total->input_tokens   += add->input_tokens;
    total->output_tokens  += add->output_tokens;
    total->cost_micro_usd += add->cost_micro_usd;
}

void usage_format_cost(long cost_micro_usd, char *out, size_t out_size) {
    if (!out || out_size == 0) return;

    /* Below a tenth of a cent, we show more decimals so it does not look like "$0.00". */
    double usd = (double)cost_micro_usd / 1000000.0;
    if (usd > 0 && usd < 0.01)
        snprintf(out, out_size, "$%.4f", usd);
    else
        snprintf(out, out_size, "$%.2f", usd);
}

const char *usage_pricing_note(void) {
    load_pricing_overrides();
    static char note[128];
    snprintf(note, sizeof(note),
             "%d prices loaded%s (prices change - check pricing.conf)",
             g_price_count,
             g_pricing_loaded ? "" : "");
    return note;
}
