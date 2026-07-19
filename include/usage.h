#ifndef USAGE_H
#define USAGE_H

#include "agent.h"

/*
 * Tracking token usage and cost.
 *
 * WHY it matters: the project's central argument is that you can put the cheap
 * model (Gemini Flash) to do the volume work and the expensive model (Claude/GPT)
 * only where reasoning is really needed. But without NUMBERS, it is just a claim.
 * With numbers, it is a demonstration: "this graph cost $0.04 instead of $0.31".
 *
 * Each provider reports usage differently:
 *   Anthropic: usage.input_tokens / usage.output_tokens
 *   OpenAI:    usage.prompt_tokens / usage.completion_tokens
 *   Gemini:    usageMetadata.promptTokenCount / candidatesTokenCount
 * The adapters normalize them into the structure below, so the rest of the code
 * does not know the difference.
 */

typedef struct {
    long input_tokens;
    long output_tokens;
    /* The cost in micro-dollars (1 USD = 1,000,000). We use integers to avoid
       floating-point rounding errors when summing. */
    long cost_micro_usd;
} usage_t;

/* Adds the usage of `add` to `total`. */
void usage_add(usage_t *total, const usage_t *add);

/*
 * Computes the cost for a given model, in micro-dollars.
 * Prices are per 1 MILLION tokens (as the providers publish them).
 * If the model is not in the table, returns 0 and logs a warning - we do not
 * invent a price, so we do not display false numbers.
 */
long usage_cost_micro(llm_provider_id_t provider, const char *model,
                      long input_tokens, long output_tokens);

/* Formats the cost for display: "$0.0342". out must be >= 16 bytes. */
void usage_format_cost(long cost_micro_usd, char *out, size_t out_size);

/*
 * What would the SAME usage have cost if everything had run on model `model`?
 * This is the number that demonstrates the savings: you compare the graph's real
 * cost (mixed models) with the hypothetical cost on a single expensive model.
 */
long usage_cost_if_all_on(llm_provider_id_t provider, const char *model,
                          long input_tokens, long output_tokens);

/* The name of the pricing table used (for transparency - prices change). */
const char *usage_pricing_note(void);

#endif
