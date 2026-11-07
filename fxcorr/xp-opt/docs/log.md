# 运行台账

按时间顺序记录每一次实验、失败与修复。正式结果索引在 `results/README.md`。

## 2026-11-08 建立工作区与首轮基线

### 1. 目录与脚本

- 新建 `fxcorr/xp-opt/`：`prepare_workdir.sh`（实验 workdir）、`run_xp.sh`
  （docker 运行 + 计时）、`results.tsv`。
- 实验 workdir：
  `/data/scalebox/mydata/fxcorr/xp-opt/workdir/workdir-2st-b1024ms-n3`
  ——`config/` `batches/` `fengine/` 软链到源 workdir，`meta/` `vis/` `product/`
  `vis-parts/` `raw/` 新建。
- 源 workdir：`/data/scalebox/mydata/fxcorr/workdir-2st-b1024ms-n3`
  （2 站 × 8 ds = 16 ds、4 组、128 通道、1.024 s）。

### 2. 失败 1：容器内缺 workdir 挂载

现象（rc=1，0.009 s）：

```text
fxcorr-xp: cannot open .../xp-opt/workdir/workdir-2st-b1024ms-n3/batches/00000001.json
```

根因：`run_xp.sh` 只挂了软链目标（源 workdir），没挂实验 workdir 本身；docker
`-w <workdir>` 指向不存在路径时静默建了空目录。

修复：挂载数组初始加入 `-v "$WD:$WD"`。修复后同一命令 rc=0（111.9 s）。

### 3. 失败 2：root 产物宿主删不掉

现象（OMP=8 首跑，rc=1）：清理 `vis/` 时大量

```text
rm: cannot remove '.../vis/test_1.difx/...': Permission denied
```

根因：容器以 root 运行，`vis/` 产物属 root；宿主用户对 `vis/test_1.difx/` 目录
无写权限。清理失败后 SWIN 未删，xp 触发互斥检查退出。

修复：`clean_outputs()` 改为在容器内执行 `rm -rf` + `mkdir -p`。

### 4. 基线（OMP 1 / 8 / 16）

条件：全量模式（16 ds、4 组）、`fxcorr/fxcorr:latest`、每次清空 `vis/`。

| OMP | wall (s) | user (s) | sys (s) | 峰值 RSS (MiB) | station (s) | read (s) | xmac (s) | wacc (s) | 相对 OMP=1 |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 119.8 | 117.3 | 2.0 | 783 | 111.1 | 0.96 | 6.12 | 0.75 | 1.00× |
| 8 | 22.4 | 146.9 | 6.7 | 768 | 15.6 | 1.01 | 4.22 | 0.59 | 5.35× |
| 16 | 14.6 | 175.0 | 5.2 | 765 | 9.23 | 1.22 | 2.61 | 0.56 | 8.20× |

- 三次 SWIN md5 相同：`82eb2c23e93d4a843e5e719cea87b231`（281,088 B）——ds 级
  并行位一致。
- `station` 是主体（OMP=1 占 93%）；OMP≥8 后 `xmac` 串行段成为次瓶颈。
- 首轮临时 runs（旧脚本）同一数据的 wall：111.9 / 20.6 / 14.7 s——与正式值相差
  约 0–8%，属宿主负载/页缓存的正常波动；以 `results/` 为准。

### 4.1 测量口径修正：内存峰值

首轮 OMP=1 报 2826 MiB，重跑报 786 MiB。根因不是程序行为：旧脚本用容器
cgroup v2 `memory.peak`，它**包含页缓存**（读 `stream.bin` 的缓存），随宿主
缓存状态变化，跨运行不可比。正式脚本改为在容器内用 `python3` wrapper 取
`RUSAGE_CHILDREN.ru_maxrss`（真实进程峰值 RSS），同时保留 cgroup 值作参考
（`metrics.json` 的 `mem_cgroup_peak_mib`）。修正后 783/768/765 MiB，与
`fxcorr-xp/CLAUDE.md` 的 ~55 MB/ds 量级一致。

### 5. 目录重构（同日）

参照 `app-fxcorr/experiments` + `scratch` + `scripts/exp` 的组织：

- `scripts/`：正式脚本（`prepare_workdir.sh`、`run_xp.sh`、`sync_results.sh`）；
- `scratch/`：临时脚本与临时结果；
- `results/<日期>/<run-id>/`：正式 run 产物（manifest/result/metrics/command/logs）；
- `calibration/`：线程、内存、I/O 标定；
- `docs/`：环境、计划、台账（本文件）。

`run_xp.sh` 升级：`--formal` 生成标准 run 产物；默认输出到 `scratch/results/`。

### 6. 正式重跑（同日）

用升级后的脚本、`--formal`、RSS 口径重跑三轮，结果在
`results/20261108/<run-id>/`，汇总 `results/summary.tsv`：

| run-id | OMP | wall (s) | 峰值 RSS (MiB) | SWIN md5 |
|---|---|---|---|---|
| `workdir-2st-b1024ms-n3-00000001-omp1` | 1 | 119.8 | 783.1 | `82eb2c23…` |
| `workdir-2st-b1024ms-n3-00000001-omp8` | 8 | 22.4 | 768.0 | `82eb2c23…` |
| `workdir-2st-b1024ms-n3-00000001-omp16` | 16 | 14.6 | 765.1 | `82eb2c23…` |

每个 run 目录含 `manifest.json`（镜像 ID、主机、脚本 sha256、batch 元数据）、
`result.tsv`、`metrics.json`（阶段耗时、RSS/cgroup 内存、SWIN 指纹）、
`command.sh`、`logs/`。结果已用 `scripts/sync_results.sh` 拉回仓库。

后续接续：`app-fxcorr/docs/experiments/difx.md` 的固定单元是 S=16、1.024 s、
3 批；下一步先补该基线，再 profile `station` 内部与 `xmac` 串行段。
