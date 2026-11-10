#!/usr/bin/env bash
# run_xpp.sh —— 以 docker 运行 fxcorr-xpp，生成标准 run 产物
#
# 用法: run_xpp.sh [options] <workdir> <batch_id>
#
# 选项:
#   --formal    正式结果 → $XP_OPT_ROOT/results/<YYYYMMDD>/<run-id>/
#               缺省为临时结果 → $XP_OPT_ROOT/scratch/results/<YYYYMMDD>-<run-id>/
#   -j N        OMP_NUM_THREADS（默认 1）
#   -g G        ds_group（分片模式；缺省 = 全量，写 SWIN）
#   -i IMG      镜像（默认 fxcorr/fxcorr:latest）
#   -b DIR      优化版二进制目录（挂到容器 /opt/xp-opt/bin 并优先 PATH）
#   -r N        重复次数（默认 1；每次清空 vis/vis-parts）
#   -t TAG      run-id 前缀（默认 <workdir 基名>-<batch>-omp<N>[-g<G>]）
#   --keep      不清空 vis/vis-parts（重复跑同一 batch 会触发 SWIN 互斥检查）
#
# 环境变量:
#   XP_OPT_ROOT     结果根（默认 /data/scalebox/mydata/fxcorr/xp-opt）
#   XP_GIT_COMMIT   写进 manifest 的源码版本（可选；远端同步副本没有 .git）
#   FXCORR_LOGLEVEL 等 FXCORR_* 由调用环境透传进容器（见 PASS_ENV）
#
# 每个 run 目录（app-fxcorr 的 run 规范）:
#   manifest.json   配置、数据、镜像、主机、命令
#   result.tsv      一行标准结果
#   metrics.json    阶段耗时、内存、SWIN 指纹
#   command.sh      实际执行的完整命令
#   logs/           run.log / time.txt / timing-brief.tsv / mem-peak-bytes.txt
# 正式 run 另追加 $XP_OPT_ROOT/results/summary.tsv。
set -euo pipefail

XP_OPT_ROOT=${XP_OPT_ROOT:-/data/scalebox/mydata/fxcorr/xp-opt}
IMAGE=${XP_OPT_IMAGE:-fxcorr/fxcorr:latest}
CONTAINER_PATH=/opt/xp-opt/bin
IMAGE_PATH=/usr/local/difx/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

die() { echo "run_xpp.sh: $*" >&2; exit 1; }
usage() { sed -n '2,31p' "$0" >&2; exit 2; }

FORMAL=0
OMP=1
GROUP=""
REPEAT=1
TAG=""
BINDIR=""
KEEP=0
while [ $# -gt 0 ]; do
	case "$1" in
		--formal) FORMAL=1; shift ;;
		-j) OMP=${2:?}; shift 2 ;;
		-g) GROUP=${2:?}; shift 2 ;;
		-i) IMAGE=${2:?}; shift 2 ;;
		-b) BINDIR=${2:?}; shift 2 ;;
		-r) REPEAT=${2:?}; shift 2 ;;
		-t) TAG=${2:?}; shift 2 ;;
		--keep) KEEP=1; shift ;;
		-h|--help) usage ;;
		-*) die "未知选项: $1" ;;
		*) break ;;
	esac
done
[ $# -eq 2 ] || usage

WD=$(cd "$1" && pwd)
BATCH=$2
[ -f "$WD/batches/$BATCH.json" ] || die "缺 batch 描述: $WD/batches/$BATCH.json"
[ -d "$WD/fengine" ] || die "缺 fengine 目录: $WD/fengine"
case "$OMP" in ''|*[!0-9]*) die "-j 必须是正整数: $OMP" ;; esac
case "$REPEAT" in ''|*[!0-9]*) die "-r 必须是正整数: $REPEAT" ;; esac
[ "$OMP" -ge 1 ] && [ "$REPEAT" -ge 1 ] || die "-j/-r 必须 >= 1"

DATASET=$(basename "$WD")
BASE_TAG=${TAG:-$DATASET-$BATCH-omp$OMP}
[ -n "$GROUP" ] && BASE_TAG=$BASE_TAG-g$GROUP
BASE_TAG=$BASE_TAG-xpp

# ---------- 容器挂载 ----------
# workdir 自身，加上 config/batches/fengine 软链的宿主目标（容器内软链要能解析；
# xp 会写回各 ds 的 pcal.bin，不能只读挂载）。
declare -A SEEN=()
MOUNT_ARGS=(-v "$WD:$WD")
add_mount()
{
	local p="$1"
	[ -n "$p" ] && [ -d "$p" ] || return 0
	[ "$p" = "$WD" ] && return 0
	[ -n "${SEEN[$p]:-}" ] && return 0
	SEEN[$p]=1
	MOUNT_ARGS+=(-v "$p:$p")
}
for d in config batches fengine; do
	target=$(readlink -f "$WD/$d" 2>/dev/null || true)
	[ -n "$target" ] || die "无法解析 $WD/$d"
	[ -e "$target" ] || die "软链目标不存在: $WD/$d -> $target"
	add_mount "$(dirname "$target")"
done

if [ -n "$BINDIR" ]; then
	BINDIR=$(cd "$BINDIR" && pwd)
	[ -x "$BINDIR/fxcorr-xpp" ] || die "$BINDIR 里没有可执行的 fxcorr-xpp"
	MOUNT_ARGS+=(-v "$BINDIR:$CONTAINER_PATH:ro")
	PATH_VALUE="$CONTAINER_PATH:$IMAGE_PATH"
else
	PATH_VALUE="$IMAGE_PATH"
fi

# ---------- 透传的 FXCORR_* ----------
PASS_ENV=()
for v in FXCORR_LOGLEVEL FXCORR_STA FXCORR_KURTOSIS FXCORR_X_ALLOW_EMPTY FXCORR_XPP_BAND_THREADS FXCORR_XPP_BLOCK_THREADS; do
	if [ -n "${!v:-}" ]; then PASS_ENV+=(-e "$v=${!v}"); fi
done

# ---------- 清空输出（仅限 xp-opt 目录） ----------
clean_outputs()
{
	case "$WD" in
		*/xp-opt/*|*/xp-opt) ;;
		*) die "拒绝清空非 xp-opt workdir 的产物: $WD" ;;
	esac
	# 产物由容器 root 创建，宿主用户删不掉（目录属 root）——清理也在容器内做
	docker run --rm -v "$WD:$WD" -e "WD=$WD" "$IMAGE" \
		bash -c 'rm -rf "$WD/vis" "$WD/vis-parts" && mkdir -p "$WD/vis" "$WD/vis-parts"' \
		|| die "清理 vis/vis-parts 失败: $WD"
}

# ---------- 主机与镜像信息 ----------
HOST_NAME=$(uname -n)
HOST_NPROC=$(nproc)
HOST_MEM_GIB=$(awk '/MemTotal/{printf "%.0f", $2/1048576}' /proc/meminfo)
IMAGE_ID=$(docker image inspect -f '{{.Id}}' "$IMAGE" 2>/dev/null || echo unknown)
SCRIPT_SHA=$(sha256sum "$0" | awk '{print $1}')
GIT_COMMIT=${XP_GIT_COMMIT:-unknown}

mkdir -p "$XP_OPT_ROOT"
if [ "$FORMAL" -eq 1 ]; then
	DATE_DIR=$(date +%Y%m%d)
	RUN_PARENT="$XP_OPT_ROOT/results/$DATE_DIR"
	SUMMARY="$XP_OPT_ROOT/results/summary.tsv"
else
	DATE_DIR=$(date +%Y%m%d)
	RUN_PARENT="$XP_OPT_ROOT/scratch/results"
	SUMMARY=""
fi
mkdir -p "$RUN_PARENT"
if [ -n "$SUMMARY" ] && [ ! -f "$SUMMARY" ]; then
	printf 'utc\trun_id\tstatus\tdataset\tstations\tbatch\tbatch_sec\tomp\tds_group\timage\timage_id\thost\twall_s\tuser_s\tsys_s\thost_wall_s\tmem_peak_mib\tswin_files\tswin_bytes\tswin_md5\tstage_station_s\tstage_read_s\tstage_xmac_s\tstage_wacc_s\trun_dir\n' > "$SUMMARY"
fi

make_run_dir()
{
	local id="$1"
	local d
	if [ "$FORMAL" -eq 1 ]; then d="$RUN_PARENT/$id"; else d="$RUN_PARENT/$DATE_DIR-$id"; fi
	if [ -e "$d" ]; then d="$d-$(date +%H%M%S)"; fi
	mkdir -p "$d/logs"
	printf '%s\n' "$d"
}

run_once()
{
	local run_id="$1" out="$2"
	[ "$KEEP" -eq 1 ] || clean_outputs

	# 进程 RSS 峰值：cgroup 的 memory.peak 含页缓存，跨运行不可比；用
	# RUSAGE_CHILDREN 的 ru_maxrss（KB）在 xp 退出后取真实进程峰值。
	cat > "$out/logs/measure.py" <<'PY'
import resource, subprocess, sys
rc = subprocess.call(sys.argv[1:])
ru = resource.getrusage(resource.RUSAGE_CHILDREN)
with open('/out/rss-peak-kb.txt', 'w') as f:
    f.write(str(ru.ru_maxrss))
sys.exit(rc)
PY

	local t0 t1 host_wall rc
	t0=$(date +%s.%N)
	local CMD=(docker run --rm
		-w "$WD"
		"${MOUNT_ARGS[@]}"
		-v "$out/logs:/out"
		-e "PATH=$PATH_VALUE"
		-e "OMP_NUM_THREADS=$OMP"
		-e "FXCORR_TIMING_BRIEF=/out/timing-brief.tsv"
		-e "BATCH=$BATCH" -e "WD=$WD" -e "DSGROUP=$GROUP"
		"${PASS_ENV[@]}"
		"$IMAGE" bash -c '
			set -uo pipefail
			export TIMEFORMAT="__TIME__ %R %U %S"
			if [ -n "${DSGROUP:-}" ]; then
				{ time python3 /out/measure.py fxcorr-xpp "$BATCH" "$WD" "$DSGROUP" > /out/run.log 2>&1; } 2> /out/time.txt
			else
				{ time python3 /out/measure.py fxcorr-xpp "$BATCH" "$WD" > /out/run.log 2>&1; } 2> /out/time.txt
			fi
			rc=$?
			if [ -r /sys/fs/cgroup/memory.peak ]; then
				cat /sys/fs/cgroup/memory.peak > /out/mem-peak-bytes.txt
			fi
			exit $rc
		')
	set +e
	"${CMD[@]}"
	rc=$?
	set -e
	t1=$(date +%s.%N)
	host_wall=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.3f", b-a}')

	local wall user sys mem_bytes mem_mib cgroup_mib swin_files swin_bytes swin_md5
	wall=$(awk '/^__TIME__/{print $2}' "$out/logs/time.txt" 2>/dev/null || true)
	user=$(awk '/^__TIME__/{print $3}' "$out/logs/time.txt" 2>/dev/null || true)
	sys=$(awk '/^__TIME__/{print $4}' "$out/logs/time.txt" 2>/dev/null || true)
	# ru_maxrss 单位 KB（Linux）；cgroup memory.peak 含页缓存，只作参考
	mem_bytes=$(( $(cat "$out/logs/rss-peak-kb.txt" 2>/dev/null || echo 0) * 1024 ))
	mem_mib=$(awk -v b="$mem_bytes" 'BEGIN{printf "%.1f", b/1048576}')
	cgroup_mib=$(awk -v b="$(cat "$out/logs/mem-peak-bytes.txt" 2>/dev/null || echo 0)" \
		'BEGIN{printf "%.1f", b/1048576}')
	swin_files=$(find "$WD/vis" -type f -name 'DIFX_*' 2>/dev/null | wc -l | tr -d ' ')
	swin_bytes=$(find "$WD/vis" -type f -name 'DIFX_*' -printf '%s\n' 2>/dev/null | awk '{s+=$1} END{printf "%.0f", s+0}')
	swin_md5=$(find "$WD/vis" -type f -name 'DIFX_*' -print0 2>/dev/null | xargs -0 -r md5sum | awk '{print $1}' | sort | paste -sd, -)

	# 实际命令（供复跑）
	printf '%q ' "${CMD[@]}" > "$out/command.sh"
	printf '\n' >> "$out/command.sh"

	# manifest / metrics / result
	RUN_ID="$run_id" OUT="$out" WD="$WD" BATCH="$BATCH" DATASET="$DATASET" \
	OMP="$OMP" GROUP="${GROUP:-full}" IMAGE="$IMAGE" IMAGE_ID="$IMAGE_ID" \
	HOST_NAME="$HOST_NAME" HOST_NPROC="$HOST_NPROC" HOST_MEM_GIB="$HOST_MEM_GIB" \
	GIT_COMMIT="$GIT_COMMIT" SCRIPT_SHA="$SCRIPT_SHA" FORMAL="$FORMAL" SUMMARY="${SUMMARY:-}" \
	WALL="${wall:-}" USER_T="${user:-}" SYS_T="${sys:-}" HOST_WALL="$host_wall" \
	MEM_BYTES="$mem_bytes" MEM_MIB="$mem_mib" MEM_CGROUP_MIB="$cgroup_mib" SWIN_FILES="$swin_files" \
	SWIN_BYTES="${swin_bytes:-0}" SWIN_MD5="${swin_md5:-}" RC="$rc" \
	python3 - "$out" <<'PY'
import json, os, sys, datetime

out = sys.argv[1]
env = os.environ
now = datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')

def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None

with open(os.path.join(env['WD'], 'batches', env['BATCH'] + '.json')) as f:
    bj = json.load(f)
stations = sorted(set(bj.get('stations', [])))
manifest = {
    'utc': now,
    'run_id': env['RUN_ID'],
    'formal': env['FORMAL'] == '1',
    'dataset': env['DATASET'],
    'workdir': env['WD'],
    'batch': env['BATCH'],
    'batch_sec': bj.get('duration_sec'),
    'stations': stations,
    'n_stations': len(stations),
    'omp': int(env['OMP']),
    'ds_group': env['GROUP'],
    'image': env['IMAGE'],
    'image_id': env['IMAGE_ID'],
    'host': env['HOST_NAME'],
    'nproc': int(env['HOST_NPROC']),
    'mem_gib': int(env['HOST_MEM_GIB']),
    'git_commit': env['GIT_COMMIT'],
    'script_sha256': env['SCRIPT_SHA'],
}
with open(os.path.join(out, 'manifest.json'), 'w') as f:
    json.dump(manifest, f, indent=2, ensure_ascii=False)
    f.write('\n')

stages = {}
brief = os.path.join(out, 'logs', 'timing-brief.tsv')
if os.path.exists(brief):
    with open(brief) as f:
        for line in f:
            p = line.rstrip('\n').split('\t')
            if len(p) >= 7:
                try:
                    stages[p[5]] = round(float(p[6]), 6)
                except ValueError:
                    pass

metrics = {
    'wall_s': num(env['WALL']),
    'user_s': num(env['USER_T']),
    'sys_s': num(env['SYS_T']),
    'host_wall_s': num(env['HOST_WALL']),
    'mem_peak_bytes': int(env['MEM_BYTES'] or 0),
    'mem_peak_mib': num(env['MEM_MIB']),
    'mem_peak_source': 'ru_maxrss (RUSAGE_CHILDREN)',
    'mem_cgroup_peak_mib': num(env['MEM_CGROUP_MIB']),
    'swin': {
        'files': int(env['SWIN_FILES'] or 0),
        'bytes': int(env['SWIN_BYTES'] or 0),
        'md5': [x for x in env['SWIN_MD5'].split(',') if x],
    },
    'stages_s': stages,
    'rc': int(env['RC']),
}
with open(os.path.join(out, 'metrics.json'), 'w') as f:
    json.dump(metrics, f, indent=2, ensure_ascii=False)
    f.write('\n')

status = 'ok' if env['RC'] == '0' else 'fail'
row = [
    now, env['RUN_ID'], status, env['DATASET'], str(len(stations)),
    env['BATCH'], str(bj.get('duration_sec', '')), env['OMP'], env['GROUP'],
    env['IMAGE'], env['IMAGE_ID'], env['HOST_NAME'],
    env['WALL'], env['USER_T'], env['SYS_T'], env['HOST_WALL'],
    env['MEM_MIB'], env['SWIN_FILES'], env['SWIN_BYTES'], env['SWIN_MD5'],
    str(stages.get('station', '')), str(stages.get('read', '')),
    str(stages.get('xmac', '')), str(stages.get('wacc', '')),
    out,
]
with open(os.path.join(out, 'result.tsv'), 'w') as f:
    f.write('\t'.join(row) + '\n')

summary = os.environ.get('SUMMARY')
if summary and env['FORMAL'] == '1':
    with open(summary, 'a') as f:
        f.write('\t'.join(row) + '\n')
PY

	printf '[%s] %s rc=%s status=%s wall=%ss user=%ss sys=%ss mem=%sMiB swin=%s files/%s B\n' \
		"$(date -u +%FT%TZ)" "$run_id" "$rc" "$([ "$rc" -eq 0 ] && echo ok || echo fail)" \
		"${wall:-?}" "${user:-?}" "${sys:-?}" "$mem_mib" "$swin_files" "${swin_bytes:-0}"
	return "$rc"
}

fail=0
for i in $(seq 1 "$REPEAT"); do
	id=$BASE_TAG
	[ "$REPEAT" -gt 1 ] && id="$BASE_TAG-r$i"
	out=$(make_run_dir "$id")
	run_once "$id" "$out" || fail=1
	[ "$fail" -eq 0 ] || break
done

if [ "$fail" -ne 0 ]; then
	echo "run_xpp.sh: 运行失败（详见 run 目录的 logs/run.log）" >&2
	exit 1
fi
