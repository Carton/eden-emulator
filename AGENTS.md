# Eden 本地开发笔记（Windows，仅本地使用）

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
4. master 工作区有 3 个未提交的 clang-cl 兼容补丁（vk/gl_graphics_pipeline.cpp 的
   `!defined(__clang__)` 守卫、image_base.cpp 的 find_if 改写），用 MSVC 编译不需要它们。

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

### 本地补丁与工具（v0.2.1 worktree，勿提交上游）
- `fsp_srv.cpp` 两处 `OpenSaveDataFileSystem` 的 `ASSERT(false)`（Temporary/ProperSystem/SafeMode
  空间）已改为正常映射 StorageId。原因：强杀进程后游戏残留 Temporary 存档，下次启动打开它即
  assert 闪退（启动 ~17s 死循环）。profiling 要反复强杀进程，必须有此补丁。
- 采集/自动化脚本存 `F:\prof\`（auto_play / focus_test / cap_only / master_run / wpr_* / cleanup）。
- trace 存档：`F:\prof\totk_cpugpu.etl`（99s 全量，已处理可查）。

