# scratch —— 临时脚本与临时结果

**定位**：调试、探针、一次性脚本和临时输出。不属于正式台账，不被 `docs/` 的
计划/结果索引引用（`docs/log.md` 可提一笔）。

```text
scratch/
├── scripts/<YYYYMMDD>-<topic>/   # 临时脚本
└── results/                      # 临时结果（run_xp.sh 默认输出）
```

## 规则

- 每个临时目录带短 README：目的、保留期限。
- 原始数据、大日志、镜像不放这里；大文件不入 git。
- 临时脚本被正式流程采用后，移入 `scripts/` 并在 `docs/log.md` 记录。
- 临时结果只有用 `scripts/run_xp.sh --formal` 重跑、生成
  `manifest/result/metrics/command/logs` 后，才晋升到 `results/`。
- 晋升后删除或归档 scratch 副本，避免两套并存。
