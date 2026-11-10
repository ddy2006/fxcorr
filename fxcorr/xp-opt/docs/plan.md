# 优化路线（2026-11-08 初版）

口径以 `app-fxcorr/docs/experiments/difx.md` 为准；本文件只写实现侧的执行计划。

## 目标与边界

- **`xp`（现役 `fxcorr-xp`）是稳定参照**，不改其行为；优化以**可替换实现**推进
  （app-fxcorr 主线里的 `xpp` 命名），同一输入、同一 workdir、同一输出布局。
- 先 profile 再定改动。候选方向（引自 `difx.md` §3.3）：
  1. **任务粒度**：当前 4 组 × 批，评估按 ds / 频段继续拆分；
  2. **串行段**：站级栈、XMAC、归约、I/O 拷贝；
  3. **内存**：stream.bin 拷贝、NUMA、页缓存；
  4. **绑核**：OMP 线程与 ds 读侧分离；
  5. **调度**：XP_SLOTS / VTS / 批间流水（平台侧，见 app-fxcorr）。
- 非目标：不在 difx 上重复 p419 的 A/B/C 全矩阵；不用端到端 B 的 wall 解释内核扩展性。

## 正确性判据（先过再谈性能）

- `xpp` vs `xp` 在同一批数据上的 SWIN/vis；
- 记录数 `nb × (8S)²`（S = 站数）；
- 优先做到**逐字节**一致；若并行归约改变顺序，用容差比较并**记录差异来源**，
  不允许用容差换速度而不记录；
- 每一轮正式结果必须记录 SWIN 的 md5（`metrics.json` / `summary.tsv`）。

## 实验矩阵

| 轴 | 取值 | 说明 |
|---|---|---|
| 实现 | `xp` / 优化版 | 同输入、同任务数、同绑核 |
| 站数 | 2 / 4 / 8 / 16 | 数据集 `workdir-<S>st-b<B>ms-n3` |
| 批长 | 1.024 / 2.048 / 4.096 / 8.192 / 16.384 s | 当前已生成的数据集范围；更短批长视需要另造 |
| OMP | 1 / 8 / 32（按需 2/4/16） | 先建立 2 站基线，再扩到 S=16 |
| ds 组 | 全量 / 分片（`-g G`） | 分片用于任务粒度实验 |

固定单元（建议，与 app-fxcorr 对齐）：

- 内核曲线：**S=16、1.024 s、3 批**（12 个 xp 任务），最能暴露串行段；
- 低负载对照：S=2/4/8 用于看任务粒度与开销；
- 每轮先跑 `xp` 基线，再跑优化版；两者用同一 workdir、同一 batch、同一 OMP。

## 指标

- 单任务 `wall_s`（p50/p95/max，多批重复时）、`user_s`/`sys_s`、峰值内存；
- 强扩展效率 `E(p) = T(1) / (p × T(p))`；
- 阶段耗时（station/read/xmac/wacc/…）用于定位串行段；
- SWIN md5 与记录数用于正确性。

## 工作流

1. 探针/一次性脚本放 `scratch/scripts/<YYYYMMDD>-<topic>/`，结果放
   `scratch/results/`；
2. 稳定的测量用 `scripts/run_xp.sh`，临时实验不加 `--formal`；
3. 进入正式台账的实验加 `--formal`，结果落 `results/<日期>/<run-id>/` 并追加
   `results/summary.tsv`；
4. 每轮跑完在 `docs/log.md` 记过程与结论，失败也记；
5. 优化版二进制通过 `run_xp.sh -b DIR` 挂载，源码改动在 `applications/fxcorr-xp/`
   （或新建的 `fxcorr-xpp`）里，构建与镜像重建按仓库 `build.md` / `fxcorr/docker/`。

## 当前状态与下一步

- 2026-11-08：2 站 1.024 s 基线完成（OMP 1/8/16），见 `results/` 与 `docs/log.md`；
- 2026-11-08：`fxcorr-xpp` 建立，band 并行实测劣化（413 s vs 串行 91.9 s）；
- 2026-11-10：**方案 A 大粒度版已实现**（槽 = 全局块索引、每 subint 一次归约，
  `FXCORR_XPP_BLOCK_THREADS`，默认关闭），编译通过；**逐字节验证待完成**。
  期间发现并修复 `make sync` 用 `rsync -az` 按时间戳跳过、导致远端长期跑旧
  二进制的事故（改为 `-azc`，旧“逐字节一致”结论作废，见 `docs/log.md`）。
- 下一步：
  1. 重建镜像并完成大粒度版的 SWIN / pcal.bin / PCAL 文本逐字节验证，
     再测 OMP=1/16 × BLOCK=2/4/8；
  2. S=16、1.024 s、3 批的 xp 基线（对齐 app-fxcorr 的固定单元）；
  3. profile：station 段内部（FFT/unpack/旋转/frac sample）与 xmac 串行段。
