# 文档索引

| 文件 | 内容 |
|---|---|
| [`environment.md`](environment.md) | difx 机器、数据资产、镜像、实验 workdir、测量口径、已知坑 |
| [`plan.md`](plan.md) | 优化目标、边界、实验矩阵、正确性判据、工作流 |
| [`log.md`](log.md) | 运行台账：按时间记录每一次实验、失败、修复与结论 |

规则：

- 新的实验先写计划（plan 的实验矩阵或 scratch 里的短 README），跑完把过程和结论
  追加到 `log.md`。正式结果同时在 `results/README.md` 建索引。
- 失败也记录：`log.md` 写现象、根因、修复与影响范围。
- 数据/机器/镜像变化时先改 `environment.md`，其他文档引用它。
