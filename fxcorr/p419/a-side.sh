#!/usr/bin/env bash
# a-side.sh —— p419 上启动原生 mpifxcorr（方案 A）的编排脚本
#
# 【这是什么】按 N 节点把 NP = 33 + 24N 个 rank 分到各节点，每节点 28 核预算
# （24 计算 + 4 I/O）。单节点走容器内 mpirun；多节点走宿主 HPC-X mpirun +
# 每 rank 一个容器的 wrapper。生成 per-host rankfile 让 32 个 datastream rank
# 按节点均分，并靠 --mca mpi_yield_when_idle 1 化解超订忙等。
#
# 【配套】run_bench.sh（同目录）负责拼 MPIARGS 并启动 mpirun。
# 完整说明（环境、设计、操作、实测、排查）见同目录 mpifxcorr-calibration.md。
#
# 【用法】（在 p419-n0，或 Slurm 作业首节点上）
#   NODES="h1 h2 h3" ./a-side.sh <N_core> <线程/核> <批数> [标签]
#   - 不给 NODES：从 SLURM_JOB_NODELIST 自动展开（scontrol show hostnames）
#   - NP = 33 + N_core；**每节点 rank 上限由 A_MAX_RANK_PER_NODE 控制（默认 30，新 A 用 57）**（超订远端 orted 崩，S2 教训）——
#     超配直接拒绝（提示减 N_core 或加节点）
#   - 例（16 节点）：NODES="..." ./a-side.sh 240 1 160 n16-nc240t1
#
# 【跑法】N≥1 默认都走宿主 HPC-X mpirun + ~/bench-wrap/bin/mpifxcorr 包装（每
#   rank 进容器；不带 --cleanenv，PMIx 透传）——wrapper 缺失时本脚本自动建，并
#   预 ssh 全部节点（采 host key）。2026-11-08 起单节点也默认这条：只有它显式设
#   FXCORR_MPI_EXTRA（含 mpi_yield_when_idle），yield 来源统一可查。容器内
#   mpirun 留作显式回退 A_MPI_LAUNCH=inproc——该路径**不设** yield，仅供诊断
#   对照，结果不与主线口径混用。
#
# 【⚠ 环境前提】① 作业独占（网络计数器整节点口径）；② 计算节点可见 /work2 与
#   /public/home（.sif 与数据都在共享盘）；③ A/B 不得同物理节点并行；④ 跑前无
#   残留 mpifxcorr（脚本会查）；⑤ .threads 被本脚本覆盖（备份在 .bak-16）。
#
# 【判据】退出码 0 + 记录数 = 批数×1024 + SWIN w 非垃圾（交付前人工用
#   cmp_swin_tol.py 对 B 侧参照，见 README）。结果行追加 a-logs/a-side.csv。
set -euo pipefail

# 脚本启动时刻（2026-11-08）：与采样器的 FIRST_SEEN 相减得 startup_s，量化
# 启动波动（历史记录 1 min ~ 27 min 不等，此前没有任何字段可查）。
T_START=$(date +%s.%N)

NC=${1:?用法：NODES="..." ./a-side.sh <N_core> <线程/核> <批数> [标签]}
NTHR=${2:?线程/核}
NB=${3:?批数}
TAG=${4:-nc${NC}t${NTHR}-nb${NB}}
W=${W:-/work2/cstu0036/mydata/fxcorr/workdir-b0512ms}
# 账号相关路径（2026-11-08）：全部由 $HOME 派生，各账号直接可用。p419-n0
# （cstu0030）与 p419-n1（cstu0036）是同一台登录机的两个账号，两边的
# repo/镜像/wrapper 各在自己 home 下；home 之间是 700，不可互相读。
# 前缀 A_ 的变量可在环境里覆盖，用于临时换账号或换镜像。
HOME_DIR=${A_HOME_DIR:-$HOME}
REPO=${A_REPO:-$HOME_DIR/fxcorr/fxcorr}
SIF=${A_SIF:-$HOME_DIR/singularity/fxcorr/fxcorr.sif}
BENCHWRAP=${A_BENCHWRAP:-$HOME_DIR/bench-wrap-26/bin}
# 站点级共享路径（与账号无关，一般不用改）
SING=${A_SING:-/public/software/singularity/singularity-3.8.0/bin/singularity}
HPCX=${A_HPCX:-/opt/hpc/software/mpi/hpcx/v2.11.0/gcc-7.3.1/bin}
PY3=${A_PY3:-/public/software/apps/miniforge3-25.3.1-0/bin/python3}
CPUSET=${CPUSET:-1-22,24-29}
A_IO_CPUSET=${A_IO_CPUSET:-1-4}
A_CORE_CPUSET=${A_CORE_CPUSET:-5-22,24-29}
# 节点间 ssh 端口（2026-10-18）：h03r3n07 仅 50022 可用（22 拒连），其余节点都通。
SSH_PORT=${SSH_PORT:-50022}
# 启动路径（2026-11-08）：默认宿主 mpirun + 每 rank 一个容器。容器内 mpirun 只是
# 显式回退（A_MPI_LAUNCH=inproc）——那条路径不设 FXCORR_MPI_EXTRA，缺
# mpi_yield_when_idle，仅供诊断对照，结果不与主线口径混用。
A_MPI_LAUNCH=${A_MPI_LAUNCH:-host}
# A_LOGDIR 可覆盖：本地 dry-run 用临时目录，生产保持共享盘路径
LOGDIR=${A_LOGDIR:-/work2/cstu0036/mydata/fxcorr/a-logs}
NP=$((33 + NC))
log=$LOGDIR/a-$TAG.log
mkdir -p "$LOGDIR"

# ---- 节点列表：NODES 环境变量优先，否则从 Slurm 取 ----
if [ -z "${NODES:-}" ]; then
	if [ -n "${SLURM_JOB_NODELIST:-}" ] && command -v scontrol >/dev/null 2>&1; then
		NODES=$(scontrol show hostnames "$SLURM_JOB_NODELIST" | tr '\n' ' ')
	else
		echo "a-side.sh: 必须给 NODES=\"h1 h2 ...\"（或 SLURM_JOB_NODELIST 环境）" >&2
		exit 2
	fi
fi
# tr -d：BSD 的 wc 会给数字补前导空格，GNU 的不补
NNODES=$(printf '%s\n' $NODES | wc -l | tr -d '[:space:]')

# ---- 版本一致性（2026-11-08）----
# 远端 run_bench.sh 3116977c（读 FXCORR_MPI_EXTRA）与本地 b62a9cc7（不读）
# 曾各自漂移，A 的 wall 因此翻 2.1× 而无人察觉。run_bench.sh 头部带
# RUN_BENCH_REV，对不上就告警并把实际版本落盘——不阻断，历史快照可能故意用旧版。
A_RUN_BENCH_REV=${A_RUN_BENCH_REV:-20261107-yield}
# `|| true`：pipefail 下 run_bench.sh 不存在时 sed|head 返回非零，会触发 set -e
rb_rev=$(sed -n 's/^RUN_BENCH_REV=//p' "$REPO/run_bench.sh" 2>/dev/null | head -1 || true)
{
	echo "run_bench.sh=$REPO/run_bench.sh"
	echo "RUN_BENCH_REV=${rb_rev:-<none>}"
	echo "expected=$A_RUN_BENCH_REV"
	echo "a-side.sh md5=$(md5sum "$REPO/p419/a-side.sh" 2>/dev/null | awk '{print $1}')"
	echo "run_bench.sh md5=$(md5sum "$REPO/run_bench.sh" 2>/dev/null | awk '{print $1}')"
} > "$LOGDIR/rev-$TAG.txt"
if [ -z "$rb_rev" ]; then
	echo "a-side.sh: ! $REPO/run_bench.sh 无 RUN_BENCH_REV（旧版，可能不读 FXCORR_MPI_EXTRA）——见 $LOGDIR/rev-$TAG.txt" >&2
elif [ "$rb_rev" != "$A_RUN_BENCH_REV" ]; then
	echo "a-side.sh: ! run_bench.sh rev=$rb_rev ≠ 期望 $A_RUN_BENCH_REV——见 $LOGDIR/rev-$TAG.txt" >&2
fi
per=$((NP / NNODES)); r=$((NP % NNODES))
MAXPER=$((per + (r > 0 ? 1 : 0)))
[ "$MAXPER" -le "${A_MAX_RANK_PER_NODE:-30}" ] || {
	echo "a-side.sh: NP=$NP 均摊 $NNODES 节点 = 每节点最多 $MAXPER rank > ${A_MAX_RANK_PER_NODE:-30}（超订崩，S2 实测）" >&2
	echo "  处置：减 N_core（≤ $((${A_MAX_RANK_PER_NODE:-30} * NNODES - 33))）或加节点" >&2
	exit 2
}
# 变量一律加花括号：紧跟全角标点时 $_VAR 的边界在非 UTF-8 locale 下会误判
echo "== A 侧：NP=${NP}（33 + ${NC} core）× ${NTHR} 线程；${NB} 批；${NNODES} 节点 =="
echo "   节点：${NODES}（每节点 rank ≤ ${MAXPER}）"

# ---- 前置检查 ----
[ -d "$W/config" ] && [ -d "$W/raw" ] || { echo "a-side.sh: $W 结构不完整" >&2; exit 1; }
echo "$SIF" | grep -q . && [ -f "$SIF" ] || { echo "a-side.sh: 找不到 $SIF" >&2; exit 1; }
[ -f "$REPO/run_bench.sh" ] || { echo "a-side.sh: 缺 $REPO/run_bench.sh" >&2; exit 1; }
if [ "$NNODES" -ge 2 ] || [ "$A_MPI_LAUNCH" = host ]; then
	[ -x "$HPCX/mpirun" ] || { echo "a-side.sh: 找不到 $HPCX/mpirun" >&2; exit 1; }
fi
n=$(pgrep -c -x mpifxcorr 2>/dev/null || true); n=${n:-0}
[ "$n" = 0 ] || { echo "a-side.sh: 本机有 $n 个 mpifxcorr——先清理" >&2; exit 1; }

# ---- wrapper（多节点）+ 预 ssh 全节点 ----
# 2026-11-01：wrapper 不再 taskset——绑定改由 mpirun 的 per-host rankfile +
# --bind-to core:overload-allowed 负责；wrapper 里再 taskset 会把逐 rank 绑定
# 覆盖成整个 28 核掩码（这正是旧多节点口径的问题）。每次重写，避免旧 wrapper 残留。
if [ "$NNODES" -ge 2 ] || [ "$A_MPI_LAUNCH" = host ]; then
	mkdir -p "$BENCHWRAP"
	echo "== 写 wrapper $BENCHWRAP/mpifxcorr =="
	cat > "$BENCHWRAP/mpifxcorr" <<EOF
#!/bin/sh
# A 侧跨节点 rank 包装（S2/S3）：宿主 mpirun → 每 rank 一个容器版 mpifxcorr
# （R12 性能口径：正式对照一律容器版；不要加 --cleanenv，PMIx 变量须透传）
# 绑定由 mpirun 的 rankfile 给出，这里不做 taskset。
exec $SING exec --bind $HOME_DIR --bind /work2 $SIF /usr/local/difx/bin/mpifxcorr "\$@"
EOF
	chmod +x "$BENCHWRAP/mpifxcorr"
fi
for h in $NODES; do
	ok=
	for try in 1 2 3; do
		if timeout 15 ssh -p "$SSH_PORT" -o BatchMode=yes -o ConnectTimeout=8 "$h" true </dev/null 2>/dev/null; then ok=1; break; fi
		sleep 3
	done
	[ -n "$ok" ] || { echo "a-side.sh: 节点 $h 不可达（3 次重试）——EXCLUDE 或换节点" >&2; exit 1; }
	# ⚠ 所有 ssh 必须带 timeout：2026-10-16 晚 p419-n0→计算节点 ssh 间歇抖动，
	#   无超时的 ssh 连接建立后挂死致脚本卡 27 分钟（踩过）
	n=$(timeout 15 ssh -p "$SSH_PORT" -o BatchMode=yes -o ConnectTimeout=8 "$h" 'pgrep -c -x mpifxcorr 2>/dev/null || echo 0' </dev/null | tail -1)
	[ "${n:-0}" = 0 ] || { echo "a-side.sh: 节点 $h 有 $n 个 mpifxcorr——先清理" >&2; exit 1; }
done

# ---- .threads（N_core 行 × NTHR）+ roots + 软链批 1（R10）----
{ printf "NUMBER OF CORES:    %d\n" "$NC"; for ((i=0;i<NC;i++)); do echo "$NTHR"; done; } \
	> "$W/config/test_1.threads"
echo "   .threads：$(head -1 "$W/config/test_1.threads")（$NC 行 × $NTHR 线程）"
bash "$REPO/set_roots.sh" "$W" >/dev/null
(
	cd "$W/raw"
	for d in */; do
		st=${d%/}
		for f in "$st"/${st}_00000001*.vdif; do
			[ -e "$f" ] || continue
			base=$(basename "$f")
			sfx=${base#"${st}"_00000001}
			ln -sf "$st/$base" "${st}${sfx}"
		done
	done
)

# ---- rankfile：manager+ds → I/O 核，core → 计算核 ----
# 2026-11-01：N≥2 也生成 per-host rankfile——manager+32 ds 落在第 1 台的 I/O 核，
# 每台 24 个 core rank 落在本机计算核；rank 数不再"均摊"（那是旧口径，
# 会把 core rank 挤到第 2 台、manager/ds 与 core 混在一起）。
A_RANKFILE=""
DS_ACTIVE=32
if [ -n "$A_IO_CPUSET" ] && [ -n "$A_CORE_CPUSET" ]; then
	# 注意：p419 节点上的 /usr/bin/python3 实际是 Python 2.7.5，用 $PY3（miniforge）
	base=$("$PY3" - "$W" <<'PY'
import json, os, sys
w = sys.argv[1]
j = os.path.join(w, 'batches', '00000001.json')
try:
    print(json.load(open(j))['config_file'].rsplit('/', 1)[-1].rsplit('.', 1)[0])
except Exception:
    print('test_1')
PY
)
	input="$W/config/${base}.input"
	[ -f "$input" ] || input="$W/config/test_1.input"
	DS=$(awk -F: '/^[ \t]*ACTIVE DATASTREAMS/ {gsub(/[^0-9]/,"",$2); print $2; exit}' "$input")
	[ -n "$DS" ] || DS=32
	DS_ACTIVE=$DS
	RFD="$LOGDIR/rankfiles"; mkdir -p "$RFD"
	A_RANKFILE="$RFD/rf_$(printf '%s' "$TAG" | tr -c 'A-Za-z0-9_' '_')"
	"$PY3" - "$A_RANKFILE" "$DS" "$NP" "$A_IO_CPUSET" "$A_CORE_CPUSET" "$NODES" <<'PY'
import sys
rf, ds, np = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
io_s, core_s, hosts_s = sys.argv[4], sys.argv[5], sys.argv[6]
hosts = hosts_s.split()
def expand(s):
    out = []
    for part in s.split(','):
        part = part.strip()
        if not part:
            continue
        if '-' in part:
            a, b = part.split('-', 1)
            out.extend(range(int(a), int(b) + 1))
        else:
            out.append(int(part))
    return out
io = expand(io_s); core = expand(core_s)
if not io or not core:
    sys.exit('empty cpuset: io=%r core=%r' % (io_s, core_s))
if not hosts:
    sys.exit('empty NODES')
ncore = np - (ds + 1)
if ncore <= 0:
    sys.exit('np=%d is smaller than ds+1=%d' % (np, ds + 1))
lines = []
# rank 0 = manager 留在第 1 台；rank 1..ds 按节点**均分**（2026-11-01 改）。
# 全压在第 1 台时：那台的 4 个 I/O 核扛 33 个 rank（8:1），其余节点的 I/O 核空转，
# 而每个 ds 都要发给所有 core——跨节点字节数是常量，但发送侧 CPU 全集中在头节点。
nhost = len(hosts)
lines.append('rank 0=%s slot=%d' % (hosts[0], io[0]))
per_ds, rem_ds = divmod(ds, nhost)
r = 1
for i, h in enumerate(hosts):
    k = per_ds + (1 if i < rem_ds else 0)
    for j in range(k):
        lines.append('rank %d=%s slot=%d' % (r, h, io[j % len(io)]))
        r += 1
# core rank：按节点顺序分片，每片在本节点的计算核上轮转
per, rem = divmod(ncore, len(hosts))
r = ds + 1
for i, h in enumerate(hosts):
    k = per + (1 if i < rem else 0)
    for j in range(k):
        lines.append('rank %d=%s slot=%d' % (r, h, core[j % len(core)]))
        r += 1
open(rf, 'w').write('\n'.join(lines) + '\n')
PY
	echo "   rankfile: $A_RANKFILE（manager+ds→${NODES%% *} 的 $A_IO_CPUSET；core→各节点 $A_CORE_CPUSET；DS=$DS NP=$NP）"
fi

# ---- I/O 超订比（2026-11-08）：**每节点** ds 数 / 每节点 I/O 核数。
# 单节点 32/4=8、8 节点 (32/8)/4=1。A_IO_CPUSET 是每节点的核号，ds 是全集，
# 所以要先按节点数摊。写进结果表，跨节点对照时用来说明超订程度——wall 基本
# 不随它变（a.md §5）。
io_n=0
if [ -n "$A_IO_CPUSET" ]; then
	io_n=$(awk -v s="$A_IO_CPUSET" 'BEGIN{
		n=0; m=split(s,a,",")
		for(i=1;i<=m;i++){
			if(index(a[i],"-")){split(a[i],b,"-"); n+=b[2]-b[1]+1}
			else n++
		}
		print n}')
fi
if [ "${io_n:-0}" -gt 0 ] && [ "${NNODES:-0}" -gt 0 ]; then
	io_ratio=$(awk -v d="$DS_ACTIVE" -v n="$io_n" -v h="$NNODES" \
		'BEGIN{printf "%.2f", d/(n*h)}')
else
	io_ratio=0
fi

# ---- 采样器（每节点一份，共享盘输出）----
SAMPLEDIR=$LOGDIR/sample-$TAG-$(date +%s)
mkdir -p "$SAMPLEDIR"
for h in $NODES; do
	timeout 15 ssh -p "$SSH_PORT" -o BatchMode=yes -o ConnectTimeout=8 "$h" \
		"nohup $PY3 $REPO/mpi_sampler.py $SAMPLEDIR >/dev/null 2>&1 & echo $h-ok" </dev/null
done
sleep 2

# ---- 跑 ----
# 2026-11-08 起默认走宿主路径（A_MPI_LAUNCH=host），单节点亦然：只有这条路显式
# 设 FXCORR_MPI_EXTRA（含 mpi_yield_when_idle），yield 来源唯一。A_MPI_LAUNCH=
# inproc 保留为显式回退——容器内 mpirun 不设 EXTRA，缺 yield，仅供诊断对照。
use_host=0
if [ "$NNODES" -ge 2 ] || [ "$A_MPI_LAUNCH" = host ]; then use_host=1; fi

# 命令行落盘 + 运行时校验（2026-11-08）：run_bench.sh 把最终 mpirun 命令行写到
# CMDLOG，并在启动 3 s 后按 EXPECT_ARGS 核对实际进程命令行，缺项就 kill。
# 期望值与下面两条路径各自的 export 一一对应，不写死。
METRICS=$LOGDIR/metrics-$TAG.env
rm -f "$METRICS"
CMDLOG=$LOGDIR/cmd-$TAG.sh
expect_args=
if [ "$use_host" = 1 ]; then
	if [ -n "$A_RANKFILE" ]; then
		expect_args="mpi_yield_when_idle --rankfile ${A_BIND:-core:overload-allowed}"
	else
		# 无 rankfile 时走 -host 分支，FXCORR_MPI_BIND 不设，命令行没有 --bind-to
		expect_args="mpi_yield_when_idle"
	fi
elif [ -n "$A_RANKFILE" ]; then
	expect_args="rankfile:file= core:overload-allowed"
fi

if [ "$NNODES" -eq 1 ] && [ "$use_host" = 0 ]; then
	echo "== 容器内 mpirun（单节点）——日志 $log =="
	echo "a-side.sh: ! inproc 路径不设 FXCORR_MPI_EXTRA（缺 mpi_yield_when_idle）——仅供诊断对照" >&2
	set +e
	if [ -n "$CPUSET" ]; then TASKSET=(taskset -c "$CPUSET"); else TASKSET=(); fi
	ENV_CMD="export FXCORR_BENCH_CMDLOG='$CMDLOG' FXCORR_BENCH_METRICS='$METRICS' FXCORR_BENCH_EXPECT_ARGS='$expect_args' && "
	if [ -n "$A_RANKFILE" ]; then
		# rankfile 只给 --map-by；绑定与超订必须分开给。把 :oversubscribe 拼进
		# --map-by 会让 OpenMPI 放弃逐 rank 绑定，所有 rank 落到整个 cpuset。
		RF_CMD="cp '$A_RANKFILE' /tmp/rf && export FXCORR_MPI_MAPBY='rankfile:file=/tmp/rf' && export FXCORR_MPI_BIND='core:overload-allowed' && export FXCORR_MPI_OVERSUBSCRIBE=1 && "
	else
		RF_CMD=""
	fi
	"${TASKSET[@]}" "$SING" exec --cleanenv --bind "$HOME_DIR" --bind /work2 "$SIF" \
		bash -c "${ENV_CMD}${RF_CMD}cd $REPO && time FXCORR_BENCH_NBATCH=$NB NP=$NP bash run_bench.sh $W" \
		</dev/null 2>&1 | tee "$log"
	rc=${PIPESTATUS[0]}
	set -e
else
	HOSTS=""
	if [ -n "$A_RANKFILE" ]; then
		echo "== 宿主 mpirun + wrapper：per-host rankfile $A_RANKFILE——日志 $log =="
	else
		HOSTS=""; i=0
		for h in $NODES; do
			n=$per
			if [ "$i" -lt "$r" ]; then n=$((per + 1)); fi
			HOSTS="${HOSTS:+$HOSTS,}$h:$n"
			i=$((i + 1))
		done
		[ -n "${A_HOSTS:-}" ] && HOSTS="$A_HOSTS"
		echo "== 宿主 mpirun + wrapper：-host $HOSTS——日志 $log =="
	fi
	set +e
	(
		cd "$REPO"
		# 宿主 /usr/bin/python3 是 2.7.5，run_bench.sh 会直接拒跑；前置 miniforge
		export PATH="$(dirname "$PY3"):$BENCHWRAP:$HPCX:$PATH"
		# 传输层实验开关：A_EXTRA_ENV="K=V K=V"（空格分隔），如
		# A_EXTRA_ENV="OMPI_MCA_coll_hcoll_enable=0"
		for kv in ${A_EXTRA_ENV:-}; do export "$kv"; done
		export A_CPUSET="$CPUSET"
		export CPUSET="$CPUSET"
		export NP=$NP
		export FXCORR_BENCH_NBATCH=$NB
		export OMPI_MCA_plm_rsh_args="-p $SSH_PORT"
		# 命令行落盘 + 运行时校验（2026-11-08）：见 run_bench.sh 同名段
		export FXCORR_BENCH_CMDLOG="$CMDLOG"
		export FXCORR_BENCH_METRICS="$METRICS"
		export FXCORR_BENCH_EXPECT_ARGS="$expect_args"
		if [ -n "$A_RANKFILE" ]; then
			# 宿主 HPC-X 只认 --rankfile <file>（Open MPI 4.1.4rc1 实测拒绝
			# --map-by rankfile:file=）；容器内单节点路径才用后者。
			export FXCORR_MPI_RANKFILE="$A_RANKFILE"
			export FXCORR_MPI_BIND="${A_BIND:-core:overload-allowed}"
			export FXCORR_MPI_REPORT_BINDINGS=1
			# 每 rank 一个容器时，容器里的 MPI 检测不到同机还有别的 rank，默认
			# 按"独占整核"纯自旋；超订（33 base rank / 4 I/O 核）下互相抢占，
			# 集合通信从 µs 退化成几十 ms（2026-11-01 实测 57 rank：48 ms → 87 µs）。
			export FXCORR_MPI_EXTRA="--mca mpi_yield_when_idle 1 ${A_MPI_EXTRA:-}"
		else
			export FXCORR_MPI_HOSTS="$HOSTS"
			export OMPI_MCA_hwloc_base_binding_policy=none
		fi
		time bash run_bench.sh "$W"
	) </dev/null 2>&1 | tee "$log"
	rc=${PIPESTATUS[0]}
	set -e
fi
[ "$rc" = 0 ] || { echo "a-side.sh: mpifxcorr 失败（rc=$rc）——看 $log" >&2; exit 1; }

# ---- 收采样 ----
for i in 1 2 3 4 5 6; do
	live=$(for h in $NODES; do timeout 15 ssh -p "$SSH_PORT" -o BatchMode=yes -o ConnectTimeout=8 "$h" 'pgrep -fc "mpi_sample[r]\.py" 2>/dev/null || echo 0' </dev/null | tail -1; done | awk '{s+=$1} END{print s+0}')
	if [ "${live:-0}" = 0 ]; then break; fi
	sleep 5
done
for h in $NODES; do
	timeout 15 ssh -p "$SSH_PORT" -o BatchMode=yes -o ConnectTimeout=8 "$h" 'pkill -f "mpi_sample[r]\.py" 2>/dev/null || true' </dev/null || true
done
peak_mb=0; tot_rx=0; tot_tx=0; nn=0; fs_min=0
for f in "$SAMPLEDIR"/*.txt; do
	[ -s "$f" ] || continue
	HOST= PEAK_PROC_MB=0 NET_RX_MB=0 NET_TX_MB=0 NRANK=0 FIRST_SEEN=0
	while IFS='=' read -r k v; do
		case "$k" in
			HOST) HOST=$v ;; PEAK_PROC_MB) PEAK_PROC_MB=$v ;;
			NET_RX_MB) NET_RX_MB=$v ;; NET_TX_MB) NET_TX_MB=$v ;; NRANK) NRANK=$v ;;
			FIRST_SEEN) FIRST_SEEN=$v ;;
		esac
	done < "$f"
	echo "  $HOST: $NRANK rank(s), single rank $PEAK_PROC_MB MB, net rx $NET_RX_MB / tx $NET_TX_MB MB"
	if [ "${PEAK_PROC_MB%.*}" -gt "$peak_mb" ] 2>/dev/null; then peak_mb=${PEAK_PROC_MB%.*}; fi
	tot_rx=$((tot_rx + ${NET_RX_MB%.*})); tot_tx=$((tot_tx + ${NET_TX_MB%.*})); nn=$((nn + 1))
	# 启动耗时（2026-11-08）：多节点取最早出现的那个，最接近"作业真正开始计算"
	if awk -v a="$FIRST_SEEN" -v b="$fs_min" 'BEGIN{exit !(a>0 && (b==0 || a<b))}'; then
		fs_min=$FIRST_SEEN
	fi
done
[ "$nn" = "$NNODES" ] || echo "⚠ 采样文件 $nn/$NNODES——目录 $SAMPLEDIR" >&2
echo "  合计：$nn 节点，峰值单 rank ${peak_mb} MB，Σrx ${tot_rx} MB / Σtx ${tot_tx} MB"

# ---- 判据：wall / 记录数 ----
wall_s=$(grep -E '^real' "$log" | tail -1 | sed 's/real[[:space:]]*//' \
	| awk -F'[ms]' '{ if (NF>=3) printf "%.1f", $1*60+$2; else printf "%.1f", $1 }' || true)
f=$(ls "$W"/bench/test_1.difx/DIFX_*.s0000.b0000 2>/dev/null | head -1)
[ -n "$f" ] || { echo "a-side.sh: 找不到 SWIN 产物" >&2; exit 1; }
rec=$(( $(stat -c%s "$f") / 1098 )); expect=$(( NB * 1024 ))

# 启动耗时（2026-11-08）：脚本启动 → 采样器首次看到 mpifxcorr。历史记录从
# ~1 min 到 27 min 不等，此前无字段可查，作业时限只能靠经验留余量。
startup_s=NA
if [ "$fs_min" != 0 ]; then
	startup_s=$(awk -v a="$T_START" -v b="$fs_min" 'BEGIN{printf "%.1f", b-a}')
fi
# 运行时参数校验结果（run_bench.sh 写）：1=三要素齐全；0=缺项/未判定
# `|| true`：同 rb_rev，pipefail 下 METRICS 不存在时 sed|tail 返回非零
mpirun_ok=$(sed -n 's/^mpirun_args_ok=//p' "$METRICS" 2>/dev/null | tail -1 || true)
[ -n "$mpirun_ok" ] || mpirun_ok=0

echo "== [$TAG] wall=${wall_s}s，记录 $rec/$expect，startup ${startup_s}s，args_ok=$mpirun_ok =="
[ "$rec" = "$expect" ] || echo "⚠ 记录数 $rec ≠ 期望 $expect（w 哨兵与对拍必查）" >&2
[ "$mpirun_ok" = 1 ] || echo "⚠ mpirun 参数校验未通过——查 $log 与 $METRICS；命令行见 $CMDLOG" >&2
csv=$LOGDIR/a-side.csv
hdr=tag,nnodes,n_core,nthr,np,nb,wall_s,nrec,exp,peak_rank_mb,tot_rx_mb,tot_tx_mb,startup_s,io_rank_ratio,mpirun_args_ok
if [ ! -f "$csv" ]; then
	echo "$hdr" > "$csv"
elif ! head -1 "$csv" | grep -q ',startup_s,'; then
	# 旧表 12 列：末尾追加三列会让新旧行错位，改名留档后重建
	mv "$csv" "$csv.pre-hardening-$(date +%Y%m%d-%H%M%S)"
	echo "$hdr" > "$csv"
	echo "a-side.sh: 旧 a-side.csv 已改名留档（12 列 → 15 列）" >&2
fi
echo "$TAG,$NNODES,$NC,$NTHR,$NP,$NB,$wall_s,$rec,$expect,$peak_mb,$tot_rx,$tot_tx,$startup_s,$io_ratio,$mpirun_ok" >> "$csv"
echo "   结果行已追加 $csv"
echo "下一步（数值判据）：python3 scripts/exp/lib/cmp_swin_tol.py <B 侧 SWIN> $f 128（R13 口径）"
