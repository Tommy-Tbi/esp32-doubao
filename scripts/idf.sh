#!/usr/bin/env bash
# 在 bash(MSYS) 中调用 ESP-IDF idf.py 的封装脚本
# 用法: ./scripts/idf.sh build
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/env.sh"
unset MSYSTEM
exec "C:\\Espressif\\tools\\python\\v5.5.3\\venv\\Scripts\\python.exe" \
     "$SCRIPT_DIR/idf_run.py" "$@"