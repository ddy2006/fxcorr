#!/usr/bin/env bash
# set_roots.sh —— 按当前环境变量重写 meta/roots/*.json（"记录改写"；fxcorr-prep
# 任务内调用 + 手工场景）
#
# **当前状态（2026-10-08 起）**：流水线内的记录改写由 **fxcorr-prep 模块**在任务
# 内调用本脚本完成（prep 落在有数据面的节点；2026-10-07–10-08 曾短暂由 app-fxcorr
# 的 router 进程代写，因 p419 上 router 落控制面、无数据面而移回数据面——见
# app-fxcorr 的 docs/p419-scale-plan.md）。**仿真数据无论用命令行方式
# （make_testdata.sh）还是流水线方式（FXCORR_SIM_ONLY）生成，跑链前都不需要手工
# 调用本脚本**；手工场景（手工投单模块任务、宿主手工跑链、单模块调试）仍可直接用。
#
# 背景（data-spec 5.2.1）：根记录描述"这批数据用了哪套布局"，三个程序启动时与它
# 比对、不一致即报错停链。本地计算模式（app-fxcorr 的 FXCORR_ROUTE=1|2）把
# raw/fengine 指到节点本地：共享侧既有的记录若不改写，后续 batch 一跑就报
# `disagrees`（护栏生效，属于预期拦阻）。本脚本把记录重写成"当前环境变量解析出的
# 布局"，workdir 内**全部** batches/*.json 一次处理。
#
# **用法前提（务必）**：在**同一条命令的环境里**先 export 好本地根变量再跑：
#   FXCORR_RAW_ROOT=/tmp/fxcorr/raw FXCORR_FENGINE_ROOT=/tmp/fxcorr/fengine \
#       ./set_roots.sh /cluster_data_root/fxcorr/l4
# 不 export 就跑 = 按共享布局写回（等于什么都没改）。根一律用**直接映射写法**
# （/tmp/... 或 /dev/shm/...，不写 /local_data_root 前缀——容器内两者同指一处，
# 但记录按字符串比对，全链必须统一一种写法；见 data-spec 5.2.1）。
#
# 只应在**确实按该布局准备/搬运了数据**时执行（route=1 的 raw-copy、route=2 的
# 本地 sim 即由此成立）：本脚本不做任何校验，只是"按你给的环境变量写记录"。
#
# 用法：./set_roots.sh [workdir]      （workdir 默认 . 或 $FXCORR_WORKDIR）
set -euo pipefail

SCRIPTDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$SCRIPTDIR/roots.sh"

if [ $# -gt 1 ]; then
	echo "用法：./set_roots.sh [workdir]（workdir 默认 . 或 \$FXCORR_WORKDIR）" >&2
	exit 2
fi
WORKDIR="${FXCORR_WORKDIR:-.}"
if [ $# -ge 1 ]; then
	WORKDIR=$1
fi
[ -d "$WORKDIR" ] || { echo "set_roots.sh: workdir ${WORKDIR} not found" >&2; exit 2; }

fxcorr_roots "$WORKDIR"		# 解析（workdir 绝对化 + 四个根，规则见 roots.sh）
WORKDIR=$FXCORR_ROOT_WORKDIR

shopt -s nullglob
batches=("$WORKDIR"/batches/*.json)
if [ ${#batches[@]} -eq 0 ]; then
	echo "set_roots.sh: no batches/*.json under ${WORKDIR}" >&2
	exit 2
fi

# 批量写记录（2026-11-05 优化）：原实现对每个批调一次 fxcorr_write_roots，
# 而每次内部有 5 个 $(...) 命令替换 + cat + mkdir——480 批约 3000+ 次 fork，
# 实测占 prep 的六成（p419，480 批 / 491.52 s：prep 107 s 里约 66 s 在这里）。
# 记录内容是**每批完全相同**的（只随四个根变化），所以在这里一次性算好，
# 循环内只用 printf 内建重定向（零 fork）；basename 也换成参数展开。
# 内容格式与 fxcorr_write_roots 逐字节一致（同样是 5 字段多行 JSON）。
rec_wd=$(fxcorr_root_norm "$FXCORR_ROOT_WORKDIR")
rec_raw=$(fxcorr_root_record "$FXCORR_ROOT_RAW" raw)
rec_fengine=$(fxcorr_root_record "$FXCORR_ROOT_FENGINE" fengine)
rec_vis=$(fxcorr_root_record "$FXCORR_ROOT_VIS" vis)
rec_product=$(fxcorr_root_record "$FXCORR_ROOT_PRODUCT" product)

rootsdir="$WORKDIR/meta/roots"
mkdir -p "$rootsdir" || { echo "set_roots.sh: cannot create ${rootsdir}" >&2; exit 2; }

template='{
  "workdir": "%s",
  "raw": "%s",
  "fengine": "%s",
  "vis": "%s",
  "product": "%s"
}
'
for f in "${batches[@]}"; do
	bid=${f##*/}
	bid=${bid%.json}
	printf "$template" "$rec_wd" "$rec_raw" "$rec_fengine" "$rec_vis" "$rec_product" \
		> "$rootsdir/$bid.json" || {
		echo "set_roots.sh: write meta/roots/${bid}.json failed" >&2
		exit 2
	}
done
echo "set_roots.sh: wrote ${#batches[@]} meta/roots/*.json"

# 打印写出的值供核对（R8：不 export 时静默写回共享布局——这里是唯一的确认点）
echo "set_roots.sh: workdir=${WORKDIR}"
printf '  %-8s %s\n' raw     "$(fxcorr_root_record "$FXCORR_ROOT_RAW" raw)"
printf '  %-8s %s\n' fengine "$(fxcorr_root_record "$FXCORR_ROOT_FENGINE" fengine)"
printf '  %-8s %s\n' vis     "$(fxcorr_root_record "$FXCORR_ROOT_VIS" vis)"
printf '  %-8s %s\n' product "$(fxcorr_root_record "$FXCORR_ROOT_PRODUCT" product)"
