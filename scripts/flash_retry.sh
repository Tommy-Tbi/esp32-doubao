#!/usr/bin/env bash
# 在用户按住 BOOT + EN 的瞬间反复尝试连接并烧录
# 用法: ./scripts/flash_retry.sh [COM口]
set -u
PORT="${1:-COM4}"
cd "$(dirname "$0")/.."
source scripts/env.sh >/dev/null 2>&1

for i in $(seq 1 60); do
  echo "=== [$(date +%T)] 尝试 $i 次 ($PORT) ==="
  if timeout 60 ./scripts/idf.sh -p "$PORT" flash; then
    echo ">>> 烧录成功"
    exit 0
  fi
  sleep 1
done
echo ">>> 60 次均失败，请检查 BOOT/EN 或 USB 连接"
exit 1
