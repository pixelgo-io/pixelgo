#ifndef TOOL_DEFS_H
#define TOOL_DEFS_H

/*
 * The tool definitions, centralized, in the format required by the Anthropic API
 * (and compatible with the MCP - Model Context Protocol - tool definition format:
 * {name, description, input_schema (JSON Schema)}).
 *
 * Why centralized here and not hard-coded in ai_loop.c:
 *   - a single source of truth for "what a tool is" (name, description, input schema)
 *   - when we add real MCP support (connecting to external MCP servers), this
 *     file is the equivalent of an "MCP tool manifest" - easy to extend or to
 *     generate dynamically from an external MCP server, without changing ai_loop.c
 *   - registry.c (the dispatch) and this file (the description sent to the model)
 *     stay in sync manually for now; when we move to real MCP, an external server
 *     provides both automatically.
 */

/* Returns the full JSON definition of a known tool, or NULL if it does not exist.
   The returned format (JSON string):
   {"name":"read_file","description":"...","input_schema":{"type":"object","properties":{...},"required":[...]}}
*/
const char *tool_def_json(const char *tool_name);

#endif
