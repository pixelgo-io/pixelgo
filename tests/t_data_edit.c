#include "test.h"
#include "events.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

/*
 * data_edit_wait() is the trickiest new piece of this feature: a decision
 * file written by one process is parsed by another, with a hand-rolled,
 * fixed-length marker glued directly onto the edited text, no separator:
 *
 *   "deny"          (4 bytes)
 *   "allow" + "U"   (6 bytes) - approved UNCHANGED, nothing follows
 *   "allow" + "E" + <text>    - approved EDITED, text follows byte for byte
 *
 * During development the "allow"/"deny" split alone was wrong once - a
 * 7-byte read silently ate the first two bytes of the edited text - and it
 * was only caught by a manual round-trip test, never checked in. This file
 * is that test, checked in, so the same mistake (here or in the mode byte
 * added after it) cannot come back unnoticed.
 *
 * Each case forks a "server" child that writes the decision file a moment
 * later (mirroring the real flow: the browser's POST arrives well after the
 * agent started waiting), while the parent blocks in the real
 * data_edit_wait().
 */

#define TMP_BASE "/tmp/pixelgo_test_data_edit"

static void write_decision_after_delay(const char *path, const char *content, size_t len) {
    pid_t pid = fork();
    if (pid == 0) {
        usleep(300 * 1000);   /* give the parent time to start waiting */
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(content, 1, len, f);
            fclose(f);
        }
        _exit(0);
    }
}

static void reap(void) {
    int status;
    waitpid(-1, &status, 0);
}

static void setup_job_env(const char *events_path) {
    remove(events_path);
    FILE *f = fopen(events_path, "w");
    if (f) fclose(f);
    setenv("PIXELGO_EVENTS_FILE", events_path, 1);
}

/* --- allow + edited, plain ASCII --- */
static void test_allow_edited_basic(void) {
    const char *events = TMP_BASE "_edited.events";
    const char *decision = TMP_BASE "_edited.data_edit";
    setup_job_env(events);

    const char payload[] = "allowEtrimmed text";
    write_decision_after_delay(decision, payload, sizeof(payload) - 1);

    char out[256] = {0};
    int edited = -1;
    int allowed = data_edit_wait("agent", "send request to AI", "original huge text",
                                 out, sizeof(out), &edited);
    reap();

    ok("edited: decision reports allowed", allowed == 1);
    ok("edited: out_edited is set to 1", edited == 1);
    eq_str("edited: exactly the text after the marker comes back", out, "trimmed text");
}

/*
 * "Approve unchanged": no text follows, and out_text/out_edited must reflect
 * that plainly - this is the path a >64KB block (too big to round-trip
 * through the capped POST body) takes when the human approves it as-is.
 */
static void test_allow_unchanged(void) {
    const char *events = TMP_BASE "_unchanged.events";
    const char *decision = TMP_BASE "_unchanged.data_edit";
    setup_job_env(events);

    const char payload[] = "allowU";
    write_decision_after_delay(decision, payload, sizeof(payload) - 1);

    char out[256];
    snprintf(out, sizeof(out), "SENTINEL_UNTOUCHED");
    int edited = -1;
    int allowed = data_edit_wait("agent", "send request to AI", "the huge original text",
                                 out, sizeof(out), &edited);
    reap();

    ok("unchanged: decision reports allowed", allowed == 1);
    ok("unchanged: out_edited is set to 0", edited == 0);
    eq_str("unchanged: out_text is left untouched, not blanked", out, "SENTINEL_UNTOUCHED");
}

/*
 * The other direction: the human DID open the editor and deliberately
 * emptied the textarea, then hit "Send trimmed" (not "Approve as-is"). That
 * is an edit to an empty string, and must be told apart from "unchanged" -
 * this is exactly why presence-of-"text" (checked in web_api.c), not just a
 * non-empty value, is what decides the mode byte.
 */
static void test_allow_edited_to_empty(void) {
    const char *events = TMP_BASE "_emptied.events";
    const char *decision = TMP_BASE "_emptied.data_edit";
    setup_job_env(events);

    const char payload[] = "allowE";   /* mode 'E', zero bytes of text follow */
    write_decision_after_delay(decision, payload, sizeof(payload) - 1);

    char out[256];
    snprintf(out, sizeof(out), "SENTINEL_SHOULD_BE_OVERWRITTEN");
    int edited = -1;
    int allowed = data_edit_wait("agent", "send request to AI", "the huge original text",
                                 out, sizeof(out), &edited);
    reap();

    ok("emptied: decision reports allowed", allowed == 1);
    ok("emptied: out_edited is set to 1 (it WAS an edit)", edited == 1);
    eq_str("emptied: out_text is genuinely empty, not the sentinel", out, "");
}

/* --- deny: out_text and out_edited must be left untouched --- */
static void test_deny_basic(void) {
    const char *events = TMP_BASE "_deny.events";
    const char *decision = TMP_BASE "_deny.data_edit";
    setup_job_env(events);

    write_decision_after_delay(decision, "deny", 4);

    char out[256];
    snprintf(out, sizeof(out), "SENTINEL_UNTOUCHED");
    int edited = -1;
    int allowed = data_edit_wait("agent", "send request to AI", "original",
                                 out, sizeof(out), &edited);
    reap();

    ok("deny: decision reports denied", allowed == 0);
    ok("deny: out_edited is reset to 0", edited == 0);
    eq_str("deny: out_text is left untouched", out, "SENTINEL_UNTOUCHED");
}

/*
 * The regression case: the edited text itself starts with the letters that
 * make up the marker (even the literal word "allow"). A parser that reads
 * too many or too few bytes, or that scans for content instead of taking a
 * fixed-length prefix, will corrupt this. Reading exactly 5 marker bytes
 * plus exactly 1 mode byte, then treating everything after as opaque, must
 * not care what those bytes look like.
 */
static void test_allow_text_starts_with_marker_word(void) {
    const char *events = TMP_BASE "_collide.events";
    const char *decision = TMP_BASE "_collide.data_edit";
    setup_job_env(events);

    const char payload[] = "allowEallow me to explain: this text starts with 'allow'";
    write_decision_after_delay(decision, payload, sizeof(payload) - 1);

    char out[256] = {0};
    int edited = -1;
    int allowed = data_edit_wait("agent", "send request to AI", "original",
                                 out, sizeof(out), &edited);
    reap();

    ok("collision: decision reports allowed", allowed == 1);
    ok("collision: out_edited is set to 1", edited == 1);
    eq_str("collision: no bytes of the edited text were eaten by the marker",
           out, "allow me to explain: this text starts with 'allow'");
}

/* --- UTF-8 (Romanian diacritics + a multi-byte symbol) must round-trip byte for byte --- */
static void test_allow_utf8(void) {
    const char *events = TMP_BASE "_utf8.events";
    const char *decision = TMP_BASE "_utf8.data_edit";
    setup_job_env(events);

    const char payload[] = "allowEtext with diacritics: \xc4\x83\xc3\xa2\xc3\xae\xc8\x99\xc8\x9b and a check: \xe2\x9c\x93";
    write_decision_after_delay(decision, payload, sizeof(payload) - 1);

    char out[256] = {0};
    int edited = -1;
    int allowed = data_edit_wait("agent", "send request to AI", "original",
                                 out, sizeof(out), &edited);
    reap();

    ok("utf8: decision reports allowed", allowed == 1);
    ok("utf8: out_edited is set to 1", edited == 1);
    eq_str("utf8: multi-byte characters survive the file-based channel",
           out, "text with diacritics: \xc4\x83\xc3\xa2\xc3\xae\xc8\x99\xc8\x9b and a check: \xe2\x9c\x93");
}

/* --- out_text_cap smaller than the edited text: must truncate, never overflow --- */
static void test_allow_truncates_to_cap(void) {
    const char *events = TMP_BASE "_cap.events";
    const char *decision = TMP_BASE "_cap.data_edit";
    setup_job_env(events);

    const char payload[] = "allowE0123456789ABCDEF";  /* 16 bytes after the marker+mode */
    write_decision_after_delay(decision, payload, sizeof(payload) - 1);

    char out[9];   /* room for 8 bytes + NUL: smaller than the 16-byte edited text */
    memset(out, 'X', sizeof(out));
    int edited = -1;
    int allowed = data_edit_wait("agent", "send request to AI", "original",
                                 out, sizeof(out), &edited);
    reap();

    ok("cap: decision still reports allowed", allowed == 1);
    ok("cap: out_edited is set to 1", edited == 1);
    ok("cap: output is NUL-terminated within the buffer", strlen(out) < sizeof(out));
    eq_str("cap: output holds the first (cap-1) bytes, not garbage", out, "01234567");
}

/*
 * Backward/defensive case: a decision file that ends right after "allow"
 * with no mode byte at all (e.g. hand-crafted, or written by an older
 * client). Must not crash or read garbage - the documented behavior is to
 * treat it as "unchanged", the safer of the two interpretations (it never
 * discards real edited content by mistake).
 */
static void test_allow_missing_mode_byte_defaults_unchanged(void) {
    const char *events = TMP_BASE "_nomode.events";
    const char *decision = TMP_BASE "_nomode.data_edit";
    setup_job_env(events);

    const char payload[] = "allow";   /* exactly 5 bytes, nothing after */
    write_decision_after_delay(decision, payload, sizeof(payload) - 1);

    char out[256];
    snprintf(out, sizeof(out), "SENTINEL_UNTOUCHED");
    int edited = -1;
    int allowed = data_edit_wait("agent", "send request to AI", "original",
                                 out, sizeof(out), &edited);
    reap();

    ok("no mode byte: decision still reports allowed", allowed == 1);
    ok("no mode byte: defaults to unchanged (out_edited == 0)", edited == 0);
    eq_str("no mode byte: out_text is left untouched", out, "SENTINEL_UNTOUCHED");
}

int main(void) {
    section("data_edit_wait: allow (edited) / allow (unchanged) / deny");
    test_allow_edited_basic();
    test_allow_unchanged();
    test_deny_basic();

    section("data_edit_wait: edited-to-empty vs. unchanged must not be confused");
    test_allow_edited_to_empty();

    section("data_edit_wait: the marker-vs-content collision that caused a real bug");
    test_allow_text_starts_with_marker_word();

    section("data_edit_wait: UTF-8 round-trips through the file-based channel");
    test_allow_utf8();

    section("data_edit_wait: an edit larger than out_text_cap truncates safely");
    test_allow_truncates_to_cap();

    section("data_edit_wait: a missing mode byte fails toward 'unchanged', not a crash");
    test_allow_missing_mode_byte_defaults_unchanged();

    return t_report("data_edit");
}
