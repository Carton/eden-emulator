# Eden 本地开发笔记（Windows，仅本地使用）

> **交接文档：性能优化全流程（基线/构建/输入自动化/profiling 管线/结论/脚本用法）已整理为
> `PROFILE_PROGRESS.md`（本仓库根目录，随 git 跟踪），新会话/新 agent 从那里开始读。**

## 项目简介

Eden 是开源（GPLv3）Nintendo Switch 模拟器，yuzu 血统的社区延续项目。
C++20 + CMake（≥3.31）构建，支持 Windows / Linux / macOS / Android 等平台。
本仓库：`https://git.eden-emu.dev/eden-emu/eden`（GitHub 镜像：`https://github.com/eden-emulator/mirror`）。
构建文档见 `docs/Build.md`、依赖说明见 `docs/Deps.md`。

## 当前工作基线（2026-09-06 实测）

- **TOTK 可玩基线 = `F:\devel\opensource\eden-v0.2.1`**（tag v0.2.1 = 58c1e20 的 git worktree），
  用 VS2022 编译的 RelWithDebInfo，`build/bin/` 下 eden.exe(+pdb) 实测王国之泪正常游玩，性能与官方版相当。
  该目录已配置好（build/ 与 build/bin/user/ 均就绪），改代码后直接 `cmake --build build` 增量编译即可。
- master（F:\devel\opensource\eden-emulator）目前**不能**用于 TOTK，见"已知问题"。

## 本机构建环境（2026-09-06 验证通过）

- Windows 11 x64，12 逻辑核，NVIDIA RTX 2060（驱动 591.86）
- Visual Studio Community **2022** @ `D:\Program Files\Microsoft Visual Studio\2022\Community`
  （MSVC 14.44，cl 19.44.35228；VS2026 已卸载）
  - CMake 3.31.6 / Ninja 为 VS 自带，**不在系统 PATH**，路径：
    - `D:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`
    - `D:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja`
- Git for Windows（Git Bash）+ Python 3.12（CPM 下载依赖需要）
- **glslang 16.5.0 独立版** @ `G:\Tools\glslang`（提供 `glslangValidator.exe`，构建期编译 shader 用。
  这是完整 Vulkan SDK 唯一被构建用到的组件；LunarG 官方安装器静默安装需要 UAC 提权，故未使用）
- 备选工具（G 盘，平时用不到）：`G:\Tools\cmake-portable`（CMake 4.1.1）、`G:\Tools\ninja`、
  `G:\Tools\llvm23`（clang-cl 23.1.0）
- 其余依赖（Qt 6.11.1 静态版、SDL3、FFmpeg、OpenSSL、Boost 等）由 CPM 在配置期自动下载到
  `.cache/cpm`（约 1 GB），无需手动安装；下载走 http_proxy 环境变量

## 快速编译（Git Bash）

```bash
# —— 主力：v0.2.1 基线（TOTK 可玩），已配置好，改代码后直接增量编译 ——
cd /f/devel/opensource/eden-v0.2.1
source /f/devel/opensource/eden-emulator/tools/windows/load-msvc-env.sh   # vswhere 自动找到 VS2022
export PATH="/g/Tools/glslang/bin:$(dirname "$(command -v cl.exe)"):/d/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin:/d/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja:$PATH"

cmake.exe --build build          # RelWithDebInfo，产物在 build/bin/

# —— 如需重新配置（仅首次/改 CMake 选项时）——
cmake.exe -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DYUZU_TESTS=OFF \
  "-DCMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO=/DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF"
```

注意：
- `/usr/bin/link.exe` 会遮挡 MSVC 链接器，务必保持 MSVC bin 目录在 PATH 前面（上面的 export 已处理）。
- `load-msvc-env.sh` 里 `vcvarsall.sh` 通过 vswhere 找"最新 VS"，当前机器即 VS2022；如装多版本需设 `VSINSTALLDIR`。
- RelWithDebInfo = `/O2 /Ob1 /Zi /MD` + `/DEBUG /OPT:REF /OPT:ICF`，性能版带调试符号，正是 profile 用的配置。

## 产物

| 位置 | 说明 |
|---|---|
| `eden-v0.2.1/build/bin/eden.exe` (+`.pdb`) | **当前主力**：Qt GUI 主程序（静态 Qt），TOTK 实测可玩 |
| `eden-v0.2.1/build/bin/eden-cli.exe` (+`.pdb`) | 命令行版（本机跑 TOTK 会在 shader 编译段崩溃，勿用于 TOTK） |
| `eden-v0.2.1/build/bin/eden-room.exe` | 独立联机房间工具（LDN 多人），单机 profile 用不到 |
| `eden-emulator/build/bin/*` | master 构建（VS2026 产物），TOTK 卡 launching，暂不可用 |

## 运行说明

- exe 旁边放 `user/` 目录即进入 portable 模式；否则使用 `%APPDATA%\eden`
- 密钥/固件/游戏路径均在数据目录配置；可从现有官方安装复制 `user` 目录（保持原安装不动，**F:\Switch\Yuzu 只读**）
- 复制的 `user/config/qt-config.ini` 里数据目录是绝对路径，需要逐条改成新 exe 的目录
- 日志在 `user/log/eden_log.txt`（每次启动覆盖）
- Vulkan / OpenGL 由运行时从显卡驱动加载，exe 无需随附任何 DLL
- 配置项坑：ini 里 `key\default=true` 表示"用编译默认值"，此时即使写了 `key=false` 也被忽略；
  要生效必须同时改 `key\default=false` 和 `key=...` 两行

## 已知问题（本地排查记录，勿外传/勿提上游）

1. **master（VS2026/MSVC 14.51 构建）**：TOTK 卡在 launching。sm_controller 的
   IpcController 服务明明注册了命令 3（QueryPointerBufferSize），运行期却查不到 → 游戏拿到
   未实现响应后 svc Break 自杀。官方同期版本 GUI 无此问题 → 疑似 MSVC 14.51 误编译或代码回归，未定论。
2. **master + VS2022**：链接失败，LNK2019 `__std_rotate`/`__std_replace_copy_1` 等——CPM 下载的
   静态 Qt 6.11.1 是上游 CI 用更新 STL 编译的，VS2022 的 14.44 STL 没有这些向量化符号。
   要用 VS2022 编 master 需自编 Qt 或等上游换包。
3. **eden-cli + TOTK**（任何版本，官方同期版也一样）：shader 编译阶段
   `CollectStorageBuffers`（global_memory_to_storage_buffer_pass.cpp）segfault → 时代性代码 bug。
   **profile 一律用 GUI 的 eden.exe**。
4. 曾在工作区的 3 个 clang-cl 兼容补丁（vk/gl_graphics_pipeline.cpp 的 `!defined(__clang__)`
   守卫、image_base.cpp 的 find_if 改写）已于 09-07 丢弃——MSVC/VS2022 编译不需要它们。
   若将来复活 clang-cl 路线需重打：守卫加在 `LAMBDA_FORCEINLINE` 定义处，find_if 改写避开
   MSVC 14.5x STL 的 `_Find_vectorized` static_assert。

## Profile 工作流（TOTK 性能热点）

本机可用的 profiling 工具（均已确认安装）：

| 工具 | 位置 | 用途 |
|---|---|---|
| Nsight Systems 2024.4.2 | `C:\Program Files\NVIDIA Corporation\Nsight Systems 2024.4.2\` | **首选**：CPU/GPU 全景时间轴 |
| VS2022 Performance Profiler | VS IDE + `Team Tools\DiagnosticsHub\Collector\VSDiagnostics.exe` | 函数级 CPU 热点、火焰图 |
| wpr.exe + **ETW MCP** | `C:\Windows\system32\wpr.exe` 采集；`.etl` 由本会话的 ETW MCP 直接分析 | CPU 采样栈、线程调度、ReadyThread、内存/IO 的符号级查询——**不需要 WPA** |
| Nsight Compute 2024.3.0 | `C:\Program Files\NVIDIA Corporation\Nsight Compute 2024.3.0\` | compute kernel 级（图形场景暂用不上） |

eden 代码里**没有** Tracy/microprofile 之类的内建插桩（externals 里的 renderdoc 是图形调试用），
所以走采样式 profile；RelWithDebInfo 的 /Zi PDB 直接可用。注意链接参数 `/OPT:ICF` 会把相同
函数体折叠，火焰图里个别符号会合并，属正常噪声。

### 步骤

**第 0 步：基线观测（不采集，先看）**
- GUI 里开叠加层：模拟 → 配置 → 叠加层，勾"显示 FPS / 帧率图"
- 区分两种慢：首次进新场景的 **shader 编译卡顿**（pipeline cache 会缓存，第二次进就顺）vs
  **持续低帧**。优化目标按后者来，别把编译 stutter 当热点。

**第 1 步：Nsight Systems 全景采集 60–90 秒游戏画面**

```bash
cd /f/devel/opensource/eden-v0.2.1/build/bin
NSYS="/c/Program Files/NVIDIA Corporation/Nsight Systems 2024.4.2/target-windows-x64/nsys.exe"

# pass 1：低开销（WDDM GPU 占用 + CPU 采样）
MSYS_NO_PATHCONV=1 "$NSYS" profile -t wddm -d 90 --force-overwrite=true \
  -o totk_p1 ./eden.exe "F:/Switch/Games/塞尔达王国之泪/[APP][0100F2C0115B6000][1.0.0][US][16.0.0].nsp"

# pass 2（需要看 Vulkan API 调用时）：加 vulkan trace，开销略高
MSYS_NO_PATHCONV=1 "$NSYS" profile -t wddm,vulkan -d 90 --force-overwrite=true \
  -o totk_p2 ./eden.exe "F:/Switch/Games/塞尔达王国之泪/....nsp"

# 看结果
"/c/Program Files/NVIDIA Corporation/Nsight Systems 2024.4.2/host-windows-x64/nsys-ui.exe" totk_p1.nsys-rep &
```

**第 2 步：判定 CPU-bound 还是 GPU-bound**
- 时间轴里 GPU 3D engine（WDDM 行）持续接近满载 → GPU-bound；反之 CPU-bound。
- CPU-bound 时看各线程占比（eden 线程名直接可读）：JIT/CPU 线程、`gpu_thread`、
  shader 编译线程池。哪个吃满哪个就是主战场。

**第 3 步：CPU-bound → 函数级热点（两条路线，可并用）**

- 路线 A —— VS2022 性能探查器（交互式，看火焰图）：VS IDE 调试 → 性能探查器 → 勾"CPU 使用率"，
  启动 eden.exe + TOTK，玩 60s 停止。PDB 自动加载。
- 路线 B —— ETW 采集 + AI 分析（无 GUI，AI 可自主迭代）：

  ```bash
  mkdir -p /f/prof
  MSYS2_ARG_CONV_EXCL="*" wpr.exe -start CPU -filemode   # 开始采集（含 CPU 采样栈，需管理员）
  # …… 玩 60–90 秒 ……
  MSYS2_ARG_CONV_EXCL="*" wpr.exe -stop /f/prof/totk_cpu.etl
  ```

  然后在会话里让 AI 用 ETW MCP `process_trace` 该文件（类别：Sampled CPU Usage Data、
  CPU Scheduling Data、Ready Thread Data、Stack Data…），直接查询每线程热点函数、
  做聚合统计和关键路径分析。eden 自身符号用 build/bin 里的 PDB；首次分析系统 DLL
  需配一次符号路径（MS symbol server，缓存目录设 D:\SymCache，符号包设
  srv*D:\SymServer*https://msdl.microsoft.com/download/symbols）。

- 预期热点区域（yuzu 血统经验）：shader recompiler、texture cache、内存访问
  （Memory::ReadBlock）、JIT（ARM 重编译）、Maxwell 命令处理。

**第 4 步：GPU-bound → 先做设置 A/B，再考虑帧分析**
- 逐项对照（每次只改一个）：GPU 精度（Normal/High）、分辨率倍数、异步呈现、
  异步 shader 编译。对照方式：重复第 1 步采集，直接对比时间轴。
- 若确需逐帧分析图形管线，再下载 Nsight Graphics 或 RenderDoc（装 D/G 盘）。

**纪律**：分析阶段不开 PGO；采集时关掉后台下载/浏览器；CLI 版崩溃是已知问题，别浪费时间排查。

## Profile 实战记录（2026-09-07，TOTK 卡卡利科村场景实测）

### 已验证的采集管线（推荐，替代 nsys）
1. **输入自动化**（免手柄）：`F:\prof\` 下 `auto_play.ps1`（抢焦点+SendInput 注入 X 键）+
   `focus_test.ps1`（单发测试）。前置条件：qt-config.ini 里 `keyboard_enabled=true` 且
   player_0 按钮 `engine:keyboard`（原手柄配置备份在 `qt-config.ini.bak-controller`，测完还原）。
   注意：游戏窗口必须被脚本强制切到前台再注入，否则按键会打进别的窗口（Windows 前台锁：
   用户正在打字时抢不赢，需用户停手配合）。
2. **采集**：`wpr -start CPU -start GPU -filemode` → 玩 60-100s → `wpr -stop xxx.etl`。
   start+sleep+stop 必须在**同一个提权脚本**里执行（脚本间分离会丢采集会话）。
   游戏用**普通权限**跑（输入自动化已验证），wpr 系统级采集不受目标进程权限影响。
3. **分析**：ETW MCP `process_trace` → `start_new_query/perform_query`。
   坑：MCP 单次调用 30s 超时，但**处理在后台继续**，超时后 `list_traces` 确认完成再查询；
   FunctionName 分组在大 trace 上超时，先按 ModuleName 分组，加过滤条件后再按函数分组。
   符号：`save_symbol_configuration`（symcache=D:\SymCache + MS 符号服务器）。
4. nsys 路线的问题：`-t wddm` 需要 nsys 提权启动游戏 → 游戏成提权进程 → 自动按键失效、
   computer-use 截图被 UIPI 挡、报告生成 10 分钟且被误关窗口就前功尽弃。wpr 管线全胜。

### TOTK 性能画像结论（RTX 2060，卡卡利科村 45→37 FPS 场景，99s 采样）
- eden.exe 持续占用 ≈5.2 核；**四个线程打满**：CPUCore_0/1/2（TOTK 正好 3 个模拟核）
  + GPU 线程（模拟器命令线程）→ **CPU 侧瓶颈，显卡有余量**（驱动 CPU 开销合计 <1%）。
- 模拟核 86% 时间在跑 JIT 后的游戏代码（匿名内存），HLE 服务仅 ~5% → 游戏核本身难再省。
- GPU 线程 67% 在 eden 自身代码，热点（CPU 时间）：`DmaPusher::ProcessCommands` 4.9s、
  `Maxwell3D::ProcessDirtyRegisters` 4.2s、`DmaPusher::CallMethod` 3.8s、
  `PushImageDescriptors` 2.0s、`BufferCache::BindHostGraphicsUniformBuffer` 2.0s、
  `MemoryManager::GpuToCpuAddress` 1.5s、`FixedPipelineState::Refresh` 1.2s、
  TextureCache LRU `Touch` 1.3s、`GraphicsPipelineCacheKey::operator==` 0.7s
  ——全是**每 draw 的命令处理/缓存查找开销**，与驱动无关。
- 优化方向：降 GPU 线程每 draw 成本（脏寄存器、绑定缓存、管线键比较）；
  模拟核侧看 fastmem/JIT 质量与精度设置。VulkanWorker 只用了 50%，有调度空间。

### 瓶颈判定实验（2026-09-07）
- 设备 GPU 利用率（nvidia-smi 2s 采样）：1x 分辨率 **37%**（43 FPS）；0.25x 分辨率 **25%**（45 FPS）。
  像素负载砍 4~16 倍 FPS 仅 +2 → **设备 GPU 不是瓶颈，帧率完全被 CPU 侧流水线卡住**。
- 模拟核非空转：CPUCore 采样 86% 在 JIT 游戏代码（eden.exe 自身仅 5%，排除 fence 自旋嫌疑）。
- 结论：优先优化 GPU 命令线程（eden 侧每 draw 开销，靶点明确）；模拟核是次级天花板。
- 实验后分辨率配置已还原（`resolution_setup\default=true`）。

### GPU 命令线程优化第一轮（2026-09-08 夜，详见 PROFILE_PROGRESS.md §6）
- 五项微优化已提交 v0.2.1 worktree（7f1f534cd0，仅本地）：寄存器冗余写过滤（ProcessDirtyRegisters
  4.23→1.66s）、LRU touch 帧内去重（两处合计 2.75→0.85s）、管线键 transition 哈希预比较、
  uniform 对齐缓存、SSBO/TBO buffer_id 复用。GPU 线程总 CPU 105.9→102.4s（-3.3%），渲染验证通过。
- **FPS 不动**（42-45 噪声内）：4 线程全饱和（3 JIT 核 + GPU 线程），帧时长=max(各级)；
  中位帧时锁 24.99ms。下一杠杆在 JIT 模拟核（fastmem 生效性 / CPU 精度档），非 GPU 线程。
- FPS 基准管线：eden 内建 `record_frame_times=true`（退出时逐帧 CSV）+ `confirmStop=2`（优雅关闭）+
  `F:\prof\bench_run.py`（自动进游戏+测量+统计，历史在 bench_results.csv）。基线 43.21/44.65 FPS。
  已封装 skill `eden-bench`（构建/基准/采集速查）。

### 第二轮：fastmem 排查 + CPU 精度 Unsafe A/B（2026-09-08 深夜，详见 PROFILE_PROGRESS.md §6.3）
- **fastmem 无问题**：缺页全家函数仅 0.08s/111.8s（0.07%），几乎无 miss；"6.6% 内核开销"
  实为大半 ETW 采集自身抓栈成本 + 饱和负载调度税——解读 kernel 占比时记住这点。勿再查。
- **CPU 精度 Unsafe：+2.0 FPS（44.8 vs 42.8）且 33ms 卡顿尖刺全消**（p99 33.4→25.4ms），
  机制含 fastmem 地址位宽 39→64 免边界检查。qt-config 已留在 Unsafe；还原改 `cpu_accuracy=0`。
- JIT 核 85.7% 纯游戏代码 → 无低垂果实，Unsafe 即该侧现实杠杆。skill 已迁至
  `.agents/skills/eden-bench`（随仓库跟踪维护）。

### JIT 模拟核攻坚轮（2026-09-09，详见 PROFILE_PROGRESS.md §11）
- 计数器实测：TOTK 会话编译 **~130 万块，97% 集中在启动 30s 爆发（23k/s）+ 加载/入局**，
  其后涓流趋零（早期"持续 6.3k/s"系测量窗口落在爆发尾巴，§11.8 已修正）；核内 99% 不同 PC、
  均匀铺满 ~48MB → 热块分级类优化无效。512MB→2GiB 码缓存改动保留（c2d9148f7f，
  reserve-only 无代价；但"玩久了变卡=缓存打穿"假说被时间戳数据削弱——常规一局 ~200MB 打不穿）。
- LTO 全量构建 A/B：**中性**（44.82 vs 44.80，build-lto/ 目录保留）；PGO 跳过。
- 领域调研：dynarmic 上游已死（azahar fork 唯一活跃）；FEX 的 BL→host call、RA 内联、
  共享 JIT 缓冲是可借鉴大方向；TSO/x87/LTO/PGO 不适用或已证无效。剩余路线见 §11.6。

### JIT 块级取证 + FEX 代码级调研（2026-09-09 深夜，详见 PROFILE_PROGRESS.md §11.8/11.9）
- 块 dump 实验（`EDEN_JIT_BLOCKDUMP=1` + `F:\prof\analyze_blocks.py`）定量回答"重复内容"：
  **51% 编译是跨核重复**（57.7% 块被 ≥2 核各编一份——三套 JIT 互不知情）；
  **21.8% 不同 PC 内容字节相同**（模板/静态库多副本，需重定基才能去重）；
  核内重复仅 0.3-0.8%（单核 JIT 无缺陷）；块中位 4 条指令、29% ≤2 条（游戏分支密，
  DMB/DSB 不切块，62% 块以条件分支收尾）；**guest 模块基址逐会话漂移**
  （磁盘码缓存键必须模块相对+内容 hash）。**时间戳数据推翻"持续编译流"**：
  97% 编译在启动 30s 内（23k/s 爆发），菜单 6 分钟静置后趋零——编译税前置，
  稳态杠杆在执行质量（BL→host call/块合并），编译量优化主攻启动/加载。
- FEX-Emu 已克隆 `F:\devel\opensource\FEX`（@208e9c3）三大招代码级核实：call-ret 影子栈
  （PR #4670，x25 影子栈+值校验+哑元/guard/清零三层兜底）、RA 内联（PR #4580，实为
  "物理寄存器号内联进 IR 数据"+两遍 block-local 分配，SRA 常驻是 x64 移植不了的）、
  共享 JIT 缓冲（PR #4479，-25% JIT 时间本体=共享翻译结果；满不清老换大缓冲）。
  路线图修订见 §11.6（共享块缓存因 51% 实测跃居第一，含"单 Jit+3 JitState"折中路径）。
- 坑：构建命令带 `| tail` 掩盖失败退出码（printf 格式串错误因此漏检跑了一轮旧 exe）——
  构建后必须验证产物时间戳/显式成功标记。

### FEX 移植实战轮（2026-09-10 深夜，详见 PROFILE_PROGRESS.md §12）
- 三段微基准：**Emit 占编译成本 83.5%**（translate 2.4µs/optimize 1.8µs/emit 21.7µs 每块）
  → IR 级缓存收益上限仅 16.5%，"磁盘码缓存"估值随之打折（除非做机器码级+重定位）。
- 移植一：**跨核 IR 共享缓存**（c10446fecc，`EDEN_JIT_IRCACHE=1` 默认关）——实测 hit 50.6%
  与 51% 跨核重复精确吻合、hash 校验 0 失配、FPS 平；定位=磁盘码缓存基建。
- 移植二（实际赢家）：**RegAlloc ValueLocation O(1) 反向索引**（c3bc2c60d0，FEX-2506
  RegToSSA 思想）——ETW 解剖出 ValueLocation 线性扫+ReleaseAll 占 eden.exe ~17%；
  name→hostloc 稠密表仅三处重跟踪。**emit -18~20.5%（21.7→17.9µs/块）**，正确性全流程验证。
- 两提交在 worktree 现分支 `test/v0.2.1-profiling`（用户整理过分支，已跟踪远端）。
- Emit 剩余热点排队：ReleaseAll（7.9%）→Xbyak label 机制（3.4%）→descriptors map（1.2%）。

### 帧率天花板调查轮（2026-09-11，详见 PROFILE_PROGRESS.md §13）
- **45 FPS 之谜破解**：TOTK 游戏内以 swap_interval=0 提交（动态 FPS 引擎）→ eden
  倍率扩展 → **合成器 120Hz 栅格（8.33ms）**；帧时长量化为 [25,25,16.7] 循环 =
  精确 45.00。菜单轻负载恒 2 栅格 = 60.00。
- **三连对照铁证 work-bound**：默认 44.84（med 24.98 量化）/ 解锁限速 44.89
  （med 22.27 连续 = 真实工作速率）/ 模拟时钟 2× 超频 44.85（纹丝不动）。
- **帧率只能整量跳变**（45→60 需每帧工作 ≤16.67ms 即 **-25.1%**）——解释了历轮
  微优化为何 FPS 全不动。关键路径 = 4 个全饱和线程（3 JIT 核 + gpu_thread）流水线，
  **单线程砍 25% 无效，必须砍公共乘数**：JIT 执行质量（BL→host call/块合并，作用于
  86% 模拟核时间）是唯一够得着 -25% 的方向；gpu_thread 需借 VulkanWorker 空闲
  50% 并行化摘出关键路径。
- **方法论**：稳态优化一律用解锁基准（use_speed_limit=false，看 med ms，无量化
  失真，1%low 还更好）；栅格模式只还原用户体验。
- IR cache 已默认开启（76a192eb28，EDEN_JIT_IRCACHE=0 关，512MiB 上限）：hits 49.7%、
  编译期 CPU -71%、FPS 无回退。HWC 节拍探针常驻（bbcf1eb47a）。

### 视角旋转掉帧攻坚轮（2026-09-12，详见 PROFILE_PROGRESS.md §14）
- **复现**：静止 44.9fps/尖刺≈0 vs 旋转 43.6fps + 每秒 ~1.5 个 33-42ms 尖刺 + 偶发
  143ms 大卡顿（**确定性复现**，两局独立会话尖刺时间戳逐帧一致）。基建沉淀
  F:\prof（rot_hold/rot_test/rot_trace + J/L 键盘右摇杆映射；**方向键被 Qt 焦点导航
  吞掉，自动化必须用字母键**）。
- **ETW 归因**（totk_rot.etl 已存档）：旋转时 GPU 线程 +7%、模拟核 -10~13%（在等它）；
  143ms 尖刺窗口 GPU 线程 94% 满负荷跑 eden 代码（排除驱动/管线编译/等待）；
  **二次旋转毫不便宜 → LRU 逐出循环**。坑：本会话 eden PDB 符号加载失败（函数级欠账）。
- **根因 = ASTC 纹理流送**：Gpu 加速上传在 GPU 线程同步执行（dispatch+barrier）+
  Uncompressed 存储显存 4B/px 膨胀 → 逐出 → 旋转重扫反复重建上传。
  workers.cpp 的解码线程池在本树是**死代码**。
- **修复（配置级，已留在 qt-config）**：`accelerate_astc=2`(CpuAsynchronous) +
  `astc_recompression=2`(Bc3) + `use_asynchronous_shaders=true` → 旋转尖刺 **-50%**、
  最大卡顿 303→58ms、二次旋转变便宜（逐出循环打破）、Bc3 画质肉眼验收通过。
- wpr 坑：C 盘 99% 满导致采集会话中途自灭（0xc5583000），**必须 -recordtempto F:**
  且脚本先 wpr -cancel 自愈（僵尸会话会让下次 0xc5583001）。

### 本地补丁与工具（v0.2.1 worktree，勿提交上游）

- `fsp_srv.cpp` 两处 `OpenSaveDataFileSystem` 的 `ASSERT(false)`（Temporary/ProperSystem/SafeMode
  空间）已改为正常映射 StorageId。原因：强杀进程后游戏残留 Temporary 存档，下次启动打开它即
  assert 闪退（启动 ~17s 死循环）。profiling 要反复强杀进程，必须有此补丁。
- 采集/自动化脚本存 `F:\prof\`（auto_play / focus_test / cap_only / master_run / wpr_* / cleanup）。
- trace 存档：`F:\prof\totk_cpugpu.etl`（99s 全量，已处理可查）。

