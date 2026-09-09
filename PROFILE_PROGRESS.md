# Eden TOTK 性能优化进度记录（Profile Progress）

> 写给接手的 agent / 开发者；随仓库跟踪维护，每轮实验后更新。原 `F:\prof\HANDOFF.md` 已并入本文件。
> 目标：在 Eden 模拟器（yuzu 血统）上优化《塞尔达传说 王国之泪》
> 的运行帧率。本文档记录当前基线、已验证的工具链、初步分析结论、实验方法和所有脚本的用法。
> **纪律：本目录及一切 AI 生成内容仅限本地使用，禁止以任何形式提交到上游仓库 / issue / PR。**

---

## 0. 一句话现状（TL;DR，2026-09-09 更新）

- 测试基线：**`F:\devel\opensource\eden-v0.2.1`**，分支 `local-profiling` =
  v0.2.1(58c1e20) + fsp_srv 崩溃修复(3ea74e6) + GPU 线程微优化(7f1f534cd0) +
  **JIT 计数器+2GiB 码缓存(c2d9148f7f，§11)**。
- 瓶颈画像（已两轮验证）：**CPU 侧四核全饱和**——3 个 JIT 模拟核（85.7% 纯游戏代码）+
  GPU 命令线程；设备 GPU 利用率 37% 有余量。帧时长 = 流水线最慢一级。
- 已完成：GPU 线程微优化五项（CPU 时间 105.9→102.4s，-3.3%）；fastmem 排查（无 miss，勿再查）；
  **CPU 精度切到 Unsafe（+2.0 FPS 且 33ms 尖刺全消，qt-config 已留在 Unsafe！）**。
  当前实际帧率：**44.8 FPS**（原始基线 43.9）。后续任何 FPS 对比都要基于 Unsafe 档跑，
  或先切回 `cpu_accuracy=0` 再比（见 §6.3）。
- 结论性判断：两侧低垂果实已摘完；JIT 攻坚轮（§11）已完成量化+调研+实验，
  关键发现：**TOTK 以 ~6300 新块/秒持续编译（连主菜单静置也有 3600/秒），99% 不同 PC、
  均匀铺满 ~48MB 代码工作集、无热点**——这是该游戏 JIT 重的结构性原因；
  长会话 512MB 码缓存会被打穿引发全清风暴（已改 2GiB）。剩余方向见 §6.4 与 §11.6。
- 速查 skill：`.agents/skills/eden-bench`（master 仓库内，构建/基准/微 profile 全流程）。
  完整分析报告：[`F:\prof\totk_profile_report.html`](F:/prof/totk_profile_report.html)。

---

## 1. 测试环境与构建方法

### 1.1 机器
- Windows 11 x64，12 逻辑核，NVIDIA RTX 2060（驱动 591.86）
- 用户偏好：安装软件放 D 盘或 G 盘，**不要 C 盘**；`F:\Switch\Yuzu`（官方安装）**只读，不许改**

### 1.2 两个仓库，分工明确
| 路径 | 用途 | 状态 |
|---|---|---|
| `F:\devel\opensource\eden-v0.2.1` | **性能实验的工作基线**（git worktree） | 分支 `local-profiling` = tag v0.2.1(58c1e20) + `3ea74e6`（fsp_srv 崩溃修复 §8）+ `7f1f534cd0`（GPU 线程微优化 §6.1） |
| `F:\devel\opensource\eden-emulator` | master，仅当代码参考 / 笔记宿主 | 本地领先 origin/master 3 个纯文档提交（AGENTS.md），**永不 push**；master 本身构建物不能跑 TOTK（历史问题，勿浪费时间） |

上游仓库 `https://git.eden-emu.dev/eden-emu/eden`（GitHub 镜像 `eden-emulator/mirror`）。

### 1.3 工具链（全部已装好）
- **VS2022 Community** @ `D:\Program Files\Microsoft Visual Studio\2022\Community`
  （MSVC 19.44；CMake 3.31.6 / Ninja 为 VS 自带，不在系统 PATH）
- glslang 16.5.0 @ `G:\Tools\glslang`（构建期 shader 编译）
- PerfView @ `G:\Tools\perfview\PerfView.exe`（火焰图，见 §6）
- Git Bash（MSYS）+ Python 3.12；**当前 agent shell 无提权**，但 UAC 为"从不通知"，
  `powershell Start-Process -Verb RunAs` 可**静默提权**（这是所有采集脚本的工作前提）

### 1.4 构建命令（Git Bash，v0.2.1 增量约几十秒～几分钟）
```bash
cd /f/devel/opensource/eden-v0.2.1
source /f/devel/opensource/eden-emulator/tools/windows/load-msvc-env.sh   # vswhere 自动找 VS2022
export PATH="/g/Tools/glslang/bin:$(dirname "$(command -v cl.exe)"):/d/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin:/d/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja:$PATH"
cmake.exe --build build    # 已配置过；产物 build/bin/eden.exe + .pdb
```
完整重配置（仅首次/改 CMake 选项）：
```bash
cmake.exe -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DYUZU_TESTS=OFF \
  "-DCMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO=/DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF"
```
注意：MSVC bin 必须在 PATH 里排在 `/usr/bin/link.exe` 之前（上面的 export 已处理）。
RelWithDebInfo = `/O2 /Ob1 /Zi` + `/DEBUG /OPT:REF /OPT:ICF`，就是 profile 友好配置；**分析阶段不要开 PGO**。
小坑：`/OPT:ICF` 会折叠相同函数体，火焰图里个别符号会合并，属正常噪声。

### 1.5 游戏与运行
- 游戏文件：`F:\prof\TOTK.nsp` —— 是 `F:\Switch\Games\塞尔达王国之泪\[APP][0100F2C0115B6000][1.0.0][US][16.0.0].nsp`
  的 **NTFS 硬链接**（`ln` 创建，零拷贝）。存在原因是部分工具（nsys）无法处理中文路径。
  另有升级包 `[UPD]...1.4.2...nsp` 未装入（游戏内显示 1.4.2 是 UltraCam 模组组的说法）。
- 运行：在 `build/bin/` 下 `./eden.exe F:/prof/TOTK.nsp`；exe 旁的 `user/` 目录 = portable 数据
  （密钥/存档/配置都在里面）。日志在 `user/log/eden_log.txt`（每次启动覆盖）。
- 游戏启动后 ~40s 到主菜单（"继续游戏"默认高亮），按 A（见 §2）→ 存档列表 → 确认 → ~25s 读档进游戏。
  **性能测试的标准场景**：卡卡利科村营地（当前存档位置），角色站立，43-45 FPS。
- **勿用 eden-cli 跑 TOTK**：shader 编译段必崩（`CollectStorageBuffers` segfault，官方同期版同样），
  profile 一律用 GUI 的 eden.exe。

---

## 2. 输入自动化（"模拟手柄点击"）

### 2.1 原理
两个前置条件，都已配置好（配置在 `build/bin/user/config/qt-config.ini`）：
1. `keyboard_enabled=true`（同时把 `keyboard_enabled\default=true` 改成 `=false`，见 §9 配置坑）
2. player_0 的按键改绑键盘：`player_0_button_a="engine:keyboard,code:88"`（X 键=Switch A）、
   `button_b=code:90`（Z）、`button_plus=code:78`（N）。**原手柄配置备份在
   `qt-config.ini.bak-controller`，想还原手柄操作就把它拷回去。**

注入机制（`F:\prof\focus_test.ps1` / `auto_play.ps1` 内的 C# `Win32` 类）：
1. `Process.GetProcessesByName("eden")` 拿 `MainWindowHandle`
2. 抢焦点：`ShowWindow(SW_RESTORE)` + `AttachThreadInput` + `SetForegroundWindow`，
   失败则先发一个**单独的 Left Alt（扫描码 0x38）**再重试（"Alt-trick"，让系统认为我们有输入权）
3. 焦点确认 `GetForegroundWindow()==hwnd` 后，`SendInput` 发**扫描码**（不是虚拟键码！SDL 认扫描码）：
   X=0x2D。`KEYEVENTF_SCANCODE` 标志，INPUT 结构体大小必须传 40（x64）

### 2.2 脚本用法
| 脚本 | 作用 | 用法 |
|---|---|---|
| `focus_test.ps1` | 单发一次 A 键（先抢焦点） | `powershell -NoProfile -ExecutionPolicy Bypass -File F:/prof/focus_test.ps1`，输出 `focus_ok=True/False` |
| `auto_play.ps1` | 全自动进游戏：等 eden 窗口（≤60s）→ 睡 110s（等启动到主菜单，时间是为配合 nsys/wpr 启动定的）→ 每 12s 按 A×5（覆盖 菜单→存档列表→确认→读档→误按无害）→ 结果写 `auto_play.log` | 由 `master_run.ps1` 自动拉起；单独用也行。**调时序就改里面的 `Start-Sleep -Seconds 110` 和循环** |
| `cleanup.cmd` | 提权强杀 eden.exe / nsys.exe / LosslessScaling.exe | `powershell -NoProfile -Command "Start-Process -Verb RunAs -WindowStyle Minimized -FilePath 'F:\prof\cleanup.cmd'"` |

实测流程（已验证可复现）：启动 eden → `sleep 45` → focus_test ×3（间隔 8s）→ `sleep 26` → 在游戏内。

### 2.3 血泪坑（务必读）
- **Windows 前台锁**：用户正在打字时，后台进程**抢不赢**前台（SetForegroundWindow 被拒），
  按键会打进用户的前台窗口（曾把 X 打进用户的 byobu 终端和记事本+输入法）。采集期间必须
  **用户完全停手**。Alt-trick 在用户活跃时也会失败。
- **提权不匹配**：UIPI 规则——低权限进程的 SendInput 无法送达高权限窗口；`computer-use` 截图/
  按键也无法作用于提权进程。因此**游戏用普通权限跑**（直接 bash 启动），auto_play 也普通权限即可。
  如果游戏是被提权的 nsys 拉起来的，auto_play 也必须提权，且截图会被 UIPI 挡。
- **焦点 ≠ 焦点**：发键前必须验证 `GetForegroundWindow()==游戏hwnd`。日志里 fg 标题显示单字符
  'E'/'F' 是 `GetWindowTextW` 的 StringBuilder 编组 bug（只取到 1 个字符），'E'=Eden 窗口、
  'F'=编辑器窗口路径开头，仅影响日志观感不影响功能。
- **输入法**：目标窗口开着中文 IME 时按键会被当拼音吃掉（曾全部变成"x"）。跑自动化前关输入法
  或确保焦点在游戏。
- MSYS/Git Bash 坑：`/参数` 会被路径转换（`MSYS_NO_PATHCONV=1` 或 `MSYS2_ARG_CONV_EXCL="*"`）；
  反斜杠路径经过工具层会被吃一层（用正斜杠或 python 改文件）；`cmd //c` 之类双斜杠写法在
  ARG_CONV_EXCL 下失效。

---

## 3. Profiling 管线（主力：wpr + ETW MCP）

### 3.1 为什么是这条管线
- eden 代码**没有** Tracy/microprofile 内建插桩 → 只能采样式。
- nsys 路线（Nsight Systems）被放弃：它要提权启动游戏才给 WDDM/CPU 采样数据 → 游戏变提权进程
  → 输入自动化失效 + computer-use 被 UIPI 挡 + 报告生成 10 分钟且怕误关窗口。
  如果将来要用：`nsys profile -t wddm -d 300 -o F:/prof/xxx ./eden.exe F:/prof/TOTK.nsp`（必须提权
  运行 nsys、必须 ASCII 路径），交互火焰图/时间轴看 `nsys-ui`。备选方案：研究 `nsys-ui` 的
  GUI attach（先普通权限起游戏再附加），能解决提权矛盾。
- **wpr（Windows 内置）无此约束**：系统级 ETW 采集，游戏随便什么权限，还能被 ETW MCP 直接符号级分析。

### 3.2 采集步骤（已脚本化）
```bash
# 0) 游戏普通权限启动 + auto_play 进游戏（见 §2.2）
# 1) 一体化采集脚本（启动→采 100s→落盘，必须同一提权进程！）
powershell.exe -NoProfile -Command "Start-Process -Verb RunAs -WindowStyle Minimized -FilePath 'F:\prof\wpr_run.cmd'"
#    wpr_run.cmd 内容 = wpr -start CPU -start GPU -filemode → timeout 100 → wpr -stop F:\prof\totk_cpugpu.etl
# 2) 轮询 F:\prof\wpr_run.log 出现 WPR_DONE 即完成（25s 小样本用 wpr_run2.cmd → totk_cpu25.etl）
```
- **坑 1**：`wpr -start` 和 `wpr -stop` 分在两个脚本/两次调用会丢采集会话（实测踩坑）——必须一体化。
- **坑 2**：`wpr -start` 需要管理员（提权方式见上）；`wpr -profiles` 可列出全部档位（CPU/GPU/DiskIO/...）。
- **坑 3**：采集时确保 Lossless Scaling 之类后台工具没在跑（会污染数据）。
- 采集大小参考：100s CPU+GPU ≈ 4.6GB；25s CPU-only ≈ 1.1GB。

### 3.3 分析：ETW MCP（agent 会话内可直接调用）
```
mcp__etw__save_symbol_configuration(symCachePath="D:\\SymCache",
    symbolPath="srv*D:\\SymServer*https://msdl.microsoft.com/download/symbols")   # 已配置过
mcp__etw__process_trace(filePath="F:\\prof\\totk_cpugpu.etl",
    categoryNames=["Sampled CPU Usage Data","CPU Scheduling Data","Ready Thread Data","Processes Data"])
mcp__etw__list_traces()      # → traceId
mcp__etw__start_new_query(traceId=..., category="Sampled CPU Usage Data",
    targetCollection="Samples", logicalOperator="And",
    conditions=[{"property":"Process.ImageName","operator":"Equal","value":"eden.exe"}],
    groupings=[{"property":"Thread.Name"},{"property":"Thread.Id"}],      # 嵌套分组
    aggregations=[{"name":"cpu_time","property":"Duration","type":"Sum"},
                  {"name":"samples","property":"Duration","type":"Count"}])
mcp__etw__perform_query(queryId=...)      # 全量查询要 allowQueryingAllData=true
```
**ETW MCP 的关键行为与坑**：
- 单次 MCP 调用 **30s 超时**，但 `process_trace` / 大查询**在后台继续跑**！超时后用
  `list_traces` 确认处理完成，再重新 `perform_query`（queryId 仍在，重发即重查）。
- `FunctionName` 全量分组在大 trace 上必超时。**套路：先按 `ModuleName` 分组（快），
  再加 `ModuleName=="eden.exe"` / `Thread.Id==...` 过滤后按 `FunctionName` 分组。**
- Schema：`Samples` 集合，字段含 `Process.ImageName / Thread.Name / Thread.Id / FunctionName /
  ModuleName / CallStack / Duration / Timestamp`；`Duration` 求和即 CPU 时间（采样粒度 ~1ms/样本）。
- 注意：**MCP 重启后 trace 列表会清空**（需重新 process_trace，符号缓存 D:\SymCache 仍在所以第二次快）。
- 可查询类别里**没有 GPU/DxgKrnl**——GPU 侧只能靠 nvidia-smi 轮询（§4）或 nsys。

### 3.4 辅助测量
- **GPU 设备利用率**：`nvidia-smi --query-gpu=utilization.gpu,utilization.memory,clocks.sm --format=csv,noheader,nounits -l 2`（轮询写入文件再 awk 求均值；GPU-bound 判定的金标准）。
- **FPS 读数**：eden 叠加层（右下角 `Game: NN FPS / Frame: NN.NN ms / Scale: Nx`），用
  computer-use 全屏截图读取（`mcp__computer-use__screenshot`，对提权进程的 app 级截图会被 UIPI 挡，
  全屏截图不受影响）。

---

## 4. 火焰图怎么看

### PerfView（推荐，已下载 `G:\Tools\perfview\PerfView.exe`，单文件免安装）
1. `PerfView.exe` → **File → Open** → 选 `F:\prof\totk_cpugpu.etl`
2. 展开后双击 **CPU Sampling (Precise)**（打开 stack 视图；首次可能弹符号窗口，eden 的 PDB 在
   exe 旁边会自动命中；系统 DLL 符号可选 Symbol Server）
3. 顶部 Filter 框输 `eden.exe` 只看模拟器进程
4. 菜单/右键选 **Flame Graph**：矩形宽度 = 该函数（含子调用）的采样占比，高度 = 调用栈深度，
   顶层宽块 = 热点函数。宽而矮的"平顶"= 该函数自身耗时间（self time），是要优化的点；
   宽而高的"塔"= 调用链入口（顺着往下看是哪个子层吃掉的）
5. 想要经典 SVG：复制 collapsed 栈（PerfView 可导出）→ brendangregg/FlameGraph 的
   `flamegraph.pl collapsed.txt > flame.svg`（需要 Perl；也可以让 agent 直接生成 HTML）

### 注意
- `F:\prof\totk_profile_report.html` 里的横向条形图是**函数自时间排行**（无调用关系），火焰图才是
  带调用链的完整视图。两者结合看：报告找"谁最热"，火焰图回答"它被谁调用、为什么被调"。
- 采样是**栈顶自时间**归因；`/OPT:ICF` 会折叠相同函数体，符号偶有合并噪声。

---

## 5. 已完成的实验与数据

### 实验 1：基线画像（99s 卡卡利科村，1x 分辨率，43→45 FPS）
- eden.exe 总占用 ≈5.2 核（514 CPU·s / 99s）。
- **四个线程打满**：CPUCore_0/1/2（TOTK 恰好用 3 个 Switch 核）各 ~100s，GPU 命令线程 105.9s；
  VulkanWorker 52.9s（50%，有余量）；其余线程可忽略。
- 模拟核组成：86% JIT 后游戏代码（匿名内存）、6.6% 内核、5.1% eden（HLE 服务）→ 是真工作量，非自旋。
- GPU 命令线程组成：67% eden 自身代码、17% 内核、7% CRT、**显卡驱动 <1%**。
- eden 模块内函数热点 Top（自时间，总 70.7s）：
  `DmaPusher::ProcessCommands` 4.94s · `Maxwell3D::ProcessDirtyRegisters` 4.23s ·
  `DmaPusher::CallMethod` 3.76s · `PushImageDescriptors` 2.01s ·
  `BufferCache::BindHostGraphicsUniformBuffer` 1.97s · `MemoryManager::GpuToCpuAddress` 1.55s ·
  `BufferCache::TouchBuffer` 1.48s · TextureCache LRU `Touch` 1.27s ·
  `GraphicsPipeline::ConfigureImpl<VertexFragment>` 1.23s · `WordManager::IterateWords` 1.12s ·
  `BufferCache::FindBuffer` 0.97s · `RefreshContents` 0.90s · `BindHostVertexBuffers` 0.90s ·
  `Buffer::MarkUsage` 0.80s · `ConsumeSinkImpl` 0.74s · `GraphicsPipelineCacheKey::operator==` 0.67s …

### 实验 2：设备 GPU 排除法（对照实验）
| 条件 | GPU 设备利用率 | FPS |
|---|---|---|
| 1x 分辨率 | **37%**（29 样本） | 43 |
| 0.25x 分辨率（像素负载↓4~16 倍） | 25% | 45（+4.7%） |
结论：绘制负载对帧率几乎无影响 → **设备 GPU 不是瓶颈，帧率完全由 CPU 侧流水线决定**。
（分辨率配置已还原默认；`resolution_setup` 的值 0=0.5x/2=1x，改值必须同时把
`resolution_setup\default=true` 改成 `=false` 才生效。）

### 排除项 / 已知死路（别再浪费时间）
- master 分支构建物跑 TOTK 卡 launching（VS2026/14.51 时代疑似误编译，未定论）；
  master+VS2022 因 CPM 静态 Qt 的 STL 符号链接失败 → **master 只当参考，实验都在 v0.2.1**。
- eden-cli 跑 TOTK 必崩（官方同期版同样），profile 用 GUI eden.exe。
- PGO 在分析阶段不开。
- **fastmem miss 排查**（09-08 已排除）：缺页路径仅占模拟核 0.07%，fastmem 工作正常（§6.3）。
- **用 FPS 评估 <4% 的微优化**：噪声 ±1.5 FPS 掩盖一切，必须用 wpr+ETW 函数级时间（§6.2）。
- **砍分辨率/调 GPU 精度找帧率**：设备 GPU 利用率 37% 且 0.25x 只 +2 FPS（实验 2），死路。

---

## 6. 初步结论与优化重点建议（优先级序）

**判定：CPU 侧双重瓶颈。3 个模拟核与 GPU 命令线程同时饱和；设备 GPU 利用率 37% 有大余量。**

### 6.1 优化实施记录（2026-09-08 起，GPU 命令线程第一优先）

**FPS 基准管线（替代截图读数）**：eden 内建 `record_frame_times`（qt-config.ini `[Debugging]`
`record_frame_times=true`）→ 游戏退出时 `~PerfStats()` 把**每帧帧时间(ms)逐行**写到
`build/bin/user/log/<日期-时刻>_<titleid>.csv`（容量 216000 帧）。配合 `confirmStop=2`
（Ask_Never，`\default=false`）实现 `taskkill /IM eden.exe`（**不带 /F**，发 WM_CLOSE）优雅退出
→ CSV 落盘。**强杀（/F）会丢 CSV。**
一键基准：`python F:/prof/bench_run.py LABEL [--measure 90]`（杀残留→清旧 CSV→启动→自动 A 键
进游戏→测量→优雅关闭→解析最后 90s 帧时间→追加到 `F:\prof\bench_results.csv`）。
基线（2026-09-08，卡卡利科村，commit 3ea74e6b0e）：**43.21 / 44.65 FPS（均值 43.9）**，
中位帧时 24.99ms 极稳定，fps_mean 与 ms_median 是主指标。跑基准时用户不能碰键盘（前台锁）。

**优化项实测结果（2026-09-08/09 夜，均已提交 local-profiling @ 7f1f534cd0）**：
| # | 改动 | 靶点热点（基线→实测） | 结论 |
|---|---|---|---|
| P0 | maxwell_3d `ProcessDirtyRegisters` 冗余写过滤（值没变直接 return） | 4.23s → **1.66s**（-61%；ConsumeSink+dirty 合计 4.97→2.88s） | ✅ 生效 |
| P1 | LRU touch 帧内去重（BufferBase/ImageBase 加 `last_touch_tick`） | TouchBuffer 1.48→**0.55s**；纹理 Touch 1.27→**0.30s** | ✅ 生效 |
| P3a | uniform buffer 对齐值构造期缓存 | 消除每次虚拟调用 | ✅ 小收益 |
| P2 | 管线键 transition 哈希预比较（懒计算：仅自键 miss 时算 CityHash） | operator== 归因消失（memcmp_avx2 吸收），量级小 | ✅ 无回退 |
| P4 | SSBO/TBO 解析结果复用（writer 保留 buffer_id + Update 守卫 + DeleteBuffer 补清 texture_buffers） | FindBuffer 0.97→0.87s（-10%：SSBO 描述符本身常变，剩余主要是 UpdateVertexBuffer） | ⚠️ 收益小于预期但无回退 |
| P5/P8 | MarkUsage tick / GpuToCpuAddress 页表缓存 | 未做（收益 ≤0.8% / 需先看调用量） | 暂缓 |

**GPU 命令线程总 CPU：105.9s → 102.4s（-3.3%），渲染正确性截图验证通过。**

### 6.2 关键实验结论：为什么 GPU 线程省了 3.5% 而 FPS 不动

- FPS 基准（同场景 ×2）：基线 43.21/44.65，优化版 42.10/43.51——**分布在噪声内，无提升**。
- 但**每次运行中位帧时都精确锁在 24.99ms**（≈40.0 FPS）→ 帧时长被量化/节流，均值差异只来自快帧占比。
- 优化版 wpr（100s）：**4 核全部打满**——CPUCore_0/1/2 = 102.3/111.8/101.2s，GPU 线程 102.4s，
  VulkanWorker 52.4s。帧时长 = 流水线最慢一级；GPU 线程单点 -3.5% 改变不了 max。
- **下一个真正的 FPS 杠杆在 3 个 JIT 模拟核**（86% 是游戏代码）：查 fastmem 是否生效
  （ntoskrnl 页保护开销 6.6% 是否为 fastmem miss 的页错误处理）、CPU 精度 Unsafe 档 A/B、
  或做更大的 GPU 线程结构性削减（DmaPusher::ProcessCommands+CallMethod 合计仍有 8.6s，
  描述符 payload 每 draw 全量重放）——但后者要动核心数据流，风险高。
- FPS 微基准注意事项：bench_run.py 早期版本会清掉历史 TOTK CSV（已改掉）；
  游戏内时钟会漂移（1 real min = 1 game hr），A/B 尽量背靠背交错跑。

### 6.3 第二轮实验（2026-09-08 深夜）：fastmem 排查 + CPU 精度 A/B

**fastmem 生效性排查（用 09-08 优化版 trace，CPUCore_1 = 111.8s）**：
- 模块构成：85.7% JIT 游戏代码（匿名内存）/ 6.5% ntoskrnl / 5.6% eden 自身。
- ntoskrnl 7.25s 函数明细：**缺页机制全家（MiUserFault/KiPageFault/MiResolveProtoPteFault 等）
  合计仅 ~0.08s = 该核 0.07% → fastmem 工作正常，几乎无 miss，无可修**。
- "6.6% 内核开销"的真实构成：~3.3s 栈展开/栈回溯族（RtlpUnwindPrologue/RtlpLookupFunctionEntry
  **ForStackWalks**/RtlpxVirtualUnwind）+ Etwp* 0.35s——**大半是 ETW 采集器自身开销**（采样抓栈），
  剩 ~3s 是 5.2 核饱和负载的正常调度/IPI。以后解读 kernel 占比时记住这 ~3-4% 是测量税+调度税。
- **结论：JIT 侧没有低垂果实；Unsafe 精度档是该侧唯一现实杠杆。**勿再查 fastmem。

**CPU 精度 Unsafe A/B（同一构建 7f1f534cd0，仅改 `cpu_accuracy` 0=Auto → 2=Unsafe）**：
| 配置 | fps（×2 轮） | p99 帧时 |
|---|---|---|
| Auto（=Accurate） | 42.10 / 43.51 | 33.40 / 33.34 ms |
| **Unsafe** | **44.80 / 44.81** | **25.19 / 25.42 ms** |

**+2.0 FPS（+4.7%）且 33ms 卡顿尖刺完全消失**，两轮一致性极佳。机制：Unsafe 启用
unsafe IR 优化（UnfuseFMA/ReducedErrorFP/InaccurateNaN/IgnoreGlobalMonitor）且
`fastmem_address_space_bits` 39→64（免每访存边界检查）。**当前 qt-config 已留在 Unsafe**；
要还原：`cpu_accuracy\default=false` + `cpu_accuracy=0`（GUI：模拟→CPU→精度）。
注意 Unsafe 对个别游戏可能有精度问题，TOTK 实测画面正常（帧数/帧时分布正常，未逐帧目检）。

**基准历史（完整数据在 `bench_results.csv`，原始逐帧 CSV 在 `build/bin/user/log/`）**：
| 标签 | 构建 | 精度 | fps | p99 |
|---|---|---|---|---|
| baseline-1/2 | 3ea74e6b0e | Auto | 43.21 / 44.65 | 33.35 / 25.37 ms |
| opt-AB-1/2 | 3ea74e6b0e* | Auto | 42.10 / 43.51 | 33.40 / 33.34 ms |
| unsafe-1/2 | 7f1f534cd0 | **Unsafe** | **44.80 / 44.81** | 25.19 / 25.42 ms |

*opt-AB 的 exe 是优化版构建但当时未提交（commit 列写的是旧值）；7f1f534cd0 之后构建与提交一致。
教训：一次无效运行（按键注入被前台锁吃掉，游戏停在 60FPS 菜单）产出 59.81 的假结果——
bench_run 已有 `fps>52 → SUSPICIOUS` 提示；看到它或 taps 的 focus_ok=False 就重跑，别采信。
中位帧时恒为 24.99ms（40FPS 量化）是场景特征，横向对比用 fps_mean / p99。


### 6.4 后续方向（给接手 agent，按 性价比/风险 排序）

上一轮（§6.1-6.3）已把两侧的低垂果实摘完。剩余可做的：

1. **验证 Unsafe 的长期稳定性**（低成本）：换个场景/时段跑几轮 bench + 目检画面，
   确认无精度性花屏/物理异常。已开 Unsafe 的 44.8 FPS 是当前"新基线"。
2. **VulkanWorker 卸载**（中风险，收益不确定）：VulkanWorker 仅 ~52% 利用率，GPU 线程 100%。
   研究 `vk_scheduler` 的 worker 队列，把描述符更新/staging 上传等 CPU 侧工作从 GPU 线程
   挪到 worker（上游 master 的多线程重构 #4254 做了类似事，可读它的 diff 找思路，但勿直接搬）。
3. **GPU 线程结构性削减**（高风险）：
   - `DmaPusher::ProcessCommands + CallMethod` 合计仍有 8.6s——命令循环本身，
     需要重写批处理（method_sink 机制已是 eden 自有优化，再往下是硬骨头）。
   - 描述符 payload 每 draw 全量重放（PushImageDescriptors 1.93s + ConfigureDraw 的模板更新）：
     可做"payload 未变则跳过"的整块比较（memcmp 上一次 payload），但涉及正确性边界。
4. **小尾巴**（≤1% 收益，闲时做）：P5 MarkUsage 帧内去重；GpuToCpuAddress 1.43s
   （本质是大页表 cache miss，加 1-entry 缓存收益存疑）。
5. **换基线**（等待型）：master 修复 TOTK 启动问题 + VS2022 链接问题后，把实验迁到新基线
   （上游已有 bindless descriptors #4251、多线程重构 #4254 等大改，届时本文件的热点表需重测）。

**验证闭环（每次改动）**：改代码 → `cmake --build build` → `python F:/prof/bench_run.py LABEL`
×2 → 若 FPS 无感（<4% 改动）必须用 wpr+ETW 微 profile 判定（skill `eden-bench` 有完整步骤；
进游戏后等 ≥30s 稳定再触发采集）。FPS 噪声 ±1.5，不要拿单轮下结论。

---

## 7. F:\prof 文件与脚本清单

| 文件 | 说明 |
|---|---|
| `TOTK.nsp` | 游戏文件的 ASCII 路径硬链接（指向 F:\Switch\Games\...），给工具链用 |
| `focus_test.ps1` | 单发 A 键注入（含抢焦点），用于交互测试/手动推进菜单 |
| `auto_play.ps1` | 全自动进游戏（等窗口→110s→A×5 每 12s），日志 `auto_play.log` |
| `master_run.ps1` | 编排器：提权拉起 auto_play + cap_only（均隐藏窗口）——nsys 时代遗留 |
| `cap_only.ps1` | nsys 采集脚本（-t wddm -d 300）——nsys 路线遗留，当前不用 |
| `wpr_run.cmd` | **主力采集**：提权一体化 CPU+GPU 100s → `totk_cpugpu.etl` |
| `wpr_run2.cmd` | 快查版：CPU-only 25s → `totk_cpu25.etl` |
| `wpr_start.cmd` / `wpr_stop.cmd` | 分离式启停（有丢会话坑，仅参考） |
| `cleanup.cmd` | 提权强杀 eden/nsys/LosslessScaling |
| `bench_run.py` | **FPS 基准一键脚本**：`python bench_run.py LABEL [--measure 90]`（自动进游戏→测 90s→优雅关闭→解析帧时间 CSV）；`--hold N` 为保持模式（供 wpr 采集用）；结果追加 `bench_results.csv` |
| `bench_results.csv` | 历次基准结果（时间/标签/commit/帧数/fps/1%low/中位/p95/p99） |
| `totk_cpugpu_0907_baseline.etl` | 09-07 基线 trace（v0.2.1 原版，本文件 §5 数据源） |
| `totk_cpugpu.etl` | 09-08 优化版 trace（7f1f534cd0，§6.1 数据源） |
| `totk_cpu25.etl` | 25s CPU-only trace（快速查询用） |
| `totk_profile_report.html` | **静态分析报告**（结论/线程表/热点条形图） |
| `gpu_util_1x.csv` / `gpu_util_05x.csv` | 实验 2 的 nvidia-smi 原始数据 |
| `auto_play.log` / `wpr_run*.log` / `nsys_p1*.log` | 各次运行日志 |
| `build/bin/user/log/*_0100F2C0115B6000.csv` | 每次运行的**逐帧帧时间原始数据**（record_frame_times，退出时落盘；文件名=日期+titleid） |

提权启动模板（UAC 静默，无弹窗）：
```bash
powershell.exe -NoProfile -Command "Start-Process -Verb RunAs -WindowStyle Minimized -FilePath 'powershell.exe' -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','F:\prof\xxx.ps1'"
```

---

## 8. 必须知道的本地补丁与配置现状

- **`fsp_srv.cpp` 补丁**（v0.2.1 已提交 3ea74e6，2 处）：`OpenSaveDataFileSystem` 遇
  Temporary/ProperSystem/SafeMode 空间原本 `ASSERT(false)`；强杀进程残留的 Temporary 存档会让
  下次启动 ~17s 必崩死循环。已改为映射 NandUser/NandSystem。**只要还会强杀游戏进程，此补丁必须保留。**
- **键盘改绑现状**：`qt-config.ini` 中 `keyboard_enabled=true`、player_0 A/B/plus 绑键盘
  （code 88/90/78）；手柄原始配置备份 `qt-config.ini.bak-controller`，用户要亲手玩时还原。
- **性能相关配置快照（2026-09-09，做对比实验前先核对）**：`cpu_accuracy=2`（**Unsafe**，
  `\default=false`，A/B 结论见 §6.3）；`record_frame_times=true`（`[Debugging]` 段）；
  `confirmStop=2`（Ask_Never，配合 taskkill 优雅退出落 CSV）；`resolution_setup` 已还原默认。
- **符号缓存**：`D:\SymCache`（eden PDB 在 exe 旁自动命中；系统 DLL 走 MS 服务器，已拉过一次）。
- **磁盘**：F:\prof 现 ~6GB（两个 etl）；分析产生的 sqlite 导出可能很大（曾生成 17.5GB，已删），
  导出前想清楚。

## 9. 配置文件编辑的坑（qt-config.ini）

- `key\default=true` 表示"用编译默认值"，此时 `key=...` 行被忽略；改配置必须同时改
  `key\default=false` + `key=新值` 两行。
- 文件是 LF、UTF-8。反斜杠（`\default`）经 shell/工具层会被吃，**用 python 按行改最稳**：
  ```python
  L = open('qt-config.ini', encoding='utf-8').read().split('\n')
  L[行号-1] = 'keyboard_enabled\\default=false'   # python 源里 \\d 会得到 \d
  open('qt-config.ini','w',encoding='utf-8',newline='\n').write('\n'.join(L))
  ```
- 游戏正常退出会重写此文件；**强杀（taskkill /F）不会**。改配置前先杀游戏。

## 10. 其他背景（少走弯路）

- 上游（git.eden-emu.dev/eden-emu/eden）master 比我们的 tag 新很多；master 本地仓库已 rebase
  到 origin/master 且本地领先若干纯文档/技能提交（AGENTS.md + .agents/skills/eden-bench）——
  **永远不要 push**。
- 键盘注入不能作用于提权进程（UIPI）；computer-use 的 app 级截图/按键同样被 UIPI 挡（全屏截图可以）。
- WPR 有 `GPU` 档但 **ETW MCP 的可查询类别里没有 GPU**——GPU 侧数据用 nvidia-smi 轮询或 nsys。
- 采集期间系统里的 ZCode 会话自身占 ~0.8 核（对照数据时心里有数）。

---

## 11. JIT 模拟核攻坚轮（2026-09-09：量化 + 领域调研 + 实验）

### 11.1 模拟核时间解剖（CPUCore_1，111.8s / 100s，优化版 trace）

| 成分 | CPU 时间 | 占比 | 说明 |
|---|---|---|---|
| **JIT 发射代码**（匿名内存） | 95.85s | **85.7%** | 翻译后的游戏代码本体 |
| ntoskrnl | 7.25s | 6.5% | ≈一半是 ETW 采集自身抓栈成本，其余为饱和负载调度/IPI |
| **HLE 内核调度/SVC** | ~2.0s | ~1.8% | KPriorityQueue::GetFront 0.28s、KAddressArbiter、fiber 切换等 |
| **内存回调+光栅化切换+锁竞争簇** | ~2.8s | ~2.5% | 见下 |
| dynarmic FP 软浮点助手 | 0.53s | 0.5% | FPRSqrtEstimate/FPUnpack/NaNHandler 等 |
| dynarmic 调度/上下文/块查找 | 0.47s | 0.4% | 分层调度工作良好，无优化空间 |
| ntdll（SRW 锁竞争） | 1.36s | 1.2% | Contended 0.43s + Backoff 0.25s 等 |

**锁竞争簇的机制**：RasterizerCachedMemory 页在页表里**故意置空指针**
（`page_table.h:134`）→ CPU 每次访问 GPU 关注内存必走慢回调
（`Memory::Impl::Read/Write` → `HandleRasterizerDownload/Write` → `GPU().OnCPURead/Write`
→ 拿 texture/buffer 缓存大锁 → 与 GPU 线程互等）。ETW 可见：Read64 0.30s +
HandleRasterizerDownload 0.37s + GetPointerFromDebugMemory 0.17s + `_Mtx_lock` 0.49s +
ntdll SRW 族 1.0s。fastmem 本身零缺页（fastmem_faults=0），此前的"fastmem 干净"结论仍成立，
但"GPU 关注内存慢路径"是模拟核侧最大的**可识别**非 JIT 开销（~2.5%）。

### 11.2 JIT 活动计数器（新基础设施，已提交 c2d9148f7f 常驻）

`src/dynarmic/.../backend/x64/jit_stats.h`：block_lookups / block_compiles /
range_invalidations / full_clears / fastmem_faults，关闭时
`LOG_INFO(Core_ARM, "dynarmic jit stats ...")` 打到 eden_log.txt。开销可忽略。

### 11.3 TOTK 代码行为画像（计数器实测，199s 会话含启动+读档+90s 游戏；
**时间分布见 §11.8 修正：编译集中在前 ~30s 启动爆发，"持续 6.3k/s"是测量窗口落在爆发期的伪象**）

| 指标 | 数值 | 推论 |
|---|---|---|
| dispatch_lookups（四级调度全 miss） | 2.72M（13.7k/s） | 块链接/RSB/FastDispatch 之外仍有海量新入口 |
| **block_compiles** | **1.26M（会话总）**；时间戳分解：启动 30s ≈ 700k（23k/s）+ 加载/入局 ~550k，其后涓流 | 编译税前置，稳态极低 |
| distinct PC 占比 | **核内 99%+**（见 §11.8：全局仅 48.6%，51% 是跨核重复） | 核内无 churn；跨核全是重复 |
| 64KB 区域直方图 | 782 区域，top 仅 0.53%，**完全平坦** | 无热点，均匀铺满 ~48MB（启动扫全库所致） |
| 采样点指令字节 | 函数序言/中段/尾混合（合法 ARM64） | 真代码，非数据误执行（日志无异常风暴） |
| 主菜单静置（带时间戳复测） | 30s 爆发后 **<200/s，6 分钟后趋零** | 早期"菜单 3.6k/s"读数是爆发尾巴；菜单稳态安静 |
| guest IC IVAU | 15,007 次，菜单局与游戏局**完全同数** | 全部发生在启动阶段；2023 年 yuzu 的 SMC 风暴已不复发 |
| full_clears / fastmem_faults | 0 / 0 | 会话无码缓存打穿、无缺页 |

解读（依 §11.8 时间戳数据修正）：TOTK 在**启动+首次加载**阶段一次性扫过巨型代码库
（KingSystem 模板库全量初始化/注册），~30 秒烧掉 ~70 万块编译；游戏内加载再补 ~55 万；
**稳态编译速率接近零**（菜单 <200/s 且趋零）。含义：
①"热块分级/tier-up"仍无效（无稳定热集），但原因改为"编译税前置"而非"持续新代码流"；
②码缓存打穿推算随之修正：单会话 ~1.3M 块 ≈ 200MB @150B，**常规一局到不了 512MiB**，
"玩久了变卡 = 全清风暴"的假说被削弱（除非超长会话累计；2GiB 改动保留，reserve-only 无代价）；
③稳态模拟核 85.7% 是已编译代码的**执行**时间——稳态 FPS 杠杆在执行质量
（BL→host call、块合并、RA），编译量类优化（共享缓存/磁盘缓存）主要改善**启动/加载时长与前期卡顿**。
**处置：Windows x64 码缓存 512→2048MiB 维持不变**（虚拟预留按需提交，c2d9148f7f）。

### 11.4 本轮实验结果

| 实验 | 结果 | 结论 |
|---|---|---|
| LTO（`build-lto/`，`-DENABLE_LTO=ON` 全量构建） | 44.82/44.78 vs 44.80 | **中性**（Δ<0.1%）；eden 自身代码不是限制因素；构建目录保留可复用 |
| PGO | 未跑 | LTO 中性 + 25ms 帧时量化 → 预期同样中性，跳过 |
| 块入口 32B 对齐 | 未做 | dynarmic 已 16B 对齐（xbyak align 默认），Zen3 边际收益存疑 |
| JIT 计数器 | 已常驻 | 后续任何会话可免费观测 JIT 活动 |

### 11.5 领域调研结论（ARM64→x64 重编译，2026-09 时点）

- **dynarmic 上游已死**：MerryMage 2024-03 删除仓库（yuzu 下架潮），末版 6.7.0；
  **azahar-emu/dynarmic 是唯一活跃 fork**（RA 去随机化、IR identity pass、SSSE3 向量 emit）。
  eden master 的 fork（7.0.0）自带 #4303/#4328 修（jitState 重排/CPUID 静态化）——可 backport 但均为小收益。
- **FEX-Emu 最可借鉴**（fex-emu.com 博客编号）：**guest BL→host call、RET→host ret**
  （FEX-2508，用硬件返回预测器替代软件 RSB，Cyberpunk 单月 +39%）；**RA 内联进 SSA IR**
  +发射期常量折叠（FEX-2506，消除寄存器搬移/溢出）；**跨线程共享 JIT 缓冲**（同客代码只编译
  一次，-25% JIT 时间）；multiblock/始终探索条件落空（FEX-2503/2509）。
- **box64**：FORWARD 前向跳块延续、延迟 flags 反向活跃度、ymm0 追踪。
- **Rosetta 2**：AOT 优先 + 块化 NZCV liveness（FFRI Champollion 系列逆向）。
- **Zen3（本机 5600）**：热块 16/32B 对齐（已有 16B）、op-cache 密度、cmp+jcc 相邻宏融合。
- **不适用/勿做**：TSO/强内存序（方向反，ARM 弱序→x86 TSO 免屏障）、x87、
  全面 branchless、AVX-512、热块 tier-up（本游戏无热集）、LTO/PGO（已证中性）。

### 11.6 JIT 侧剩余路线图（2026-09-09 依 §11.8/§11.9 证据修订；编译税已证实前置，分双轨）

**A 轨——启动/加载时长与前期卡顿**（编译量集中在头 ~2 分钟，~1.3M 块）：

1. **3 核共享 JIT 块缓存**：§11.8 实测 **51% 编译是跨核重复**（57.7% 的块被 ≥2 核编过）
   ——浪费量从推测变为实锤，FEX #4479 同型修复实测省 11~26% JIT 时间。两条实现路径：
   (a) FEX 式共享缓冲+共享查找表（需解决 dynarmic 烘焙绝对地址/Xbyak 无重定位，大工程）；
   (b) **单 Jit 实例 + 3 份 JitState 轮换**（拓扑等价，改动集中 eden 侧，先评估）。
   另可顺手借鉴 FEX "满不清老、换大缓冲老代码续命"替代全清风暴。
2. **跨会话磁盘代码缓存**：每个 eden.exe 进程生命周期（"会话"）内 JIT 全部从零——
   每次启动重付 ~1.3M 次编译税；落盘复用可直接砍启动/加载的编译时间。
   **设计要点（§11.8 实测）**：guest 模块基址逐会话漂移 → 键须 (模块 ID+偏移+内容 hash)；
   host 代码含绝对地址 → 要么固定预留地址加载、要么存重定位表；SMC 安全靠内容 hash 校验。
   参考 FEX AOT（序列化 IR，缓存前端产物回避重定位）。中偏大工程，收益确定性高。

**B 轨——稳态 FPS**（稳态编译≈0，模拟核 85.7% 是已编译代码执行时间，杠杆=执行质量）：

3. **guest BL→host call / RET→host ret**：dynarmic 后端手术（影子返回栈+多入口块+链接改造），
   FEX 实证最大单项（Cyberpunk +39%）；x64 无 ret-reg 是最大不确定项。高风险高回报。
4. **块合并/multiblock**：29% 的块 ≤2 条指令、中位 4 条，62% 块以条件分支收尾——
   每块固定开销（序言/终端/调度）占比畸高；FEX multiblock 有先例。
   与 3 可同做（多入口块是共同前置）。
5. **azahar fork backport** + eden master #4303 jitState 重排；FEX RA 可移植件
   （tied/spill 启发/post-RA 窥孔）归此类，SRA 在 x64 上不可移植。
6. **RasterizerCached 慢路径**（§11.1 的 2.5%+锁竞争）：GPU 无 pending 工作时免锁快查。

### 11.7 本轮产物

- v0.2.1 worktree 提交 **c2d9148f7f**（计数器 + 2GiB 码缓存，bench 44.85 无回退）
- `build-lto/` 完整 LTO 构建目录（与 `build/` 共享 user 目录 junction，bench 用
  `EDEN_DIR=.../build-lto/bin` 切换）；配置命令见 §1.4 加 `-DENABLE_LTO=ON -B build-lto`
- `bench_run.py` 新增 `--no-tap`（菜单静置对照）与 `EDEN_DIR` 环境变量
- 诊断期临时补丁（PC 去重集/直方图/指令转储）已移除，只保留基础计数器

### 11.8 块级 dump 实验（2026-09-09 深夜：重复内容/块形态定量）

工具：`EDEN_JIT_BLOCKDUMP=1` 环境变量 → 每次 GetBlock 编译时写一行
`tid,start_pc,end_pc,FNV内容hash,ms,terminal类型` 到 exe 旁 `jit_blocks.csv`；
离线分析 `F:\prof\analyze_blocks.py`。两轮独立会话（标准 bench ~5min + 游戏内静置 8min）
数字完全一致，代表性可信。**注意 printf 格式串曾编译失败被 tail 掩盖退出码**——
构建命令必须检查 BUILD_OK/退出码，勿信管道尾部。

| 指标 | 数值（bench 局 / hold 局） | 含义 |
|---|---|---|
| 总编译 | 1.26M / 1.29M | 与计数器口径一致 |
| 核内重复编译 | 0.3~0.8% | **单核 JIT 自身缓存健康**（仅启动期失效/FPCR churn） |
| **跨核重复编译** | **51.1% / 51.4%** | 同一 PC 被 2~3 个核各编一份 |
| 被 ≥2 核编过的 PC | 57.0% / 57.7% | TOTK 线程在核间高频迁移，3 套 JIT 互不知情 |
| **内容级重复** | **21.8%**（13.6 万 PC） | 不同地址上字节完全相同的块（模板/静态库多副本）；最热同款 6 指令序列 ×4451 份、1 指令 ×2997 份 |
| 块长（指令数） | med 4 / mean 6 / p90 12；**29% ≤2 条** | 游戏分支密度高（比较链/switch/模板校验），非引擎切块问题：DMB/DSB 不切块，仅 ISB/MSR/分支/异常切 |
| 块中入口碎片化 | 5.8% | 从已编译块中部进入的新块，正常范围，非主要浪费 |
| 代码足迹 | 8.9k 个 4K 页 = 34.7MB，跨度 78MB，3 簇（43.67+7.83+6.38MB span） | 3 簇 = main + 两个 subsdk 模块 |
| **guest 模块基址** | 0x80efc000 vs 0x803c7000（两局不同，簇大小相同） | **模块加载地址逐会话漂移** → 磁盘码缓存的键不能用裸 guest PC，须模块相对+内容 hash |

回答"是不是 JIT 引擎的问题、这么大缓存没有重复内容吗"：
1. **核内**：无缺陷，重复率 <1%；
2. **跨核**：51% 编译是纯重复——这是 eden/dynarmic "每核一套 JIT 实例"架构的真实代价，
   FEX #4479 修的就是这个（他们省 11~26% JIT 时间，我们的重复率更高，收益应更大）；
3. **跨地址**：21.8% 的块内容与其他块完全相同（游戏链接器把模板代码复制到多个地址/模块），
   理论上可内容寻址去重，但 IR 里烘焙了绝对 guest 地址，需重定基，工程复杂收益次之；
4. 块小是游戏性质（分支密），不是 bug；真正可动的是"块合并/multiblock"（FEX 有先例）。

**时间分布（带时间戳局，菜单静置 466s；两轮独立菜单局 709k/705k 完全一致）**：
t=0-30s 爆发 **22.9k/s**（≈97% 的编译量，即启动+初始化扫全库），30s 后 <200/s，
6 分钟后趋零。→ 早期"持续 6.3k/s"结论作废（测量窗口在爆发尾巴）。
游戏内稳态由总量三角定位：游戏局 1.26-1.29M = 菜单底座 ~0.71M + 进图/加载爆发
~0.55M + 360s 静置 × 稳态 → **游戏内稳态亦仅 ~100/s 量级**（编译税同样前置）。
（hold-v2/v3 两轮 tap 焦点抢夺失败停在菜单，反成两次独立的菜单复测；游戏局总量
来自 bench 与 hold-v1。）
**块终止类型分布**（全量）：CheckBit 34.4% + If 27.6%（合计 62% = 条件分支收尾，
印证分支密度）、LinkBlockFast 23.3%（直连跳转）、PopRSBHint 8.9%（返回）、
FastDispatchHint 5.8%（间接跳转）、CheckHalt/ReturnToDispatch ≈0。

### 11.9 FEX 三大招代码级调研（仓库已克隆 `F:\devel\opensource\FEX` @ 208e9c3）

**① call-ret 影子栈（PR #4670，FEX-2508，Cyberpunk +39% FPS / Clang +10%）**
机制：专用 host 寄存器 x25 作影子返回栈指针，每项 16B `{guest RIP, host 返回落点}`。
guest CALL：返回地址照写 guest 栈（正确性），同时 `stp` 压影子栈，发射真 host `bl`
（首次经 thunk，执行后 backpatch 成直接 bl）；guest RET：无条件弹影子栈，比对弹出的
guest RIP 与实际返回目标——相等则 `ret TMP2` 直落 CALL 点后（硬件 RAS 完美预测），
不等则走 L1 查表→dispatcher（仍以 `ret` 返回，保持 host call/ret 配对）。
兜底四层：返回块未知时压哑元保持深度同步；guard page 溢出 SIGSEGV 重置 x25 到栈中部；
cache 失效时整栈清零（陈旧 host 指针必然值不匹配）；前置要求 = 多入口块
（返回地址即块入口）+ 重写的块链接（call/branch 可区分的 patch 标记）。
关键代码：`FEXCore/Source/Interface/Core/JIT/BranchOps.cpp:160-238`（发射）、
`JIT.cpp:531-612`（运行时链接/backpatch）、`Frontend.cpp:1140-1161`（返回地址登记为入口）。
**dynarmic 移植难点**：x64 无 `ret reg`（须 `push target; ret` 且 host SP 由 JIT 掌控，
或退化为 `jmp` 丢 RSB 收益——这是 +39% 能否复现的最大变量）；rel32 ±2GB 需 thunk 两级；
AArch64 guest 大量 tail call 靠值不匹配回退兜底；dynarmic 的 BL 写 X30（寄存器而非栈），
比对反而比 x86 简单。

**② RA 内联进 IR（PR #4580，FEX-2506）——注意：与传闻不同**
实际是"把分配结果（物理寄存器号）内联进 IR 数据结构"（`OrderedNode::Reg` 1 字节 +
参数槽改写为 PhysicalRegister 立即数），RA 仍是独立 pass。真正的算法重写是 2024-05
`725d0e18`：两遍 block-local 线性分配，利用 FEX IR 不变量"无跨块活跃值"（跨块状态
住在 SRA 固定 host 寄存器：arm64 主机 pin 18 GPR + 16 FPR）。move 消除四支柱：
PreferredReg 归位 coalescing、TiedSource 绑定、post-RA 窥孔（mov 折叠/压弹栈配对）、
furthest-first spill + 常量 remat。
**dynarmic 对照**：其 x64 RA 本就是块局部在线分配（`reg_alloc.cpp`），已有 last-use 覆写；
可移植件 = tied 元数据/spill 启发/post-RA 窥孔（中工程量，收益边际）；
SRA 常驻在 x64 16 GPR 上 pin 不下（需要 34 个），**不可移植**——move 消除的最大来源天然缺失。

**③ 跨线程共享 JIT 缓冲（PR #4479，-25% JIT 时间；Tracy 实测 Mirror's Edge 26%/GoW 19%）**
机制：进程单块 RWX 大缓冲，无锁原子 bump 分割；线程先编到私有临时缓冲再 memcpy 重定位
进共享区（避免全局锁，早期全局锁方案实测吃掉 JIT 时间 5~14%）；**共享 L3 guest→host
查找表**才是省时的本体——别的线程编过的块直接复用，"每块恰好编译一份"是软不变量
（竞态窗口容忍泄漏一份重复）。缓冲满不清老：换 2× 新缓冲、老缓冲只读续命
（"partially persistent"）——**这是对全清风暴的结构性解法**，值得借鉴到 2GiB 打穿场景。
**dynarmic 移植障碍**（比 FEX 多一层）：生成代码烘焙了 per-instance 绝对地址
（Devirtualize 回调+this、tpidr 存储、prelude 地址），Xbyak 单阶段直写无重定位，
patch 注册表/emitter 私有，失效协议 per-instance。**§11.8 实测 51% 跨核重复 →
收益直接可期**；务实折中 = 单 Jit 实例 + 3 份 JitState 轮换进入（拓扑等价，改动集中
在 eden 侧持有模型）。

先例补充：FEX 另有 AOT 工具链（`Source/Tools/FEXInterpreter/AOT/`，序列化 IR 供离线
编译——缓存的是前端产物而非机器码，天然回避重定位），是磁盘码缓存设计的重要参考。

### 11.10 本轮（09-09 深夜）产物

- v0.2.1 worktree：块 dump 补丁（`a64_interface.cpp`，env `EDEN_JIT_BLOCKDUMP` 门控，
  正式提交见 git log "block dump instrumentation"）；开启时对 FPS 无可测影响（44.85）
- `F:\prof\analyze_blocks.py`（去重/碎片化/块长/簇/terminal/衰减 离线分析）
- `F:\prof\jit_blocks_gameplay_0909.csv`、`jit_blocks_hold360_0909.csv`（4 列原始 dump 存档）、
  `jit_blocks_holdv2_0909.csv`、`jit_blocks_holdv3_0909.csv`（6 列带时间戳/terminal，两轮菜单复测）
- `F:\devel\opensource\FEX`（浅克隆 master @ 208e9c3，代码级调研用）
- 坑：构建命令带 `| tail` 会掩盖失败退出码——检查产物时间戳或显式 echo 标记
