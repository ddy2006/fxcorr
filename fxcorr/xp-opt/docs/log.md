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

## 2026-11-08 xpp 分支与 band 并行

### 1. xpp 架子

- `applications/fxcorr-xpp` = `fxcorr-xp` 逐字复制（仅改名）；`install-difx` 4 处
  注册、`fxcorr/docker/Dockerfile` COPY 行已加；镜像重建后容器内与 xp 并存。
- **镜像内 xpp 默认路径与 xp 逐字节一致**：2 站 / 1.024 s / batch 00000001、
  OMP=8，md5 均 `82eb2c23…`（281,088 B），wall 20.87 s vs 20.91 s。

### 2. band 并行实现

- 落点 `libraries/fxcorrcommon`：`Mode::setBandParallel(n)`（默认关闭，其他工具
  行为/内存不变），每线程私有 `BandScratch`（旋转/frac/FFT 工作区/FFT plan）。
  zoom、linear2circular、单 band 数据流自动禁用。
- `fxcorr-xpp` 用环境变量 `FXCORR_XPP_BAND_THREADS=N` 启用；band 模式下 station
  的 ds 循环串行、band 循环作为顶层并行（`main.cpp` 已改为 lambda 双分支）。
- **修复的构建坑**：`fxcorrcommon/configure.ac` 的 `AX_OPENMP` 原先在
  `AC_LANG(C++)` 之前，只设置 `OPENMP_CFLAGS`，库从未带 `-fopenmp` 编译
  （`libraries/CLAUDE.md` 有记录）。
- 并行机制迭代：OpenMP 内层 `parallel for`（每 block 一次）在 GCC libgomp 下
  嵌套死锁/极慢；外层改 static 仍死锁；最终改为**持久线程池 `BandPool`**
  （每 Mode 一份 worker），运行时无嵌套 region。

### 3. 验证状态与局限

- 正确性：串行 vs band=8 的完整产物已对齐，SWIN md5 均 `82eb2c23…`（281,088 B）。
- 性能：band=8 墙钟 **413 s**（user 645 s、sys 695 s），比同批串行 **91.9 s** 慢
  4.5×。任务粒度是"每 block 每 band"，2 站 1.024 s 下约十万级任务提交，
  同步成本完全压过计算。band 路径保留为开关，不作为优化方向。
- 后续方向：block 并行（每线程独立 Mode 状态）或归约段并行化；XMAC 展平并行
  也在待办列表。

## 2026-11-08~10 方案 A：ds×block 组合并行（实验性，验证中）

### 1. 结果回顾与一次验证事故

- band 并行（任务 = 每 block 每 band）实测比串行慢 4.5×（413 s vs 91.9 s），
  每 block 一次提交的同步成本完全压过计算；保留为开关，不再作为优化方向。
- 块并行先后试过三种形态：
  1. 每 ds 一个 OpenMP 并行区、每 fftloop 归约（J1 B8 约 76 s，不能与 ds 并行叠加）；
  2. 自管 `SegmentPool`（std::thread）支持 ds×block 组合，条件变量唤醒在本机
     约 35 ms/次，wall 反而劣化；改自旋池后又引入确定性结果偏差；
  3. 大粒度（当前）：`Mode` 的频谱槽扩到整个 subint（槽号 = 全局块索引，
     `Mode::setSubloopCapacity`），每 subint 只处理一次、归约一次，去掉每
     fftloop 的同步。
- **验证事故（2026-11-10）**：`fxcorr/Makefile` 的 `make sync` 使用
  `rsync -az`，按大小+时间戳判断；本地与远端 mtime 相同时跳过传输。一段时间内
  远端 `main.cpp`/`mode.cpp` 一直是旧版本，多轮“回退/逐字节验证”实际都在旧
  二进制上执行（SWIN md5 恒定不变即为信号）。**此前记录的“块并行逐字节一致”
  结论不可靠，已作废**。`make sync` 已改为 `rsync -azc`（按内容比较），关键
  文件已强制同步。教训：同步后必须用 `md5sum` 核对源文件，不能只看 make 成功。

### 2. 当前实现（大粒度，默认关闭）

- `FXCORR_XPP_BLOCK_THREADS=N`（N>1）：
  - 每 ds 构造 N-1 个 Mode 副本；
  - 主 Mode 与副本的 `subloopCapacity` 扩到 `blockspersend`，`process(i, i)`
    把块 i 写进槽 i，不再需要 `blockSlotBase` 映射；
  - 每 subint 一次 `SegmentPool` 调度：每段处理 `blockspersend/N` 个连续块，
    段>0 把谱/数据权重拷回主 Mode；随后一次按块序归约（ac/weight/pcal/
    kurtosis）、一次 `collectBand(fftloop, true)`、一次 ac 批次；
  - ds 由 `OMP_NUM_THREADS` 并行，池任务不嵌套 OpenMP。
- 限制：`maxacblocks >= blockspersend`（否则回退串行）、无 zoom、pcal 提取器
  需支持原始累加器、`perbandweights` 未启用。
- 改动文件：`libraries/fxcorrcommon/src/mode.{h,cpp}`（`fftslots` /
  `setSubloopCapacity`）、`applications/fxcorr-xpp/src/{main,memreader}.*`。

### 3. 验证状态

- 大粒度版已编译通过（fxcorrcommon + xpp）。
- **尚未完成逐字节验证**：容器镜像内的 fxcorrcommon 缺少新符号，正在重建镜像；
  重建后需核对 batch 1/2 的 SWIN、`pcal.bin`、`PCAL_*` 与 xp 逐字节一致，并测
  OMP=1/16 × BLOCK=2/4/8。
- 在验证通过前，不要用块模式产出正式结果；xp 仍是唯一稳定参照。
