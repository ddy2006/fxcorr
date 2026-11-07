# xp-opt：fxcorr-xp 性能优化工作区

**建立**：2026-11-08。优化实现侧的工作区：脚本、标定、正式结果与过程文档都在这里。

## 与 app-fxcorr 的分工

| 位置 | 职责 |
|---|---|
| **本目录** | xp 优化的实现与实测：docker 运行、run 产物、脚本、标定、运行台账 |
| `app-fxcorr/docs/experiments/difx.md` | difx 主线的决策口径：xp/xpp 定位、实验矩阵、门禁与指标 |
| `app-fxcorr/experiments/difx/xp-opt/` | 平台侧正式产物入口（走 scalebox 正式 runner 时） |

口径冲突时以 `app-fxcorr/docs/experiments/` 为准；本目录只记录实现与实测。

## 目录

```text
xp-opt/
├── README.md                  # 本页：入口与规则
├── docs/
│   ├── README.md              # 文档索引
│   ├── environment.md         # 机器、数据、镜像、测量口径
│   ├── plan.md                # 优化路线与实验矩阵
│   └── log.md                 # 运行台账：已做过的每一步（含失败与修复）
├── scripts/                   # 正式脚本（可复跑、被文档引用）
│   ├── prepare_workdir.sh     # 构造实验 workdir
│   ├── run_xp.sh              # docker 运行 + 标准 run 产物
│   └── sync_results.sh        # 从 difx 拉正式结果回仓库
├── results/                   # 正式结果
│   ├── README.md              # 索引与判据
│   ├── summary.tsv            # 跨 run 汇总表
│   └── <YYYYMMDD>/<run-id>/   # manifest.json / result.tsv / metrics.json / command.sh / logs/
├── calibration/               # 标定（线程、内存、I/O、NUMA）
└── scratch/                   # 临时脚本与临时结果，可清理
    ├── scripts/<YYYYMMDD>-<topic>/
    └── results/
```

## 正式 / 临时规则

- **正式脚本**：`scripts/`，可复跑，必须被 `docs/` 或本页引用。
- **临时脚本**：`scratch/scripts/<YYYYMMDD>-<topic>/`，一次性探针，不进入正式流程。
- **正式结果**：`scripts/run_xp.sh --formal` 产生，落 `results/<日期>/<run-id>/`，
  含 `manifest.json`、`result.tsv`、`metrics.json`、`command.sh`、`logs/`。
- **临时结果**：`run_xp.sh` 不带 `--formal`，落 `scratch/results/`。
- 临时脚本稳定并被正式流程采用后，移入 `scripts/` 并在 `docs/log.md` 记一笔；
  临时结果只有在正式 runner 下重跑并生成上述五个文件后，才进入 `results/`。
- 大文件（fengine、SWIN、镜像）不入 git；`results/` 只保留元数据与日志。

## 快速开始（新终端接续）

```bash
ssh difx
cd ~/fxcorr/fxcorr/xp-opt

# 1) 实验 workdir（一次）
./scripts/prepare_workdir.sh /data/scalebox/mydata/fxcorr/workdir-2st-b1024ms-n3

# 2) 临时实验
./scripts/run_xp.sh -j 8 /data/scalebox/mydata/fxcorr/xp-opt/workdir/workdir-2st-b1024ms-n3 00000001

# 3) 正式实验（生成标准 run 产物）
./scripts/run_xp.sh --formal -j 8 /data/scalebox/mydata/fxcorr/xp-opt/workdir/workdir-2st-b1024ms-n3 00000001

# 4) 优化版二进制（编译目录挂进容器，优先 PATH）
./scripts/run_xp.sh --formal -j 8 -b /path/to/bin ... 00000001

# 5) 拉正式结果回本地仓库
#    本地执行（仓库根）：
#    rsync -av difx:/data/scalebox/mydata/fxcorr/xp-opt/results/ fxcorr/xp-opt/results/
#    或运行 fxcorr/xp-opt/scripts/sync_results.sh
```

运行前的数据、镜像与口径见 [`docs/environment.md`](docs/environment.md)；优化路线见
[`docs/plan.md`](docs/plan.md)；已运行过程见 [`docs/log.md`](docs/log.md)。
