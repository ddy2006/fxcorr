# 环境与数据

## 机器（ssh difx）

| 项 | 值 |
|---|---|
| 主机名 | `gjnode5`（`ssh difx` 入口） |
| CPU | 384 核（`nproc`） |
| 内存 | 754 GiB |
| 数据盘 | `/data/scalebox/mydata/fxcorr/`（28 TB 卷，2026-11-08 已用 46%） |
| docker | `/usr/bin/docker`；镜像 `fxcorr/fxcorr:latest`（ID `sha256:48c0d…`，2026-11-08） |

容器内 `fxcorr-xp` 位于 `/usr/local/difx/bin/fxcorr-xp`；镜像含 `python3`，
不含 GNU `/usr/bin/time`（计时用 bash `time`，内存用 cgroup v2 `memory.peak`）。

## 数据资产

根：`/data/scalebox/mydata/fxcorr/`

| workdir | 站数 | 批长 | batch 数 |
|---|---|---|---|
| `workdir-2st-b1024ms-n3` … `b16384ms-n3` | 2 | 1.024 / 2.048 / 4.096 / 8.192 / 16.384 s | 3 |
| `workdir-{4,8,16}st-b1024ms-n3` … `b16384ms-n3` | 4/8/16 | 同上 | 3 |

- 每套 workdir 含 `config/`、`batches/`、`raw/`、`fengine/`；**xp 的输入是
  `fengine/<batch>/<g>/<station>/ds_<N>/stream.bin`**（fxcorr-fp 产出，v9 链）。
- `workdir-2st-b1024ms-n3`：2 站 × 8 ds = 16 datastream、4 个 ds 组；1.024 s、
  subint 5.12 ms、每 ds 128 通道；fengine 2.0 GB/batch。
- 2 站的 `raw/` 是指向 `workdir-16st-b1024ms-n3/raw` 的相对软链；xp 不读 raw。
- batch 元数据：`batches/<batch_id>.json`（站列表、时长、config/im 路径）。

## 实验 workdir

源 workdir 直接跑会被拦：

1. `vis/` 已有上一轮 SWIN → **SWIN 互斥检查**拒绝重写；
2. `meta/roots/<batch>.json` 记录的是 scalebox 平台路径
   （`workdir=/cluster_data_root/...`、`raw=/tmp/fxcorr/raw`）→ 宿主直跑根检查
   报 `disagrees`。

`scripts/prepare_workdir.sh` 构造实验 workdir：

```text
/data/scalebox/mydata/fxcorr/xp-opt/workdir/workdir-<...>/
  config   -> 源 workdir/config     （软链，只读）
  batches  -> 源 workdir/batches    （软链）
  fengine  -> 源 workdir/fengine    （软链；xp 幂等写回 pcal.bin）
  meta/ vis/ product/ vis-parts/ raw/  新建空目录
```

`meta/` 为空是有意的：无根记录 → `FxcorrPath::checkRoots` 放行。

## 容器运行

```text
docker run --rm -w <workdir> -v <workdir>:<workdir> -v <源 workdir>:<源 workdir>
  -v <run>/logs:/out -e OMP_NUM_THREADS=N -e FXCORR_TIMING_BRIEF=/out/timing-brief.tsv
  fxcorr/fxcorr:latest fxcorr-xp <batch> <workdir>
```

- 源 workdir 的挂载来自 `config/batches/fengine` 软链目标的父目录，脚本自动解析。
- 优化版二进制：`-b DIR` → `-v DIR:/opt/xp-opt/bin:ro`，PATH 前置。
- 容器默认 root：`vis/` 产物宿主用户删不掉，`run_xp.sh` 的清理在容器内执行。

## 测量口径

| 指标 | 来源 |
|---|---|
| `wall_s` / `user_s` / `sys_s` | 容器内 bash `time` 包住 `fxcorr-xp`（`TIMEFORMAT='__TIME__ %R %U %S'`） |
| `host_wall_s` | 宿主侧 `docker run` 前后时间差（含容器启停） |
| `mem_peak_mib` | 容器内 `python3` wrapper 取 `RUSAGE_CHILDREN.ru_maxrss`（真实进程峰值 RSS） |
| `mem_cgroup_peak_mib` | 容器 cgroup v2 `/sys/fs/cgroup/memory.peak`（**含页缓存**，只作参考，跨运行不可比） |
| 阶段耗时 | `FXCORR_TIMING_BRIEF` → `logs/timing-brief.tsv`（station/read/zero/xmac/wacc/uvshift/cbw/auto/addsub） |
| SWIN 指纹 | `vis/**/DIFX_*` 的文件数、总字节、逐文件 md5 |

## 已知坑

1. **漏挂 workdir**：只挂软链目标不够，容器内 `<workdir>` 不存在时 docker 会静默
   建空目录，xp 报 `cannot open batches/<id>.json`。
2. **root 产物**：见上，清理必须在容器内做。
3. **fengine 软链可写**：xp 写回 `pcal.bin`，源数据需可写（幂等）。
4. **镜像与宿主数值差异**：跨构建对照不要用"逐字节"判据（容器与宿主基础库不同，
   约 2 ULP；见仓库 memory 与 `fxcorr/CLAUDE.md`）。本工作区的对拍都在同一镜像内。
