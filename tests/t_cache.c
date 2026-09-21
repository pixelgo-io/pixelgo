/*
 * Tests for prompt caching: the CACHE_MODE_UNSET -> PIXELGO_CACHE_MODE
 * resolution, and the CACHE_MODE_AUTO decision itself.
 *
 * The interesting part of the decision is CACHE_MODE_AUTO: it must cache
 * exactly the cases where a cached prefix is likely to be READ again (an
 * agent with tools, which almost never stops after one request; or a
 * conversation that already has history, i.e. a continuing `agent chat` or a
 * later turn of a tool loop), and must NOT cache the one case where caching
 * is pure cost with no possible payoff: a tool-less agent's very first,
 * one-shot request.
 */
#include "provider_internal.h"
#include "test.h"
#include <stdlib.h>

int main(void) {
    section("resolve: an explicit stored mode always wins, env or not");
    {
        unsetenv("PIXELGO_CACHE_MODE");
        ok("explicit ON, no env var",  provider_resolve_cache_mode(CACHE_MODE_ON)  == CACHE_MODE_ON);
        ok("explicit OFF, no env var", provider_resolve_cache_mode(CACHE_MODE_OFF) == CACHE_MODE_OFF);
        ok("explicit AUTO, no env var",provider_resolve_cache_mode(CACHE_MODE_AUTO)== CACHE_MODE_AUTO);

        setenv("PIXELGO_CACHE_MODE", "off", 1);
        ok("explicit ON overrides env=off", provider_resolve_cache_mode(CACHE_MODE_ON) == CACHE_MODE_ON);
        unsetenv("PIXELGO_CACHE_MODE");
    }

    section("resolve: UNSET (no --cache flag) inherits PIXELGO_CACHE_MODE");
    {
        setenv("PIXELGO_CACHE_MODE", "on", 1);
        ok("env=on -> ON",  provider_resolve_cache_mode(CACHE_MODE_UNSET) == CACHE_MODE_ON);

        setenv("PIXELGO_CACHE_MODE", "off", 1);
        ok("env=off -> OFF", provider_resolve_cache_mode(CACHE_MODE_UNSET) == CACHE_MODE_OFF);

        setenv("PIXELGO_CACHE_MODE", "auto", 1);
        ok("env=auto -> AUTO", provider_resolve_cache_mode(CACHE_MODE_UNSET) == CACHE_MODE_AUTO);

        unsetenv("PIXELGO_CACHE_MODE");
    }

    section("resolve: UNSET with no env var, or a bad one, falls back to AUTO");
    {
        unsetenv("PIXELGO_CACHE_MODE");
        ok("no env var at all", provider_resolve_cache_mode(CACHE_MODE_UNSET) == CACHE_MODE_AUTO);

        setenv("PIXELGO_CACHE_MODE", "yes-please", 1);
        ok("garbage env value", provider_resolve_cache_mode(CACHE_MODE_UNSET) == CACHE_MODE_AUTO);
        unsetenv("PIXELGO_CACHE_MODE");
    }

    section("CACHE_MODE_ON / OFF always win, regardless of the signals");
    {
        ok("ON with no tools, first message",     provider_cache_decision(CACHE_MODE_ON,  0, 1) == 1);
        ok("ON with tools, mid-conversation",      provider_cache_decision(CACHE_MODE_ON,  3, 5) == 1);
        ok("OFF with tools, mid-conversation",     provider_cache_decision(CACHE_MODE_OFF, 3, 5) == 0);
        ok("OFF with no tools, first message",     provider_cache_decision(CACHE_MODE_OFF, 0, 1) == 0);
    }

    section("AUTO: the one true no-benefit case");
    {
        ok("no tools, single message -> do not cache",
           provider_cache_decision(CACHE_MODE_AUTO, 0, 1) == 0);
        ok("no tools, empty message array -> do not cache",
           provider_cache_decision(CACHE_MODE_AUTO, 0, 0) == 0);
    }

    section("AUTO: an agent with tools is cached from the very first request");
    {
        ok("one tool, first message",   provider_cache_decision(CACHE_MODE_AUTO, 1, 1) == 1);
        ok("several tools, first message", provider_cache_decision(CACHE_MODE_AUTO, 5, 1) == 1);
    }

    section("AUTO: a conversation that already has history is cached, even with no tools");
    {
        ok("no tools, but this is turn 2+ (agent chat resuming)",
           provider_cache_decision(CACHE_MODE_AUTO, 0, 2) == 1);
        ok("no tools, deep into a long chat",
           provider_cache_decision(CACHE_MODE_AUTO, 0, 40) == 1);
    }

    section("AUTO: tools AND history both present - still cached");
    {
        ok("tools and a growing tool-loop conversation",
           provider_cache_decision(CACHE_MODE_AUTO, 2, 7) == 1);
    }

    return t_report("cache");
}
