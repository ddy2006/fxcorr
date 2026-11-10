# fxcorr-xpp 目录说明

**最后更新**：2026-11-08（**新建：fxcorr-xp 的并行优化分支**——逐字复制
`applications/fxcorr-xp`（仅改名），作为与 xp 的对照基线；同日加入**可选的站级
band 并行**（`Mode::process` 的 recorded-frequency 循环），默认关闭，开启后产物
与 xp 逐字节一致。实现落在 `libraries/fxcorrcommon`：新增
`Mode::setBandParallel()` 与私有 `BandScratch`，**默认关闭，其他工具行为与内存
占用均不变**；xpp 通过环境变量 `FXCORR_XPP_BAND_THREADS=N` 启用）。

## 与 fxcorr-xp 的差异（2026-11-08）

- 程序名 `fxcorr-xpp`；`install-difx` 4 处注册与 `fxcorr/docker/Dockerfile` 的
  COPY 行均已加入，镜像内与 xp 并存。
- **ds×block 组合并行（可选开关，方案 A，实验性）**：`FXCORR_XPP_BLOCK_THREADS=N`
  （N>1）时每 ds 构造 N-1 个 Mode 副本；主 Mode 与副本的频谱槽扩到整个 subint
  （`Mode::setSubloopCapacity`，槽号 = 全局块索引），每 subint 只做一次块段
  处理与一次按块序归约（pcal 按 band 分担）。ds 由 `OMP_NUM_THREADS` 并行，
  每 ds 的段任务由自管 `SegmentPool` 执行，不嵌套 OpenMP。要求
  `maxacblocks >= blockspersend`、无 zoom、pcal 提取器支持原始累加器，否则自动
  回退串行；默认关闭，与 band 开关互斥。**验证状态（2026-11-10）**：大粒度版
  已编译，逐字节验证待镜像重建后完成（此前“逐字节一致”的旧结论因远端源码未
  同步而作废，见 `fxcorr/xp-opt/docs/log.md`），验证前不要用于正式结果。
- **band 并行（可选开关）**：`FXCORR_XPP_BAND_THREADS=N`（N>1）时，`process()`
  把 recorded-frequency 循环分到 N 个线程，每线程使用私有 scratch
  （`Mode::BandScratch`：FFT 输出工作区、旋转/frac-sample 中间数组、kurtosis
  scratch 等）；每 band 的运算顺序不变，所以 `.part` / SWIN 与 xp **逐字节一致**。
  未设置或 N=1 时走与原 xp 完全相同的串行路径。zoom 配置自动禁用（band 可能
  别名父带，两个迭代会写同一 `fftoutputs`）；单 band 数据流也自动串行。
  **实测劣化**：每 block 提交一次任务，2 站 1.024 s 下约十万级提交，wall 413 s
  vs 串行 91.9 s；保留为对照开关，不作推荐。
- 线程层次：默认 ds 并行（`OMP_NUM_THREADS`）；band 模式 = ds 串行 × band 内层
  并行（两层，`omp_set_max_active_levels(2)`）；block 模式 = ds 串行 × 块段
  并行（单层）。总线程数需自行控制在物理核预算内（384 逻辑核建议 ≤192）。
- 对照入口：`fxcorr/xp-opt/scripts/run_xp.sh --exe fxcorr-xpp ...`（`run_xp.sh`
  已加 `--exe`，结果字段与 xp 完全一致，可直接对比）。

以下为继承自 `fxcorr-xp` 的说明（2026-10-13 的 v9 阶段 0–3 记录），描述 xpp 与
xp 共有的行为；后续优化进展记录在 `fxcorr/xp-opt/docs/`。

v9 链的站级+基线一体化工具（**x′**）：无 MPI，从 fxcorr-x 生长而来并**并入站级栈**。
每个 datastream 一个 `Mode`，数据来自 fxcorr-fp 的 **`stream.bin`**（1× 时间槽排布数据，
fxcorr/v9-plan.md §4）——原样重放 `setValidFlags`/`setData`/`setOffsets`，自己跑
`Mode::process`（unpack/pcal/FFT/旋转/frac sample/autocorr/kurtosis 全在 Mode 内部），
再走原样搬来的 **XmacEngine + Integrator** 出分片 `.part` / SWIN。所以 v8 链（f+x）与
v9 链（fp+xp）在同一数据上产出**逐字节相同**（判据锚，见测试节）。角色对照 mpifxcorr 的
core（站级处理 + 交叉乘），`fxmanager.cpp:333` 的"每份数据 FFT 恰好一次"在 xp 里表现为
**每 ds 单线程块序串行 process**。

## 调用方式

```
fxcorr-xpp <batch_id> [workdir] [ds_group]      # 全量（写 SWIN）/ 分片（写 .part）
fxcorr-xpp merge <batch_id> [workdir]           # batch 级归并 → merged.part
fxcorr-xpp merge --experiment [workdir]         # 实验级归并 → SWIN（唯一写入者）
```

- 命令形态、参数语义、根解析（`FxcorrPath` 三根）、batch.json / `.input` 加载、
  `FXCORR_X_GROUPS_ONLY` 诊断出口、SWIN 互斥检查、`FXCORR_X_MERGE_FORCE`、
  `FXCORR_X_ALLOW_EMPTY` 空输入防呆、ds 组划分与分片掩码、DifxMessage manager 角色
  （mpiId=0、Starting → 每积分 RUNNING → Ending/Done）——**全部与 fxcorr-x 相同**。
- **数据输入**：`<FENGINE_ROOT>/<batch>/<g>/<station>/ds_<N>/stream.bin`（fp 写），
  不再是 `band_XX.sp`；header 与 `.input` 交叉校验（batch_id/station/ds/subintns/
  blockspersend/numbufferedffts/nsubints），口径同 x 当年校验 `.sp` header。
- **站级产物由本工具写**：`ds_<N>/pcal.bin`（fengine 目录内）与
  `vis/<exp>.difx/PCAL_<mjd>_<sec>_<station>_ds<N>`（PCAL 文本）——同格式、同位置、
  同幂等语义，原来由 fxcorr-f 写（v9-plan D6/D7）。`autocorr.bin` 与 `band_XX.sp`
  **取消**（谱与自相关在内存直取）。
- **挡掉的配置**（启动即报错，不静默；v9-plan D11）：多相位中心、pulsar binning、
  相位阵（按需后补）。**`FXCORR_STA` / `FXCORR_KURTOSIS` 已支持**（阶段 2 从 f 移植；
  语义与 f 一致，含 P9 的 STA 前平均分支；见"关键实现要点"）。zoom 与 crosspol 已
  验证（判据 4 逐字节，见测试节）。
- `FXCORR_LOGLEVEL` 同 fxcorr-x（log.h 为拷贝，改一处必须同步另一处）。

## 文件与来源对照

| 本目录 | 作用 | 来源 |
|---|---|---|
| main.cpp | 命令形态 / batch.json / 根解析 / merge 两子命令（**未改**）、挡掉项、站级阶段 + XMAC 阶段主循环、`sendSTA`/`sendKurtosis`（阶段 2 从 f 逐字拷入）、三档时间戳（采集点 station/read/zero/xmac/wacc/uvshift/cbw/auto/addsub，TSV prog 列 = `fxcorr-xpp`） | 从 fxcorr-x/main.cpp 生长：删 beam 分支（相位阵挡掉）、删 `.sp` reader 构造与并行读段；新增站级阶段（见下）与产物收尾 |
| spreader.h | **纯接口**（`SpReader`：`numChannels/spectra/weights` 三个虚方法 + 虚析构） | 从 fxcorr-x/src/spreader.{h,cpp} 改造：文件读实现删除，只留 XmacEngine 实际调用的接口面——**xmac.cpp 因此一行未改** |
| memreader.{h,cpp} | `MemReader : SpReader`——一个 (ds, total band) 的谱视图，从 Mode 的槽直取 | 新写（对应 fxcorr-x 的 SpReader 文件读；带宽/槽语义与 f 的 `writeSpectraBand` 一致） |
| streamreader.{h,cpp} | `stream.bin` 顺序读：header 校验 + 每 subint 定长 record（status/validbytes/时间戳/flags/data） | 新写；格式 = fxcorr-fp StreamWriter 的镜像（两处 static_assert 定长） |
| acbatch.{h,cpp} | `AcBatchCollector`——每 (ds, subint) 的 ac 批次记录的**内存形态**（`averageFrequency` + 按 total band 取 ac/权重，zoom 权重映射到父带） | 新写；逐行对应 fxcorr-f 的 `FEngineWriter::writeAutocorrelationBatch` |
| integrate.{h,cpp} | `Integrator`：SWIN 积分/写盘、RUNNING 状态（**未改**）；`addAutocorrs` 改为**收内存记录**（`std::vector<std::vector<AutocorrBatch>>`），数值路径（resultindex/weightindex 递推、used-frequency 门、crosspol 续段）与文件读版逐行相同 | fxcorr-x/src 拷贝 + addAutocorrs 接口改造（v9-plan R4） |
| pcalwriter.{h,cpp} | `pcal.bin` 写盘（header + 每 subint tone 块），写盘判错三步照 f | 新写；实现从 fxcorr-f 的 `FEngineWriter::writePcal*` 摘出 |
| pcaltextwriter.{h,cpp} | PCAL 文本（intTime 累加、幂等替换、缩放公式） | **fxcorr-f/src 逐字节拷贝**（2026-10-13）；**改一处必须同步另一处**，回归 = 判据 5 |
| xmac.{h,cpp} | XmacEngine（交叉乘 + uvshift/average + 权重累加）——**xmac.cpp 一行未改**；xmac.h 仅注释更新 | fxcorr-x/src 拷贝 |
| log.h / ompcompat.h | 同 fxcorr-x | 拷贝 |
| （无 beamengine.{h,cpp}） | 相位阵被挡掉，beam 分支未搬入（fxcorr-x 保留原样） | — |

## 关键实现要点（易错，改前必读）

### 站级阶段 = fxcorr-f 的 [t=0 路径 + 归约段] 合并（位一致的根）

每 subint、每 ds（`#pragma omp parallel for` over ds，**每线程独占一个 Mode**）：

1. 重放 fp 的 record：`setValidFlags` → `setData(data, validbytes, scan, sec, ns)` →
   `setOffsets(scan, offsetsec, offsetns)`（顺序与 f 的 setup 段一致；`sec` 在无效
   subint 时是 `Mode::INVALID_SUBINT`，原样透传）；
2. 每 fftloop：`process(fftloop×nbf + b, b)` **块序串行**（slot = 全局块号，
   `setBlockSlotBase(0)`、`enableBlockAccumulators(blockspersend)` 在构造处调用——
   与 f 的 nthreads=1 路径同款）；
3. 紧随的**归约段**是 f 并行区归约的单线程版：ac/权重按 (crosspol, band) 的
   `getBlockAc`/`getBlockWeight` **逐块序**累回 `autocorrelation`/`getWeightReduced`；
   pcal 用 `getBlockPcal` → `addPcalBlock` 同序回填（extractor 不支持 per-block 时
   二者自动空转，process 已直接累加——与 f 的降级路径同语义）；
4. `MemReader::collectBand(fftloop)`（= f 的 `writeSpectraBand` 位置，槽被复用前）；
5. ac 批次边界（`maxacblocks`，公式同 f）：`pcaltext->accumulateWeight` →
   `AcBatchCollector::collect` → `zeroAutocorrelations`；subint 尾处理尾部 partial；
6. subint 尾（有 pcal 时）：`PcalWriter::writeSubint`（内部 `finalisepcal`）→
   `pcaltext->accumulate` → intTime 边界 `flush`（时间基准 = `batchstartabsns`，
   与 f 同式）。

**D8 的另一半**：XMAC 阶段（`xmacBatch`/`accumulateWeights`/`uvshiftAndAverage`/
`copyBaselineWeights`）是 x 现役代码原样，在每个 subint 的站级阶段**之后**串行执行；
`addAutocorrs` 消费本 subint 的 `acrecords`。

### STA / kurtosis（阶段 2 移植）

- `sendSTA`/`sendKurtosis` 与 fxcorr-f 逐字相同；**站级阶段从 ds 并行区内调用**，
  因此调用点带 `#pragma omp critical(fxcorr_sta)`（f 只在 t=0 调、无并发；staSend
  在 container 模式下自带 fopen/fclose，并发调用需序列化）。container 模式的
  `.sta` 落盘路径 = `meta/difxmsg/<exp>_<batch>.sta`（manager 角色无站后缀；f 是
  `<exp>_<batch>_<station>.sta`）。
- P9 的**预平均分支**：`minpostavfreqchannels >= STADumpChannels` 时 STA 前先
  `averageFrequency()`，`AcBatchCollector::collect(mode, datastreamsaveraged=true)`
  跳过自己的平均（core.cpp:1263-1268）——两处必须成对，否则平均被折叠两次。
- kurtosis：`zeroKurtosis` 每 subint 在 setup 段、`sendKurtosis` 在 subint 尾
  （pcal 段之后，f 的 main 次序）；`setDumpKurtosis(dokurtosis)` 在 Mode 构造后设置。

### 其它易错点

- **crosspol 的段内偏移**：`addAutocorrs` 的 `readsection` 在 parallel 段与 cross-pol
  段**各自从 offset=0 起**（只有 resultindex/weightindex 跨段连续）。阶段 2 初版把
  offset 放在 lambda 外跨段累积 → cross-pol 段 `&ac[4096]` 越界读、段错误
  （`genericAdd_32fc_I` 的 src 指向垃圾地址；文件读版从连续流读、无此对应物）——
  由 crosspol 样本抓出，修复处见 integrate.cpp 的注释。
- **槽索引 vs 块索引**：`getFreqs`/`getDataWeight` 的第二个参数是**槽号**（b），
  `getBlockAc`/`getBlockPcal` 的 block 是**全局块号**（index）——混用即静默错数据。
- **MemReader 的 zoom 语义**：Mode 内部 `fftoutputs[zoomband]` 就是父 recorded band
  的切片指针，所以 MemReader 直接用 `getFreqs(band, slot)`、nchan 取 zoom freq 的
  通道数——**不需要** x 当年 SpReader 的 channeloffset 逻辑。
- **ac 记录 = autocorr.bin 的内存镜像**：`AutocorrBatch` 的 ac/acx 是 total band 顺序
  的扁平拼接、w/wx 每 total band 一个（zoom 已映射到父带权重）；`addAutocorrs` 的
  resultindex/weightindex 递推与 crosspol 续段照 X 现役文本逐行移植。
- **pcal.bin 与 PCAL 文本的次序**：`accumulateWeight` 必须在 `zeroAutocorrelations`
  之前；`pcaltext->accumulate` 必须在 `writeSubint`（即 `finalisepcal`）之后——与 f
  的 main 逐处对应。
- **`getWeightReduced`/`setWeightReduced`** 读写 `weights[cp][band]`（ac 聚合权重），
  与 `getDataWeight`（写 .sp/谱视图用的每块权重）**不是同一个数组**。
- **挡掉的位置**：restriction 段在多相位/pulsar/相位阵/STA/kurtosis 上启动即 fail——
  注意它**在 reader 构造之前**，但根一致性检查与 batch.json 读取在其前。
- **时间戳探针**：`g_probes` 的行号必须与 `STAGE_LAP` 站点同步（brief 档输出
  `file:line`）；加行/删行后重新 grep 回填。另：**每个 lap 要用独立 timer**——
  xp 把站级段插在 read 与 zero 之间，zero 若复用 `st` 会把整段站级耗时算进 zero
  （阶段 3 A 段抓出：显示 0.897 s，改独立 timer 后 0.00038 s，与 v8 x 的 0.00036
  量级一致；修复只动计时、产物位一致）。

## 构建与注册

- `configure.ac` 照 fxcorr-x（fxcorrcommon + fftw3f，**含 AC_OPENMP**——ds 级站级并行；
  无 mark5access，不读 VDIF）。
- install-difx 注册 4 处（components 字典、setNormalComponentsFalse、apptargets、
  --doonly 帮助文本）——2026-10-13 已加。
- 测试机上裸构建：`autoreconf -i && ./configure --prefix=/usr/local/difx && make
  && make install`。镜像（`fxcorr/docker/Dockerfile`）的 COPY 行**已加**（2026-10-13，
  随 v9 接入重建：`fxcorr/fxcorr:latest` = `e41dfca36e4f`，容器内 `fxcorr-xpp` 可用）。

## 测试（2026-10-13）

样本副本均在 `/root/fxcortest/`（每个 = 原 workdir 的 `cp -r`，清产物后两条链各跑
一遍；`.input` 的 `OUTPUT FILENAME` 与 config 链（CALC→IM）的绝对路径已改指副本——
原值指向源 workdir，直接跑会把产物写回去）。两条链均 `OMP_NUM_THREADS=1`（并行
位一致单独测）。

### 阶段 3（判据 8，p419 真实规模；全表见 `fxcorr/test/v9/README.md`）

批 00000001（4 站 × 8 ds × 0.512 s，OMP=1 串行）：fp **0.07–0.2 s/任务**（RSS 3.5 MB）、
xp **68.8–69.9 s/组**（RSS 400–417 MB）；对照 v8（f 8.4–8.6 s、x 24.2–30.4 s）单组
串联 **−26%**；xp OMP 扫描 1/2/4/8/16：total 69.5 → 42.3 → 29.3 → **15.6** → 16.1 s
（`station` 完美 2× 阶梯到 8 = 组内 ds 数；`xmac` 1–4 线程平坦、≥8 才分）；fengine
33.77 GB → 2.135 GB（**15.8×**）。**结构验证**：`station` 52.0 s ≈ 8×（f 的
proc 5.41 + reduce 0.94）= 50.8 s——同一份 `Mode::process` 的代价守恒（页缓存口径，
详见 README）。

### 阶段 2（判据 4/6/7）

- **STA / kurtosis**：v9sta（p8reg raw、`CHANS TO AVG=4` 激活 P9 平均分支）上
  v8 f 与 v9 xp 各带 `FXCORR_STA=1 FXCORR_KURTOSIS=1 FXCORR_RUN_MODE=container` →
  `.sta` 记录流 `cmp_sta.py` 对拍 **422 条逐位相同**（0 mismatch），SWIN 逐字节一致。
- **zoom**：v9zoom（batch 指 `test-zoom.input`；freq 0 = 4096ch 主带 / freq 1 =
  1024ch zoom）全量 SWIN：`cmp_swin_zoom.py` 12 条记录 vis 最大相对差 **0** +
  文件**逐字节一致**。
- **crosspol**：v9crosspol（POL PRODUCTS 4 + WRITE AUTOCORRS）全量 SWIN **逐字节
  一致**（**首轮段错误**——修复 `readsection` 的段内 offset 越界，见"关键实现要点"；
  该 bug 阶段 1 无 crosspol 样本、未能暴露）。
- **ds 级并行位一致**：v9mds 上 xp `OMP_NUM_THREADS=1/2/4` 的分片 `.part` **三者
  逐字节相同**，且都等于 v8 基线（v9-plan D8 机理的实测确认）。
- **判据 6（中间物 1×）**：同配置同数据（p4 的 v8 fengine vs v9pcal 的 v9 fengine）
  147.9 MB → 8.58 MB，**≈17.3×**（>16× 的余量来自 autocorr.bin 的消失；每站纯谱
  67.1 MB → 4.27 MB）。
- **内存**：xp 跑 v9mds（4 ds、512 块/subint、4096 通道、OMP=4）峰值 RSS **221 MB**
  （≈55 MB/ds；对照 f 单 ds 46 MB）——≤GB 量级，与 v9-plan §7 的 8 ds 估算相容。
- **失败路径**：截断 stream.bin → `failed to read subint N` rc=1；缺失 → `cannot
  read .../stream.bin` rc=1；fp 侧写不进去（FXCORR_FENGINE_ROOT 指非目录）→
  `cannot create ...` rc=1——三项全部失败可见。
- **回归重跑**（crosspol 修复后）：v9pcal 判据 3/5 复确认全部 IDENTICAL。

### 阶段 1（判据 3/5）

- **判据 3（分片 .part 逐字节，v8 x 分片 vs v9 xp 分片）**——三个样本全过：
  - `v9pcal`（p4 副本：2 站 T1/T2 单组、4 tones pcal、4 subints）：
    `ds0.part` 一致（md5 `f7769313…`）；
  - `v9mds`（mds 副本：每站双极化 2 ds、共 4 ds、无 pcal）：一致；
  - `a1`（**病态数据**：含缺口/filler——病态重放路径一并覆盖）：一致。
- **判据 5（pcal.bin + PCAL 文本逐字节）**——`v9pcal` 上全过（两份 pcal.bin、两份
  PCAL 文本，均逐字节一致）。
- **判据 4 提前验证**（阶段 2 的锚，顺手跑掉）：`v9pcal` 全量模式（无 ds_group）的
  SWIN 与 v8 全量 x 的 SWIN **逐字节一致**（197,052 B）。
- **挡掉实测**：`FXCORR_STA=1` / `FXCORR_KURTOSIS=1` 报错退出（rc=1，消息含
  "not implemented in v9 stage 1"）；pulsar、相位阵配置在其 workdir 上启动即报
  "stage-2 item"。**多相位中心未实测**（测试机的多相位资产 .im 已被覆盖为单源，造
  合法双源配置需要 vex2difx——测试机没有；代码与 pulsar/相位阵同段同通路）。
  顺带确认：非法配置（pointing centre 不是第一个相位中心）下 xp 的 rc=139 段错误
  是 **fxcorr-x 既有行为**（同输入下 fxcorr-x 同样 rc=139），非 v9 引入。
- 未实测项：多相位中心 / pulsar binning / 相位阵（启动挡掉，按需后补）；gaps 七用例
  的 xp 侧重跑（阶段 0 的 f/fp 诊断一致与 a1 病态样本的判据 3 已覆盖，全数重跑属
  编排级回归、随全链回归一起做）；性能标定（阶段 3：fxcorr-xpp 线程数扫描、两链 A/B）。
