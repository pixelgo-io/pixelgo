#include "tool_defs.h"
#include <string.h>

/* Each entry = an "MCP-style" tool: name + description + input_schema (JSON Schema).
   When a new tool is added to registry.c (e.g. run_command, search_files), its
   definition is added here too - otherwise the model does not know it exists. */

static const char *READ_FILE_DEF =
    "{"
      "\"name\":\"read_file\","
      "\"description\":\"Reads the full content of a file within the agent's working directory. Path must be relative to the working directory.\","
      "\"input_schema\":{"
        "\"type\":\"object\","
        "\"properties\":{"
          "\"path\":{\"type\":\"string\",\"description\":\"Relative path to the file to read\"}"
        "},"
        "\"required\":[\"path\"]"
      "}"
    "}";

static const char *WRITE_FILE_DEF =
    "{"
      "\"name\":\"write_file\","
      "\"description\":\"Writes (creates or overwrites) a file within the agent's working directory.\","
      "\"input_schema\":{"
        "\"type\":\"object\","
        "\"properties\":{"
          "\"path\":{\"type\":\"string\",\"description\":\"Relative path to the file to write\"},"
          "\"content\":{\"type\":\"string\",\"description\":\"Full content to write to the file\"}"
        "},"
        "\"required\":[\"path\",\"content\"]"
      "}"
    "}";

static const char *LIST_DIR_DEF =
    "{"
      "\"name\":\"list_dir\","
      "\"description\":\"Lists files and subdirectories within a directory in the agent's working directory. Omit path to list the root of the working directory.\","
      "\"input_schema\":{"
        "\"type\":\"object\","
        "\"properties\":{"
          "\"path\":{\"type\":\"string\",\"description\":\"Relative path to the directory to list (default: '.')\"}"
        "},"
        "\"required\":[]"
      "}"
    "}";

static const char *RUN_COMMAND_DEF =
    "{"
      "\"name\":\"run_command\","
      "\"description\":\"Runs a command in the agent's working directory and returns its combined stdout and stderr, plus the exit code. Use this to compile code, run tests, or verify that what you wrote actually works. Only commands in this agent's allowlist may be run. IMPORTANT: there is no shell, so shell operators (semicolon, pipe, &&, redirects, command substitution) are NOT interpreted. Pass the program name in 'command' and each argument separately in 'args'.\","
      "\"input_schema\":{"
        "\"type\":\"object\","
        "\"properties\":{"
          "\"command\":{\"type\":\"string\",\"description\":\"Program to run, e.g. 'make', 'gcc', 'python3'. Must be in the allowlist.\"},"
          "\"args\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Arguments, each a separate string. For 'gcc -o out main.c' use command='gcc', args=['-o','out','main.c'].\"},"
          "\"timeout_seconds\":{\"type\":\"number\",\"description\":\"Max seconds to wait (default 3600)\"}"
        "},"
        "\"required\":[\"command\"]"
      "}"
    "}";

static const char *SEARCH_FILES_DEF =
    "{"
      "\"name\":\"search_files\","
      "\"description\":\"Searches for a text pattern in files under the agent's working directory (recursive, like grep). Returns matching lines with file name and line number. Use this to locate where something is defined or used, instead of reading every file.\","
      "\"input_schema\":{"
        "\"type\":\"object\","
        "\"properties\":{"
          "\"pattern\":{\"type\":\"string\",\"description\":\"Text to search for (plain substring, not a regex)\"},"
          "\"path\":{\"type\":\"string\",\"description\":\"Directory or file to search in (default: '.')\"}"
        "},"
        "\"required\":[\"pattern\"]"
      "}"
    "}";

const char *tool_def_json(const char *tool_name) {
    if (strcmp(tool_name, "read_file") == 0) return READ_FILE_DEF;
    if (strcmp(tool_name, "write_file") == 0) return WRITE_FILE_DEF;
    if (strcmp(tool_name, "list_dir") == 0) return LIST_DIR_DEF;
    if (strcmp(tool_name, "run_command") == 0) return RUN_COMMAND_DEF;
    if (strcmp(tool_name, "search_files") == 0) return SEARCH_FILES_DEF;
    return NULL; /* unknown tool - ai_loop.c will skip it */
}
