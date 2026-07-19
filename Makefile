CC = gcc
CFLAGS = -Wall -Wextra -Iinclude -Ithird_party/cJSON -g
LDFLAGS = -lcurl

SRC = \
	src/cli/main.c \
	src/core/workspace.c \
	src/core/sandbox.c \
	src/core/agent_run.c \
	src/core/flow.c \
	src/core/orchestrator.c \
	src/core/log.c \
	src/core/history.c \
	src/core/envfile.c \
	src/core/usage.c \
	src/core/selfhost.c \
	src/tools/json_util.c \
	src/tools/tools_common.c \
	src/tools/file_tools.c \
	src/tools/exec_tools.c \
	src/tools/registry.c \
	src/tools/tool_defs.c \
	src/runtime/ai_loop.c \
	src/runtime/providers/provider.c \
	src/runtime/providers/http.c \
	src/runtime/providers/anthropic.c \
	src/runtime/providers/openai.c \
	src/runtime/providers/gemini.c \
	src/web/http_server.c \
	src/web/web_api.c \
	src/web/web_assets.c \
	src/web/jobs.c \
	src/web/events.c \
	src/web/daemon.c \
	third_party/cJSON/cJSON.c

OBJ = $(SRC:.c=.o)
BIN = pixelgo

# A stamp file marks that the demo has been set up, so a plain `make` does not
# rebuild it every time (setup.sh wipes workspaces/demo, which would throw away
# any work you did in there).
DEMO_STAMP = workspaces/.demo-ready

all: $(BIN)
	@$(MAKE) --no-print-directory demo

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $(BIN) $(LDFLAGS)

# `make demo` prepares the demo workspace: agents, graphs, and a small C project
# with intentional bugs. It runs automatically as part of `make`.
# Use `make demo-reset` to rebuild it from scratch.
#
# The stamp deliberately does NOT depend on $(BIN): otherwise every recompile
# would re-run setup.sh, which wipes workspaces/demo and would throw away any
# work done in there. The demo is set up once; `make` afterwards only rebuilds
# the binary.
demo: $(DEMO_STAMP)

$(DEMO_STAMP):
	@$(MAKE) --no-print-directory $(BIN)
	@./demo/setup.sh
	@mkdir -p workspaces && touch $(DEMO_STAMP)

demo-reset:
	@rm -f $(DEMO_STAMP)
	@$(MAKE) --no-print-directory demo

# ---------------------------------------------------------------- tests
#
# Unit tests for the parts where a mistake is expensive and hard to spot:
# the sandbox, the permission gates, and the flow transition rules.
#
# They link against the compiled objects rather than recompiling the sources, so
# what is tested is exactly what ships. Everything except cJSON is pulled in
# because workspace.c reaches into the provider layer for its config strings.

TEST_SRC = $(wildcard tests/t_*.c)
TEST_BIN = $(TEST_SRC:tests/t_%.c=tests/run_%)

# All objects except main.o - the tests bring their own entry point.
TEST_OBJ = $(filter-out src/cli/main.o,$(OBJ))

tests/run_%: tests/t_%.c $(TEST_OBJ)
	@$(CC) $(CFLAGS) -Itests -o $@ $< $(TEST_OBJ) $(LDFLAGS)

test: $(TEST_BIN)
	@fail=0; \
	for t in $(TEST_BIN); do \
	  PIXELGO_LOG_LEVEL=error ./$$t || fail=1; \
	done; \
	./tests/check_language.sh || fail=1; \
	if [ $$fail -eq 0 ]; then \
	  echo "  ALL TESTS PASSED"; echo; \
	else \
	  echo "  SOME TESTS FAILED"; echo; exit 1; \
	fi

# Install pixelgo as a user service (systemd on Linux, launchd on macOS) so it
# starts at login. Set PIXELGO_PORT to use a port other than 8080.
service: $(BIN)
	@./packaging/install-service.sh

service-uninstall:
	@./packaging/install-service.sh --uninstall

# cJSON is third-party code - we compile it without -Wall/-Wextra so we do not
# pollute the build with warnings that are not ours.
third_party/cJSON/cJSON.o: third_party/cJSON/cJSON.c
	$(CC) -Ithird_party/cJSON -g -c $< -o $@

# The frontend is compiled INTO the binary: web/index.html -> src/web/web_assets.c
# (a C string). It is regenerated automatically when index.html changes.
src/web/web_assets.c: web/index.html tools/embed_html.sh
	./tools/embed_html.sh web/index.html src/web/web_assets.c

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TEST_BIN)
	rm -f $(OBJ) $(BIN)
	rm -rf workspaces jobs
	rm -f pixelgo.pid pixelgo.log

.PHONY: all clean test demo demo-reset service service-uninstall
