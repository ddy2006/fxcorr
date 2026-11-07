#!/usr/bin/env bash
# prepare_workdir.sh —— 为 xp-opt 构造实验 workdir
#
# 源 workdir 的 config/ batches/ fengine/ 用软链挂进来（只读输入；xp 会幂等写回
# 各 ds 的 pcal.bin），raw/ meta/ vis/ product/ vis-parts/ 是新建空目录：
#   - meta/ 故意为空：源 workdir 的 meta/roots 记录的是 scalebox 平台路径，
#     宿主直跑会被根一致性检查挡下（FxcorrPath::checkRoots）；无记录即通过。
#   - vis/ product/ 为空：避免 SWIN 互斥检查（同一 batch 不允许重复写）。
#
# 用法: prepare_workdir.sh <src_workdir> [dest_workdir]
# 默认 dest = $XP_OPT_ROOT/workdir/<src 基名>
set -euo pipefail

XP_OPT_ROOT=${XP_OPT_ROOT:-/data/scalebox/mydata/fxcorr/xp-opt}

die() { echo "prepare_workdir.sh: $*" >&2; exit 1; }

if [ $# -lt 1 ] || [ $# -gt 2 ]; then
	sed -n '2,12p' "$0" >&2
	exit 2
fi

[ -d "$1" ] || die "源 workdir 不存在: $1"
SRC=$(cd "$1" && pwd)
for d in config batches fengine; do
	[ -d "$SRC/$d" ] || die "源 workdir 缺目录: $SRC/$d"
done

DEST=${2:-$XP_OPT_ROOT/workdir/$(basename "$SRC")}
mkdir -p "$DEST"/{raw,meta,vis,product,vis-parts}

ln -sfn "$SRC/config"   "$DEST/config"
ln -sfn "$SRC/batches"  "$DEST/batches"
ln -sfn "$SRC/fengine"  "$DEST/fengine"

printf '实验 workdir: %s\n' "$DEST"
printf '  config   -> %s/config\n' "$SRC"
printf '  batches  -> %s/batches\n' "$SRC"
printf '  fengine  -> %s/fengine  (xp 会写回 pcal.bin)\n' "$SRC"
printf '  meta/vis/product/vis-parts/raw 为新建空目录\n'
