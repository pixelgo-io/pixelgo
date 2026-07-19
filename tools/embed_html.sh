#!/bin/sh
# Turns web/index.html into a C string (src/web/web_assets.c).
# Run automatically by the Makefile when index.html changes - so the frontend is
# compiled INTO the binary, with no external files to serve.
set -e
IN="$1"
OUT="$2"

{
  echo '/* AUTO-GENERATED from web/index.html - do not edit directly. */'
  echo '#include "http_server.h"'
  echo ''
  echo 'const char *WEB_INDEX_HTML ='
  # escape: backslash, quotes; keep the lines
  sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$/\\n"/' "$IN"
  echo ';'
} > "$OUT"
