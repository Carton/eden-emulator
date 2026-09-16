# Eden TOTK 性能优化进度记录（Profile Progress）

> 写给接手的 agent / 开发者；随仓库跟踪维护。原 `F:\prof\HANDOFF.md` 已并入本文件。
> 目标：在 Eden 模拟器（yuzu 血统）上优化《塞尔达传说 王国之泪》
> 的运行帧率。本文档记录当前基线、已验证的工具链、初步分析结论、实验方法和所有脚本的用法。
> **纪律：本目录及一切 AI 生成内容仅限本地使用，禁止以任何形式提交到上游仓库 / issue / PR。**
> **本文档 = 优化过程的唯一记录处**（AGENTS.md 只维护通用规则/流程/当前状态）：
> **每取得阶段性成果（优化提交、新结论、新坑、状态变化）必须更新本文件（含数据与复现要点）再收尾**；
> 改代码的顺序纪律 = commit → 编译测试（保证产物对应 commit，便于归档存档/trace 复现对比）。

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

> **2026-09-14 起**：活跃脚本（check_config / bench_run / fixverify_run / visual_run /
> rot_test / rot_trace / patch_input）的 EDEN_DIR 默认值已改为 master 构建目录
> `F:\devel\opensource\eden-emulator\build-vs22\bin`（EDEN_DIR 环境变量仍可覆盖；
> bench_run 的 commit 标签读 eden-emulator 仓库）。历史一次性分析脚本
> （analyze_* / rot_probe* / cap_* / ninja_windows / config_diff / reanalyze_good）
> 保留 v0.2.1 时代路径不动——它们记录的是当年的分析对象。
>
> **trace/存档归档（2026-09-15 建立）**：`F:\prof\archive\<日期>_<主题>_<commit>\`
> 收纳关键 trace + 配对存档/配置/二进制，各目录带 README（commit 配对、恢复方法、
> 注意事项）。现存：`2026-09-15_pond-round_18aaa38cef`（pond.etl + 水塘存档 + exe/pdb）、
> `2026-09-13_idle-t3_v021_06c7a2a6b2`（t3.etl，存档未留存已注明）、
> `2026-09-12_rotation_v021_pre-fix`（rot.etl）。**已删除**（结论均已文档化）：
> rot_v1_gpuuncompressed / cpugpu_0907_baseline / cpugpu_0908 / cpu25 / boot_emit 共 20.6G。
> 规矩：新里程碑按"commit → 编译 → 采集 → 归档"走，**PDB 必须随 trace 归档**
> （pond.etl 的时代 PDB 已丢失，教训）。

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

## 12. FEX 移植实战轮（2026-09-10 深夜：编译成本解剖 → IR 缓存 → RegAlloc O(1)）

### 12.1 决策依据：三段微基准（GetBlock 计时，jit_stats 常驻）

菜单 hold 局（703k 编译）：**translate=1.71s（2.4µs/块）optimize=1.30s（1.8µs/块）
emit=15.27s（21.7µs/块）——Emit（RA+x64 发射）占编译成本 83.5%**。
推论：①IR 级共享/缓存（只跳过 translate+optimize）收益上限仅 ~16.5%；
②此前"磁盘码缓存性价比最高"的估值隐含全编译成本假设，**只缓存 IR 的版本同样要打折**；
③编译税的真靶子在 Emit 内部。

### 12.2 实施一：进程级跨核 IR 共享缓存（c10446fecc，EDEN_JIT_IRCACHE=1，默认关）

设计：`ir_cache.h/cpp`——Optimize 后的 IR 序列化进全局 map（键=LocationDescriptor u64，
内容=inst{op,name,args}/terminal 递归/块元数据）；其他核命中时反序列化跳过
Translate+Optimize（Emit 仍各自做）。**正确性核心两点**：
- 内容 hash 在翻译时"边读边织"（get_code 包装器累积 FNV）——条目哈希与产生它的字节
  天然一致，SMC/重译竞态窗口不存在；
- hit 路径重算 hash 校验当前 guest 字节，不匹配即 miss 重译（替代主动失效）。
反序列化用 SetArg 语义自动重建 use_count 与伪指令链；块 >30 指令时复刻
PrependNewInst 的池分配逻辑（两阶段：先建 inst 后连 arg）。
实测（游戏局 1.26M 编译）：**hits=639k/stores=624k（50.6%，与 51% 跨核重复实测吻合），
hash_mismatch=0，FPS 44.76 无回退**；代价 335MB/62 万条。定位=实验特性+磁盘码缓存基建
（收益 ~2.8s CPU/会话，本身性价比有限）。

### 12.3 实施二：RegAlloc ValueLocation O(1) 反向索引（c3bc2c60d0）——本轮实际赢家

ETW 启动期采样（totk_boot_emit.etl，50s 爆发窗口）函数级 Top（eden.exe 35.3k 采样）：
**HostLocInfo::ReleaseAll 7.9% + RegAlloc::ValueLocation 5.7% + __std_find_trivial_impl
3.3%**——`ValueLocation` 每次 Use/Define 都 O(48) 线性扫全部 HostLoc（内层还是 std::find）。
这正是 FEX-2506 RA 重写消灭的东西（他们的 RA 状态=Available 位图 + RegToSSA[32] 直接映射）。

dynarmic 移植（最小切口）：`name_to_hostloc[4096]`（u8，inst 名→hostloc+1，0=未跟踪），
NamingPass 的块内稠密编号 1..N 做键；RegAlloc 每块 placement-new 天然清零；
**死值的陈旧项无害**（uses 耗尽后永不再查询）→ 只需 Define/Move/Exchange 三处重跟踪；
>4096 指令的块回退原线性扫描。审计确认 AddValue/values 触碰点仅 reg_alloc.*。

实测：菜单局 emit **15.27→12.54s（-17.9%）**；游戏局 **22.5→17.9µs/块（-20.5%）**；
translate/optimize 每块成本不变；游戏全流程+FPS 44.4（基线带内）验证正确性。

### 12.4 结论与下一步

- 两个提交都在 `test/v0.2.1-profiling`（worktree 现所在分支）：c10446fecc + c3bc2c60d0
- Emit 剩余 17.9µs/块的下一批热点（按 ETW 排）：ReleaseAll（7.9%，48 个 HostLoc 的
  small_vector 清理——可用占用位图跳过空槽）、Xbyak label/编码机制（~3.4%）、
  descriptors/patch map（~1.2%）。FEX RA 的 furthest-first spill/tied 绑定在此之上。
- 稳态 FPS 不受编译优化影响（稳态编译≈0）；本轮收益=启动/加载更快更顺 + 长加载场景
  （读图/传送）卡顿减少。
- 工具沉淀：GetBlock 三段计时常驻（每局 eden_log 可见）；ETW 查询大 trace 的正确姿势
  =先模块分组（秒回）再加 ModuleName 过滤做函数分组（直接函数分组会 30s 超时）；
  PerfView CLI 备选。UAC 提权采 wpr：powershell Start-Process -Verb RunAs。

## 13. 帧率天花板调查轮（2026-09-11：45 FPS 之谜 → 栅格量化 + work-bound 铁证）

### 13.1 问题与线索

用户实测默认帧率无法超过 45 FPS。逐帧 CSV 分析发现帧时长不是连续分布而是**严格量化**：
游戏内 `25.0, 25.0, 16.7` 循环（3 帧和恰 66.7ms = 精确 45.00 FPS），菜单则完美锁 60.00
（纯 16.67ms）。均值 44.8 = ~2/3 帧 25.0ms + ~1/3 帧 16.67ms 的混合。

### 13.2 节拍机制溯源（代码级）

帧 CSV 边界 = `nvdisp_disp0::Composite`（游戏经 HOS 合成器提交显示的时刻，非主机呈现）。
驱动链：`VI::Conductor`（core_timing 循环事件）→ `ProcessVsync` → `ComposeLocked`。
**Conductor 周期 = 16.67ms × speed_scale**，`speed_scale = (100/speed_limit) / compose_speed_scale`。

探针实测（bbcf1eb47a，每 600 帧打 raw_swap_interval/speed_scale）：
**TOTK 游戏内以 `swap_interval=0` 提交 buffer**（其动态 FPS 引擎行为）→ eden 的
NormalizeSwapInterval 把非正值当"速度倍率"（×2）→ **合成器 120Hz 运转，栅格 8.33ms**
（日志时间戳每 5.0s 恰 600 tick 验证）。25.0ms=3 栅格、16.67=2 栅格：游戏每帧占
2~3 个栅格，[3,3,2] 节拍 → 45.00 FPS 整。菜单负载轻 → 恒 2 栅格 → 60.00。

另：user/load 里有 TOTK mod（"!!!!TOTK Optimizer" exefs 补丁 + 10xDurability，
自 yuzu 安装复制而来）。~~45fps 意味着游戏以 1.5 倍速运行~~（09-11 用户实测更正：
**游戏速度体感正常**——TOTK 动态 FPS 引擎将逻辑步进与渲染帧率解耦，逻辑按
模拟时间恒为 1 倍速，45fps 只是多渲染的插值帧，此前的 1.5 倍速推断不成立）。

### 13.3 三连对照实验（同 exe、同场景、仅改 qt-config）

| 实验 | 配置 | FPS | med | 结论 |
|---|---|---|---|---|
| A grid-default | 默认（限速 100%） | 44.84 | 24.98ms（量化） | 栅格 [3,3,2] 节拍 |
| B grid-unlock | use_speed_limit=false | 44.89 | **22.27ms（连续）** | **真实工作速率 44.9** |
| C grid-boost | fast_cpu_time=Boost(2.0×) | 44.85 | 24.98ms | 超频无效 |

- B：speed_scale=0.01 → 合成器 ~6kHz → 量化消失，med=p95≈p99≈22.3ms → **纯工作
  吞吐 = 44.9 FPS，栅格不损失吞吐**（[25,25,16.7] 均值 22.2 与工作速率一致）。
- C：模拟时钟 ×2 完全不影响帧率 → governor 无等待偷时，**100% work-bound**。
- 排除项复核：sync_core_speed=false（默认）→ 经典限速器路径本就不生效；
  fast_cpu_time 超频只改游戏内部时间流速不改 FPS（work-bound 下的预期行为）。

### 13.4 结论：为什么优化了却不动 FPS + 真正的杠杆

1. **45 不是巧合也不是软上限，是 22.27ms/帧工作量的 120Hz 栅格量化显示**。
2. **帧率只能整量跳变**：45（3 栅格）→ 60（2 栅格）。工作砍 10% → 解锁模式可见
   49fps，但栅格模式仍是 45 不动；**必须 ≤16.67ms（-25.1%）才跳 60**。这解释了
   GPU 线程 -3.3%、emit -20%、LTO 全中性——都没跨过栅格阈值，也没砍到稳态关键路径。
3. **关键路径 = 4 个全饱和线程的流水线**（3 个 JIT 模拟核 + gpu_thread），
   每帧每线程满负荷 ~22.27ms。**单线程砍 25% 无效**（其余 3 个还卡着）；
   CPU Unsafe 是过去唯一动了 FPS 的优化（+2），因为它同时砍了全部 3 个模拟核。
4. **按预期帧率收益排序的方向**（B 轨正式定级）：
   - **JIT 执行质量**（唯一够得着 -25% 的公共乘数，作用于 86% 的模拟核时间）：
     BL→host call 影子栈、块合并/尾重复、FEX RA 思想深化；
   - **gpu_thread 摘出关键路径**：VulkanWorker 仅 ~50% 利用率，命令处理并行化/
     批量化（PushImageDescriptors/绑定缓存/管线键比较再砍）；
   - 已到顶：CPU Unsafe（已开）、分辨率无关性（已证）、超频（已证无效）、
     异步 shader（只影响卡顿不影响均值）。
   - 待查的免费项：12 逻辑核上 4 个饱和线程的超线程配对争抢（亲和性检查）。
5. **方法论**：以后一切稳态优化判定改用**解锁模式基准**（use_speed_limit=false，
   看 med ms 连续反映工作速率，无量化失真；本实验 1%low 还从 35.05 提到 38.64，
   自旋无观测开销）。栅格模式只用于还原用户真实体验。

### 13.5 IR cache 默认开启（76a192eb28，本轮顺带完成）

EDEN_JIT_IRCACHE 默认值反转为**开**（EDEN_JIT_IRCACHE=0 关闭），加 512MiB 内存上限
（EDEN_JIT_IRCACHE_MAXBYTES 可调；TOTK 整局实测 335MB 不触顶）。Run A 无 env 验证：
hits=629k/stores=635k（49.7%）、hash_mismatch=0、translate+optimize 从 ~9.3s 降至
2.73s（**编译期 CPU -71%**）、FPS 44.84 基线带内无回退。收益定位：启动/加载更快
更顺（编译税前置在启动 30s 爆发期）+ 未来磁盘码缓存的现成基建。

### 13.6 本轮产物

- 提交：76a192eb28（IR cache 默认开+cap）、bbcf1eb47a（HWC 节拍探针，常驻，
  每局 eden_log 可看 raw_swap_interval/speed_scale）。
- bench_results.csv 新增 grid-default / grid-unlock / grid-boost 三行。
- qt-config 已还原（speed_limit=100 默认、fast_cpu_time=Off）。

## 14. 视角旋转掉帧攻坚轮（2026-09-12：复现 → ETW 归因 → ASTC 流送根因 → 配置修复）

### 14.1 测试基建（全部沉淀在 F:\prof，可复跑）

- **输入自动化**：qt-config player_0_rstick 改 `analog_from_button` + 键盘 J(=左)/L(=右)。
  血泪坑：**方向键（Arrow）不行**——Qt 焦点导航吃掉方向键（X 键等普通键可达
  GRenderWindow::keyPressEvent，方向键被导航消费），必须用字母键；SendInput 用 VK。
- 脚本链：`rot_hold.ps1`（抢焦点+按住 J/L）→ `rot_test.py`（idle/rotR/rotL 交替窗口，
  落 rot_windows.json sidecar）→ `rot_trace.py`（编排 wpr 100s 采集+窗口对齐）。
  坑：脚本内"帧墙钟映射"一直没修好——**一律用 sidecar + 末端锚定离线分析**（CSV 末帧
  ≈ t_close+2s，误差 <1s）；窗口各留 2s 边距。
- **掉帧是确定性复现**：两局独立会话的尖刺时间戳逐帧一致（+0.81s:33.4ms、
  +1.47s:142.7ms）——同视角扫描触发同批工作。

### 14.2 复现画像（async shaders ON，2×[idle20/rotR20/idle20/rotL20]）

- idle：44.7-44.9fps，>30ms 尖刺 0-5 个/20s（PS 抢焦点操作本身的噪声）
- 旋转：43.6-44.1fps，**尖刺 20-32 个/20s**（几乎全部 33.3/41.7ms = 4/5 个 8.33ms 栅格），
  偶发 50-91ms；**async shaders OFF 时首见内容有 303ms 大卡顿**（同步管线编译，ON 后消失）
- ETW 采集开销会把整体压到 35-40fps（采样抓栈税），差分结论不受影响但绝对值不可比。

### 14.3 ETW 归因（totk_rot.etl，9.1GB，100s 旋转序列，0 丢事件）

- **线程差分（每秒采样归一）**：旋转时 GPU 线程 +7%（925 vs 865/s），**三个模拟核反而
  -10~13%，总 CPU 更少**——瓶颈不在 CPU 烧量，在 GPU 线程每帧耗时变长（帧少了它的
  总时间反增）；模拟核在等它 → GPU 线程是旋转场景的节拍器。
- **大尖刺窗口 [1.3,1.9]s**（含 143ms 卡顿）：GPU 线程 563ms/600ms = **94% 满负荷运行
  eden.exe 自己的代码**（非等待驱动、非调度延迟）；VulkanWorker 7195 次切换（小任务风暴）。
- **排除项**：采集窗口内 CreateGraphicsPipeline=0（管线缓存全命中）→ 管线编译排除；
  驱动模块（nvlddmkm/nvoglv64）采样占比 <2% → 排除。
- **关键异常**：二次旋转（rotR#2 尖刺 204 vs rotR#1 202）**毫不便宜** → 缓存逐出循环。
- 坑：本会话 eden.exe 的 PDB 符号始终不加载（save_symbol_configuration + close/re-process
  均无效，FunctionName 全 null；内核符号正常）——函数级精确归因欠账，trace 已存
  F:\prof\totk_rot.etl 待符号修复后复查。

### 14.4 根因与修复实验（代码级路径 + A/B 闭环）

代码链（vk_texture_cache.cpp Image::Image 构造 + texture_cache.h UploadImageContents）：
RTX 2060 不支持原生 ASTC → 每张 ASTC 纹理标记 Converted+CostlyLoad；当前配置
（accelerate_astc=Gpu + astc_recompression=Uncompressed）走 **AcceleratedUpload 分支：
GPU 线程同步执行 ReadBlock+swizzle+compute dispatch+barrier**；且解码结果以
**RGBA 未压缩存储（4B/px，ASTC 源约 1B/px）→ 显存膨胀 → 纹理缓存 LRU 快速逐出 →
旋转重扫时反复重建+上传**——与"二次旋转不便宜"精确吻合。
（textures/workers.cpp 的 ImageTranscode 线程池在本树是死代码，无人调用。）

A/B（同 async shaders on，2×4 窗口）：

| 配置 | 旋转尖刺>30ms | >50ms | max | 旋转 fps | 二次旋转 |
|---|---|---|---|---|---|
| Gpu+Uncompressed | 107 | 8 | 91.6ms | 43.6-44.1 | 不变便宜 |
| CpuAsynchronous+Bc3 | **53（-50%）** | 7 | **58.3ms** | **44.3-44.7** | **19→11 变便宜** |

机制：CpuAsynchronous 走 QueueAsyncDecode（真异步路径，texture_cache.h:1149）把解码
挪出 GPU 线程；Bc3 让显存占用 1B/px（驻留量×4）打破逐出循环。
**结论：旋转掉帧根因=ASTC 纹理流送（GPU 线程同步上传 + 未压缩存储逐出循环）**。

### 14.5 最终推荐配置（已留在 qt-config，日常可用）

- `use_asynchronous_shaders=true`（消 300ms 级管线编译大卡顿）
- `accelerate_astc=2`（CpuAsynchronous，解码出关键路径）
- `astc_recompression=2`（Bc3，显存 1/4，打破逐出循环；有损压缩需肉眼验收）
- 叠加效果：旋转场景尖刺 -50%、最大卡顿 303→58ms、旋转掉幅 -1.2→-0.3fps。
- Bc3 画质验收（已做，10:30 截图 + 视觉模型检查）：无 4×4 块状伪影、无泥糊纹理、
  HUD 锐利，天空渐变轻微 banding 属原生级（FSR 贡献为主），45fps/22.10ms——**通过**；
  若后续察觉不可接受的 banding，
  可只保留 CpuAsynchronous + Uncompressed（解码仍出关键路径，但逐出循环会回来一半）。

### 14.6 遗留与下一步

- ~~函数级归因欠账~~（09-12 已补）：**符号失败根因=D 盘 100% 满**（SymCache 写不进
  eden.pdb 的 263MB 转换；ntoskrnl 能用是因早已缓存）。修复=符号设施整体挪 F:
  （SymCache+SymServer+symbolPath 加本地 PDB 目录），旧 D 盘缓存 14GB 已清（D 盘
  100%→73%）。补上的函数级证据：旋转窗口 GPU 线程 eden.exe 采样里纹理流送三件套
  清晰可见——`Tegra::Texture::SwizzleImpl<0,16>` 249（上传 swizzle）、
  `RemoveImageViewReferences` 124（逐出清理，ICF 折叠名）、`RefreshContents` 100
  （内容重传），与 ASTC 根因结论互证；其余 Top 与 §5 基线画像一致
  （ProcessCommands 450/CallMethod 367/PushImageDescriptors 189/GpuToCpuAddress 147）。
  **注意**：CpuAsynchronous+Bc3 的 trace 是旧配置采的——新配置下的函数级再采样
  可确认 SwizzleImpl/RefreshContents 是否如期消失于 GPU 线程（下轮验证项）。
- 每帧 -0.3fps 的持续项=旋转时 GPU 线程命令处理变贵（视野 draw 增多），属 §13 已定的
  GPU 线程吞吐方向，非本轮新问题。
- 代码级可做（后续）：ASTC 异步解码路径的 worker 池化质量（现走 Common::ThreadWorker）、
  纹理缓存 LRU 预算自适应；rot_test.py 脚本内帧映射 bug 仍未修（用 sidecar 离线分析绕过）。

## 15. 第一梯队收官轮（2026-09-12 午：复测验证 → 剩余尖刺定性 → 600Hz quick win）

### 15.1 新配置复测（totk_rot.etl 第二版，CpuAsynchronous+Bc3+解锁）

- **纹理摘出 GPU 线程已证实**：新 trace 旋转窗口 GPU 线程函数级分布中
  SwizzleImpl/RemoveImageViewReferences/RefreshContents 几乎归零（旧配置分别
  249/124/100 采样）——§14 的配置修复机制闭环。
- ImageTranscode 解码 worker 全程仅 431 采样（~0.4%）——解码池不是瓶颈（B1 优化证伪）。
- **剩余 140ms 级尖刺定性**（确定性复现于旋转起手 ~1s）：尖刺窗口 GPU 线程
  92% 满负荷跑**常规命令流**（ProcessCommands/CallMethod/PushImageDescriptors 均匀分布，
  无单一大热点、无长等待）——是"新视野内容 draw 命令洪峰的量堆积"，属 GPU 线程
  每 draw 吞吐范畴（第三梯队），不再是纹理路径。
- 注意：带 ETW 采集时该尖刺放大到 140ms（采集税 ~15-20%）；无采集时同场景
  max 58-63ms——用户日常以无采集数字为准。

### 15.2 解锁自旋税与 600Hz quick win（c8b0c853f8）

- 复测发现解锁（use_speed_limit=false，speed_scale=0.01）的代价被低估：
  **HostTiming 线程 36% 核 + VSyncThread 19%**（6-12kHz 节拍自旋+唤醒风暴），
  对 4 线程饱和流水线是净干扰。
- **修复**：conductor.cpp 解锁 speed_scale 下限 0.1（≈600Hz 轮询，栅格 1.67ms
  仍比帧时长细 13 倍，视觉无损）。
- **验收**（解锁+CpuAsync+Bc3+600Hz，2×4 窗口 vs 6kHz 基线）：旋转尖刺>30ms
  **53→30（-43%）**、>50ms **7→3**、max 63.2（首见窗口偶发）、med 22.48ms
  连续无量化、fps 持平（rot 44.4 / idle 44.8）——调度干扰降低的净收益。

### 15.3 环境事故与坑（本轮连环排雷，全部记录）

1. **ETW MCP 进程吃 24.5GB commit**（两个 9.1GB trace 的处理数据常驻）——
   close_trace 30s 超时没释放，直接 taskkill 进程（trace 文件在磁盘无损）。
2. **僵尸 DiagTrack 会话**（`WPR_initiated_DiagTrackMiniLogger_*_20260908`，挂了
   4 天）——logman stop 报"找不到元素"，**重启 DiagTrack 服务**（net stop/start）
   才清掉。wpr_run.cmd 的自愈 cancel 只管 wpr profile 会话，管不了 DiagTrack 的。
3. **内存计数器假读数**：上述清理后 WMI/Get-Counter 仍报 FreeCommit=0.01GB/
   FreeRAM=0.01GB，但系统流畅、所有进程合计 ~3GB——读数坏了（可能被杀的
   MCP 进程留下的计数器状态）；eden 实测能正常 8GiB reserve，无视读数。
   判别法：看系统是否流畅+进程总和，别只信计数器。
4. **qt-config 的 use_speed_limit 会被游戏退出写回 true**：外部改 ini 后游戏
   正常退出时按配置分层重写，解锁丢失（启动时读到的是 false，运行时确实解锁，
   但下次启动前又被覆盖）——**持久解锁需在 GUI 里关一次"限制速度"**（GUI 写盘
   语义完整）。排障时记得每局启动前 grep 确认。
5. rot_hold.ps1 的 stderr 偶发 None（进程捕获竞态）——拼接前 `or ""` 防御。

### 15.4 第一梯队结论

- 纹理工作摘出 GPU 线程：**配置级达成**（CpuAsync+Bc3），代码级 B1（解码池）证伪、
  B2/B3（swizzle/逐出）目标已被配置覆盖，无需再动。
- 旋转场景当前状态：idle 44.8-44.9fps 零尖刺；旋转 44.4fps、尖刺 30 个/80s、
  max ~60ms——从最初（idle 也有噪声、旋转 107 尖刺、max 303ms）改善：
  **尖刺 -72%、最大卡顿 -80%、fps 掉幅 -75%**。
- 剩余项=draw 洪峰堆积（第三梯队 GPU 线程每 draw 吞吐）+ 模拟核 JIT 执行质量
  （第二梯队 45→60 主战场）。

## 16. 第二梯队量化 + 旋转回归排查轮（2026-09-12 下午）

### 16.1 第二梯队量化（指令密度 + 钉核证伪）

- **指令构成**（dynarmic 翻译期计数器，临时改动已 stash：`git stash list` 可见）：
  total=3,839,596 翻译指令中 BL 3.39% / BLR 0.81% / BR 0.09% / RET 1.56% /
  B.cond 4.25% / CBZ 3.49% / TBZ 1.72% → 折算 ~1 亿次 guest 调用/秒 + 3700 万返回/秒，
  **BL→host call 影子栈路线（§11.6）靶量充足**。
- **钉核实验证伪**（bench_run.py 新增 `--affinity`，DEVNULL 防 reader 崩溃）：
  mask=63（逻辑核 0-5 = 6 物理核，关 SMT 让位）fps 43.19 / 1%low 23.86，**差于**
  默认 44.83/32.43——eden ~5.2 核 + 配套线程需要 SMT 余量，硬钉 6 核反而挤。
  路线关闭，勿再试。

### 16.2 用户构建崩溃取证（简记）

- 用户 12:29 构建的 eden.exe（现名 build/bin/eden_held.exe，PE 时间戳吻合）12:52/12:53
  两次加载崩溃；CrashDumps 有 dmp：**0xC0000409 fastfail=7 = CRT abort，即 UNREACHABLE()**
  （AssertFatalImpl）。后续按用户指示不再深挖；注意 `%LOCALAPPDATA%\CrashDumps` 有
  WER LocalDumps 落盘，python `minidump` 包可直接解析异常码/线程栈 RVA。
- stash 后按 AGENTS.md 标准命令重编的 eden.exe 与用户 12:29 产物**字节大小完全一致**
  （55,609,344）——代码同源，构建方法无差异。

### 16.3 旋转性能回归排查（本轮主体）

统一口径重测（修正分析器单位 bug 后，见 16.4）：**回归真实**——旋转 fps 38.9-39.2 /
尖刺 288-302 个（40s 实转），而 10:24/10:33/12:08 三个好会话为 44.2-44.8 / 8-21 个；
**idle 完全无恙**（44.0-44.9）。

排查采用"逐项证伪常量"，全部排除：

| 嫌疑 | 结论 | 证据 |
|---|---|---|
| exe 代码 | 恒定 | 字节大小一致（同 commit c8b0c853f8 重编） |
| 全局 qt-config | 恒定 | 与官方安装全量 diff，除调优键外一致；FSR/FXAA/GPU Low 官方也是 6/1/0 = 用户基线 |
| **游戏级配置**（新发现） | 恒定 | `user/config/custom/0100F2C0115B6000.ini` 覆盖全局（resolution=2/vram=1/fence=3/8GB/CPU Unsafe），与官方逐行一致，9/6 后未改 |
| shader/管线缓存 | 恒定 | user/cache/shader 的 vulkan.bin(318MB)+vulkan_pipelines.bin(127MB) 9/6 冻结，好/坏会话同状态 |
| 游戏存档/场景 | 恒定 | nand 内存档文件 9/11 后从未写入（优雅关闭不落存档） |
| NVIDIA Overlay | 排除 | 15:55:42 才启动（DRS 配置库 15:55:52 同步变更），杀掉后重测依旧差 |
| 驱动版本/功耗 | 排除 | 591.86 未变；活体采样 SM/显存频率全程顶满（1350+/7200MHz），util ~38% = 历史基线，显存 2.4GB 无膨胀 |
| 内存/系统负载 | 排除 | RAM 15GB 空闲；idle 窗口干净 |

**剩余唯一解释：内核/WDDM/驱动会话状态累积劣化**。机器 10:09 开机，好成绩集中在
开机后 2 小时内；之后经历 2 次 fastfail 崩溃、官方版跑局（14:28 起 nvcontainer）、
NVIDIA App 15:55 配置变更、多轮强杀/ETW 采集。**验收流程：重启 →
`python F:/prof/rot_test.py afterreboot-1`（自带 preflight）→ 旋转回 44+ 即版本合格**；
仍差则抓旋转窗口 ETW 微 profile 对比 §14.3 热点画像。

### 16.4 工具沉淀与坑

- **`F:\prof\check_config.py`**（启动前检查，已接入 rot_test.py / bench_run.py preflight，
  rc≠0 拒绝起测）：守护调优键（ASTC CpuAsync/Bc3/async shaders/CPU Unsafe）+
  **用户基线键（FSR=6/FXAA=1/GPU Low=0，勿"修"回编译默认）** + 默认键白名单 +
  **禁跑进程（NVIDIA Overlay.exe / LosslessScaling.exe）**。用户问过"qt-config 是不是
  全局"——是；游戏级 custom/<ID>.ini 会叠加覆盖，排障必须两份都看。
- **分析器单位 bug**：rot_test.py 内置 frame_wall 把帧毫秒当秒用（`/1000` 缺失），
  历史脚本输出的绝对尖刺数与新口径不可直接比（§14.2/14.4 的 53/107 等绝对值慎引用，
  fps 与相对结论仍有效）。已修 rot_test.py；离线版 analyze_rot.py / 历史重算
  reanalyze_good.py（窗口按时序重建+偏移拟合，边车被覆盖也能算）。
- **ini 固化陷阱（use_speed_limit 坑的推广）**：把键改成 `\default=true` 期间跑过
  游戏局，退出时会把"生效值"写回成显式 `key=编译默认+\default=false`——恢复基线必须
  值和 \default 两行一起改，且改完别让游戏先跑一局。
- `F:\prof\csv_timeline.py`：全天逐帧 CSV 的尾段统计速查（定位"哪个会话开始变质"）。

## 17. 第二梯队路线裁决轮（2026-09-12 深夜：合并分支基线 + 分发层 A/B 矩阵）

### 17.1 合并分支整合与新基线

- 用户 merge 入 `test/v0.2.1-profiling-fix`（IR cache 加固/跨通道 buffer 失效/fsp_srv 校验/
  **stats gating（EDEN_JIT_STATS，Windows 默认开）**/libc++ 可移植性，45 文件）。
  stash（指令计数器）pop 冲突已解决：采用上游累计快照日志结构，inst-mix 并入同一闸门
  （e60925cb89），实测日志输出正常（total=2.6M，BL 3.54%/BLR 0.81%/RET 1.62%）。
- **合并版旋转基线（merged-base2，解锁模式）**：idle 43.1-44.9fps 干净；旋转窗口尖刺
  **81→31→10→7 快速递减**（首转贵=纹理首扫热身，二次旋转变便宜=Bc3 逐出循环保持打破），
  max 89ms。此为 tier-2 后续 A/B 的对照锚点。

### 17.2 分发层 A/B 矩阵（裁决性数据）

新增环境变量闸门（local-only，与 Unsafe 档正交）：`EDEN_JIT_NOLINK / EDEN_JIT_NORSB /
EDEN_JIT_NOFASTDISPATCH`。解锁 idle 基准（med ms 口径，各 60s）：

| 配置 | med | 1%low | p99 |
|---|---|---|---|
| m-base（全开） | **22.49** | 35.74 | 25.05 |
| m-norsb（关返回栈缓冲） | **22.48** | 27.98 | 30.25 |
| m-nofastdispatch（关快速分发） | **22.47** | 6.32 | 39.04 |
| m-nolink / m-allslow | 未进游戏作废×3（自动按键间歇失败） | | |

**结论：块边界控制流层（RET 预测/分发/链接）med 成本 = 0**。RSB/FastDispatch 只保护
尾延迟（1%low/p99），对稳态帧时毫无贡献 → **FEX call-ret 影子栈路线对中位帧率的收益
天花板≈0，路线终结**（§11.6 排序据此作废）。m-nofast 的 1%low=6.3 也说明慢速分发
路径的尾延迟代价极大——影子栈/RSB 这类优化只值得作为尾延迟手段保留，现状已够。

### 17.3 剩余 JIT 侧理论与下一步

- A/B 改变的只是边界的**控制流**部分；边界真正的剩余成本疑似**寄存器越界流量**
  （dynarmic RA 是块局部的：每边界把 guest 寄存器 spill 到 JitState 再重载；实测平均
  块长仅 3.7 条 guest 指令，B.cond/CBZ/TBZ 占 9.5% 指令 = 大量碎块）。
- 可行杠杆=**穿越条件分支的块合并**：新增内联条件退出 IR 算子（ExitIf(cond, target)，
  后端 cmp+jcc 到尾桩，fallthrough 继续内联），B.cond/CBZ/TBZ 不再终结块 → 块长 3-5×，
  边界寄存器流量摊薄。估算收益个位数 %（边界流量 ≈ 0.15-0.3 核秒/秒的量级），
  **撑不起 25.1%**——且 §13 指出 60fps 需 JIT 核与 gpu_thread **同时**降到 ≤16.67ms，
  两梯队必须合并推进才可能达 60。
- 深夜自动化坑：解锁模式菜单 150fps 会伪装成好成绩（bench 的 SUSPICIOUS 行已防）；
  A-tap 实际注入 X 键（scan 0x2D），button_a 必须映射 keyboard code=88 而非 65；
  用户在场时前台锁吃按键（成功率下降，m-nolink×2 作废）；NVIDIA Overlay 每次重启复活，
  preflight 已拦截。

## 18. 第三梯队启动：合并版 GPU 线程新鲜画像（2026-09-13 凌晨）

### 18.1 采集与线程总账（totk_t3.etl，90s 稳态 idle 局内，1kHz 采样）

- 线程占用（样本/90s）：**GPU 线程 10398（≈92% 核，饱和）**、CPUCore_0/1/2 各
  ~8.3k（~80%）、**VulkanWorker 5264（≈50%，一半余量确认）**、
  **EmuControlThread 4233（≈37%，新信号，HLE 内核调度线程，待解剖）**、
  HostTiming 680（600Hz 修复生效）。
- 采集坑补遗：Bash 工具调用里 `&` 后台的进程随调用结束被回收（hold 会话死掉、
  采到空桌面）——**一律用 run_in_background**；wpr 日志的 T*_DONE 标记复用前必须
  先删（陈旧标记秒匹配假成功）；火绒（HipsDaemon/Tray ~1.4k 样本）与 Windows
  Update（TiWorker 3.1k）后台活动会进 trace，绝对值解读时注意。

### 18.2 GPU 线程函数簇排名（剔除 ETW 抓栈税内核帧）

| 簇 | 代表（样本） | 占 GPU 线程 |
|---|---|---|
| 命令流处理 | ProcessCommands 458 / CallMethod 366 / ProcessDirtyRegisters 159 / ConsumeSink 109 / Macro 系 190 | **~13.5%** |
| 缓冲同步上传 | SynchronizeBuffer 66 + WordManager 脏区 ~233 + memcmp 175 + 拷贝系 ~390 + MarkUsage/FindBuffer/UpdateVB ~367 | **~11%** |
| 堆分配+锁 churn | LFH alloc ~210 / RtlFreeHeap 35 / SRWLock ~400（堆锁联动） | ~5-6% |
| 描述符/纹理 | PushImageDescriptors 189 / PrepareImageView 77 / RefreshContents 83 | ~3.5% |
| 管线特化配置 | ConfigureImpl×3 + CurrentGraphicsPipeline 55 + key== 41 | ~3% |

结论：60fps 需 GPU 线程 -25%，**A（缓冲同步）+B（命令流）两簇理论合计 ~25% 正好够**，
但都是硬骨头；A 簇的 memcmp/memcpy 是 SynchronizeBuffer 逐 draw 比较+搬运客存内存，
方向=版本化跳过比较/staging 批量/arena 化消灭分配锁 churn；B 簇 CallMethod 是语义
热路径。VulkanWorker 50% 余量是承接面。块合并（JIT 侧个位数 %）+ 本梯队需并行推进
才有机会 60。

**§19.7 归因勘误（2026-09-13 深夜复查）**：上表"拷贝系 ~390"里 355 样本的函数名是
**MoveSmall4**（ucrt memcpy 的分派 helper 符号，不是"memcpy"——真 memcpy 符本仅 27）。
按调用栈细分 MoveSmall4 355：
- **uniform 流式拷贝 ~213（60%）**：`BindHostGraphicsUniformBuffer → DeviceMemoryManager::
  ReadBlockUnsafe`——Vulkan fast 路径逐 draw 无条件整段重拷 uniform（设计如此：游戏 CPU
  直写客存不可见，只能重读）。这是 A 簇真正的 memcpy 主战场（~2% GPU 线程）。
- TIC/TSC 描述符重读 ~60（17%）：VisitImageView 51 + GetGraphicsSamplerId 9（注意走
  Tegra::MemoryManager 而非 DeviceMemoryManager）。
- 计算管线上传 ~14、CBData 写回 ~11、宏参数 vector ~18（5%，ProcessMacro 的
  macro_params.insert——仅 0.17% GPU 线程，勿当目标）、顶点绑定结构 ~8。
- 教训：**ucrt 拷贝类热点按符号名聚合会被分派 helper 拆散（MoveSmall4/SetSmall8/
  Mov0YmmBlocks/memcpy_repmovs_amd…），必须并起来按调用栈归因**。

### 18.3 下轮队列

1. SynchronizeBuffer/memcmp 路径代码级解剖（vk buffer_cache.h SynchronizeBuffer）
   + EmuControlThread 37% 解剖（可能是可白拿的调度税）
2. ExitIf 块合并设计验证（dynarmic reg_alloc 逐退出点活跃集）
3. per-draw 小对象 arena（分配锁簇）

### 16.5 补遗（2026-09-13）

- **重启复测已确认**：用户重启后旋转性能恢复历史水平——§16.3 的"内核/WDDM 状态
  劣化"结论坐实，版本本身无回归。
- **Limit Speed Percent 被强制勾回的根因与修复（d99a45d638，local-only）**：
  `MainWindow::OnShutdownBegin()`（main_window.cpp）在每次游戏退出时无条件
  `use_speed_limit.SetValue(true)`（上游防加速模式泄漏），退出保存把它持久化，
  覆盖用户取消的勾选。修复=BootGame 开始时记录 pre_boot 值、退出时恢复；
  加速/减速模式重置保持不变。已实测：退出后 ini 保持 false。
- 坑：git 推送走 http_proxy=127.0.0.1:7777，代理进程挂掉时用
  `git -c http.proxy= -c https.proxy= push carton <branch>` 直连即可（GitHub 可达）。

## 19. 第三梯队第一刀：GPU 线程 memcmp 归因 + 管线键 memoization（2026-09-13）

### 19.1 memcmp 175 样本调用方归因（totk_t3.etl CallStack 分组）

§18.2 缓冲同步簇里"memcmp 175"是三类完全不同的调用方，按调用栈分组拆开：

| 调用方 | 样本 | 性质 |
|---|---|---|
| `GraphicsPipelineCacheKey::operator==` | 75 | 逐 draw 的管线键整键 memcmp（Next() 命中路径 + map 查找），键含整份 FixedPipelineState |
| `TextureCache::VisitImageView` 系 | ~56 | 纹理视图遍历中的比较 |
| `DescriptorTable<TSCEntry>::Read` | 38 | 逐 draw 重读 32B 客存 TSC 描述符 + 与缓存副本比较 |

→ operator== 是纯"没变也要比"的浪费（游戏稳态下寄存器值逐 draw 大多不变），
先打它。

### 19.2 EmuControlThread 37% 破案：拆除税，非稳态

按函数分组解剖：`RtlpInsertFreeBlock` 2478 + `RtlFreeHeap` 472 + IR cache
区间树 erase 88（逐区间销毁 unordered_dense 表）+ FFmpeg::Frame frees 114 +
dynarmic PatchInformation 析构 26 + 内核 decommit 650。Driver = TOTK 常态
内存重映射（range_invalidations=15007/会话）→ 释放路径在模拟控制线程上串行执行。
**它是拆除/重映射税，不在逐帧关键路径上**（稳态 GPU 线程/JIT 核不受它影响），
优化价值=降背景 CPU 占用与内存行为，不直接挣帧率。排在队列后面。

### 19.3 参考项目排查结论（勿重复调研）

- **azahar 是 3DS 模拟器**（Lime3DS 血统，包名 io.github.lime3ds.android），
  不是 Switch GPU 工作的参考对象——此前"借鉴 azahar"的方向作废。
- citron-emu/citron GitHub 404（仓库没了）。
- **eden master（本地克隆）是唯一正确参考**。其 v0.2.1 后演化：PSO Optimizations
  #4294（仅会话热身期收益）、boost::unordered_flat #4326、Bindless Descriptors
  #4251、Multithreading refactor #4254——**均未解决逐 draw memcmp 问题**。

### 19.4 管线键 memoization（8cb6300f26，local-only，本轮主角）

**设计**：Maxwell3D 暴露 `change_generation` 计数器，`ProcessDirtyRegisters`
里只有写值真正变化才自增（第一轮的相同写过滤顺带提供）；PipelineCache 用
`key_build_gen` 记住 current_pipeline（或上次失败构建）对应的代数——
**逐 draw 代数不变 ⇒ 键不变 ⇒ 直接返回现管线**，跳过 RefreshStages +
FixedPipelineState::Refresh + Next()/map 的整键比较。

**安全性论证**（已逐点核实）：`regs.reg_array` 只在 ProcessDirtyRegisters 一处
写入（ConsumeSinkImpl 也路由到它、宏走 CallMethod）；`graphics_key` 只在
vk_pipeline_cache.cpp 读取；运行期图形管线从不失效。FixedPipelineState::Refresh
读的 regs + draw_state.topology（寄存器派生）+ 静态 DynamicFeatures 全部被代数覆盖。

**验证**（TOTK 合并基线 8c14d24403 vs 打补丁，旋转测试同口径）：

| 指标 | 基线 | memoization | 变化 |
|---|---|---|---|
| 旋转首扫尖刺（>33ms 计数） | 81 | **17** | **-79%** |
| 旋转窗口尖刺合计 | 129 | **39** | **-70%** |
| 旋转段 fps | 41.45 | **44.01** | +6.2% |
| 稳态 med 帧时（两轮） | 22.49ms | 22.48ms | 中性（噪声内） |
| 1%low | 35.74 | 34.15/33.95 | 轻降（噪声边缘） |

渲染正确性：进游戏截图验收（峡谷营地场景、帐篷/林克/HUD/心心齐全，无黑块花屏，
45 FPS 正常）。

**定性**：这是§18 命令流簇里"每 draw 管线查找税"的直接消灭——旋转起手 draw
洪峰不再重复付整键比较；稳态 med 不动符合预期（稳态下 Next() 本来就靠第一轮的
transition 哈希预比较快速命中，此补丁主要收割"哈希算完还得整键 memcmp"的尾巴
+ 洪峰场景）。1%low 轻降待下轮复测（两轮 34.15/33.95 接近基线 35.74 的方差带）。

### 19.5 下轮队列（更新，按 §19.7 勘误后的归因）

1. **uniform 流式拷贝**（A 簇 memcpy 主战场 ~2% GPU 线程）：先测"内容逐 draw 是否
   真变"（EDEN_UNIFORM_STATS=1 已埋计数器，拷贝后 memcmp 影子副本，退出时打印
   identical 比例）→ 高比例才做 stream 区域复用去重；低比例则死心。
2. TIC/TSC 描述符重读（~0.6% + memcmp 175 里的大头）：同上需先测变更率；寄存器代数
   **不覆盖客存内容变化**，§19.4 的 memoize 思路不能平移（此处勘误，原队列第 1 项作废）。
3. SynchronizeBuffer/WordManager 脏区簇（~2.2%）代码级解剖。
4. ExitIf 块合并设计验证（JIT 侧）。
5. per-draw 小对象 arena（分配锁簇 ~5-6%）。
6. IR cache 区间树拆除税（EmuControlThread，背景收益）。
7. 宏参数 vector（0.17%）——**已证伪不值得做**，勿再排队。

### 19.6 本轮坑

- NVIDIA Overlay 杀不死（nvcontainer.exe 系统服务父进程，杀了就复活）。
  `check_config.py --tolerate-overlay`（2026-09-13 加）把它降级为 WARN，其余被禁
  进程（LosslessScaling 等用户可关的）仍硬拦截；`visual_run.py` 已接入该模式做
  预检（此前完全跳过预检，导致用户忘关的叠加层混进测量局——已修复）。
  **计时类采集（bench/wpr）前仍须从 NVIDIA App 真关或停服务**（overlay 有 hook 税）。
- azahar/citron 参考方向作废（§19.3），勿再花时间。
- `MSYS2_ARG_CONV_EXCL="*"` 与 `taskkill //IM` 组合会翻车（`//IM` 不再被转换成
  `/IM`，taskkill 报无效参数）——二选一：要么不设 EXCL 用 `//IM`，要么设 EXCL
  用单斜杠 `/IM`。
- subprocess 抓 powershell 输出必须 `encoding="utf-8", errors="replace"`——
  powershell 偶发输出 GBK 中文（0xd5），text=True 默认 utf-8 会让 reader 线程
  崩掉、r.stdout 变 None（bench_run.py 的 tap_a 同样隐患，2026-09-13 已在
  visual_run.py 修复）。
- **自动按键"失灵"的头号嫌疑是配置而不是环境**（2026-09-13 实录，勘误掉早先
  "环境瞬时问题"的猜测）：还原手柄配置（bak-controller → qt-config.ini）会静默
  杀死 X 键自动化（keyboard_enabled=false + button_a 回 SDL 手柄），而
  focus_ok=True 只证明窗口抢到了焦点、**不证明游戏收到了按键**——harness 对
  这种失效完全无感，游戏卡标题屏还能"跑完全程"。排障顺序：先 grep
  player_0_button_a 是否还是 `engine:keyboard,code:88`，再怀疑前台锁/电脑。
  跑完测量要还原手柄配置时，记住下次测量前得重打 patch_input.py。

### 19.8 uniform 流式拷贝测量（EDEN_UNIFORM_STATS=1，2026-09-13 深夜）

埋点方式（local-only）：Vulkan fast/stream 路径在 ReadBlockUnsafe 拷贝后 memcmp
影子副本（按 stage×index 90 槽，addr+size 匹配才比较），退出时 ~BufferCache 打
一行汇总。开销仅测量局存在（默认关）。

三份独立样本（F:\prof\uniform_stats_results.txt 存档）：

| 会话 | copies | identical | 比例 | 流量 |
|---|---|---|---|---|
| 手动引导（296s，含菜单/局内） | 6,370,351 | 2,766,085 | **434‰** | 3435 MiB |
| 自动局 1（199s） | 3,838,911 | 1,665,160 | **433‰** | 2063 MiB |
| 自动局 2（199s，预检+X 键自动化全程干净） | 4,038,941 | 2,123,993 | **525‰** | 1430 MiB |

- **43-53% 的 uniform 流式拷贝内容与上一次完全相同**（近半是白拷，拷贝速率
  ~20k 次/秒，平均单拷 370-570B）。
- 语义：Vulkan fast 路径因"游戏 CPU 直写客存不可见"而无条件重拷——重拷内容近半
  没变，去重空间实测存在。
- 口径注意：比例是全会话累计（含菜单/标题静态 uniform），纯局内比例未拆分。
- 若实现"相同即复用旧 stream 区"（memcmp 换 memcpy + 记住上次区域偏移 + 旧区域
  有效性判定），按 ~50% 相同率可省 uniform 拷贝簇约一半写流量与对应 staging 消耗，
  估 **~0.5-1% GPU 线程**——A 簇切片之一，够不上 60fps 主杠杆；实现复杂点在
  stream 环形复用后旧偏移的有效性判定。测量基建已提交（94fc50545a，env 默认关），
  下轮可决策。

### 19.9 GPU 线程攻坚阶段总账（09-07～09-13，跨 §6/§14/§18/§19）

**确定有收益（按含金量排序）**：

| 项 | 收益 | 状态 |
|---|---|---|
| ASTC 流送配置修复（CpuAsync+Bc3+异步 shader，§14） | 旋转尖刺 -50%、最大卡顿 303→58ms、二次旋转逐出循环打破 | 配置级，已在用户日常配置 |
| 管线键 memoization（§19.4，8cb6300f26） | 旋转首扫尖刺 81→17（-79%）、合计 129→39、旋转 fps 41.45→44.01、med 中性、渲染验收过 | 已提交 |
| 第一轮五项微优化（§6，7f1f534cd0） | GPU 线程 CPU -3.3%；相同写过滤是 memoization 的地基 | 已提交 |
| 解锁自旋 600Hz（§15，c8b0c853f8） | 旋转尖刺再 -43% | 已提交 |

**明确负结果（勿重查）**：影子栈/分发层 A/B（med 免疫）；TSC/TIC 用寄存器代数
memoize（代数不覆盖客存内容）；宏参数 vector（实测 0.17%）；azahar（3DS）/citron
（404）参考；LTO/PGO（中性）；四线程流水线下单线程 -3.3% 不动 FPS（帧率整栅格
跳变，45→60 需 GPU 线程与 JIT 核同时 ≤16.67ms）。

**当前 GPU 线程剩余地图**（§18.2+§19.7 修正后，占 92% 饱和线程）：命令流 ~13.5%
（语义热路径，最难）、缓冲同步 ~11%（uniform 拷贝 ~2% 已测、WordManager ~2.2%、
簿记 ~3.5% 未解剖）、堆分配锁 ~5-6%、描述符纹理 ~3.5%、管线配置 ~3%（memoization
已吃掉大块）。**VulkanWorker 50% 空闲是唯一结构性承接面**。

**建议方向（按杠杆排序）**：
1. 逐 draw 工作并行化到 VulkanWorker（描述符推送/uniform 流送）——唯一可能一次
   拿两位数百分比的路线，参考 eden master #4254 Multithreading refactor，单开一轮。
2. uniform stream 去重（数据在手：~50% 冗余，估 0.5-1%）。
3. WordManager 脏区 + 缓冲簿记簇代码级解剖（合计 ~5.7% 未开垦）。
4. per-draw 小对象 arena（~5-6% 分配锁簇）。
5. 命令流簇（13.5%）最后碰。

体验维度结论：**尖刺/卡顿类问题已基本解决**（旋转场景稳定 ~44fps、无大卡顿）；
稳态 45→60 仍是长仗，剩余单项全在 0.5-2% 量级，无捷径。

## 20. ASTC 异步解码窗口花屏：诊断与修复（2026-09-13 深夜）

### 20.1 症状与定性
用户日常游玩（**注意：F:\Switch\Yuzu\eden.exe 是 09-12 拷贝的我们的构建
06c7a2a6b2，非官方版**）在加载画面出现"贴图错误"——截图形态为**瓦片内容有效
（加载壁画）但位置错乱/重复**，非噪点非黑块非涂抹。截图时间为 09-12 22:55，
恰在 §14 ASTC 配置修复（accelerate_astc=2）应用当天。

### 20.2 根因（代码级实锤）
`AstcDecodeMode::CpuAsynchronous` 的解码窗口：`Image` 构造只分配 VkImage 不初始化；
`RefreshContents→QueueAsyncDecode` 设 `IsDecoding` 后入队即返回，解码数据要等后续
某帧 `TickAsyncDecode` 才上传；而 `IsDecoding` 全库仅两处消费（逐出保护 +
unswizzle 重入），**采样路径零检查** → 窗口内的 draw 采样**从未写入的显存**
（复用堆里的陈旧纹理数据 = "内容有效但排布错乱"）。加载画面 = 大 ASTC 集中流送
= 解码窗口最长 = 必中。该路径是 eden v0.2.1 原生半成品（代码自带
`UNIMPLEMENTED_IF` 和 `// TODO: Do we need this lock?`），我们 09-12 改配置才激活。
用户日志 2637 条 `Queuing async texture decode` 佐证路径活跃。
零成本验证法：grep 日志该字符串即可确认路径在跑。

### 20.3 修复（514a095406，local-only）
入队即零初始化：`QueueAsyncDecode` 设完标志立即经正常 staging 管线上传全零
（新 helper `ZeroUploadCopies` 镜像 `ConvertImage` 的三分支缓冲几何：Uncompressed/
Bc1/Bc3），窗口期采样读到黑色而非垃圾。模板层实现 = GL/VK 双后端同享
（GL 纹理规范上零初始化，代价为零无害）。同构隐患 `QueueAsyncUnswizzle`
（BCn 3D 大纹理，跨帧延迟上传）**未动**——TOTK 未触发，留观。
验收：两次过加载画面连拍干净（`F:\prof\fixverify_run.py` + `astcfix_shots/`）、
解锁基准 med 22.48ms 与基线逐位一致（44.85fps，零回归）、解码路径仍活跃。
**用户侧生效需把新 eden.exe 拷到 F:\Switch\Yuzu**（含 09-13 memoization，
若画面有异样先回报——此前已单独验收过）。

### 20.4 坑与方法论
- 排查组合矩阵（GPU/异步 × BC3/不压缩）已不需要——根因单点实锤；但备用：
  若修后仍残留**对齐类**错位（mip 链尾非 4 倍尺寸），下一嫌疑是
  `ConvertImage` 里 `buffer_row_length=mip_size.width` 未按 BC 块宽对齐（#2 悬案）。
- PowerShell `-Command` 模式 `$args` 不填充，路径必须内插进脚本串（截图静默
  失败教训）；Read 工具在本环境只上传图片不渲染，看图走 analyze_image。
- NVIDIA Overlay taskkill 后 ≥60s 才复活且本 shell 无服务停止权限——杀完
  立即起跑可与历史基准同条件。
- **用户日常 exe = 我们的构建拷贝**这个事实要记住：凡我们改渲染相关代码，
  用户日常游玩即成真实世界回归测试场；出视觉问题先查我们的 worktree 变更。

## 21. v0.2.1 优化移植 master 轮（2026-09-14 凌晨）

### 21.1 任务与盘点
把 v0.2.1 worktree 的本地优化移植到主仓库 test/master-profiling（master + 本地
profiling 文档），剔除 master 原生已有项。v0.2.1 分支本地提交 42 个，处置：
- **剔除（master 原生已有）**：fsp_srv Temporary 存档修复 ×2（master 2026-07-14
  起原生有同构映射，非我方回流；上游独立修复）；HWC 探针 ×2（vi/conductor 被
  #4254/#4238 整体重写，目标代码不存在）；docs/chore ×4。
- **剔除（无意义）**：caps padding/`std::exchange` 丢弃等纯 libc++ 移植修饰 ×2
  （冲突大、MSVC 路线零价值；caps 那个整文件取 theirs 会回退 master 演化，教训）。
- **适配移植**：解锁自旋税 c8b0c853f8 → master 新 conductor 的
  `GetNextTicks` 里 `speed_scale=0.01f→0.1f` 一行（新代码仍是 6kHz 事件率）。
- **按时序 cherry-pick 29 个**（dynarmic IR cache/RegAlloc/计数器系列 + GPU 线程
  微优化 + memoization + uniform stats + ASTC 零初始化 + 速度限制恢复 + 构建修复）。
- **适配提交**：ir_cache.cpp 终端序列化重写（master #4218 把递归 boost::variant
  终端树扁平化为 std::variant<monostate|LeafTerminal|If|CheckBit|CheckHalt>，
  叶子单列；线格式进程内自洽即可）；block dump 的 `.which()→.index()`。

### 21.2 构建突破：crtvec shim（否决 WSL 路线）
- WSL 交叉构建产物（wsl-windows-build.md 所述 .local-review 工具链/Qt/OpenSSL）
  **已不存在**（F: 与 WSL home 均无），重造要数小时 → 用户拍板走 VS2022 原生。
- 已知问题 #2（LNK2019 `__std_rotate/__std_replace_copy_1` 等 6 符号）的正解：
  这些是 MSVC STL 向量化算法 helper（microsoft/STL `stl/src/vector_algorithms.cpp`
  开源实现），Qt 6.11.1 静态库引用而 VS2022 14.44 CRT 无。**写标量实现编成
  obj 挂链接参数即可**（签名照抄 STL 源码；__stdcall x64 下空操作；只有 Qt GUI
  路径调用，性能无关）。已入库 `tools/windows/crtvec_shim.cpp`，用法在提交信息里：
  cl /c /O2 /MD /std:c++20 编出 obj，加进 CMAKE_EXE_LINKER_FLAGS。
  无需新运行库 DLL、无需管理员、无需 app-local 部署。系统 msvcp140.dll（2025-05）
  无这些导出，redist 提取（Burn 容器 7z/layout 均失败）全部作废。
- CMakeLists 的 YUZU_USE_BUNDLED_QT 下的 AddQt(Eden-CI/Qt 6.11.1) 缓存包为
  **纯静态**（bin 只有工具无 DLL），无共享变体可换。

### 21.3 验收结果（build-vs22，RelWithDebInfo）
- **编译全过**（适配后无警告级问题）；eden.exe 52.9MB。
- **冒烟（fixverify 流程）**：进游戏正常、卡卡利科村渲染干净无花屏、加载画面
  正常——**旧已知问题 #1（master 卡 launching，sm_controller）在 VS2022+shim
  构建不复现**，该问题应记为 VS2026/MSVC 14.51 构建特有。
- **基准**：warm fps=44.89 / med=22.50ms / p99=26.68 / 1%low=31.89，与 v0.2.1
  （44.85/22.48/25.15/33.08）持平；首轮曾 49.06fps（med 同为 22.49）——master
  的呈现路径（#4238/#4254 重构后）有时不吃 120Hz 栅格量化，帧次波动属节奏非负载。
- **旋转**：idle 44.96fps 零尖刺；旋转段 41.7-43.4fps、>40ms 每窗 2-15 个、
  max 64ms、零 >66ms——与 v0.2.1 修后状态（max 58ms）同档，可接受。
- **遗留**：①优雅关闭偶发超时（3 局里 1 次强杀，CSV 丢失；关停路径待查，不阻塞）；
  ②旋转 >40ms 计数略多于 v0.2.1 调优态（shader 缓存刚重建 / master 流送差异，
  未定性）；③bench_run/rot_test 的 commit 标签仍读 v0.2.1 仓库（REPO 硬编码，
  纯标签问题）。build-vs22/bin/user 已从 v0.2.1 拷贝并改好路径（3.3GB）。
- 分支：port 工作 ff 进 test/master-profiling（06063fc9db..bb59b29f1d）已推送。

### 21.4 坑
- 后台构建命令自带 `| tail` 会吃掉退出码和错误行（§11.8 重演）——日志必须落盘
  全量再 grep；`grep -ac FAILED` 需二进制安全模式。
- cherry-pick "干净落地"≠语义正确，构建前做了地标审计（change_generation/
  寄存器过滤/IR cache）；caps 冲突整文件取 theirs 回退了 master 演化
  （ResolveCallerProgramId/Capture::ScreenShotAttribute 丢失），发现后 reset
  丢弃该提交——**冲突解一律逐 hunk，禁整文件取边**。

## 22. 解锁模式菜单节奏修复 + 工作重心转移 master（2026-09-14 夜）

### 22.1 问题与方案
用户反馈：解锁（不勾 Limit Speed Percent）后 TOTK 暂停菜单本应 30fps（省 CPU、
输入节奏正常）却跑到 100+fps，LT/RT 导航过敏易误操作；勾 100% 又失去解锁收益。
机制：菜单以 swap interval=2 请求节奏，但解锁分支一刀切 0.1 → 合成事件 600Hz →
hardware_composer 的"每 interval 次合成取一帧"门控在 600Hz 基准上失效。
**两全方案（568605ebf7）**：yuzu 扩展里 interval≤0 语义是"速度倍率"
（NormalizeSwapInterval 置 compose_speed_scale>1），即动态 FPS 游戏内自报身份；
解锁分支只对 `compose_speed_scale > 1` 放开 0.1，显式节奏请求（interval 1..4）
回归硬件精确 60Hz 合成基准；最终 tick 再 clamp ≥600Hz（防倍率除法把事件率推回
多 kHz 自旋）。**已知代价（本地策略）**：从不使用 interval 0 的固定 30fps 游戏
解锁后也不会超过 30fps。
验收：文件选择界面实测锁定 60fps（med 16.67ms）；游戏内解锁不变
（44.92fps / med 21.67ms，较修复前 22.50 略好）；渲染干净（fixverify 连拍）。
注：菜单变 paced 后自动化过标题的按键时序偶发不进游戏（一局 bench SUSPICIOUS
menu=60fps 即此），重跑即过——测试脚本未来可加"确认进游戏"探针。

### 22.2 工作流变更（用户拍板）
- **此后只在 test/master-profiling（主仓库）上工作；v0.2.1 worktree 冻结**，
  停在 514a095406（ASTC 零初始化修复），不再修改。
- 主仓库 build/（废弃 VS2026 产物，5.3GB）已删除；build-clang（3.7GB）按用户
  要求保留待后续 clang 尝试；现役 = build-vs22。
- 用户日常 exe（F:\Switch\Yuzu，v0.2.1 血统）如需本修复，需换拷新 master 构建。

## 23. 水塘场景攻坚：描述符直读 + 呈现量化减半（2026-09-14 深夜）

### 23.1 场景与判定
用户把默认存档改为卡卡利科村面对水塘（水面占屏 50-60%、完整倒影），静态 idle
即 42.06fps/med 23.33（普通场景 44.9/21.67），怀疑水面倒影图形负载。
**判定：非 GPU-bound**——设备利用率 37.7%（与旧场景持平），瓶颈是**GPU 命令线程
单点饱和（98.6%）**，三个 JIT 核仅 ~70%（旧场景是四线程全饱和）。倒影 pass 使
每帧纹理/采样器绑定量翻倍，把 GPU 线程的每绑定/每 draw 路径整体放大。
（totk_pond.etl vs totk_t3.etl 同口径对比，注意两 trace 采样率差 10 倍，只比占比。）

### 23.2 差分热点（占 GPU 线程 eden 侧比例）
视图解析（VisitImageView+PrepareImageView）1.5→3.1%；采样器解析 0.6→2.0%；
宏分发 0.5→1.6%；管线键 operator== 0.6→0.7%（memoization 扛住了绑定量翻倍）；
**客存翻译仪式簇（GpuToCpuAddress+ReadBlockUnsafe/WalkBlock+MemoryOperation）
~7%**。采样器/视图的槽位 memo 本来就有（挂 DescriptorTable 内容比对，安全），
真正的税是每绑定一次的 32B 客存读取走了 WalkBlock 全套仪式。

### 23.3 两项优化（18aaa38cef）
1. **DescriptorTable::Read 直读**：双 GetPointer 翻译 + 首尾连续性校验通过则
   直接 memcpy，跨页/未映射回退 ReadBlockUnsafe。A/B（同 overlay 条件）：
   fps 均值 42.3→43.5。
2. **解锁事件下限 600→1200Hz**：发现 600Hz 下限把呈现量化到 1.667ms 栅格——
   工作负载贴边界时 med 在 21.67/23.33（13/14 tick）双峰翻转，贴边浪费可达
   1.5ms。1200Hz（0.833ms 粒度）后 med 稳定 22.50、fps 44.0±0.03（n=3）；
   唤醒成本仍比 6-12kHz 自旋区低一个量级。
   **阶梯：基线 42.2 → 直读 43.5 → +1200Hz 44.0**；渲染验收（倒影完整）通过。
   普通场景 med 21.67=26 tick 不变（量化只在贴边界时伤人）。

### 23.4 剩余差距与方向
水塘 44.0 vs 普通场景 44.9：残余 = 每帧 bind/draw 量 inherent ×2 的每 draw 税
（ConfigureImpl 3.2%、PushImageDescriptors 2.6%、宏 2.6%…全是长尾）+ 驱动侧
33%。**结构性出路仍是逐 draw 工作并行化到 VulkanWorker（48% 空闲）**——
水塘场景 GPU 线程单点饱和的形态让这个方向的价值比以往更明确。
本轮方法论收获：①事件下限本身是量化栅格，调优时要看 med 是否落在 tick 整数倍
（21.67/23.33/22.50 全是 tick 倍数，一眼假）；②场景负载时变（水流动画相位），
单跑结论必翻车，A/B 各 n≥2；③wpr 两轮采样率不一致（10398 vs 118333/类似时长），
跨 trace 只能比占比。

## 24. 随机卡顿攻坚：哨兵自动抓 trace 基建（2026-09-16）

### 24.1 问题与方案选型
用户报告：稳定 45FPS + 小黄鸭插帧后日常基本 OK，但优化版经常出现**突然掉帧，
持续 <1s 但明显可感知**——随机、亚秒级，固定窗口采集（以前旋转/首扫的打法）
抓不到。对应 Android systrace+simpleperf 的 Windows 等价物其实更强：ETW 单条
trace 同时含调度时间轴+采样栈+GPU 行，天然时间对齐。基建三层：
1. **PresentMon v2.5.1 CLI**（`G:\Tools\PresentMon\PresentMon-2.5.1-x64.exe`，
   GitHub GameTechDev 独立 exe 即够用，MSI 不必要）：逐帧 CSV 哨兵，QPC 时间戳，
   列含 MsBetweenPresents/MsBetweenDisplayChange/MsInPresentAPI/MsGPULatency/
   MsCPUBusy/MsCPUWait——app/GPU/显示三级"迟到"第一道分类不开 ETL 就能做。
2. **wpr 内存环形缓冲**（无 -filemode）：常驻零磁盘写，检测到卡顿立即 -stop
   冻结最近 ~50s 现场。CPU-only profile（CPU+GPU 事件量 200MB/s 环形只盖 5s，
   CPU-only 24MB/s、默认池 ~1GiB ≈ 42-50s 覆盖）。
3. **`F:\prof\stutter_watch.py`**（提权哨兵，入口 `stutter_watch_start.bat`
   自提权）：检测规则 = 帧间隔 ≥ severe_ms(默认50) 且 ≥ ratio(1.6)×滚动中位
   (120帧) 且过冷却期(5s) → 后台线程 wpr -stop 落盘 `stutter_<时间>.etl` +
   侧车 JSON（detected_epoch / wpr_start_epoch / frame_ms / med / 最近30帧 /
   行内 QPC+disp+api）→ 自动重启环形继续盯（--max-captures 默认 4 份配额，
   每份 ~0.5-1GB）。抓取 flush 期间（F 盘 ~25MB/s，1GB 约 30-60s）暂停触发、
   继续记录帧——连发卡顿只抓第一发。退出（Ctrl+C/--duration）时环形还在则
   补落 manual etl 兜底漏检。

### 24.2 本机三坑（复现/换机必读，已固化在脚本头部注释）
1. **C: 100% 满（剩 1.6G）→ wpr 内存模式 start/stop 都在 C: 临时区 staging
   挂死或报 0x80070070**（表象：start 无限阻塞、stop 挂 1 分钟后报磁盘满、
   连锁出现 0xc5580601 duplicate instance）。修复 = 所有 wpr 子进程 env 里
   TMP/TEMP 重定向到 F:。与 filemode 时代 -recordtempto F: 同族问题，
   DiagTrack 重启不是解药（试过，没用）。诊断现场 exp1-7 已清理。
2. **PresentMon 独占持有 --output_file 句柄**，外部 tail 该文件会
   PermissionError（共享冲突被 Python 映射为 EACCES）。修复 = --output_stdout
   管道 + 读线程，CSV 由哨兵转存 pm_live.csv（顺带解决文件轮转检测）。
3. PresentMon 自带 ETW 会话与 wpr 会话共存无冲突（TMP 修复后实测）。

### 24.3 端到端验证（2026-09-16 实测通过）
哨兵提权运行 + eden 冷启动 TOTK：开机期 **201ms / 81ms 两次卡顿自动捕获**，
ETL 1074MB / 414MB 落盘 rc=0，环形重启后 15s 再武装，配额、到时收尾、
Ctrl+C 收尾全路径正常。侧车 JSON 锚点齐全；201ms 那帧 disp=200ms——
"app 迟到且显示迟到"的分类信息在侧车里就有。环形覆盖实测 50.4s。

### 24.4 分析流程（拿到首批样本后）
ETW MCP `process_trace`（先 CPU Scheduling Data + Sampled CPU Usage）→
侧车锚点切卡顿窗 → **同 trace 等长正常窗做差分**（天然控制变量）→ 三分支归因：
关键线程 running 且热点变（代码热点）/ ready 未跑（被谁抢核，按进程聚合）/
waiting（do_critical_path_analysis_by_thread_and_time 拉阻塞链）。
已知模式 checklist：管线/shader 编译、纹理流送 LRU 重上传、JIT 编译风暴
（IR cache miss）、EmuControlThread 拆除/重映射税、分配锁、后台进程抢核；
**新嫌疑：栅格节拍滑档**（45↔48↔40 混合，帧时长 16.7/25/33.3 跳变本身就是
可感知卡顿，代码热点查不到，用 PM display 序列看节拍模式）。第二轮再考虑
eden 埋 TraceLogging 帧事件（atrace 等价物，~20 行 local-only）。
若 50s 基线窗不够差分：自定义 .wprp 调大内存池。

### 24.5 首批真实样本分析（2026-09-16 深夜，用户实玩 3 次抓取）

**样本**：用户日常配置（F:\Switch\Yuzu，exe 与 build-vs22 哈希一致、pdb 配对无忧）
实玩 TOTK，3 次 ~52ms 卡顿自动捕获（52.4/52.6/53.5ms，会话 med 25-27ms≈37fps）。

**归因过程与结论（卡顿 #1，全法医式排查）**：
1. PM 完整行：卡顿帧 **MsGPUBusy 平坦（12ms≈基线）**、MsGPUWait 翻倍、呈现间隔=CPU 节奏
   → 显卡不是原因，CPU 侧管线拖的。
2. 三分支判定：无线程被抢（ready 全为小量）、无线程长等待（GPU 线程 max wait 0.2ms
   **从未断粮**、三核 max wait ~9ms）、无进程抢核（全系统 34% 空闲，dwm/csrss 正常）。
3. 函数级差分：卡顿窗 vs 基线无单点热点（RefreshContents 纹理簇 8.5→9.2 样本/s 持平，
   属慢性背景流送；另 ~9% 是 ETW 抓栈自身噪声，老 caveat）。
4. GPU 线程单次最长运行 18.5ms、全窗 93-96% 连续饱和 → **卡顿帧 = 常规每 draw 命令流
   的量堆积（帧内多 ~20ms 常规工作，散布在正常函数里）**，与 §15 旋转残余尖刺同类。
   1kHz 采样对"散布的量堆积"是分辨率下限——这正是它的签名（什么都查不到=量多了）。

**会话级异常发现（用户日常环境 vs 基准环境差异，待跟进）**：
- 用户 GPU 线程在 ~30fps 就饱和（基准配置 45fps 才饱和）→ 日常实玩（动态场景+移动）
  每帧命令量更大，GPU 线程吞吐就是日常帧率上限；**VulkanWorker 并行化（48% 空闲）
  从"结构优化"升格为"用户可感知的日常瓶颈"**。
- **HostTiming 满核自旋（98%）**：基准解锁模式只有 ~36%（§15 修复后）。差异候选：
  cpu_accuracy=2（Accurate）下事件粒度更细 / 窗口化 vsync 路径 / 场景本身。待专项查。
- **窗口化游玩（fullscreen=false）→ "Composed: Copy with GPU GDI" 呈现**：显示更新
  量化到 16.7/33.3/50ms（DWM 拾取节奏），加呈现路径开销。
- 用户日常配置 cpu_accuracy=2 —— **勘误（2026-09-17）：master 枚举为 Auto=0/Accurate=1/
  Unsafe=2，=2 即 Unsafe，用户本来就在最优档**；此前误记为 Accurate 并建议改 0 是错的
  （0=Auto=编译默认，改后被规范回 default=true 属正常行为）。基准配置同为 =2，两边同档，
  本节其余结论不受影响。窗口化→全屏已由用户实测有收益，后续基准一律全屏。
- 干扰注记：分析 agent（ZCode.exe ~1.7 核）在用户游玩期间也在跑——未来采集时
  agent 应静默（本次非成因，34% 空闲）。

**工具链补课**：环形 ETL 的调度数据在"冻结时刻"截止（=检测时刻），其后是 flush 噪声
（采样数据会长 ~2×，弃用尾部）；卡顿在 etl 内的位置 = 尾部前 ~1s（PM 延迟）。
侧车 ring_session_s 与调度截止时刻互相印证。无时间窗的 Contains 全表扫描查询会
30s 超时——**务必带时间窗**。

### 24.6 HostTiming"满核"专项：结案（2026-09-16 深夜）

**现象**：用户日常会话 HostTiming run% 98%，基准配置历史值 ~36%。

**归因链（全部实证）**：
1. 采样热点 = `Common::Event::WaitFor`（扣 ETW 抓栈噪声后占实际大头），线程 98%"运行中"。
2. 读码：master 的 `Event::WaitFor`（src/common/thread.cpp:647，上游重写）三分支：
   AMD MWAITX（monitorx）/ Intel WAITPKG（umwait）/ 后备 `while(...) NtDelayExecution(-1)`
   ——最后一个等于睡 100ns = 纯忙等。
3. 本机 = Ryzen 5600（Zen 3）→ CPUID 8000_0001h EDX[29]=1 → **走 MWAITX 分支**：
   `_mm_mwaitx(定时器提示, C1)` 硬件等待。调度器视角永不切换=98% run%，实际 C1 停机。
4. 历史账：v0.2.1 同名函数是 `condition_variable::wait_for`（真睡→36%）；master 换
   MWAITX 后变"假忙"。§23 上 1200Hz 时只测了端到端、没看线程级——当时的 caveat 落地。

**定性（本机）**：**非吞吐小偷**——MWAITX C1 释放执行资源给 SMT 兄弟线程，卡顿窗仍有
27% 空闲、无抢占。代价：(a) 该核进不了深 C-state（轻微功耗/boost 影响）；
(b) 一切基于 run% 的 profile 会把它读满（今天第一遍差分就被它误导过）。
**移植地雷**：老 Intel（无 WAITPKG，约 Alder Lake 之前）走后备分支=真·100% 硬自旋。

**修复裁决**：暂不改。hybrid wait（睡到 deadline-100µs 再 MWAITX 收尾）可降 run% 到
~15% 并救老 Intel，但动的是全程序共用的等待原语、有伤 vsync pacing 精度的风险，
优先级低于 VulkanWorker 并行化。若将来改：先量 pacing 精度基线再动。
分析注意：今后所有 trace 里 HostTiming 的 run% 直接当"~100% 常态"扣除，
不作为异常信号（除非换 Intel 无 WAITPKG 机器）。
