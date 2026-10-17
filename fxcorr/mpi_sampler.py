#!/usr/bin/env python3
"""mpi_sampler.py —— 单个计算节点上的作业采样器（V7 P5）

由 run_bench.sh 在**每个计算节点**上起一份（多节点 Slurm 分配下用 srun 分发，单节点
直接后台跑），输出 <outdir>/<hostname>.txt。两个数字：

  内存  单 rank VmHWM 峰值 / 同刻 VmRSS 之和 / 各 rank 峰值之和——口径与 P4 逐字相同
        （P4 时这段逻辑内联在 run_bench.sh 里）。跨节点后**每节点各报一份**，别拿单
        节点的数字去推整机。
  网络  mpifxcorr 存续期间本节点对外的收发字节数。**这是 P5 的正题**：fxcorr 侧预期
        ≈0（只有 KB 级 SWIN 走共享盘），mpifxcorr 侧是 F 数据的多对一汇聚。

**怎么读网络这两个数**（2026-09-30 记，别读错）：

  - 计数器是**整节点**的，不是本作业的——所以只在独占节点上可信（P5 申请就是
    `--exclusive`）。
  - **共享存储的读写也会走网络**：两侧都要从 /work2 读 4.21 GB/batch 的 raw，这笔
    流量两边都有，是背景。要看的是**两侧的差**——多出来的那部分才是 MPI 交换。
  - 跨节点流量是**单向的同一份数据**：A 发给 B 的字节既算 A 的 tx、也算 B 的 rx，
    所以 Σtx 与 Σrx 应当接近相等，取其一即为跨节点总量。
  - `/proc/net/dev` 与 IB 端口计数器**可能是同一份流量的两次计数**（跑了 IPoIB 时
    ib0 会同时出现在两边）。两类分列输出，不要去重，读的时候自己判断。

取两个来源的累计值做差，不依赖额外工具（perfquery / iftop 在计算节点上未必有）。
"""

import glob
import os
import signal
import socket
import sys
import time

outdir = sys.argv[1] if len(sys.argv) > 1 else '.'
host = socket.gethostname()
out = os.path.join(outdir, host + '.txt')
# 进程名覆盖（缺省 mpifxcorr；诊断用，如 FXCORR_SAMPLER_PROC=sleep）
PROC_NAME = os.environ.get('FXCORR_SAMPLER_PROC', 'mpifxcorr')

# 采样的轮询间隔：P4 用 0.2 s 跑 13 分钟的作业，抓得住峰值
INTERVAL = 0.2
# 等 mpifxcorr 出现的上限。跨节点时若某个节点没分到 rank（-host 写错、或 NP 小于
# 节点数），这里就会一直等不到——不该让它挂到 6 小时兜底上，那外面看着像"卡住"。
# 自测时用 FXCORR_SAMPLER_WAIT_START 调小（否则干等半小时）。
WAIT_START = int(os.environ.get('FXCORR_SAMPLER_WAIT_START', 1800))
# 兜底：真挂住了也不让采样器永久驻留
DEADLINE = 6 * 3600


def snapshot():
	"""各 mpifxcorr 进程的 (VmHWM, VmRSS)，单位 kB。

	只认进程名恰好是 mpifxcorr 的（/proc/<pid>/status 的 Name 字段）。mpirun 与 orted
	不计：它们不持有 F 数据，算进去会把 launcher 的内存混进"相关器占了多少"。
	"""
	procs = []
	for d in os.listdir('/proc'):
		if not d.isdigit():
			continue
		try:
			with open('/proc/%s/status' % d) as f:
				st = f.read()
		except (IOError, OSError):
			continue	# 采样瞬间进程退出，正常
		# Name 是 /proc/<pid>/status 的**第一个**字段，所以比首行就行。P4 时这行曾经
		# 写成 '\nName:\tmpifxcorr\n'——以为它前面还有别的字段，于是永远匹配不上：采样器
		# 一路空转到 6 小时兜底，外面看起来就是"卡住不返回"（实测踩过）。
		if st.split('\n', 1)[0] != 'Name:\t' + PROC_NAME:
			continue
		hwm = rss = 0
		for line in st.splitlines():
			if line.startswith('VmHWM:'):
				hwm = int(line.split()[1])
			elif line.startswith('VmRSS:'):
				rss = int(line.split()[1])
		procs.append((hwm, rss))
	return procs


def net_snapshot():
	"""{接口: (rx_bytes, tx_bytes)}，含 /proc/net/dev 与 IB 端口计数器。

	两类键前缀不同（dev: / ib:），便于外面判断重叠。
	"""
	c = {}
	try:
		with open('/proc/net/dev') as f:
			for line in f.readlines()[2:]:	# 前两行是表头
				name, rest = line.split(':', 1)
				name = name.strip()
				if name == 'lo':
					continue	# 回环不是跨节点流量
				fld = rest.split()
				# 字段序：rx bytes packets errs drop fifo frame compressed multicast
				#         tx bytes packets ...
				c['dev:' + name] = (int(fld[0]), int(fld[8]))
	except (IOError, OSError, ValueError, IndexError):
		pass
	# IB 端口计数器：单位是 4 字节，不是字节。路径形如
	# /sys/class/infiniband/mlx5_0/ports/1/counters/port_rcv_data
	for path in glob.glob('/sys/class/infiniband/*/ports/*/counters/port_*_data'):
		try:
			with open(path) as f:
				v = int(f.read().strip()) * 4
		except (IOError, OSError, ValueError):
			continue
		parts = path.split('/')
		if len(parts) < 9:
			continue
		rx = parts[-1].startswith('port_rcv')
		key = 'ib:%s.%s.%s' % (parts[4], parts[6], 'rx' if rx else 'tx')
		old = c.get(key, (0, 0))
		c[key] = (v, old[1]) if rx else (old[0], v)
	return c


def net_delta(a, b):
	"""b - a，逐接口；只保留动过的。"""
	d = {}
	for k in set(a) | set(b):
		ra, ta = a.get(k, (0, 0))
		rb, tb = b.get(k, (0, 0))
		rx, tx = rb - ra, tb - ta
		if rx or tx:
			d[k] = (rx, tx)
	return d


peak_proc = peak_sum_rss = peak_sum_hwm = nrank = 0
started = False
net0 = net1 = None
# 打点（route=3 冷启动量化）：mpifxcorr 进程首次出现 / 最后消失的时刻（epoch 秒）。
# 与 run_bench.sh 的 mpirun_start/end 联用 → MPI 建链 = first_seen − mpirun_start。
first_seen = last_seen = None


def report():
	# /proc 的字段是 kB，报 MB（整除，够用）
	lines = [
		'HOST=%s' % host,
		'PEAK_PROC_MB=%d' % (peak_proc // 1024),
		'PEAK_SUM_RSS_MB=%d' % (peak_sum_rss // 1024),
		'SUM_HWM_MB=%d' % (peak_sum_hwm // 1024),
		'NRANK=%d' % nrank,
		'FIRST_SEEN=%.3f' % (first_seen or 0),
		'LAST_SEEN=%.3f' % (last_seen or 0),
	]
	d = net_delta(net0, net1) if (net0 is not None and net1 is not None) else {}
	tot_rx = sum(v[0] for v in d.values())
	tot_tx = sum(v[1] for v in d.values())
	lines.append('NET_RX_MB=%d' % (tot_rx // 1024 // 1024))
	lines.append('NET_TX_MB=%d' % (tot_tx // 1024 // 1024))
	for k in sorted(d):
		rx, tx = d[k]
		# 接口名里有 '.'/':'，做成独立的两行而不是拼进键名，外面好解析
		lines.append('NETIF %s rx=%dMB tx=%dMB' % (k, rx // 1024 // 1024, tx // 1024 // 1024))
	with open(out, 'w') as f:
		f.write('\n'.join(lines) + '\n')


def bail(signum, frame):
	global net1, last_seen
	# 终点快照在这里**补记**：net1 原本只在"检测到 mpifxcorr 消失"那一刻记，而
	# run_bench.sh 在 mpirun 返回后**立刻** kill，两个事件几乎同时、谁先到不一定。
	# 不补记的话被 kill 的那一侧**网络数恒为 0**——2026-09-30 实测：主机报 rx/tx 0、
	# 远端报 1089 MB，同一份流量的两端对不上（内存数不受影响，它在循环里就采到了）。
	if started and net1 is None:
		if last_seen is None:
			last_seen = time.time()
		net1 = net_snapshot()
	report()
	sys.exit(0)


# **必须在主循环之前**。P4 的内联版就是这么写的，2026-09-30 抽成独立文件时误挪到了
# 循环**后面**——于是主循环运行期间 SIGTERM 走默认动作、进程当场死、**一个字节都不写**，
# 外面只看到 "sampler produced no data"（手工起的那个反倒正常：它能等到 mpifxcorr 消失
# 后走正常出口）。这类 bug 只在"被 kill"这条路径上显形。
signal.signal(signal.SIGTERM, bail)
signal.signal(signal.SIGINT, bail)

t0 = time.time()
# 调试（FXCORR_SAMPLER_DEBUG 非空）：每 25 次循环（~5 s）向 stderr 打一行
DBG = os.environ.get('FXCORR_SAMPLER_DEBUG', '') != ''
it = 0
while time.time() - t0 < DEADLINE:
	procs = snapshot()
	it += 1
	if DBG and it % 25 == 0:
		sys.stderr.write('sampler-dbg: t=%.1f nproc=%d matched=%d started=%s\n' % (
			time.time() - t0, len(os.listdir('/proc')), len(procs), started))
		sys.stderr.flush()
	if procs:
		if not started:
			started = True
			first_seen = time.time()
			# 基线取"mpifxcorr 出现"这一刻，不是采样器启动那一刻——srun 分发有启动
			# 延迟，用后者会把 mpirun 启动前的等待算进流量窗口
			net0 = net_snapshot()
		# VmHWM 是内核记的**历史最高**常驻集、只增不减，同一个 rank 采到一次即可；
		# VmRSS 是当前值，同刻求和取最大——答"跑起来时节点上同时占了多少"。
		# 三个数各有用处：单 rank 峰值看单进程吃多少，同刻总和看节点装不装得下，
		# "各 rank 峰值之和"是各进程峰值不同时出现时的上界（保守值）。
		peak_proc = max(peak_proc, max(p[0] for p in procs))
		peak_sum_rss = max(peak_sum_rss, sum(p[1] for p in procs))
		peak_sum_hwm = max(peak_sum_hwm, sum(p[0] for p in procs))
		nrank = max(nrank, len(procs))
	elif started:
		last_seen = time.time()
		net1 = net_snapshot()
		break
	elif time.time() - t0 > WAIT_START:
		break	# 没等到：nrank 报 0，外面看得出这个节点没跑
	time.sleep(INTERVAL)

report()
