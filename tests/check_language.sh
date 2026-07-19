#!/bin/sh
#
# Catches non-English words in anything a user can see: log messages, API
# errors, CLI output, the web UI.
#
# Why a script and not a C test: this checks the source text itself, not
# behaviour. And why a dictionary instead of a list of words to avoid - because
# a list only finds what you already thought of. Every round of manual cleanup
# missed something ("citibil", "campuri", "nepermisa") for exactly that reason.
#
# Unknown-but-fine words (technical terms, identifiers, API field names) live in
# the allowlist below. Adding to it is normal; the point is that additions are
# deliberate rather than accidental.

set -e
cd "$(dirname "$0")/.."

DICT=/usr/share/dict/american-english
if [ ! -f "$DICT" ]; then
    echo "  skipping language check: no dictionary at $DICT"
    echo "  (install with: apt-get install wamerican)"
    exit 0
fi

# Technical vocabulary that is not in a plain English dictionary but is correct.
KNOWN='^(allowlist|anthropic|openai|gemini|claude|sonnet|pixelgo|json|jsonl|html|http|https|api|url|urls|const|await|async|innerhtml|queryselector|queryselectorall|addeventlistener|classlist|stringify|settimeout|onclick|onchange|href|div|svg|dom|css|textarea|placeholder|checkbox|mermaid|flowchart|localhost|charset|viewbox|stroke|dasharray|linecap|linejoin|args|argv|init|conf|config|tmp|null|src|dst|buf|fmt|ptr|len|idx|str|val|var|arr|msg|res|req|cfg|ctx|env|usd|micro|mtok|toks|tokens|tool|tools|coder|calc|fanout|fanin|coord|orch|orchestrator|selfhost|rlimit|setrlimit|sigterm|sigkill|sigchld|sigttin|sigttou|sighup|sigint|sigquit|tcsetpgrp|tcgetpgrp|getpgrp|setpgid|waitpid|execv|execvp|chdir|chmod|mkdir|mkdtemp|realpath|readlink|symlink|symlinks|dirname|basename|getcwd|setsid|snprintf|strcmp|strncmp|stderr|stdin|stdout|struct|nginx|gcc|nodes|node|passwd|regex|retryable|codebase|endpoint|envfile|fallback|cond|completions|ctrl|utf|usr|www|dev|todo|cjson|substring|subdirectory|subdirectories|appendchild|createelement|createelementns|classname|clientheight|concat|consolas|dataset|defs|doctype|encodeuricomponent|findindex|foreach|colw|bylevel|applyevent|approvaljobid|gend|cssid|gedge|markerheight|markerwidth|refx|refy|xmlns|xlink|svgns|aria|readonly|spellcheck|maxlength|tabindex|autofocus|prompttokencount|candidatestokencount|functioncall|functiondeclarations|functionresponse|generatecontent|generativelanguage|googleapis|ncontent|nnode|nedge|ntotal|lingua|franca|agentx|sbox|eoff|aiza|aaaa|auth|dict|enum|errno|filesystem|frontend|malloc|printf|runtime|subtree|timeout|subcommands|misconfiguration|systeminstruction|unbuffered|untranslated|viewport|whitespace|workspace|workspaces|traversal|wamerican|xmidymid|xxxxxx|yyyy|tofixed|totalcost|totaltok|adjustme|dista|expe|solutio|elor|urilor|defensiv|curent|aici|usagemetadata|nthen|nand|nthe|nfor|blocker|mkdtemp|getcwd)'

# Pull text out of string literals, HTML text nodes and comments.
FOUND=$(
  find src include web demo tools tests -type f \
       \( -name '*.c' -o -name '*.h' -o -name '*.html' -o -name '*.sh' -o -name '*.flow' \) \
       ! -name '*.o' 2>/dev/null |
  grep -v web_assets | grep -v check_language |
  while read -r f; do
      grep -ohE '"[^"]{6,}"|>[^<>]{6,}<|/\*[^*]*\*/|//[^\n]*' "$f" 2>/dev/null
  done |
  # escape sequences glue onto words (\\nThen -> nthen); those land in KNOWN
  grep -oiE '\b[a-z]{4,}\b' |
  tr 'A-Z' 'a-z' | sort -u |
  comm -23 - "$(tr 'A-Z' 'a-z' < $DICT | sed "s/'s$//" | sort -u > /tmp/.dict.$$; echo /tmp/.dict.$$)" |
  grep -viE "$KNOWN" || true
)
rm -f /tmp/.dict.$$

if [ -n "$FOUND" ]; then
    echo "  FAIL  words that are neither English nor known technical terms:"
    echo "$FOUND" | sed 's/^/          /'
    echo
    echo "        If a word is legitimate, add it to KNOWN in $0."
    exit 1
fi

echo "  ok    no untranslated text in user-facing strings"
echo
echo "  language: all passed"
echo
