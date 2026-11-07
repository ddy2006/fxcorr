# 正式结果

## 目录规范

每个正式 run 由 `scripts/run_xp.sh --formal` 生成，目录
`results/<YYYYMMDD>/<run-id>/`：

```text
manifest.json   数据集、batch、站数、OMP、镜像与 ID、主机、脚本 sha256、源码版本
result.tsv      一行标准结果（与 summary.tsv 同列）
metrics.json    阶段耗时、内存、SWIN 指纹
command.sh      实际执行的完整 docker 命令
logs/           run.log / time.txt / timing-brief.tsv / mem-peak-bytes.txt
```

跨 run 汇总：`summary.tsv`（正式 run 自动追加）。

## 索引

| 日期 | run-id | 条件 | wall | 峰值 RSS | SWIN |
|---|---|---|---|---|---|
| 2026-11-08 | `20261108/...-omp1` | 2 站、1.024 s、全量、OMP=1 | 119.8 s | 783 MiB | `82eb2c23…`，281,088 B |
| 2026-11-08 | `20261108/...-omp8` | 同上、OMP=8 | 22.4 s | 768 MiB | 同上 |
| 2026-11-08 | `20261108/...-omp16` | 同上、OMP=16 | 14.6 s | 765 MiB | 同上 |

完整字段见 `summary.tsv`；阶段耗时见各 run 的 `metrics.json`。

### 2 站 1.024 s 基线汇总

| OMP | wall (s) | user (s) | sys (s) | 峰值 RSS (MiB) | station (s) | read (s) | xmac (s) | wacc (s) | 相对 OMP=1 |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 119.8 | 117.3 | 2.0 | 783 | 111.1 | 0.96 | 6.12 | 0.75 | 1.00× |
| 8 | 22.4 | 146.9 | 6.7 | 768 | 15.6 | 1.01 | 4.22 | 0.59 | 5.35× |
| 16 | 14.6 | 175.0 | 5.2 | 765 | 9.23 | 1.22 | 2.61 | 0.56 | 8.20× |

三次 SWIN md5 相同；`station` 主导，OMP≥8 后 `xmac` 为次瓶颈。

## 判据

- `rc=0`；SWIN 文件存在、md5 与参照一致（优化版必须与 `xp` 逐字节或按记录容差）；
- `metrics.json` 的 `stages_s` 覆盖 station/read/xmac/wacc；
- 失败 run 也保留目录，`result.tsv` 的 status=fail。

## 临时结果

不带 `--formal` 的运行在 `scratch/results/`，不进入本目录与 `summary.tsv`。
晋升条件：用正式脚本重跑并生成上述五个文件。
