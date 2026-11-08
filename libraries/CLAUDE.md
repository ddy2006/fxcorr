# libraries 目录说明

**最后更新**：2026-09-27（fxcorrcommon 加**分片接口**：`Visibility` 的 `setActiveBaselines` / `setActiveDatastreams` / `setOutputPath`，见下节末条；09-20：新增 `fxcorrpath.{h,cpp}` 目录根解析——本库首次引入路径策略）

14 个共享库，每个都是独立 autotools + libtool 包，通过 pkg-config 相互发现（无统一根构建）。fxcorrcommon 已建成。

## 库一览

| 目录 | 版本 | 作用 |
|---|---|---|
| difxio | 3.8.1 | 解析/生成 DiFX 配置与模型文件（.input/.im/.calc/.flag/.threads），核心结构 DifxInput |
| fxcorrcommon | 0.1 | 去 MPI 相关核心（station-based 算法 + 配置/模型 + SWIN 写盘），mpifxcorr libfxcorr.a 的共享库版，见下节 |
| difxmessage | 2.9.0 | 组播状态/告警/参数消息收发；内含 mark5ipc 子库（Mark5 锁） |
| mark5access | 1.7 | 原始基带解包：mark5_stream 两段式 API，支持 Mark4/Mark5B/VLBA/VDIF/CODIF 等 |
| vdifio | 1.6 | VDIF 帧头、vdifmux 多线程合流、corner-turner、vdifreader |
| codifio | 0.9 | CODIF 帧头解析（mark5access/vdifio 可选消费） |
| dirlist | 0.2 | Mark5/Mark6 介质目录清单 |
| mark6meta | 0.4 | Mark6 .meta 元数据读写 |
| mark6sg | 2.0.4 | Mark6 scatter-gather 类文件访问（含 jsmn JSON） |
| beamformer | 0.1.0 | 波束合成（Armadillo+BLAS+LAPACK） |
| python | 1.4 | DiFX Python 包（difxdb/difxfile/difxutil 等） |
| rpfits / vex / perl | — | RPFITS 格式、VEX 解析（bison/flex）、Perl 模块；走独立构建分支 |

## 关键库接口速查（fxcorr-f / fxcorr-x 依赖）

- **difxio**：头 `difxio.h`（顶层聚合）→ `difxio/` 子目录。`loadDifxInput()` / `loadDifxCalc()` 读配置；`evaluateDifxInputDelayRate()` / `evaluateDifxInputUVW()` 算延迟/UVW（fxcorr-x 必需）；`DifxParameters` 是 .input 底层解析器。**注意：mpifxcorr 目前不用 difxio**（自带 configuration.cpp 解析）。
- **difxmessage**：唯一公开头 `difxmessage.h` 装在 `$(includedir)` 根。`difxMessageInit(mpiId, identifier)` / `difxMessageReceiveOpen` / `difxMessageSendDifxStatus*`；组播地址由环境变量 `DIFX_MESSAGE_GROUP` / `DIFX_MESSAGE_PORT` 给定（setup.bash 默认 224.2.2.1:50201）；expat 为强制依赖。
- **mark5access**：`mark5_stream` 两段式构造（source + format），`new_mark5_format_mark5b/vdif/vlba/...`；解包入口 `mark5_unpack(ms, packed, unpacked, nsamp)` / `mark5_stream_decode`；`mark5_stream_open()` 自动探测格式；`USE_CODIFIO` / `USE_MARK6SG` 条件编译。
- **vdifio**：头 `vdifio.h` 装 `$(includedir)` 根。帧头读写（createVDIFHeader / getVDIFFrameMJD / setVDIF...）、`vdifmux()` 合流、`getCornerTurner()`、`vdifreader*` 多线程读取。

## 新库样板（照抄 mark6meta，最小 4 手写文件）

```
libraries/<name>/
├── configure.ac        # AC_INIT + LIBRARY_VERSION + LT_INIT + AC_PROG_CXX
│                       # + PKG_CHECK_MODULES(依赖) + AC_CONFIG_FILES([Makefile <name>.pc src/Makefile])
├── Makefile.am         # SUBDIRS = src；pkgconfigdir = $(libdir)/pkgconfig；pkgconfig_DATA = <name>.pc
├── <name>.pc.in        # Name/Description/Requires/Version/Libs: -L${libdir} -l<name>/Cflags: -I${includedir}
└── src/Makefile.am     # lib_LTLIBRARIES = lib<name>.la；-version-info $(LIBRARY_VERSION)
```

要点：`LIBRARY_VERSION` 必须 `AC_SUBST`；`AC_CONFIG_FILES` 漏列子目录 Makefile 会报 "cannot find Makefile"；若用 AX_OPENMP 需在 `m4/` 放 `openmp.m4`（参考 vdifio/m4/）。

## fxcorrcommon（已建成）

- 源 = mpifxcorr 的 `libfxcorr_a_SOURCES` 11 文件（configuration/pcal/mathutil/sysutil/mode/mk5mode/polyco/visibility/model/datamuxer/alert）+ 公共头 architecture.h/mpifxcorr.h/fraction.h（去 MPI 改动见 `mpifxcorr/CLAUDE.md` 与 fxcorr/v1-plan 2.1）；**2026-09-13 加 switchedpower.{h,cpp}（P6）**：SwitchedPower 类去 MPI 拷贝（构造 (conf, configindex, dsindex)、datastreamId = mpiid-1 = dsindex），feed 接口下沉为 feed(u8*, nbytes)（内部构造 mark5_stream_memory + formatname，fxcorr-f 零 mark5access 依赖）。
- 依赖（configure.ac 探测）：difxmessage >= 2.9.0、mark5access >= 1.7、vdifio >= 1.6、codifio >= 0.2（CODIF_HEADER_BYTES 必需）；IPP 可选否则 fftw3+fftw3f；mark6sg/mark5ipc/dirlist 零引用不探测。
- 头装 `$(includedir)/fxcorrcommon/` 子目录（避免与 mpifxcorr 同名头冲突）；**pcal.h 引用 fraction.h，fraction.h 必须进安装头清单**（曾误归 internal 导致 fxcorr-f 编译失败）。
- **目录根解析（V5 P5，2026-09-20）**：新增 `fxcorrpath.{h,cpp}`（类 `FxcorrPath`）——四个可重定向根的三档回退（`FXCORR_<X>_ROOT` 已设置就用它，否则 `<workdir>/<规范目录名>`）。各工具在解析完 workdir 后调一次 `init()`；`root()` 惰性求值并缓存；`FXCORR_PRINT_ROOTS=1` 时 `print()` 打印各根与来源；`checkRoots()` 与编排层写的 `meta/roots/<batch_id>.json` 比对（**只比调用方用到的根**，否则会被与自己无关的根误伤）。**2026-10-04 起比对前两侧先归一化**（"等于自己那份 workdir/<规范名>"折成 `./<名>`、末位 `/` 归一）：记录写的是**布局**、不是某个挂载视图下的路径，容器化的路径别名（宿主 `/shared/mydata/x` vs 容器 `/cluster_data_root/x`）不再误报，旧式绝对记录经一次折叠即等价（`data-spec` 5.2.1）。**这是本库首次引入路径策略**（此前是上游代码的忠实副本，只有 log.h/ompcompat.h 一类库外小改）——四个打开点接了线：`configuration.cpp` 的 `CALC FILENAME`（拼 `.input` 所在目录）、`OUTPUT FILENAME`（拼 `FXCORR_VIS_ROOT`）、DATA TABLE 的 `FILE d/d:`（拼 `FXCORR_RAW_ROOT`），以及 `model.cpp` 的 `IM FILENAME`（拼 `.calc` 所在目录）。**只对相对路径拼根，绝对路径一律原样**——真实观测的 `FILE` 行就是绝对路径，改写它会读错文件。规则权威是 `fxcorr/data-spec.md` 5.2.1，bash 侧同一套规则在 `fxcorr/roots.sh`（两处必须同改）。
- **分片接口（V5 分片改造，2026-09-27）**：`Visibility` 新增 `setActiveBaselines` / `setActiveDatastreams` / `setOutputPath` 三个 setter——**默认（空/未调用）时所有循环与改动前逐字节一致**。fxcorr-x 的分片模式用它把记录写到 `vis-parts/<batch_id>/ds<G>.part`、并只写本分片的基线与自相关（**autocorr 段必须过滤**：它无条件写，不像基线记录要 `weight > 0`）。`setOutputPath` 的**第一次写截断、后续追加**（一次运行要写多个积分周期，重跑分片却要覆盖自己的 `.part`）。**注意 `threadcrosscorrs` 的布局回放不受这套屏蔽影响**——偏移是 baseline 号的固定函数，跳过推进会全盘错位（见 `applications/fxcorr-x/CLAUDE.md`）。
- **IPP 必须关掉，而且"关"要靠构建命令（2026-09-28，V6 S2.5 调查发现）**：`build.md:19` 早就写明"无 IPP 的环境必须带 `--noipp`"——它的作用是**跳过生成 `ipp.pc`**，于是本库 configure 里的 `PKG_CHECK_MODULES(IPP, ipp, ...)` 失败、`HAVE_IPP` 不被定义。**漏了 `--noipp` 的代价不是慢，是错**：IPP 构建下 `architecture.h` 把 `vectorAlloc_f64` 映射到 `ippsMalloc_64f`，而 `model.cpp` 用它分配的 `Model` 延迟模型数组**数值不对**——fxcorr-sim 生成的仿真数据延迟偏差约一个样本，表现为**极少数 2bit 量化电平翻转**（t25362 4 站 1.024 s batch：33511 字节 / 131 MB，0.025%），**全程没有任何报错**。三个 fxcorr 工具都不用 IPP（fxcorr-sim 的 FFT 走 fftw3f）。**排查提示**：这个缺陷只在"真实观测参数 + 延迟非零 + 逐字节对拍"三者齐备时暴露——坐标复制的合成站（延迟恒为 0）测不出来，小配置也测不出来。
- **顺带修掉的真 bug（同日）**：`configure.ac` 里 `AC_DEFINE([HAVE_IPP])` 挂在 `PKG_CHECK_MODULES` 的成功分支上，`--disable-ipp` 只设 `ipp_enabled`、管不到它——选项形同虚设，且**默认启用**。现已让选项真正 gate 探测，并把默认值翻转为禁用（`--enable-ipp` 才开）。这样即使某台机器上 `ipp.pc` 存在，也不会再静默启用。
- **`model.cpp` 的 `clock` 释放**（同日，与之独立的另一处上游疏漏）：`vectorAlloc_f64` 分配、`delete []` 释放，在 IPP 构建下必崩（`double free or corruption`），无 IPP 时因两者等价而相安无事。已改为配对的 `vectorFree`。
- **`configuration.h` 九个裸指针成员未初始化**（2026-09-29，在目标集群的 `-O2` 下暴露，**又一例同类上游疏漏**）：`Configuration` 的析构函数**无条件** `delete` 它们，而分配都藏在"文件能打开"的分支里——最典型的是 `numprocessthreads`，只在 `.threads` 存在时才 `new`（`configuration.cpp:1798`），没有 `.threads` 的配置下它一直是未初始化的垃圾值。**`-O0` 下栈上恰好是 0，`delete [] NULL` 合法，所以一路看不出来；`-O2` 下直接段错误**，而且崩在自由退出时（`main.cpp` 的 `Configuration` 栈对象析构），**把真实错误全盖住**（"SWIN 已有记录"那条提示就被吞成了 `Segmentation fault`）。九个成员（`numprocessthreads` / `scanconfigindices` / `configs` / `rules` / `freqtable` / `telescopetable` / `baselinetable` / `datastreamtable` / `model`）全部初始化为 `NULL`。**排查提示**：见到"程序把活干完了、产物也对，却在退出时段错误"，先怀疑这个模式——`gdb` 里会停在 `free()` ← `Configuration::~Configuration` ← `main`。
- **ds 组推导（2026-10-09）**：`configuration.{h,cpp}` 加两个自由函数——`deriveDsGroups(config, configindex)`（BASELINE 表连通分量 → ds 组，组序 = 组内最小 ds 序）与 `groupOfDs(groups, ds)`（ds 全局序号 → 组号）。从 fxcorr-x 应用内的 static 副本挪入，**fxcorr-f（fengine 按组落盘）与 fxcorr-x（分片/读路径）共用同一实现**。**四处同规则、必须同改**：此处、`fxcorr/fxinput.py` 的 `derive_ds_groups`、`../app-fxcorr/router/internal/fxin`（Go）；机理见 `data-spec` 第 8 节。
- 实用工厂：`Configuration::getMode(configindex, dsindex)`（各 Mode 子类创建，fxcorr-f 直接用，无需暴露 clock offsets 等内部 getter）。
- `fxcorr.pc` 名字被 mpifxcorr 占用（"不含 MPI 的对象库"），本库用 `fxcorrcommon.pc`。
- **按块累加器（V7 P3，2026-09-30）**：`Mode` 新增一组**可选**的 per-block 累加器——`enableBlockAccumulators()` / `clearBlockAccumulators(block)` / `getBlockAc()` / `getBlockWeight()` / `addWeightReduced()`，`acTarget()` 是 autocorr 的写入口。**默认关闭**（`blockacblocks == 0`），关闭时与改动前**逐字节相同**，mpifxcorr 与串行路径不受影响。
  **为什么需要**：autocorrelation 与 weight 是**"归约型"产物**——每个块加进**同一个**累加器，结果依赖加法顺序。而 fxcorr-f 的并行形式给每个线程**一段连续的块**，把各段的和再加起来 ≠ 串行逐块左结合（浮点加法不结合）。对比之下 `fftoutputs` 是**"位置型"**：每块的贡献落在自己的槽位、归约就是 `memcpy`，**天然与顺序无关**。所以同一个并行实现里，`.sp` 一直是逐字节正确的，`autocorr.bin` 却错了 **85% 的元素**——这是个静默的错数据，`OMP_NUM_THREADS=1` 与 `=8` 才看得出来。
  开启后每块的贡献落在自己的槽位、主线程按块序累加。判据：**同一个 f 任务在 `OMP_NUM_THREADS` 1 / 8 / 24 下，`.sp` 与 `autocorr.bin` 三者逐字节相同**。布局取 `[crosspol][block][band][chan]`（block 维在前，给将来的加速器留的形状，见 `v7-plan.md` §19.3）。
  **改一处不够**：`process()` 里 autocorr 的写入要经 `acTarget()`、weight 的累加要经 `addWeight()`——`mode.cpp` 原有**六处** `weights[...] += ...` 是直接写的、绕过 `addWeight`。只改 autocorr 时差异从 530122 字节降到 9440 字节，**剩下那 9440 全在 acweight 上**（每条记录一个 float，正是 band 段的 weight 字段）。`clearBlockAccumulators` 由 `process()` 自己在开头调用（每块每 subint 只处理一次），**不要**改到 `zeroAutocorrelations()` 里去清全部槽——那会为每个 Mode 每个 subint 白写 ~10 MB。
  **第二批（2026-10-11）**：接口扩展为 **`enableBlockAccumulators(int slotcount)`**（只分配本线程的槽）+ **`setBlockSlotBase(int)`**（全局块索引→槽的映射，每并行批由调用方设置；`slot = index - blockslotbase`）——全量槽在 32000 块配置下 ~1 GB/副本、首次触碰的 page fault 随线程数叠加（OMP=8 白付 3.7 s），按段分配后 RSS 7.9→1.8 GB、blk 3.7→0.8 s。新增 **per-block pcal 槽**（`getBlockPcal`/`addPcalBlock`/`blockPcalCapable()`，配 `PCal` 的 `getAccumSize/getAccum/addAccum/resetAccum`——**只有线性原始累加器能这样归约，finalize 后的 DFT 输出不行**）与 weight 的 **`getWeightReduced`/`setWeightReduced`**（供分片归约做读-改-写、保持串行加序）。`Mode::addPcal`（段归约，实际是死代码）已删。
- **无 phasecal 配置下的崩溃修复（2026-10-13，v9 阶段 0 发现）**：`PCal ** extractor` /
  `cf32 ** pcalresults` 只在 `getDPhaseCalIntervalHz > 0` 时分配（`mode.cpp` 构造函数），
  此前**不在成员初始化列表里**——无 pcal 的配置下它们是未初始化指针。P3 第二批的
  `blockPcalCapable()` 用 `extractor == 0` 表示"无 pcal 提取器"，于是**无 pcal 配置在
  `enableBlockAccumulators` → `blockPcalCapable` 处必崩**（SIGSEGV；P3 之前的代码只在
  `haspcal` 保护下碰 extractor，这个未初始化一直没暴露；P3 的回归面全是带 pcal 的配置）。
  修复：成员初始化列表加 `pcalresults(0), extractor(0)`（析构本就用
  `getDPhaseCalIntervalHz > 0` 守卫，构造/析构对称）。回归：`fxcorr/test/gaps/` 七个
  用例（无 pcal 的 test 配置）全 PASS。
- **OpenMP 检测的语言顺序（2026-11-08，band 并行排查发现）**：`configure.ac` 里
  `AX_OPENMP` 探测的是**当前语言**的 flag。此前 `AX_OPENMP` 写在 `AC_LANG(C++)`
  之前，实际检测的是 C 编译器（日志：`checking for OpenMP flag of C compiler`），
  只设置 `OPENMP_CFLAGS`；`CXXFLAGS` 里的 `${OPENMP_CXXFLAGS}` 为空 →
  **整个库从未带 `-fopenmp` 编译**，`mode.cpp`/`datamuxer.cpp` 的 `#pragma omp`
  全部静默失效（实测：镜像内 `libfxcorrcommon.so` 无 `GOMP_parallel` 符号）。
  修复：把 `AC_LANG(C++)` 移到 `AX_OPENMP` 之前。任何"库内新增并行 pragma
  但不见加速"的排查先确认这一条。
