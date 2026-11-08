#!/usr/bin/env bash
# run_baseline.sh —— 基准批量执行器（1024ms 数据集起）
#
# 对每个数据集（workdir-<S>st-b<B>ms-n3）：
#   1) 缺实验 workdir 时先 prepare_workdir.sh 构造；
#   2) 预热：最大 OMP 对首个 batch 跑一次临时 run（不计入正式结果）；
#   3) 逐 batch × OMP 调 run_xp.sh --formal -r R；
#   4) 校验同 (dataset,batch,ds_group) 的 SWIN md5 唯一，打印本轮新增汇总行。
#
# 用法: run_baseline.sh [选项]
#   -S 2,4,8,16      站数列表（默认 2）
#   -b 1024         批长毫秒列表（默认 1024）
#   -B 00000001,...  batch 列表（默认数据集内全部 batches/*.json）
#   -j 1,8,16        OMP 列表（默认 1,8,16）
#   -r 3             每组合重复次数（默认 3）
#   --scratch        冒烟模式：不写正式 results/（run_xp.sh 不带 --formal）
#   -n               dry-run，只打印将执行的命令
#
# 环境变量: DATA_ROOT（默认 /data/scalebox/mydata/fxcorr）
#           XP_OPT_ROOT（默认 /data/scalebox/mydata/fxcorr/xp-opt）
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
DATA_ROOT=${DATA_ROOT:-/data/scalebox/mydata/fxcorr}
XP_OPT_ROOT=${XP_OPT_ROOT:-/data/scalebox/mydata/fxcorr/xp-opt}
RUN_XP=$HERE/run_xp.sh

die() { echo "run_baseline.sh: $*" >&2; exit 1; }
usage() { sed -n '2,23p' "$0" >&2; exit 2; }

STATIONS=2
BMS=1024
BATCHES=""
OMPS="1,8,16"
REPEAT=3
SCRATCH=0
DRY=0
while [ $# -gt 0 ]; do
	case "$1" in
		-S) STATIONS=${2:?}; shift 2 ;;
		-b) BMS=${2:?}; shift 2 ;;
		-B) BATCHES=${2:?}; shift 2 ;;
		-j) OMPS=${2:?}; shift 2 ;;
		-r) REPEAT=${2:?}; shift 2 ;;
		--scratch) SCRATCH=1; shift ;;
		-n) DRY=1; shift ;;
		-h|--help) usage ;;
		*) die "未知参数: $1" ;;
	esac
done
[ -x "$RUN_XP" ] || die "缺可执行脚本: $RUN_XP"
[ -x "$HERE/prepare_workdir.sh" ] || die "缺可执行脚本: $HERE/prepare_workdir.sh"

FORMAL_ARGS=()
if [ "$SCRATCH" -eq 0 ]; then
	FORMAL_ARGS=(--formal)
	SUMMARY=$XP_OPT_ROOT/results/summary.tsv
	if [ -f "$SUMMARY" ]; then START_LINE=$(wc -l < "$SUMMARY"); else START_LINE=0; fi
else
	SUMMARY=""
	START_LINE=0
fi

run() {
	if [ "$DRY" -eq 1 ]; then
		printf '[dry-run]'; printf ' %q' "$@"; printf '\n'
	else
		printf '[%s] ' "$(date -u +%FT%TZ)"
		printf '%q ' "$@"; printf '\n'
		"$@"
	fi
}

max_omp() {
	local IFS=, best=1 x
	for x in $OMPS; do [ "$x" -gt "$best" ] && best=$x; done
	printf '%s\n' "$best"
}

T0=$(date +%s)
WARM_OMP=$(max_omp)
for s in ${STATIONS//,/ }; do
	for b in ${BMS//,/ }; do
		ds=workdir-${s}st-b${b}ms-n3
		SRC=$DATA_ROOT/$ds
		WD=$XP_OPT_ROOT/workdir/$ds
		[ -d "$SRC" ] || { echo "跳过（源不存在）: $SRC" >&2; continue; }
		echo "=== $ds ==="

		if [ -d "$WD" ]; then
			echo "实验 workdir 已存在: $WD"
		else
			run "$HERE/prepare_workdir.sh" "$SRC"
		fi

		if [ -n "$BATCHES" ]; then
			batches=(${BATCHES//,/ })
		else
			batches=()
			while IFS= read -r f; do
				batches+=("$(basename "$f" .json)")
			done < <(ls "$SRC"/batches/*.json | sort)
		fi
		[ ${#batches[@]} -gt 0 ] || die "没有可用 batch: $SRC/batches"

		echo "--- 预热: $ds batch=${batches[0]} OMP=$WARM_OMP"
		run "$RUN_XP" -j "$WARM_OMP" "$WD" "${batches[0]}"

		for omp in ${OMPS//,/ }; do
			for bid in "${batches[@]}"; do
				echo "--- 正式: $ds batch=$bid OMP=$omp r=$REPEAT"
				run "$RUN_XP" "${FORMAL_ARGS[@]}" -j "$omp" -r "$REPEAT" "$WD" "$bid"
			done
		done
	done
done

T1=$(date +%s)
echo
echo "总耗时: $((T1 - T0)) s"
[ "$DRY" -eq 1 ] && exit 0

if [ -z "$SUMMARY" ] || [ ! -f "$SUMMARY" ]; then
	echo "（scratch 模式，无 summary.tsv 校验）"
	exit 0
fi

echo "本轮新增汇总（SWIN md5 按 dataset|batch|ds_group 校验）:"
awk -F'\t' -v start="$START_LINE" '
NR > start && $1 != "utc" && NF >= 20 {
	key = $4 "|" $6 "|" $9
	cnt[key]++
	if (!(key in md5)) md5[key] = $20
	else if (md5[key] != $20) { bad[key] = 1; alt[key] = $20 }
}
END {
	rc = 0
	for (k in cnt) {
		printf "  %-46s runs=%-2d md5=%s", k, cnt[k], md5[k]
		if (k in bad) { printf " MISMATCH(%s)", alt[k]; rc = 1 }
		printf "\n"
	}
	if (!length(cnt)) { print "  （无新增行）" }
	exit rc
}' "$SUMMARY" || { echo "SWIN md5 不一致，见上" >&2; exit 1; }
