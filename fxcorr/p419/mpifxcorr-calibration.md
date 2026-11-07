# p419 上 mpifxcorr（方案 A）运行手册

**版本**：2026-11-07
**范围**：本文档 + 同目录的 `run_bench.sh` / `a-side.sh` / `results.tsv` 构成一套
**不依赖任何其他仓库**的完整记录——环境、设计、操作、实测、排查都在这里。

---

## 0. 一句话概括

在 p419 集群上用**原生 mpifxcorr**（未做任何算法改造）跑相关处理，每节点固定
**28 核预算**（24 计算 + 4 I/O），通过 rankfile 把 32 个 datastream rank 分散到
各节点、并用 `mpi_yield_when_idle` 化解超订带来的忙等互抢。实测 1/2/4/8 节点的
wall 都落在 **1325–1383 s**，对节点数与批长都几乎不敏感。

---

## 1. 背景与目标

### 1.1 为什么要在 p419 上跑 mpifxcorr

fxcorr 是对 DiFX/mpifxcorr 的串行化改造（把一次相关拆成 prep → f → x → merge
的批处理流水线，交给集群调度器编排）。要证明改造的价值，需要一个**对照组**：
**未经改造的 mpifxcorr 在同一份数据、同一套硬件、同样的核数预算下跑出什么**。

这就是"方案 A"：直接启动原生 mpifxcorr，不加任何中间层。

### 1.2 核心约束

| 约束 | 数值 | 来源 |
|---|---|---|
| 每节点核预算 | **28 核** = 24 计算 + 4 I/O | 与串行化方案对齐，保证公平 |
| 每节点 rank 上限 | 41（2 节点时最紧） | `NP / N`，必须由 Slurm 给足核 |
| 数据量 | 每节点 61.44 s | 固定母数据集 |
| rank 结构 | 1 manager + 32 ds + 24×N core | mpifxcorr 的固有结构 |

### 1.3 rank 数为什么是 `NP = 33 + 24N`

mpifxcorr 的并行维度在源码里是固定的（`mpifxcorr.cpp:393`）：

```
numcores = NP - 33
```

- **1 个 manager rank**：协调、归并
- **32 个 datastream（ds）rank**：4 站 × 8 datastream，**与节点数无关**
- **N_core 个 core rank**：真正的相关计算，唯一可扩的维度

所以每加一个节点、给它 24 个 core rank：

```
NP = 1 + 32 + 24 × N节点
```

| 节点 | NP | 每节点 rank（不均时） |
|---:|---:|---|
| 1 | 57 | 57 |
| 2 | 81 | 41 / 40 |
| 4 | 129 | 33 / 32 |
| 8 | 225 | 29 / 28 |

---

## 2. 集群环境

### 2.1 作业系统

- **Slurm**，分区 `cstdemo`（另有 `cstwork`，同一批物理机）
- 节点按小时申请，到期后作业被 `TIMEOUT` 回收——**跑长实验前必须先看
  `squeue -u $USER -o "%.14i %.9T %.20e %.25N"` 的 END_TIME**
- 每节点申请 `-c 30 --mem=100G`（默认 1 核会撞 cgroup OOM）
- 控制机 `login0`（= p419-n0）负责发起 mpirun，计算节点由它 ssh 过去

### 2.2 单节点规格

| 项 | 值 |
|---|---|
| CPU | 30 核可见（affinity `0-22,24-30`） |
| cgroup 上限 | `0-31`（32 核；23 与 31 被 Slurm 保留） |
| 主频 | 2000 MHz |
| 内存 | 约 123 GB |
| 网卡 | 1 × 1 GbE（`eno1`）+ **4 × InfiniBand 200 Gb/s**（`mlx5_0`–`mlx5_3`） |

### 2.3 核的分配

28 核预算在节点上的落法：

| 核 | 用途 | 环境变量 |
|---|---|---|
| **0** | 中断处理（IRQ），**排除** | — |
| **23、31** | Slurm 保留，**排除** | — |
| **1–4** | I/O 核：manager + 32 个 ds rank 轮转使用 | `A_IO_CPUSET=1-4` |
| **5–22, 24–29** | 计算核：24 个 core rank 各占一核 | `A_CORE_CPUSET=5-22,24-29` |
| 30 | 未分配（余量） | — |

全部 28 核的合集：`CPUSET=1-22,24-29`。

> **为什么排除核 0**：它是 IRQ 汇聚核，被中断打断会让该核上的 rank 抖动。
> **为什么跳过 23 和 31**：Slurm 把这两个核留给系统，我们的作业用不到。

### 2.4 存储

| 项 | 值 |
|---|---|
| 共享盘 | Lustre，挂载点 `/work2/cstu0036/mydata/fxcorr` |
| 单流带宽 | **190 MB/s** |
| 8 节点并发 | **6.6 GB/s** |
| 母数据集 | `workdir-b1024ms/raw` = **3.9 TB**（480 批） |

**3.9 TB 远大于 8 节点内存合计 984 GB**，所以 raw **不可能**全量驻留 page cache。
任何"前面跑过一轮所以这次是热缓存"的推测都不成立（这一点曾被当作嫌疑，实测排除）。

### 2.5 ssh 端口

- 多数节点 22 与 50022 都通
- **h03r3n07 只开 50022**（22 拒连），所以脚本统一用 `SSH_PORT=50022`

---

## 3. 数据集

### 3.1 目录结构

```
workdir-b1024ms/
├── config/              # vex2difx 生成：.input / .calc / .im
├── raw/                 # VDIF 原始数据
│   ├── BA/BA_00000001_ds0.vdif ...
│   ├── BX/ ...
│   ├── NL/ ...
│   └── WB/ ...
├── batches/             # 每批一个 json（指向 config 与输出目录）
├── bench/               # mpifxcorr 的输入描述（*.difx）
├── meta/roots/          # 根记录
└── vis/                 # 输出（SWIN + DIFX）
```

**4 个站**：BA / BX / NL / WB；每站 **8 个 datastream** → 32 个 ds rank。

### 3.2 批长档位

同一个母数据集按批长切成不同 workdir，**数据总时长相同**：

| 批长 | workdir | 批数（数据量 61.44 s/节点） | 8 节点批数 |
|---|---|---|---|
| 0.128 s | `workdir-b0128ms` | 480 | 3840 |
| 0.256 s | `workdir-b0256ms` | 240 | 1920 |
| 0.512 s | `workdir-b0512ms` | 120 | 960 |
| 1.024 s | `workdir-b1024ms` | 60 | 480 |

### 3.3 数据量与 wall 的关系

**关键规律**：wall 由**每节点数据量**决定，与批长、节点数几乎无关。

| 每节点数据量 | 典型 wall |
|---|---:|
| 61.44 s（1/2/4/8 节点都是这个） | 1330–1385 s |
| 122.88 s（2 节点跑满 1.024×120 以外的配置） | 约 1330 s |
| 245.76 s（4 节点） | 1338–1377 s |
| 491.52 s（8 节点） | 1325–1383 s |

> 准确地说：**每节点 61.44 s 数据对应约 1335 s**，N 节点合计 `61.44×N` 秒，
> wall 基本不变——这就是"弱扩展"（问题规模随节点数线性增长，时间不变）。

---

## 4. 运行架构

### 4.1 三层结构

```
控制机 login0
  │
  ├─ mpirun（HPC-X v2.11.0，/opt/hpc/software/mpi/hpcx/v2.11.0/gcc-7.3.1/bin/mpirun）
  │    │  按 rankfile 把 NP 个 rank 分到各节点
  │    │
  │    └─ 每个 rank 执行 wrapper：~/bench-wrap-26/bin/mpifxcorr（一个 sh 脚本）
  │         │
  │         └─ singularity exec fxcorr.sif /usr/local/difx/bin/mpifxcorr
  │
  └─ mpi_sampler.py（每节点一份，采内存峰值与网卡计数）
```

### 4.2 为什么 mpifxcorr 要跑在容器里

**口径统一**。同一份 mpifxcorr 二进制，裸机 vs 容器的性能差 **4.6 倍**（R12 实测，
构建选项不同）。所有对照实验一律用容器版，避免工具链差异污染结论。

### 4.3 wrapper 的形态

```sh
#!/bin/sh
# 绑定由 mpirun 的 rankfile 给出，这里不做 taskset。
exec /public/software/singularity/singularity-3.8.0/bin/singularity exec \
     --bind /public/home/cstu0030 --bind /work2 \
     /public/home/cstu0030/singularity/fxcorr/fxcorr.sif \
     /usr/local/difx/bin/mpifxcorr "$@"
```

**两个要点**：

1. **不加 `--cleanenv`**——PMIx 的通信变量必须透传，否则容器内的 MPI 无法与
   宿主 mpirun 对接。
2. **不加 taskset**——见 §6.5。

---

## 5. 三段优化设计

### 5.1 设计原则

**目标**：在 28 核/节点 的硬约束下，让 NP 个 rank 尽量不互相干扰。

**两个矛盾**：

- rank 数 > 核数（2 节点 41/28、4 节点 33/28、8 节点 29/28），必然有 rank 共享核
- ds rank（I/O 密集）和 core rank（CPU 密集）混在一起会互相拖累

**对策**：

1. 用 rankfile 把 ds 按节点**均分**，避免全压在首节点（否则首节点 TX 从 227 GB
   涨到 1.77 TB）
2. 用 `--bind-to core:overload-allowed` 允许共享核，但不拒绝启动
3. **用 `--mca mpi_yield_when_idle 1` 让等待中的 rank 让出 CPU**（最关键）

代码层面只有两个分支（单节点 / 多节点），但按节点数分三段说明更清楚。

### 5.2 第一段：单节点（N=1）

**架构**：容器内 mpirun（宿主不起 mpirun，不起 wrapper）。

```
FXCORR_MPI_MAPBY=rankfile:file=/tmp/rf
FXCORR_MPI_BIND=core:overload-allowed
FXCORR_MPI_OVERSUBSCRIBE=1
```

**rank 分布**（NP=57）：

| rank | 角色 | 绑核 |
|---|---|---|
| 0 | manager | 1 |
| 1–32 | datastream | 1–4 轮转（每核 8 个 ds） |
| 33–56 | core（24 个） | 5–22,24-29 轮转（1:1） |

**实测**：

| 批长×批数 | wall |
|---|---:|
| 1.024×60 | 1339.7 s |
| 0.512×120 | 1336.5 s |
| 0.256×240 | 1345.5 s |
| 0.128×480 | 1335.9 s |

**说明**：单节点时 8 个 ds 共用一个 I/O 核（8:1），但 wall 与多节点相同——
**I/O 核的 CPU 需求确实不高**。

### 5.3 第二段：2–4 节点

**架构**：宿主 mpirun + per-host rankfile + wrapper。

```bash
mpirun --rankfile <rf> \
       --bind-to core:overload-allowed \
       --report-bindings \
       --mca mpi_yield_when_idle 1 \
       -np <NP> mpifxcorr bench/<input>
```

**rankfile 生成算法**（`a-side.sh` 内嵌 python）：

```python
lines.append('rank 0=%s slot=%d' % (hosts[0], io[0]))   # manager → 首节点核 1

# ds：按节点均分
per_ds, rem_ds = divmod(ds, nhost)      # ds=32
r = 1
for i, h in enumerate(hosts):
    k = per_ds + (1 if i < rem_ds else 0)
    for j in range(k):
        lines.append('rank %d=%s slot=%d' % (r, h, io[j % len(io)]))
        r += 1

# core：按节点分片，每片 24 个
per, rem = divmod(ncore, len(hosts))
r = ds + 1
for i, h in enumerate(hosts):
    k = per + (1 if i < rem else 0)
    for j in range(k):
        lines.append('rank %d=%s slot=%d' % (r, h, core[j % len(core)]))
        r += 1
```

**2 节点的 rankfile 长这样**（N=2，NP=81）：

```
rank 0=h1 slot=1
rank 1=h1 slot=1     # ds 1-16 在 h1
...
rank 16=h1 slot=1
rank 17=h2 slot=1    # ds 17-32 在 h2
...
rank 32=h2 slot=4
rank 33=h1 slot=5    # core 33-64 在 h1
...
rank 64=h2 slot=5    # core 65-80 在 h2
```

**实测**：

| 节点 | ds/节点 | I/O 核共享 | 每节点 rank | wall |
|---:|---:|---|---:|---:|
| 2 | 16 | 4:1 | 41 / 40 | **1330.0 s** |
| 4 | 8 | 2:1 | 33 / 32 | **1338.2 / 1340.2 / 1350.7 s** |

**这个结果否定了"超订会拖慢"的直觉**：2 节点 16 个 ds 共用 4 个核（4:1），
wall 与 8 节点 1:1 独占时完全相同。

### 5.4 第三段：8 节点及以上

**架构与第二段相同**，差别只在 rank 分布。

| 节点 | ds/节点 | I/O 核共享 | 每节点 rank | wall（四档批长） |
|---:|---:|---|---:|---|
| 8 | 4 | **1:1** | 29 / 28 | **1324.7 / 1343.3 / 1353.9 / 1383.2 s** |

**8 节点的特殊性**：32 个 ds 恰好对上 8 × 4 = 32 个 I/O 核，**天然 1:1**，
不需要额外的均衡策略。

**A 对批长不敏感**：四档批长的 wall 全部落在 1324.7–1383.2 s（差 4.4%），
因为每档的数据总量都是 491.52 s。

### 5.5 三段对比

| 维度 | 1 节点 | 2–4 节点 | 8 节点 |
|---|---|---|---|
| mpirun 位置 | 容器内 | 宿主 | 宿主 |
| 绑核来源 | 容器内 rankfile | 宿主 rankfile | 宿主 rankfile |
| ds / I/O 核 | 8:1 | 4:1 / 2:1 | **1:1** |
| `yield_when_idle` | 不需要（无跨节点等待） | **必需** | **必需** |
| 典型 wall | 1336–1346 s | 1330–1351 s | 1325–1383 s |

---

## 6. 关键参数详解

### 6.1 `FXCORR_MPI_RANKFILE` → `--rankfile`

**作用**：把 rank → (节点, 核) 的映射写死在文件里，mpirun 照单执行。

**为什么不能用 `-host`**：`-host` 只均分 rank 总数，而 **rank 1–32 恒为 ds**
的固定顺序让首节点吃掉 28 个 ds，所有 core 要跨节点从首节点拉数据：

| 布局 | 首节点 TX | wall |
|---|---:|---:|
| rankfile（ds 4/节点） | **227 GB** | 1324.7 s |
| `-host` 均分（ds 28 个在首节点） | **1.77 TB** | 2904.8 s |

**注意**：rankfile 只在**不缺 yield** 的前提下才有这个效果。缺 yield 时两者
都是 2806–2833 s，差 1%。

### 6.2 `FXCORR_MPI_BIND` → `--bind-to core:overload-allowed`

**作用**：按 rankfile 的 slot 绑核，但**允许两个 rank 共用一个核**（否则
Open MPI 会以 "not enough slots" 拒绝启动）。

**为什么不是 `--bind-to core`**：2 节点时 manager 和 ds1 共用核 1，严格绑定会失败。

### 6.3 `FXCORR_MPI_EXTRA` → `--mca mpi_yield_when_idle 1` ← **最关键**

**作用**：MPI 在等待消息时**让出 CPU**（`sched_yield`），而不是忙等自旋。

**为什么必需**：每节点 rank 数都超过核数——

| 节点 | rank/节点 | 核 | 超订比 |
|---:|---:|---:|---:|
| 2 | 41 | 28 | **1.46** |
| 4 | 33 | 28 | 1.18 |
| 8 | 29 | 28 | 1.04 |

没有 yield 时，等待中的 rank 持续占用 CPU，把同核的另一个 rank 挤掉。
而 MPI 集合通信是**全局同步**的——任何一个 rank 被拖慢，225 个 rank 全部要等。

**实测代价**：

| 场景 | 有无 yield | wall | 倍数 |
|---|---|---:|---:|
| 8 节点 1.024×480 | 有（11-05） | 1324.7 s | 1.00× |
| 8 节点 1.024×480 | **无**（11-07） | **2806.3 s** | **2.12×** |
| 4 节点 1.024×240 | 有（11-04） | 1340.2 s | 1.00× |
| 4 节点 1.024×240 | **有**（11-07 修复后） | **1354.6 s** | **1.01×** |

**历史依据**：2026-11-01 在 57 rank 场景实测，集合通信从 **48 ms 降到 87 µs**
（约 550 倍）。

**超订越重，放大越多**：2 节点（1.46）退化 2.8×，8 节点（1.04）退化 2.1×。

### 6.4 `FXCORR_MPI_OVERSUBSCRIBE`

**作用**：`--oversubscribe`，允许 NP 大于可用核数。

**单节点**必须给（57 rank / 30 核）。**多节点**不给——slot 数已由 rankfile
写明，再给 `--oversubscribe` 会与映射指令冲突。

### 6.5 禁止给 wrapper 加 taskset

**反例**：wrapper 改成

```sh
exec taskset -c 1-22,24-29 singularity exec ... mpifxcorr "$@"
```

8 节点从 2806 s 退化到推算 **5442 s**（跑 40 分钟只完成 44%）。

**原因**：taskset 把 mpirun 按 rankfile 设的**单核亲和**覆盖成整个 28 核掩码，
与 `--bind-to core` 冲突，rank 在 28 个核之间来回迁移，缓存全失效。

### 6.6 参数到底有没有生效——必须验证

**不要相信"我设置了环境变量"**。验证方式：

```bash
ps -u $USER -o cmd | grep 'mpirun --rankfile' | head -1
```

看到类似这样才算生效：

```
mpirun --rankfile /.../rf_xxx -x PATH -x LD_LIBRARY_PATH \
       --bind-to core:overload-allowed --report-bindings \
       --mca mpi_yield_when_idle 1 -np 225 mpifxcorr bench/test-sim.input
```

**再加一道保险**：`--report-bindings` 会在日志里打印每个 rank 的实际绑定，
行数应等于 NP：

```bash
grep -c "MCW rank" a-logs/a-<tag>.log     # 应等于 NP
```

---

## 7. 详细操作步骤

### 7.1 前置准备

**① 确认节点与剩余时间**

```bash
squeue -u $USER -o "%.14i %.9T %.12M %.20e %.25N"
sinfo -N -o "%.12N %.11T" | grep -v demo
```

**必须有至少 N 台 `allocated` 且 END_TIME 晚于预计运行时长 + 1 小时**
（清理与突发的余量）。

**② 确认无残留进程**

```bash
for h in <节点列表>; do
  ssh -o BatchMode=yes -p 50022 $h 'pgrep -cx mpifxcorr' 
done
```

全部为 0 才能开始。有残留先 `pkill -x mpifxcorr`。

**③ 清 workdir 产物**

```bash
W=/work2/cstu0036/mydata/fxcorr/workdir-b1024ms
rm -rf "$W/vis" "$W/vis-parts" "$W/product" "$W/fengine"
```

**④ 清节点本地根**（并行，秒级完成）

```bash
for h in <节点列表>; do
  ssh -o BatchMode=yes -p 50022 $h 'rm -rf /tmp/fxcorr /dev/shm/fxcorr' &
done
wait
```

### 7.2 单节点运行

```bash
cd /public/home/cstu0030/fxcorr/fxcorr

NODES="h04r1n07" \
W=/work2/cstu0036/mydata/fxcorr/workdir-b1024ms \
bash p419/a-side.sh 24 1 60 tag-n1
```

参数：`<N_core> <线程/核> <批数> [标签]`。N_core=24 → NP=57。

### 7.3 多节点运行

```bash
NODES="h04r1n07 h04r2n05 h03r3n11 h03r3n07" \
W=/work2/cstu0036/mydata/fxcorr/workdir-b1024ms \
bash p419/a-side.sh 96 1 240 tag-n4
```

N_core = 24 × 4 = 96 → NP = 129。

**脚本内部会**：

1. 从 `NODES` 推节点数，生成 `<LOGDIR>/rankfiles/rf_<tag>`
2. 写 wrapper `~/bench-wrap-26/bin/mpifxcorr`
3. 预 ssh 全节点（采 host key，最多重试 3 次）
4. 覆盖 `.threads`（备份为 `.bak-16`）
5. 起 mpirun，同时启 `mpi_sampler.py` 采内存/网络
6. 结束后汇总采样，结果追加到 `<LOGDIR>/a-side.csv`

**关键环境变量**：

| 变量 | 默认 | 说明 |
|---|---|---|
| `NODES` | 从 `SLURM_JOB_NODELIST` 展开 | 物理主机名，空格分隔 |
| `W` | `workdir-b0512ms` | workdir 路径 |
| `CPUSET` | `1-22,24-29` | 总核约束 |
| `A_IO_CPUSET` | `1-4` | ds/manager 用的核 |
| `A_CORE_CPUSET` | `5-22,24-29` | core rank 用的核 |
| `SSH_PORT` | `50022` | 节点间 ssh |
| `A_MAX_RANK_PER_NODE` | `30`（run-a.sh 里设 57） | 超订保护 |

### 7.4 结果判读

**① 日志位置**

```
/work2/cstu0036/mydata/fxcorr/a-logs/
├── a-<tag>.log              # mpifxcorr 完整输出
├── rankfiles/rf_<tag>       # 本次的 rankfile
├── sample-<tag>-<ts>/       # 每节点一份采样（内存峰值、网卡计数）
└── a-side.csv               # 所有轮次的结果汇总（追加）
```

**② 判据**

| 项 | 期望 |
|---|---|
| 退出码 | 0 |
| 记录数 | `批数 × 1024`（4 站×8 ds 的平方） |
| `MCW rank` 行数 | = NP（证明 rankfile 生效） |
| wall | 与同数据量的历史值差 < 5% |

**③ 从 a-side.csv 取数**

```bash
grep "^<tag>," a-logs/a-side.csv
```

格式：`tag,nnodes,n_core,nthr,np,nb,wall_s,nrec,exp,peak_rank_mb,tot_rx_mb,tot_tx_mb`

### 7.5 清理

```bash
# 杀 mpirun
pids=$(ps -u $USER -o pid,cmd | grep -E 'mpirun --rankfile' | grep -v grep | awk '{print $1}')
kill $pids

# 杀各节点 mpifxcorr
for h in <节点列表>; do ssh -p 50022 $h 'pkill -x mpifxcorr' & done
wait
```

> **不要用 `pkill -f <模式>`**——它会匹配到自己的命令行，把自己杀掉。
> 用 `pkill -x mpifxcorr`（精确进程名）或先 `pgrep` 拿 PID 再 `kill`。

---

## 8. 实测数据

完整数据见同目录 `results.tsv`。核心规律：

### 8.1 全部结果

| 节点 | 批长×批数 | 数据量 | NP | wall |
|---:|---|---:|---:|---:|
| 1 | 1.024×60 | 61.44 s | 57 | 1339.7 |
| 1 | 0.512×120 | 61.44 s | 57 | 1336.5 |
| 1 | 0.256×240 | 61.44 s | 57 | 1345.5 |
| 1 | 0.128×480 | 61.44 s | 57 | 1335.9 |
| 2 | 1.024×120 | 122.88 s | 81 | 1330.0 |
| 4 | 1.024×240 | 245.76 s | 129 | 1340.2 |
| 4 | 0.512×480 | 245.76 s | 129 | 1338.2 |
| 4 | 0.256×960 | 245.76 s | 129 | 1350.7 |
| 8 | 1.024×480 | 491.52 s | 225 | 1324.7 |
| 8 | 0.512×960 | 491.52 s | 225 | 1343.3 |
| 8 | 0.256×1920 | 491.52 s | 225 | 1353.9 |
| 8 | 0.128×3840 | 491.52 s | 225 | 1383.2 |

**复现验证（2026-11-07，修复 yield 后）**：

| 节点 | 批长×批数 | wall | 对照历史 | 差 |
|---:|---|---:|---:|---:|
| 4 | 1.024×240 | **1354.6** | 1340.2 | +1.1% |
| 4 | 0.512×480 | **1377.5** | 1338.2 | +2.9% |

### 8.2 三条规律

1. **wall 由每节点数据量决定**，与节点数、批长几乎无关
   （61.44 s → 1330–1346；245.76 s → 1338–1378；491.52 s → 1325–1383）
2. **弱扩展成立**：节点数翻倍、数据量翻倍、wall 不变
3. **I/O 核共享不是瓶颈**：8:1（单节点）、4:1（2 节点）、1:1（8 节点）
   wall 相同

---

## 9. 问题与排查记录

### 9.1 事件：2026-11-06 多节点性能集体退化

**现象**：2026-11-05 之前所有多节点 A 轮都在 1325–1383 s；**2026-11-06 06:47
之后全部变成 2547–6043 s**，且与节点数、批长、rankfile 与否都无关。

**影响范围**：

| 轮次 | 日期 | 节点 | wall | 相对正常 |
|---|---|---:|---:|---:|
| p22 | 11-05 | 8 | 1324.7 | 1.00×（正常） |
| p24 | 11-06 | 8 | 2547.7 | 1.92× |
| p26 | 11-06 | 2 | 3780 | 2.84× |
| p27 | 11-06 | 2 | 6043.5 | 4.54× |
| p32-rf | 11-06 | 8 | 2612.3 | 1.97× |
| p38 | 11-06 | 8 | 2833.1 | 2.14× |
| p41 | 11-07 | 8 | 2806.3 | 2.12× |

### 9.2 排查过程（按时间）

| 步骤 | 假设 | 做法 | 结果 |
|---|---|---|---|
| 1 | rank 分布不对（57+24 全压首节点） | 改成均分 | 无效（2833 vs 2905） |
| 2 | 需要 rankfile | 加 `--rankfile` + `--report-bindings` | 无效（2806，bound 225 行齐全） |
| 3 | wrapper 缺 taskset | 加 `taskset -c 1-22,24-29` | **更慢**（5442） |
| 4 | 脚本版本回退导致 | 恢复 11-01 版脚本 | 无效（2833） |
| 5 | 缓存（前面跑过 B/C 预热） | `du -sh raw` | **排除**：3.9 TB > 984 GB 内存 |
| 6 | 节点被别的用户抢占 | `sacct` 查 11-07 全部作业 | **排除**：只有自己的作业 |
| 7 | **参数被静默丢弃** | `ps` 看 mpirun 实际命令行 | **命中**：缺 `--mca mpi_yield_when_idle 1` |

**定位后的验证**：加回该参数，4 节点 1.024×240 跑出 **1354.6 s**，对照历史
1340.2 s 差 1.1%；换批长 0.512×480 跑出 **1377.5 s**，对照 1338.2 s 差 2.9%。
两档独立复现。

### 9.3 根因

`a-side.sh` 从 2026-11-01 起就 export 了：

```bash
export FXCORR_MPI_EXTRA="--mca mpi_yield_when_idle 1 ${A_MPI_EXTRA:-}"
```

但 `run_bench.sh` 的某些版本**根本不读 `FXCORR_MPI_EXTRA`**（grep 计数为 0），
参数被静默丢弃。mpirun 命令行里从头到尾没有这个参数。

**时间线**：

| 日期 | `run_bench.sh` | EXTRA | 表现 |
|---|---|---|---|
| 11-05（p22） | 支持 RANKFILE 的某版本 | ✅ | 1324.7 s |
| 11-06 06:47（p24） | 支持 RANKFILE，**不读 EXTRA** | ❌ | 2547.7 s |
| 11-07 | 修复后 | ✅ | 1354.6 s |

### 9.4 教训

1. **e xport 与读取必须成对核对**。设置了环境变量 ≠ 它进了命令行。
   唯一的验证方式是 `ps -o cmd` 看实际命令行。
2. **不要用"配置相同"推断"行为相同"**。p41 与 p22 的脚本 md5 相同、
   rankfile 相同、流量相同（RX 3816 vs 3818 GB），但 wall 差 2 倍——
   差异在一个看不见的 MPI 参数上。
3. **流量相同是个强信号**。如果 RX/TX 一致而 wall 差很多，问题一定在 CPU 侧，
   不要再去查 I/O、网络、缓存。
4. **性能实验要留参数快照**。这次能定位，靠的是 a-side.csv 里有每轮的
   wall + 流量，以及 p22-8node.sh 保留了节点顺序。

---

## 10. 已知问题（未解决）

### 10.1 8 节点加 yield 后尚未实测

4 节点已复现（1354.6 vs 1340.2），**8 节点的完整轮次还没跑过**。
预期不会更差（8 节点超订最轻，29/28），但需要实测确认。

### 10.2 启动时间波动大

同一套脚本，从 mpirun 启动到处理第一个 subint：

| 轮次 | 节点 | 启动耗时 |
|---|---:|---|
| p41 | 8 | 约 1 分钟 |
| p45 | 4 | **23 分钟** |
| p43 | 8 | 21 分钟后失败（节点回收） |

原因未查明。候选：容器镜像冷加载、PMIx 握手、某节点 sshd 抖动。
**影响每轮的时间预算**（clean + 启动 + 计算），但不影响 wall（wall 从 mpirun
启动计时）。

### 10.3 数据无法全量缓存

raw 3.9 TB > 8 节点内存 984 GB。这一点曾被当作"历史快是因为缓存"的嫌疑，
`du` 实测后排除。

### 10.4 启动阶段的 `nice` 告警

日志开头有：

```
nice: cannot set niceness: Permission denied
HSA Error: Incompatible kernel and userspace, AMD Radeon Graphics disabled.
```

前者是 mpifxcorr 内部尝试降低优先级失败（容器内无 CAP_SYS_NICE），后者是
GPU 探测失败。**两者都不影响计算**，与性能无关。

---

## 11. 文件与版本

### 11.1 本目录文件

| 文件 | 说明 |
|---|---|
| `mpifxcorr-calibration.md` | 本文档 |
| `run_bench.sh` | 宿主侧启动器：读环境变量 → 拼 `MPIARGS` → 起 mpirun → 采样 |
| `a-side.sh` | 多节点编排：生成 rankfile、写 wrapper、预 ssh、调用 run_bench.sh |
| `results.tsv` | 实测数据（可直接导入表格） |

### 11.2 版本

| 文件 | md5 | 状态 |
|---|---|---|
| `run_bench.sh` | `3116977cc3e39df3d1d03150bd17891f` | ✅ 含 `FXCORR_MPI_EXTRA` + `FXCORR_MPI_RANKFILE` |
| `a-side.sh` | `70213c8716ad2f3da313bad11da5027b` | ✅ 含宿主 rankfile 路径 + `FXCORR_MPI_EXTRA` export，wrapper 不做 taskset；头注释已去掉外部仓库引用 |

### 11.3 关键代码位置

| 位置 | 内容 |
|---|---|
| `run_bench.sh` 的 `MPIARGS` 组装 | `--rankfile` / `--bind-to` / `--report-bindings` / **`FXCORR_MPI_EXTRA`** 分支（紧随 report-bindings 之后） |
| `a-side.sh` 的 `A_RANKFILE` 段 | python heredoc 生成 rankfile |
| `a-side.sh` 的宿主分支 | `if [ -n "$A_RANKFILE" ]` 内 export 四个 `FXCORR_MPI_*` 变量 |
| `a-side.sh` 的 wrapper 生成 | `cat > "$BENCHWRAP/mpifxcorr"`，**不含 taskset** |
