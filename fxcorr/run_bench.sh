#!/usr/bin/env bash
# run_bench.sh —— mpifxcorr 基准（对拍基准生成器，规格见 fxcorr/v1-plan.md 2.4）
#
# 步骤：① 定位 batch（DATA TABLE 软链 target 里的 batch_id，fallback batches/ 最新 json）
# → ② 从 batch.json + .input 推导 EXECUTE TIME（整秒字段，取 floor(initsec + N*intTime)
# + 1——多留一个整秒，mpifxcorr 才把 N 个积分都写完整，见下方 ② 的注释）
# → ③ 复制 config .input → bench/，sed EXECUTE TIME / OUTPUT FILENAME → bench/<exp>.difx
# → ③b（FXCORR_BENCH_NBATCH > 1）把 DATA TABLE 展开成连续多批的多 FILE 行
# → ④ mpirun mpifxcorr 出基准 SWIN（含内存峰值采集）→ ④½ 截掉 EXECUTE TIME 多留出的第 N+1 个积分
# → ⑤ 打印 cmp_swin.py 对拍提示。
#
# 用法：./run_bench.sh [workdir]
#   workdir  项目根目录（默认 .，须含 make_testdata.sh 布局：config/ + batches/ + DATA TABLE 软链）
#   环境变量：NP（mpirun 进程数，默认 4）；FXCORR_WORKDIR（项目根目录，位置参数优先）
#             FXCORR_BENCH_NBATCH 连续批数 M（默认 1）：基准段 = 软链批起向后 M 个连续批，
#               DATA TABLE 每 ds 展开为 M 条 FILE 行、EXECUTE TIME 与截断组数随段长放大
#             跨节点（V7 P5，见 v7-plan.md §16）：
#               FXCORR_MPI_PPN   每节点 rank 数，节点列表由 Slurm 分配自动展开
#               FXCORR_MPI_HOSTS 直接给 mpirun 的 -host 串（`a:17,b:17`），绕开自动展开
#               FXCORR_MPI_MAPBY 给 mpirun 的 --map-by（如 node）
#               FXCORR_MPI_BIND 给 mpirun 的 --bind-to（rankfile 用 core:overload-allowed）
#               FXCORR_MPI_OVERSUBSCRIBE=1 单独加 --oversubscribe
set -euo pipefail

SCRIPTDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# 目录根解析（V5 P5）：mpifxcorr 不认这些根，但基准的 .input 副本里要把
# 相对路径绝对化，得知道 raw 根在哪
. "$SCRIPTDIR/roots.sh"
# 判据同 make_testdata.sh：用 fxcorr 自己的工具，不用 mpifxcorr——机器上可能已经
# 有另一份 difx 的 bin 在 PATH 里（测试机的 BASH_ENV 注入 /opt/difx/2.9.0），
# 那里有 mpifxcorr 却没有 fxcorr 三工具，用它会跳过 source 而混用两份 difx。
if ! command -v fxcorr-f >/dev/null 2>&1 && [ -f "$SCRIPTDIR/../setup.bash" ]; then
	# setup.bash 的 PurgePath 引用可能未设置的变量（PERL5LIB 等），
	# 与 set -u 冲突，source 时临时放开
	set +u
	. "$SCRIPTDIR/../setup.bash"
	set -u
fi

usage()
{
	cat >&2 <<'EOF'
用法：./run_bench.sh [workdir]
  workdir  项目根目录（默认 .，须含 make_testdata.sh 布局：config/ + batches/ + DATA TABLE 软链）
  环境变量：NP（mpirun 进程数，默认 4）；FXCORR_WORKDIR（项目根目录，位置参数优先）
            FXCORR_BENCH_NBATCH 连续批数 M（默认 1，从软链批起向后取 M 个连续批）
            FXCORR_MPI_PPN   每节点 rank 数，节点从 Slurm 分配展开（跨节点，V7 P5）
            FXCORR_MPI_HOSTS 直接给 mpirun 的 -host 串，如 "n1:17,n2:17"
            FXCORR_MPI_MAPBY 给 mpirun 的 --map-by，如 node
EOF
	exit "${1:-2}"
}

[ $# -le 1 ] || usage
WORKDIR="${FXCORR_WORKDIR:-.}"
if [ $# -gt 0 ] && [ -d "$1" ]; then
	WORKDIR=$1	# 位置参数优先于环境变量
	shift
fi
[ $# -eq 0 ] || usage
WORKDIR=$(cd "$WORKDIR" && pwd)
fxcorr_roots "$WORKDIR"
WORKDIR=$FXCORR_ROOT_WORKDIR
if [ ! -d "$WORKDIR/batches" ] || [ ! -d "$WORKDIR/config" ]; then
	echo "run_bench.sh: $WORKDIR does not look like a make_testdata.sh workdir (need config/ and batches/)" >&2
	exit 2
fi
command -v mpifxcorr >/dev/null 2>&1 || { echo "run_bench.sh: mpifxcorr not found (source setup.bash or install mpifxcorr)" >&2; exit 2; }
command -v mpirun >/dev/null 2>&1 || { echo "run_bench.sh: mpirun not found" >&2; exit 2; }
if ! [[ ${NP:-4} =~ ^[1-9][0-9]*$ ]]; then
	echo "run_bench.sh: NP must be a positive integer" >&2
	exit 2
fi
# 连续批数（多批基准，见 ② 与 ③b）：1 = 原单批行为，逐字节不变
NBATCH_BENCH="${FXCORR_BENCH_NBATCH:-1}"
if ! [[ $NBATCH_BENCH =~ ^[1-9][0-9]*$ ]]; then
	echo "run_bench.sh: FXCORR_BENCH_NBATCH must be a positive integer" >&2
	exit 2
fi
# python3 必须是 3：目标集群的 /usr/bin/python3 是 RHEL 7 的 **2.7.5**，得先 source
# env.sh 让 PATH 前置 miniforge（fxcorr/build.md 坑 1）。本脚本一直依赖 python3 解析
# batch.json，但 P5 多了一个**后台**起的 mpi_sampler.py——它语法错会静默退出，外面只
# 看到"sampler produced no data"，连不到真正的原因上。所以提前卡在这里。
python3 -c 'import sys; sys.exit(0 if sys.version_info[0] >= 3 else 1)' 2>/dev/null || {
	echo "run_bench.sh: python3 is not Python 3 ($(python3 --version 2>&1)) — source env.sh first" >&2
	exit 2
}

# ---- ①② 定位 batch、推导 EXECUTE TIME 截断 ----
# python3：json 读取 + .input 解析 + 浮点（bash 无浮点算术）
OUT=$(mktemp)
trap 'rm -f "${OUT:-}"' EXIT
python3 - "$WORKDIR" "$FXCORR_ROOT_RAW" "$NBATCH_BENCH" <<'PYEOF' > "$OUT"
import glob, json, math, os, re, sys

workdir = sys.argv[1]
rawroot = sys.argv[2]
nbatch = int(sys.argv[3])
batchdir = os.path.join(workdir, 'batches')

# ① batch 定位：优先从 DATA TABLE 软链 target 解析，与数据精确对应（-n 多 batch
#    时软链指向最后 batch）；无软链（手工布局）时用 batches/ 下 mtime 最新的 json。
#    target 是 make_testdata.sh 布局的 raw/<station>/<station>_<batch_id>[_ds<N>].vdif：
#    站名从 target 的父目录取、不解析文件名——站名可能含下划线，多 datastream
#    还有 _ds 后缀，按 "_" 切分的解析会错。反过来与各 batch.json 的编号比对。
def softlink_bid():
    target = None
    for j in glob.glob(os.path.join(batchdir, '*.json')):
        b = json.load(open(j))
        cfg = os.path.join(workdir, b['config_file'])
        if not os.path.exists(cfg):
            continue
        m = re.search(r'^FILE \d+/\d+:\s*(\S+)\s*$', open(cfg).read(), re.M)
        if not m:
            continue
        # FILE 行是相对路径时按 RAW 根拼——与程序内的 FxcorrPath 同一规则
        # （data-spec 5.2.1）；绝对路径原样（真实观测的 FILE 行就是绝对路径，
        # 那里也没有软链可读，自然退到 fallback）。软链由 make_testdata.sh 建
        # （2026-10-02 起 run_batch.sh 不再重指——本脚本是它保留的消费者之一）。
        # 2026-09-28 之前这里拼的是 workdir，只在 RAW 根未重定向（= workdir 的
        # 默认值）时才碰巧正确；一旦按 Q2 把 raw 指到大盘就找不到软链，静默退到
        # "取最新 batch.json"——多 batch 场景下那个 fallback 很可能选错 batch。
        fn = m.group(1)
        lnk = fn if os.path.isabs(fn) else os.path.join(rawroot, fn)
        if os.path.islink(lnk):
            target = os.readlink(lnk)
            break
    if target is None:
        return None
    st = os.path.basename(os.path.dirname(target))
    base = os.path.basename(target)
    for j in glob.glob(os.path.join(batchdir, '*.json')):
        bid = os.path.basename(j)[:-len('.json')]
        if base == '%s_%s.vdif' % (st, bid) or base.startswith('%s_%s_ds' % (st, bid)):
            return bid
    return None

# 显式批号（批级多节点：FILE 行是真实路径、无软链可解析——softlink_bid 会失效并
# fallback 到"最新 batch.json"，导致 EXECUTE TIME 按错批计算（2026-10-18 实测
# 批 4 得 EXECUTE TIME=0、mpifxcorr 0.3 s 空跑））。
bid = os.environ.get('FXCORR_BENCH_BATCH') or softlink_bid()
if bid is None:
    jsons = sorted(glob.glob(os.path.join(batchdir, '*.json')), key=os.path.getmtime)
    if not jsons:
        sys.exit('run_bench.sh: no batch.json in %s' % batchdir)
    bid = os.path.basename(jsons[-1])[:-len('.json')]
    print('run_bench.sh: no symlinked batch_id, using latest batch.json %s' % bid,
          file=sys.stderr)

bj = json.load(open(os.path.join(batchdir, bid + '.json')))
cfgrel = bj['config_file']
cfg = os.path.join(workdir, cfgrel)
if not os.path.exists(cfg):
    sys.exit('run_bench.sh: config_file %s not found' % cfgrel)
text = open(cfg).read()

def one(key):
    m = re.search(r'^%s:\s*(.*)$' % re.escape(key), text, re.M)
    if not m:
        sys.exit('run_bench.sh: %s not found in %s' % (key, cfgrel))
    return m.group(1).strip()

mjd0 = int(one('START MJD'))
sec0 = float(one('START SECONDS'))
exp = os.path.basename(one('OUTPUT FILENAME').rstrip('/'))
if exp.endswith('.difx'):
    exp = exp[:-len('.difx')]

# ② EXECUTE TIME：mpifxcorr 的 writedata 在积分满 intTime 时按积分起点
# currentstartseconds + scanstartsec >= executeseconds 判定停写（mpifxcorr EXECUTE
# TIME 语义，fxcorr-x CLAUDE.md 已实证），EXECUTE TIME 又是整秒字段。
#
# 不能取 floor(initsec + (N-1)*intTime) + 1（曾经如此，按"积分 N+1 起点 >= 截断值即停"
# 推导）：mpifxcorr 不只在积分起点处判停，它把数据读到 EXECUTE TIME 就停，末积分因此
# 只累积到 EXECUTE TIME 为止。t25362 真实数据实测（2026-09-18）：EXECUTE TIME=11 时
# 末积分 [24621.752, 24622.776) 的 weight 只有 0.7356，而基准应为 0.9956——差值恒为
# 0.26，与"mpifxcorr 读到 24622.5 前后停止累积"吻合。合成测试未暴露此问题，因为
# batch 起点即 START SECONDS、末积分恰好压在整秒上（6/6 对拍时 ET=2 与 ET=3 结果相同）。
#
# 取 floor(initsec + N*intTime) + 1：N 个积分全部落在 EXECUTE TIME 之前，mpifxcorr 会
# 把它们写完整；代价是多写第 N+1 个积分（起终点都在 batch 窗口之外），跑完由 ④½ 截掉。
nsub = int(bj['n_subints'])
subint = int(bj['subint_ns'])
inttime = float(bj['integration_sec'])
# 批级（route=3 grain=batch）用 FXCORR_BENCH_START_MJD 覆盖 .input 的 START：
# 此时 initsec 相对"被覆盖后的 START"计算（= 0 附近，R10 由模块保证软链同步）。
start_override = os.environ.get('FXCORR_BENCH_START_MJD', '').strip()
if start_override:
    initsec = (bj['start_mjd'] - float(start_override)) * 86400.0
else:
    initsec = (bj['start_mjd'] - mjd0 - sec0 / 86400.0) * 86400.0
batchdur = nsub * subint / 1.0e9
nint = round(batchdur / inttime)
if abs(nint * inttime - batchdur) > 1e-6:
    sys.exit('run_bench.sh: batch duration %g is not an integer multiple of INT TIME %g '
             '(integrals would not line up for comparison)' % (batchdur, inttime))

# 多批（FXCORR_BENCH_NBATCH > 1）：基准段 = 软链批起向后 M 个连续批。批号跳号、
# 时间不连续、subint/积分参数不一致都在这里挡掉——不挡的话 mpifxcorr 会拿着排错
# 的 FILE 行静默读错段（数据时间戳与 START 对不上正是 R10 那类慢速空转的成因）。
if nbatch > 1:
    if not bid.isdigit():
        sys.exit('run_bench.sh: batch id %r is not numeric; multi-batch needs '
                 'sequential numeric ids' % bid)
    prev_mjd = bj['start_mjd']
    for k in range(1, nbatch):
        nbid = '%08d' % (int(bid) + k)
        npath = os.path.join(batchdir, nbid + '.json')
        if not os.path.exists(npath):
            sys.exit('run_bench.sh: FXCORR_BENCH_NBATCH=%d needs batch %s, not found'
                     % (nbatch, nbid))
        nb = json.load(open(npath))
        if (int(nb['n_subints']) != nsub or int(nb['subint_ns']) != subint
                or abs(float(nb['integration_sec']) - inttime) > 1e-9):
            sys.exit('run_bench.sh: batch %s has different subint/integration '
                     'parameters than %s' % (nbid, bid))
        # 相邻差 = 批长。容差 1e-4 s：start_mjd 是 MJD 天数的 f64，两个 ~61037 的
        # 数相减有 ~2 µs 的表示误差；真正的错位（跳批/断段）差一个批长（≥0.128 s）。
        dt = (nb['start_mjd'] - prev_mjd) * 86400.0 - batchdur
        if abs(dt) > 1e-4:
            sys.exit('run_bench.sh: batch %s start_mjd is not continuous with the '
                     'previous batch (off by %.6f s)' % (nbid, dt))
        prev_mjd = nb['start_mjd']

nint_total = nbatch * nint
exec_ = int(math.floor(initsec + nint_total * inttime)) + 1

print('BID=%s' % bid)
print('CFGIN=%s' % cfgrel)
print('EXEC=%d' % exec_)
print('NINT=%d' % nint_total)
print('EXP=%s' % exp)
print('OUTDIR=%s/bench/%s.difx' % (workdir, exp))
print('NCHAN=%d' % int(bj['n_channels']))
PYEOF

BID= CFGIN= EXEC= NINT= EXP= OUTDIR= NCHAN=
while read -r line; do
	[ -n "$line" ] || continue
	k=${line%%=*}
	v=${line#*=}
	case "$k" in
		BID) BID=$v ;;
		CFGIN) CFGIN=$v ;;
		EXEC) EXEC=$v ;;
		NINT) NINT=$v ;;
		EXP) EXP=$v ;;
		OUTDIR) OUTDIR=$v ;;
		NCHAN) NCHAN=$v ;;
	esac
done < "$OUT"
[ -n "$BID" ] || { echo "run_bench.sh: failed to locate a batch" >&2; exit 2; }
# 输出目录覆盖（2026-10-18）：多节点批级并行时每个批任务必须用独立目录，否则
# 两个节点同时 rm -rf/重建 bench/<exp>.difx 会互相踩（实测批 2 写失败）。
# 空 = 原行为（bench/<exp>.difx）。
# 自定义 .input（批级多节点：每批独立副本，避免共享软链竞争）
if [ -n "${FXCORR_BENCH_INPUT:-}" ]; then CFGIN=$FXCORR_BENCH_INPUT; fi
if [ -n "${FXCORR_BENCH_OUTDIR:-}" ]; then
	OUTDIR=$FXCORR_BENCH_OUTDIR
fi

# ---- ③ 复制 config .input → bench/，sed EXECUTE TIME 截断 + OUTPUT FILENAME ----
# OUTPUT FILENAME 改指 bench/<exp>.difx：基准 SWIN 与 fxcorr 侧（.input OUTPUT
# FILENAME 目录）分开落盘，对拍时互不覆盖。CALC FILENAME 为绝对路径，无需改。
mkdir -p "$WORKDIR/bench"
# V5 P5 起 config 里的相对路径（.input 的 CALC FILENAME / FILE 行、.calc 的
# IM/FLAG FILENAME）是相对各自根的，而 mpifxcorr 按 **cwd**（workdir）解析。
# 这里把它们绝对化，并把 .calc 复制一份到 bench/ 一并处理——这正是 Q15
# "凡调用原 difx 程序，由 bash 做规范转换"的那一层。
CFGDIR="$WORKDIR/$(dirname "$CFGIN")"
CALCIN=$(python3 -c "
import re, sys
for line in open(sys.argv[1]):
    m = re.match(r'^CALC FILENAME:\s*(\S+)\s*$', line)
    if m:
        print(m.group(1))
        break
" "$WORKDIR/$CFGIN")
CALCBASE=$(basename "$CALCIN")
# 批级并行（FXCORR_BENCH_TAG）：bench/ 下的 input/calc 副本按批唯一，防多批互踩
BENCH_TAG=${FXCORR_BENCH_TAG:-}
if [ -n "$BENCH_TAG" ]; then
	BENCH_IN="$(basename "$CFGIN" .input)-${BENCH_TAG}.input"
	CALC_OUT="${CALCBASE%.calc}-${BENCH_TAG}.calc"
else
	BENCH_IN=$(basename "$CFGIN")
	CALC_OUT=$CALCBASE
fi
# 批级 START 覆盖（FXCORR_BENCH_START_MJD 非空；缺省空 = 原行为）
SMJD=""
SSEC=""
if [ -n "${FXCORR_BENCH_START_MJD:-}" ]; then
	read -r SMJD SSEC < <(awk -v m="$FXCORR_BENCH_START_MJD" \
		'BEGIN{ i=int(m); printf "%d %.6f\n", i, (m-i)*86400.0 }')
fi
SEDARGS=(-e "s|^EXECUTE TIME (SEC):[[:space:]]*[0-9]*$|EXECUTE TIME (SEC): $EXEC|" \
    -e "s|^OUTPUT FILENAME:[[:space:]]*.*$|OUTPUT FILENAME:    $OUTDIR|" \
    -e "s|^CALC FILENAME:[[:space:]]*.*$|CALC FILENAME:      $WORKDIR/bench/$CALC_OUT|" \
    -e "s|^\(FILE [0-9]*/[0-9]*:[[:space:]]*\)\([^/[:space:]].*\)$|\1$FXCORR_ROOT_RAW/\2|")
if [ -n "$SMJD" ]; then
	SEDARGS+=(-e "s|^START MJD:[[:space:]]*.*\$|START MJD:          $SMJD|" \
	          -e "s|^START SECONDS:[[:space:]]*.*\$|START SECONDS:      $SSEC|")
fi
sed "${SEDARGS[@]}" "$WORKDIR/$CFGIN" > "$WORKDIR/bench/$BENCH_IN"
# 捕获组用 `[^/[:space:]]` 而不是 `[^/]`：BRE 的 `[[:space:]]*` 会**回溯**——当值本来是
# 绝对路径（`/work2/...`）时，`[[:space:]]*` 少匹配一个空格、让 `[^/]` 去吃那个空格，
# 于是"相对路径才拼 $CFGDIR"的意图失效，拼成 `$CFGDIR/ /work2/...`（中间一个空格），
# mpifxcorr 报 `FATAL Error opening IM file .../config/ /work2/...` 而**不退出**，继续用
# 空模型算完——结果看着像"跑通了"，其实 UVW 全错。把空白也排除出捕获组就没有回溯余地。
# （2026-09-30 实测踩到：2 站的 .calc 没经过 wrap_difxcalc.sh 规范化，值仍是绝对路径。）
# 值**已经是**绝对路径时这两条 sed 不匹配、原样保留，正是想要的：CALC/IM 的绝对路径由
# 各自的根规则处理，与这里的拼接是两件事。
sed -e "s|^IM FILENAME:[[:space:]]*\([^/[:space:]].*\)$|IM FILENAME:        $CFGDIR/\1|" \
    -e "s|^FLAG FILENAME:[[:space:]]*\([^/[:space:]].*\)$|FLAG FILENAME:      $CFGDIR/\1|" \
    "$CFGDIR/$CALCBASE" > "$WORKDIR/bench/$CALC_OUT"
rm -rf "$OUTDIR"    # 幂等重跑
mkdir -p "$OUTDIR"

# ---- ③b 多批展开（FXCORR_BENCH_NBATCH > 1） ----
# 每个 ds 的 DATA TABLE 从 1 条 FILE 行展开成 M 条：第 0 条保留原（软链）路径，
# 追加批 k 的真实文件路径。命名模板从软链 target 取（make_testdata.sh 规则：
# raw/<station>/<station>_<batch_id>[_ds<N>].vdif，链接与目标同在 raw/ 树内、
# target 是相对路径），逐条校验存在。批号连续性与参数一致性已在 ② 挡过。
# 顺序即批序：mpifxcorr 按 FILE 行下标顺序推进读（fileindex >= confignumfiles
# 才停），所以数组次序必须与时间次序一致。
if [ "$NBATCH_BENCH" -gt 1 ]; then
	python3 - "$WORKDIR/bench/$BENCH_IN" "$BID" "$NBATCH_BENCH" <<'PYEOF'
import os, re, sys

path, bid, nbatch = sys.argv[1], sys.argv[2], int(sys.argv[3])
out = []
nseen = 0
for line in open(path).read().split('\n'):
    m = re.match(r'^D/STREAM (\d+) FILES:\s*(\d+)\s*$', line)
    if m:
        if int(m.group(2)) != 1:
            sys.exit('run_bench.sh: D/STREAM %s FILES is %s, expected 1 as the '
                     'multi-batch expansion template' % (m.group(1), m.group(2)))
        out.append('%s%d' % (line[:m.start(2)], nbatch))
        continue
    m = re.match(r'^FILE (\d+)/(\d+):\s*(\S+)\s*$', line)
    if m:
        i, j, fn = int(m.group(1)), int(m.group(2)), m.group(3)
        if j != 0:
            sys.exit('run_bench.sh: unexpected FILE index %d/%d before expansion'
                     % (i, j))
        out.append(line)
        if not os.path.islink(fn):
            sys.exit('run_bench.sh: FILE %s is not a symlink (make_testdata.sh '
                     'layout expected)' % fn)
        rel = os.readlink(fn)
        if not os.path.isabs(rel):
            rel = os.path.join(os.path.dirname(fn), rel)
        base = os.path.basename(rel)
        mm = re.match(r'^(.*)_\d{8}((_ds\d+)?\.vdif)$', base)
        if not mm:
            sys.exit('run_bench.sh: cannot derive the batch pattern from %s' % base)
        for k in range(1, nbatch):
            nfn = os.path.join(os.path.dirname(rel), '%s_%08d%s'
                               % (mm.group(1), int(bid) + k, mm.group(2)))
            if not os.path.exists(nfn):
                sys.exit('run_bench.sh: multi-batch file %s not found' % nfn)
            out.append('%-20s%s' % ('FILE %d/%d:' % (i, k), nfn))
        nseen += 1
        continue
    out.append(line)
if nseen == 0:
    sys.exit('run_bench.sh: no FILE lines to expand (not a DATA TABLE?)')
open(path, 'w').write('\n'.join(out))
PYEOF
fi

# ---- ④ mpirun mpifxcorr（含内存峰值与跨节点流量采集） ----
# DATA TABLE 文件名为相对路径（软链在 workdir 根），故 cwd 在 workdir 内跑。
# mpifxcorr 需要 LD_LIBRARY_PATH 找 libmark5access（install-difx 的 bin/lib 目录）。
cd "$WORKDIR"
export LD_LIBRARY_PATH="${DIFXROOT:-/usr/local/difx}/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
MPIARGS=()
if [ "$(id -u)" = 0 ]; then
	MPIARGS+=(--allow-run-as-root)
fi

# ---- 跨节点映射（V7 P5）----
# **跨节点必须显式指定**：mpirun 默认按 slot 把 rank 填满一台再用下一台，34 rank 会
# 全挤进第一台，跨节点流量根本不发生——测出来的还是"单节点"的结论，而且看不出错。
# FXCORR_MPI_PPN 是最省事的写法：节点列表从 Slurm 分配取，每台加 slot 数，拼成
# mpirun 的 -host 串（`a:17,b:17`，等于 `scontrol show hostnames` 那句的展开）。
if [ -n "${FXCORR_MPI_PPN:-}" ]; then
	if ! [[ ${FXCORR_MPI_PPN} =~ ^[1-9][0-9]*$ ]]; then
		echo "run_bench.sh: FXCORR_MPI_PPN must be a positive integer" >&2
		exit 2
	fi
	[ -n "${SLURM_JOB_NODELIST:-}" ] || {
		echo "run_bench.sh: FXCORR_MPI_PPN needs SLURM_JOB_NODELIST (submit under Slurm, or give FXCORR_MPI_HOSTS)" >&2
		exit 2
	}
	command -v scontrol >/dev/null 2>&1 || { echo "run_bench.sh: scontrol not found (use FXCORR_MPI_HOSTS)" >&2; exit 2; }
	hosts=$(scontrol show hostnames "$SLURM_JOB_NODELIST" | paste -sd,)
	FXCORR_MPI_HOSTS=$(printf '%s' "$hosts" | tr ',' '\n' | sed "s/\$/:$FXCORR_MPI_PPN/" | paste -sd,)
	echo "run_bench.sh: FXCORR_MPI_PPN=$FXCORR_MPI_PPN -> -host $FXCORR_MPI_HOSTS" >&2
fi
if [ -n "${FXCORR_MPI_HOSTS:-}" ]; then
	MPIARGS+=(-host "$FXCORR_MPI_HOSTS")
	# 跨节点要显式转发两个环境变量——**少了哪个都起不来，而 Open MPI 两次报的是同一句
	# "unable to find the specified executable file"，指的方向都是错的**（2026-09-30
	# 在目标集群上逐个复现过）：
	#   PATH            可执行文件写的是**裸名** `mpifxcorr`，本机靠 PATH 找到，远端 orted
	#                   的 PATH 里没有 install/bin——于是"找不到文件本身"
	#   LD_LIBRARY_PATH mpifxcorr 依赖 libfftw3f.so.3.5.7，而那个库**没有 SONAME**
	#                   （build.md 坑 7），只能靠这个变量找到；Open MPI 不转发它，远端一条
	#                   fxcorr/fftw 路径都没有——实测远端 `ldd` 有 7 个 not found、本机 0 个
	# 两次报错都会把人往"共享盘没挂载 / 权限 / 路径拼写"上引，其实都在环境变量上。
	MPIARGS+=(-x PATH)
	MPIARGS+=(-x LD_LIBRARY_PATH)
fi
# rankfile（2026-11-06 补）：a-side.sh 的多节点路径 export FXCORR_MPI_RANKFILE
# （per-host 绑核文件），但本脚本此前只认 FXCORR_MPI_HOSTS——于是 8 节点 A 的
# mpirun 既没有 -host 也没有 --rankfile，225 个 rank 全挤在登录节点上，实测
# 2026-11-05 23:02 起卡死 7 小时（p24-a-n8-2.048）。宿主 HPC-X 只认
# `--rankfile <file>`（Open MPI 4.1.4rc1 拒绝 --map-by rankfile:file=）。
if [ -n "${FXCORR_MPI_RANKFILE:-}" ]; then
	MPIARGS+=(--rankfile "$FXCORR_MPI_RANKFILE")
	MPIARGS+=(-x PATH)
	MPIARGS+=(-x LD_LIBRARY_PATH)
fi
if [ -n "${FXCORR_MPI_MAPBY:-}" ]; then
	MPIARGS+=(--map-by "$FXCORR_MPI_MAPBY")
fi
# rankfile 的 binds 必须单独给 --bind-to；把 :oversubscribe 拼进 --map-by 会让
# OpenMPI 放弃逐 rank 绑定（2026-10-31 h04r4n17 实测：57 rank 全落到整个 cpuset）。
# ds/manager 多个 rank 共用一个 I/O 核，故用 core:overload-allowed。
if [ -n "${FXCORR_MPI_BIND:-}" ]; then
	MPIARGS+=(--bind-to "$FXCORR_MPI_BIND")
fi
# 观测开关（2026-11-06）：打印每个 rank 的**实际**绑定位置，排查 taskset 与
# rankfile 的覆盖顺序（wrapper 模式下 mpirun 绑的是 wrapper，容器内 mpifxcorr
# 继承它的掩码，光看配置看不出谁生效）。
if [ -n "${FXCORR_MPI_REPORT_BINDINGS:-}" ]; then
	MPIARGS+=(--report-bindings)
fi

# --oversubscribe：允许 NP 大于核数。P4 的 4 站配置是**有意**超订的（34 进程 / 30 核，
# v7-plan §11 拿它压边界），不加这个 Open MPI 会以 "not enough slots" 直接拒绝启动。
# 核数够时它不改变行为，所以常开。
# **跨节点时不给**：--oversubscribe 就是 --map-by :OVERSUBSCRIBE，与上面的 -host /
# --map-by 属于同一类映射指令，同时给会争；跨节点的 slot 数已由 host:slots 写明。
if [ "${FXCORR_MPI_OVERSUBSCRIBE:-0}" = 1 ]; then
	MPIARGS+=(--oversubscribe)
elif [ -z "${FXCORR_MPI_HOSTS:-}${FXCORR_MPI_MAPBY:-}${FXCORR_MPI_RANKFILE:-}" ]; then
	MPIARGS+=(--oversubscribe)
fi

# ---- 内存峰值 + 跨节点流量采集（每节点一份）----
# P4 的 Q6：V6 S3 只记了 wall 与 CPU，没记内存峰值，于是"目标节点装不装得下
# mpifxcorr"这一问没法回答。现成手段都不行——`mpirun` 自己报不了子进程内存，
# `/usr/bin/time -v` 量的又只是 launcher 一个进程。所以自己采。采样逻辑（后台按
# /proc 轮询，**先等 mpifxcorr 出现、再等它们全部消失**就退出，不用赌 kill 信号的
# 时机）在 mpi_sampler.py：P4 时它内联在这里，P5 抽出去并加了网络计数。
#
# **P5 起每节点起一份**：内联那份只看得到启动节点，跨节点时其余节点的内存与流量
# 全漏掉，而那两个数正是 P5 要的。Slurm 多节点分配下用 srun 分发（--overlap：与
# mpirun 并存、不排队等资源），单节点直接后台跑——两条路走同一个脚本，口径不漂。
SAMPLEDIR="$WORKDIR/bench/.p5-sample.$$"
rm -rf "$SAMPLEDIR"; mkdir -p "$SAMPLEDIR"
# mpirun 失败时 set -e 会直接跳出，采样器与临时目录得由 trap 收——P4 那份内联采集器
# 没有这条，mpirun 报错时它会一直挂到 6 小时兜底才走。
# `|| true`（2026-10-14）：正常路径第 323 行已 kill 过采样器，到这里它多半已死，
# 裸 kill 的"no such process"返回 1，而 set -e 在 EXIT trap 里会把它带成脚本退出码
# （并中止 trap 剩余部分、跳过下面的 rm -rf）——实测"脚本跑完却恒返回 1"的根因。
trap 'rm -f "${OUT:-}"; if [ -n "${SAMPID:-}" ]; then kill "$SAMPID" 2>/dev/null || true; fi; rm -rf "${SAMPLEDIR:-}"' EXIT
NNODES=${SLURM_JOB_NUM_NODES:-1}
if [ "$NNODES" -gt 1 ] && command -v srun >/dev/null 2>&1; then
	srun --nodes="$NNODES" --ntasks-per-node=1 --overlap \
		nice -n -5 python3 "$SCRIPTDIR/mpi_sampler.py" "$SAMPLEDIR" &
	echo "run_bench.sh: sampler dispatched to $NNODES node(s) via srun" >&2
else
	# nice -5（容器内 root 可降）：34 rank / 30 核超订时，采样器（0.2 s 轮询）
	# 会被 CPU 饿死到 mpirun 结束才拿到首个快照——first_seen 失真（2026-10-17 实测
	# route=3：mpi_init=101.35 s = 全程）。降 nice 后恢复可测。
	nice -n -5 python3 "$SCRIPTDIR/mpi_sampler.py" "$SAMPLEDIR" &
fi
SAMPID=$!

# ---- 可选打点（FXCORR_BENCH_EPOCH_FILE 非空；route=3 冷启动量化用）----
# 只写 epoch 行（`<key>,<epoch>`，追加）；env 为空时零行为（不影响既有口径）。
# mpirun 失败时 set -e 直接退出，mpirun_end 缺失可作失败标志。
epoch_mark() {
	# $1=key；$2=显式 epoch（缺省取当前时刻）——sampler 的 first/last seen 必须
	# 传采样值本身（2026-10-17 踩过：只传 key 时写成了"解析时刻"= mpirun 结束后，
	# 误判 sampler 失效）
	[ -n "${FXCORR_BENCH_EPOCH_FILE:-}" ] || return 0
	local t=${2:-$(date +%s.%N)}
	printf '%s,%s\n' "$1" "$t" >> "$FXCORR_BENCH_EPOCH_FILE" 2>/dev/null || true
}
epoch_mark mpirun_start
# FXCORR_MPI_TARGET：mpirun 的目标（缺省 mpifxcorr；route=3 用 rank wrapper
# 记录各 rank 进程启动时刻——rank 自身有 CPU 份额，不受采样器被 throttle 影响）
mpirun "${MPIARGS[@]}" -np "${NP:-4}" "${FXCORR_MPI_TARGET:-mpifxcorr}" "bench/$BENCH_IN"
epoch_mark mpirun_end

kill "$SAMPID" 2>/dev/null || true
wait "$SAMPID" 2>/dev/null || true
# 诊断：保留采样文件（FXCORR_BENCH_KEEP_SAMPLES 给目录）
if [ -n "${FXCORR_BENCH_KEEP_SAMPLES:-}" ]; then
	mkdir -p "$FXCORR_BENCH_KEEP_SAMPLES"
	cp -a "$SAMPLEDIR"/*.txt "$FXCORR_BENCH_KEEP_SAMPLES"/ 2>/dev/null || true
fi

# 汇总：每节点一行 + 合计。**别只看启动节点**——P4 那份内联采集器的盲区就在这儿。
SAMPLES=("$SAMPLEDIR"/*.txt)
if [ -s "${SAMPLES[0]:-/nonexistent}" ]; then
	TOT_NRANK=0 TOT_RSS=0 TOT_HWM=0 TOT_RX=0 TOT_TX=0
	for f in "${SAMPLES[@]}"; do
		HOST= PEAK_PROC_MB=0 PEAK_SUM_RSS_MB=0 SUM_HWM_MB=0 NRANK=0 NET_RX_MB=0 NET_TX_MB=0
		first_seen= last_seen=
		while IFS='=' read -r k v; do
			case "$k" in
				HOST)            HOST=$v ;;
				PEAK_PROC_MB)    PEAK_PROC_MB=$v ;;
				PEAK_SUM_RSS_MB) PEAK_SUM_RSS_MB=$v ;;
				SUM_HWM_MB)      SUM_HWM_MB=$v ;;
				NRANK)           NRANK=$v ;;
				NET_RX_MB)       NET_RX_MB=$v ;;
				NET_TX_MB)       NET_TX_MB=$v ;;
				FIRST_SEEN)      first_seen=$v ;;
				LAST_SEEN)       last_seen=$v ;;
			esac
		done < "$f"
		[ -n "${first_seen:-}" ] && epoch_mark "mpifxcorr_first_seen_${HOST}" "$first_seen"
		[ -n "${last_seen:-}" ] && epoch_mark "mpifxcorr_last_seen_${HOST}" "$last_seen"
		echo "run_bench.sh: $HOST: $NRANK rank(s), single rank $PEAK_PROC_MB MB," \
		     "concurrent sum $PEAK_SUM_RSS_MB MB, net rx $NET_RX_MB MB / tx $NET_TX_MB MB" \
		     "first_seen=${first_seen:-?} last_seen=${last_seen:-?}"
		TOT_NRANK=$((TOT_NRANK + NRANK))
		TOT_RSS=$((TOT_RSS + PEAK_SUM_RSS_MB))
		TOT_HWM=$((TOT_HWM + SUM_HWM_MB))
		TOT_RX=$((TOT_RX + NET_RX_MB))
		TOT_TX=$((TOT_TX + NET_TX_MB))
	done
	if [ "${#SAMPLES[@]}" -gt 1 ]; then
		echo "run_bench.sh: totals over ${#SAMPLES[@]} node(s): $TOT_NRANK rank(s)," \
		     "concurrent sum $TOT_RSS MB (per-rank peaks summed $TOT_HWM MB)," \
		     "net rx $TOT_RX MB / tx $TOT_TX MB"
		echo "run_bench.sh:   跨节点流量取 Σtx 或 Σrx（同一份数据的两端），不是两者之和；" \
		     "共享盘的读写也在这两个数里，要的是与 fxcorr 侧之差"
	fi
else
	# mpirun 压根没起来（找不到 mpifxcorr、参数错等）：采样数据无从谈起，但不是
	# 本脚本的失败——真正的失败 mpirun 已经用非零退出码报了
	echo "run_bench.sh: sampler produced no data (mpifxcorr never started?)" >&2
fi
rm -rf "$SAMPLEDIR"

# ---- ④½ 截掉 EXECUTE TIME 多留出的第 N+1 个积分 ----
# ② 多留一个整秒的代价：mpifxcorr 会多写一个积分（起终点都在 batch 窗口之外，fxcorr
# 侧不产出）。按 sec 分组保留前 NINT 组、把尾部多余记录从基准文件里截掉，使两侧记录数
# 一致，cmp_swin.py 无需 maxrecords 参数。无多写时不动文件。
# NINT = 单批 nint（M=1）或 M×nint（多批，② 输出）——分组数即积分数，天然按批扩展。
python3 - "$OUTDIR" "$NINT" "$NCHAN" <<'PYEOF'
import glob, os, struct, sys

outdir, nint, nchan = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
rec = 74 + nchan * 8          # SWIN 记录长度（74 字节头 + nchan 复数）
SECOFF = 16                   # 头内 sec 字段偏移（2I sync/ver + 2i bl/mjd 之后）
for path in sorted(glob.glob(os.path.join(outdir, 'DIFX_*'))):
    size = os.path.getsize(path)
    nrec = size // rec
    if nrec * rec != size:
        sys.exit('run_bench.sh: %s size %d not a multiple of record size %d'
                 % (path, size, rec))
    with open(path, 'rb') as f:
        data = f.read()
    keep, seen, prev = nrec, 0, None
    for i in range(nrec):
        sec = struct.unpack_from('<d', data, i * rec + SECOFF)[0]
        if prev is None or abs(sec - prev) > 1e-9:
            seen += 1
            if seen > nint:
                keep = i
                break
            prev = sec
    if keep < nrec:
        with open(path, 'r+b') as f:
            f.truncate(keep * rec)
        print('run_bench.sh: %s: %d -> %d records (dropped %d integration(s) outside '
              'the batch window)' % (os.path.basename(path), nrec, keep, seen - nint))
PYEOF

# ---- ⑤ 对拍提示 ----
# 两侧 SWIN 逐基线文件对拍：fxcorr 侧由 run_batch.sh 产出（.input OUTPUT FILENAME
# 目录），基准侧在本脚本的 bench/<exp>.difx/。2 站 1 基线为单文件
# DIFX_<mjd>_<6 位秒>.s0000.b0000。
echo "run_bench.sh: batch $BID, bench/$EXP.difx holds $NINT integration(s) (EXECUTE TIME $EXEC)"
echo "对拍（run_batch.sh 产出 fxcorr 侧 SWIN 后，逐基线文件）："
echo "  python3 $SCRIPTDIR/test/cmp_swin.py <fxcorr SWIN>/DIFX_*.s0000.b0000 bench/$EXP.difx/DIFX_*.s0000.b0000 $NCHAN"
