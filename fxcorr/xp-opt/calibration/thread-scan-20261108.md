# 2 站 1.024 s：OMP 线程扫描（2026-11-08）

**数据**：`workdir-2st-b1024ms-n3`，batch `00000001`，2 站 × 8 ds = 16 ds、4 组、
128 通道、1.024 s。**模式**：全量（写 SWIN）。**镜像**：`fxcorr/fxcorr:latest`。

## 结果

数据源：`results/20261108/`（正式 run，RSS 口径 `ru_maxrss`）。

| OMP | wall (s) | user (s) | sys (s) | 峰值 RSS (MiB) | station (s) | read (s) | xmac (s) | wacc (s) | 加速比 |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 119.8 | 117.3 | 2.0 | 783 | 111.1 | 0.96 | 6.12 | 0.75 | 1.00× |
| 8 | 22.4 | 146.9 | 6.7 | 768 | 15.6 | 1.01 | 4.22 | 0.59 | 5.35× |
| 16 | 14.6 | 175.0 | 5.2 | 765 | 9.23 | 1.22 | 2.61 | 0.56 | 8.20× |

SWIN md5 三次相同：`82eb2c23e93d4a843e5e719cea87b231`（281,088 B）。

> 内存口径：`ru_maxrss`（真实进程峰值）。cgroup `memory.peak` 含页缓存，首轮
> 曾读到 2826 MiB，不可用于跨运行比较；两个值都在 `metrics.json` 里。

## 观察

1. **`station` 段主导**：OMP=1 时 111.1 s / 119.8 s = 93%；16 线程降到 9.23 s
   （12.0×）。`station` 按 ds 并行，16 个 ds 时理论上限 16×，实测受超订、
   FFTW 计划/内存带宽与 I/O 限制。
2. **`xmac` 是 OMP≥8 后的次瓶颈**：6.12 → 4.22 → 2.61 s，随线程数收益远小于
   `station`。XMAC 阶段的并行方式与 station 不同，后续 profile 的重点。
3. **`read` 约 1 s**：OMP=1 时 0.96 s，OMP≥8 约 1.0–1.2 s。相对总 wall 占比小，
   但冷缓存/多批时需单独标定。
4. **峰值内存**：783 / 768 / 765 MiB，基本不随 OMP 变化（≈49 MB/ds × 16 ds 的
   Mode 工作集）；与 `fxcorr-xp/CLAUDE.md` 的 ~55 MB/ds 量级一致。

## 后续

- OMP 2/4/32 细扫，定位 `station` 的饱和点；
- `station` 内部拆分（unpack/FFT/旋转/frac sample/autocorr）——需要 profile 工具；
- `xmac` 串行段的来源（每 subint 串行执行、归约/写盘）；
- S=16、1.024 s、3 批的固定单元基线（对齐 app-fxcorr `difx.md`）。
