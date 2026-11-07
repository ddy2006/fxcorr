#!/usr/bin/env bash
# sync_results.sh —— 把 difx 上的正式结果拉到本地仓库 results/
#
# 用法: sync_results.sh [host]
# 环境变量: XP_OPT_ROOT（远端结果根，默认 /data/scalebox/mydata/fxcorr/xp-opt）
set -euo pipefail

HOST=${1:-${XP_OPT_HOST:-difx}}
REMOTE_ROOT=${XP_OPT_ROOT:-/data/scalebox/mydata/fxcorr/xp-opt}
LOCAL_DIR=$(cd "$(dirname "$0")/.." && pwd)/results

mkdir -p "$LOCAL_DIR"
rsync -av "$HOST:$REMOTE_ROOT/results/" "$LOCAL_DIR/"
echo "已同步: $HOST:$REMOTE_ROOT/results/ -> $LOCAL_DIR/"
