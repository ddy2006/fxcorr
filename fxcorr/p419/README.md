# p419 上 mpifxcorr（方案 A）

本目录是一套**自包含**的记录：在 p419 集群上用原生 mpifxcorr 跑相关处理的
环境、设计、操作、实测与排查，不依赖任何其他仓库。

| 文件 | 说明 |
|---|---|
| **[mpifxcorr-calibration.md](mpifxcorr-calibration.md)** | 主文档：环境、三段优化、参数详解、操作步骤、实测数据、排查记录 |
| `run_bench.sh` | 宿主侧启动器（读环境变量 → 拼 MPIARGS → 起 mpirun → 采样） |
| `a-side.sh` | 多节点编排（生成 rankfile、写 wrapper、预 ssh、调用 run_bench.sh） |
| `results.tsv` | 全部实测数据，`valid` 列区分（缺 `mpi_yield_when_idle` 的轮次标注为 no） |

## 一句话结论

每节点 28 核（24 计算 + 4 I/O），NP = 33 + 24N。1/2/4/8 节点的 wall 都落在
**1325–1383 s**，对节点数与批长都几乎不敏感（弱扩展）。

**性能关键不是绑核，而是 `--mca mpi_yield_when_idle 1`。** 缺它时 wall 翻 2.1 倍
（8 节点 2806 vs 1324.7）；wrapper 加 taskset 反而再慢一倍（5442）。

## 快速开始

```bash
export W=/work2/cstu0036/mydata/fxcorr/workdir-b1024ms

# 4 节点，每节点 24 core rank
NODES="h04r1n07 h04r2n05 h03r3n11 h03r3n07" \
  bash p419/a-side.sh 96 1 240 tag-n4

# 结果
grep "^tag-n4," /work2/cstu0036/mydata/fxcorr/a-logs/a-side.csv
```

（脚本默认 `REPO=/public/home/cstu0030/fxcorr/fxcorr`，即 `run_bench.sh` 所在目录。
本目录需部署到该位置，或改 `a-side.sh` 里的 `REPO`。）

## 版本

| 文件 | md5 |
|---|---|
| `run_bench.sh` | `3116977cc3e39df3d1d03150bd17891f` |
| `a-side.sh` | `70213c8716ad2f3da313bad11da5027b` |
