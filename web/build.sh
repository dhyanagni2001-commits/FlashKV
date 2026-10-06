#!/usr/bin/env bash
#
# Builds the browser demo: compiles the FlashKV engine to WebAssembly
# and inlines it into a single self-contained web/dist/index.html.
#
# Uses a local emcc if available, otherwise the emscripten/emsdk
# Docker image.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="$ROOT/web/dist"

mkdir -p "$DIST"

EMCC_ARGS=(
    -std=c++20 -O2 -fexceptions
    -Iinclude
    src/database.cpp
    src/resp.cpp
    src/commands.cpp
    src/aof.cpp
    src/reply_format.cpp
    web/flashkv_wasm.cpp
    -sMODULARIZE=1
    -sEXPORT_NAME=createFlashKV
    -sSINGLE_FILE=1
    -sSINGLE_FILE_BINARY_ENCODE=0
    -sENVIRONMENT=web
    -sALLOW_MEMORY_GROWTH=1
    -sEXPORTED_FUNCTIONS=_flashkv_execute,_flashkv_tick,_flashkv_state_json
    -sEXPORTED_RUNTIME_METHODS=cwrap
    -o web/dist/flashkv.js
)

cd "$ROOT"

if command -v em++ >/dev/null 2>&1; then
    em++ "${EMCC_ARGS[@]}"
else
    docker run --rm \
        -u "$(id -u):$(id -g)" \
        -v "$ROOT:/src" -w /src \
        emscripten/emsdk:latest \
        em++ "${EMCC_ARGS[@]}"
fi

python3 - "$ROOT" <<'PY'
import sys
from pathlib import Path

root = Path(sys.argv[1])
template = (root / "web" / "index.html").read_text()
engine = (root / "web" / "dist" / "flashkv.js").read_text()

# Keep "</script>" inside the bundle from closing the inline tag.
engine = engine.replace("</script", "<\\/script")

page = template.replace("/*__FLASHKV_ENGINE__*/", engine)

# artifact.html: bare page content for hosts that add their own skeleton.
(root / "web" / "dist" / "artifact.html").write_text(page)

# index.html: complete standalone document (GitHub Pages, local files).
head, body = page.split('<div class="wrap">', 1)
standalone = (
    "<!doctype html>\n<html lang=\"en\">\n<head>\n"
    "<meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
    + head
    + "</head>\n<body>\n<div class=\"wrap\">"
    + body
    + "</body>\n</html>\n"
)
(root / "web" / "dist" / "index.html").write_text(standalone)
print(f"Wrote web/dist/index.html and artifact.html ({len(page) // 1024} KiB)")
PY
