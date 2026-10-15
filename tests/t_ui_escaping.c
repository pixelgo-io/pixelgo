/*
 * The UI must treat every field of every API response as hostile.
 *
 * Locally the risk is small: the user writes workspace.conf themselves. In
 * isolated mode it is real - ops serves this page on the HOST, the data comes
 * from inside the VM, and the agent there can edit workspace.conf or replace
 * the pixelgo binary. So a value that reaches innerHTML unescaped is an
 * injection into a page running on the operator's machine.
 *
 * There is no JavaScript engine here, so this suite checks the two halves of
 * the guarantee that CAN be checked from C:
 *
 *   1. the page's source contains no interpolation of an API value that
 *      skips esc()/num() - a grep, but a precise one, and it is what would
 *      regress the day someone adds a field;
 *   2. the server sends the Content-Security-Policy that limits the damage
 *      if one ever does.
 *
 * The first is deliberately a source check rather than a rendering test:
 * pulling in a JS engine to prove a string is escaped would cost more than
 * the whole web layer.
 */

#include "test.h"
#include "http_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UI_PATH "web/index.html"

static char *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)size, f);
    buf[n] = 0;
    fclose(f);
    if (out_len) *out_len = n;
    return buf;
}

/* Is `needle` present at all? */
static int has(const char *hay, const char *needle) {
    return strstr(hay, needle) != NULL;
}

int main(void) {
    size_t len = 0;
    char *ui = slurp(UI_PATH, &len);
    if (!ui) {
        /* run from the project root, like every other suite */
        printf("    FAIL  cannot read %s - run make test from the project root\n", UI_PATH);
        return 1;
    }

    section("esc() covers the characters that matter inside an attribute");
    ok("escapes &",  has(ui, "replace(/&/g, '&amp;')"));
    ok("escapes <",  has(ui, "replace(/</g, '&lt;')"));
    ok("escapes >",  has(ui, "replace(/>/g, '&gt;')"));
    ok("escapes \" - without this, a value closes the attribute it sits in",
       has(ui, "replace(/\"/g, '&quot;')"));
    ok("escapes '",  has(ui, "replace(/'/g, '&#39;')"));

    section("the helpers for values that are supposed to be numbers or arrays");
    ok("num() exists",  has(ui, "function num("));
    ok("num() refuses anything not finite", has(ui, "Number.isFinite"));
    ok("alen() exists", has(ui, "function alen("));
    ok("alen() checks it really is an array", has(ui, "Array.isArray"));

    section("the sites that used to interpolate an API value raw");
    ok("chat agent list: the id in data-id= is escaped",
       has(ui, "data-id=\"${esc(a.id)}\""));
    ok("chat agent list: provider and model are escaped",
       has(ui, "${esc(a.provider)} &middot; ${esc(a.model)}") ||
       has(ui, "${esc(a.provider)} · ${esc(a.model)}"));
    ok("agents list: the type badge is escaped",
       has(ui, "<span class=\"badge\">${esc(a.type)}</span>"));
    ok("agents list: the tool list is escaped",
       has(ui, "esc(a.tools.join(', '))"));
    ok("workspace options: the name is escaped in value= and in the text",
       has(ui, "<option value=\"${esc(w.name)}\">${esc(w.name)}"));
    ok("workspace options: the count goes through num()",
       has(ui, "(${num(w.agent_count)})"));
    ok("flow list: the name is escaped in data-f=",
       has(ui, "data-f=\"${esc(f)}\""));
    ok("node cost: token counts go through num()",
       has(ui, "${num(o.input_tokens)} in") && has(ui, "${num(o.output_tokens)} out"));
    ok("cost summary: the totals go through num()",
       has(ui, "num(j.total_input_tokens)") && has(ui, "num(j.total_output_tokens)"));
    ok("cost summary: the total cost goes through num()",
       has(ui, "num(j.total_cost_micro_usd)"));
    ok("usd() coerces its argument rather than trusting it",
       has(ui, "const usd = v => { const m = num(v);"));

    section("none of the old raw forms survive anywhere in the page");
    ok("no data-id=\"${a.id}\"",        !has(ui, "data-id=\"${a.id}\""));
    ok("no data-f=\"${f}\"",            !has(ui, "data-f=\"${f}\""));
    ok("no value=\"${w.name}\"",        !has(ui, "value=\"${w.name}\""));
    ok("no bare ${a.type}",             !has(ui, ">${a.type}<"));
    ok("no bare ${o.input_tokens}",     !has(ui, "${o.input_tokens}"));
    ok("no bare ${o.output_tokens}",    !has(ui, "${o.output_tokens}"));
    ok("no bare ${w.agent_count}",      !has(ui, "${w.agent_count}"));

    section("the page has nothing that a strict CSP would break");
    ok("exactly one <script> tag", has(ui, "<script>") &&
       strstr(strstr(ui, "<script>") + 1, "<script") == NULL);
    ok("no inline onclick=\"...\" in the markup", !has(ui, " onclick=\""));
    ok("no inline onchange=\"...\"",              !has(ui, " onchange=\""));
    ok("no external stylesheet or font",          !has(ui, "<link "));

    section("the Content-Security-Policy the server sends");
    /* Read from the source rather than over a socket: the header is a
       compile-time constant, and a socket test here would add a server
       fixture for one string. */
    size_t srclen = 0;
    char *srv = slurp("src/web/http_server.c", &srclen);
    ok("http_server.c defines a CSP", srv && has(srv, "Content-Security-Policy"));
    if (srv) {
        ok("default-src 'none' - nothing loads unless named",
           has(srv, "default-src 'none'"));
        ok("connect-src 'self' - an injected script has nowhere to send what it read",
           has(srv, "connect-src 'self'"));
        ok("base-uri 'none'",      has(srv, "base-uri 'none'"));
        ok("form-action 'none'",   has(srv, "form-action 'none'"));
        ok("frame-ancestors 'none' - no clickjacking",
           has(srv, "frame-ancestors 'none'"));
        ok("X-Content-Type-Options: nosniff",
           has(srv, "X-Content-Type-Options: nosniff"));
        free(srv);
    }

    free(ui);
    return t_report("ui_escaping");
}
