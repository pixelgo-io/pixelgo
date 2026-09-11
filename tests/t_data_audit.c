#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "data_request_audit.h"
#include "agent.h"
#include "usage.h"

static int test_count = 0;
static int test_passed = 0;
static int test_failed = 0;

#define TEST_START(name) \
    printf("\n  test_%s...", name); \
    fflush(stdout); \
    test_count++;

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            printf(" FAILED\n    Assertion: %s\n", msg); \
            test_failed++; \
            return; \
        } \
    } while (0)

#define TEST_PASS \
    printf(" OK\n"); \
    test_passed++;

/* ============================================================================ */

void test_estimate_tokens_zero(void) {
    TEST_START("estimate_tokens_zero");
    ASSERT(data_audit_estimate_tokens(0) == 0, "0 bytes -> 0 tokens");
    TEST_PASS;
}

void test_estimate_tokens_nonzero(void) {
    TEST_START("estimate_tokens_nonzero");
    /* ~4 bytes/token rule of thumb */
    ASSERT(data_audit_estimate_tokens(4) == 1, "4 bytes -> 1 token");
    ASSERT(data_audit_estimate_tokens(1) == 1, "1 byte still rounds up to 1 token, not 0");
    ASSERT(data_audit_estimate_tokens(1024 * 1024) == 262144, "1 MB -> 262144 tokens");
    TEST_PASS;
}

void test_estimate_tokens_1_2mb(void) {
    TEST_START("estimate_tokens_1_2mb");
    size_t bytes = (size_t)(1.2 * 1024 * 1024);
    long tokens = data_audit_estimate_tokens(bytes);
    /* Should be in the right ballpark - around 314572 */
    ASSERT(tokens > 300000 && tokens < 330000, "1.2 MB gives a plausible token estimate");
    TEST_PASS;
}

void test_build_summary_contains_provider_and_model(void) {
    TEST_START("build_summary_contains_provider_and_model");

    char summary[256];
    long tokens = data_audit_estimate_tokens(1024 * 1024);
    long cost_micro = usage_cost_micro(LLM_PROVIDER_ANTHROPIC, "claude-sonnet-5",
                                       tokens, 0);

    data_audit_build_summary(LLM_PROVIDER_ANTHROPIC, "claude-sonnet-5",
                             1024 * 1024, tokens, cost_micro,
                             summary, sizeof(summary));

    ASSERT(strstr(summary, "anthropic") != NULL, "Summary mentions the provider");
    ASSERT(strstr(summary, "claude-sonnet-5") != NULL, "Summary mentions the model");
    ASSERT(strstr(summary, "MB") != NULL, "Summary mentions payload size in MB");
    ASSERT(strstr(summary, "$") != NULL, "Summary mentions the estimated cost");

    TEST_PASS;
}

void test_build_summary_does_not_overflow(void) {
    TEST_START("build_summary_does_not_overflow");

    char summary[256];
    /* A model name at the edge of MAX_STR - must not overflow the fixed
       summary buffer regardless of input size. */
    char long_model[MAX_STR];
    memset(long_model, 'x', sizeof(long_model) - 1);
    long_model[sizeof(long_model) - 1] = 0;

    data_audit_build_summary(LLM_PROVIDER_OPENAI, long_model,
                             5UL * 1024 * 1024 * 1024, 999999999, 123456789,
                             summary, sizeof(summary));

    ASSERT(strlen(summary) < sizeof(summary), "Summary stays within its buffer");

    TEST_PASS;
}

void test_log_decision_does_not_crash(void) {
    TEST_START("log_decision_does_not_crash");

    /* Best-effort logging: should never crash, even with NULL workspace_id
       (the ai_loop.c call site passes NULL for workspace, since ai_loop does
       not carry a workspace_id today). */
    data_audit_log_decision(NULL, "test-agent", 1024 * 1024, 262144,
                            600000, "approved");
    data_audit_log_decision("test-ws", "test-agent", 2048, 512,
                            100, "denied");

    ASSERT(1, "did not crash");
    TEST_PASS;
}

void test_cost_matches_usage_table(void) {
    TEST_START("cost_matches_usage_table");

    /* Sanity check against the real pricing table (usage.c): Claude Sonnet
       input is $3.00 / 1M tokens = 3,000,000 micro-USD / 1M tokens. */
    long cost_1m = usage_cost_micro(LLM_PROVIDER_ANTHROPIC, "claude-sonnet-5",
                                    1000000, 0);
    ASSERT(cost_1m == 3000000, "1M input tokens on claude-sonnet costs $3.00 (3,000,000 micro-USD)");

    TEST_PASS;
}

/* ============================================================================ */

int main(void) {
    printf("\n=== DATA REQUEST AUDIT -- Unit Tests ===\n");

    test_estimate_tokens_zero();
    test_estimate_tokens_nonzero();
    test_estimate_tokens_1_2mb();
    test_build_summary_contains_provider_and_model();
    test_build_summary_does_not_overflow();
    test_log_decision_does_not_crash();
    test_cost_matches_usage_table();

    printf("\n=== Results: %d tests, %d passed, %d failed ===\n\n",
           test_count, test_passed, test_failed);

    return (test_failed == 0) ? 0 : 1;
}
