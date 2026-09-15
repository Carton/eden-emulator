# Eden 本地开发笔记（Windows，仅本地使用）

> **过程记录唯一入口 = `PROFILE_PROGRESS.md`**（本仓库根目录，随 git 跟踪）：
> 基线/构建/输入自动化/profiling 管线/历轮实验数据/结论/坑，全在那里，新会话从它读起。
> 本文件只维护**通用规则、流程与当前状态**，不记录优化过程。

## 项目简介

Eden 是开源（GPLv3）Nintendo Switch 模拟器，yuzu 血统的社区延续项目。
C++20 + CMake（≥3.31）构建，支持 Windows / Linux / macOS / Android 等平台。
本仓库：`https://git.eden-emu.dev/eden-emu/eden`（GitHub 镜像：`https://github.com/eden-emulator/mirror`）。
构建文档见 `docs/Build.md`、依赖说明见 `docs/Deps.md`。

## 当前工作基线

- **工作重心 = master：`test/master-profiling` 分支**（本仓库），一切修改只落这里，构建走 `build-vs22/`。
- **v0.2.1 worktree（F:\devel\opensource\eden-v0.2.1）已冻结**（停在 514a095406，勿改），仅作历史参照。
- **用户日常游玩 exe = F:\Switch\Yuzu\eden.exe（我们 master 构建的手动拷贝）**——凡改渲染/输入/节奏相关代码，
  用户日常游玩就是真实世界回归测试场，出视觉/手感问题先查我们的变更。
- build-vs22 已含全部 v0.2.1 移植优化 + master 侧后续优化（清单见 PROFILE_PROGRESS 各章），
  build-vs22/bin/user 数据已配好（含水塘测试存档）。

## 本机构建环境（2026-09-06 验证通过）

- Windows 11 x64，12 逻辑核，NVIDIA RTX 2060（驱动 591.86）
- Visual Studio Community **2022** @ `D:\Program Files\Microsoft Visual Studio\2022\Community`
  （MSVC 14.44，cl 19.44.35228；VS2026 已卸载）
  - CMake 3.31.6 / Ninja 为 VS 自带，**不在系统 PATH**，路径：
    - `D:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`
    - `D:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja`
- Git for Windows（Git Bash）+ Python 3.12（CPM 下载依赖需要）
- **glslang 16.5.0 独立版** @ `G:\Tools\glslang`（构建期编译 shader 用，Vulkan SDK 唯一被用到的组件）
- 其余依赖（Qt 6.11.1 静态版、SDL3、FFmpeg、OpenSSL、Boost 等）由 CPM 在配置期自动下载到
  `.cache/cpm`，无需手动安装；下载走 http_proxy 环境变量
- 备选工具（G 盘）：`G:\Tools\cmake-portable`（CMake 4.1.1）、`G:\Tools\ninja`、`G:\Tools\llvm23`（clang-cl 23.1.0）；
  `build-clang/` 目录按用户要求保留待后续 clang 尝试

## 快速编译（Git Bash）

```bash
cd /f/devel/opensource/eden-emulator
source tools/windows/load-msvc-env.sh   # vswhere 自动找到 VS2022
export PATH="/g/Tools/glslang/bin:$(dirname "$(command -v cl.exe)"):/d/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin:/d/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja:$PATH"

cmake.exe --build build-vs22     # RelWithDebInfo，产物在 build-vs22/bin/
```

- 重建 build 目录时须先把 `crtvec_shim.obj` 挂回 `CMAKE_EXE_LINKER_FLAGS`（编译方法见
  `tools/windows/crtvec_shim.cpp` 提交信息），否则 LNK2019（CPM 静态 Qt 6.11.1 引用 VS2022 CRT 没有的 `__std_*` 符号）。
- `/usr/bin/link.exe` 会遮挡 MSVC 链接器，务必保持 MSVC bin 目录在 PATH 前面（上面的 export 已处理）。
- RelWithDebInfo = `/O2 /Ob1 /Zi /MD` + `/DEBUG /OPT:REF /OPT:ICF`，性能版带调试符号，正是 profile 用的配置。
- 构建命令不要接 `| tail` 等管道（吃掉退出码和错误行）；构建后必须验证产物时间戳。

## 产物

| 位置 | 说明 |
|---|---|
| `build-vs22/bin/eden.exe`(+`.pdb`) | **现役 master 构建**（test/master-profiling，TOTK 已验收，user 数据已配） |
| `build-vs22/bin/eden-cli.exe` | 命令行版（**TOTK 勿用**，shader 编译段崩溃，见已知问题） |
| `build-clang/` | 留待 clang 尝试 |
| `eden-v0.2.1/build/bin/` | 冻结的历史基线（对照用） |

## 运行说明

- exe 旁边放 `user/` 目录即进入 portable 模式；否则使用 `%APPDATA%\eden`
- 复制的 `user/config/qt-config.ini` 里数据目录是绝对路径，需逐条改成新 exe 的目录
- 日志在 `user/log/eden_log.txt`（每次启动覆盖，被强杀时缓冲内容会丢）
- 配置项坑：ini 里 `key\default=true` 表示"用编译默认值"，此时即使写了 `key=false` 也被忽略；
  要生效必须同时改 `key\default=false` 和 `key=...` 两行；改 `\default` 期间跑局会把当前值固化

## 工作规则与纪律

1. **过程记录**：每取得阶段性成果（优化提交、新结论、新坑、状态变化），**必须先更新
   PROFILE_PROGRESS.md**（含收益数据、复现要点）再收尾；AGENTS.md 不写过程。
2. **顺序**：改代码 → commit → 再编译测试（保证产物可对应到 commit，方便打包 save/profile 复现对比）。
3. **保密**：本笔记及一切 AI 生成内容仅限本地使用，**不要**以任何形式提交到上游仓库/issue/PR/社区；
   提交信息用 `(local-only)` 标记；推送到自己的 fork（`carton` remote）是允许的。
4. **F:\Switch\Yuzu 只读**（用户日常安装目录，读可以，写不行）。
5. **profile 一律用 GUI 的 eden.exe**（CLI + TOTK 会崩）；分析阶段不开 PGO；计时采集前关后台干扰
   （NVIDIA Overlay/LosslessScaling 等，check_config.py 会拦截；overlay 对 med 实测无扰，可 `--tolerate-overlay`）。
6. **测试按键自动化**：`python F:\prof\patch_input.py`（自动备份到 `.bak-autotest`，测完
   `--restore` 还原用户手柄配置）；focus_ok=True 不代表游戏收到按键，测前 grep
   `player_0_button_a` 确认是 keyboard,code:88。
7. **A/B 纪律**：场景负载时变（如水流动画相位），结论必须各 n≥2；med 落在 tick 整数倍时要
   警惕是量化栅格不是真实负载（详见 PROFILE §23.4）。

## 已知问题（仅列活跃项；结案过程与历史见 PROFILE_PROGRESS.md 对应章节）

1. **master 优雅关闭偶发超时**（~3 局 1 次）：WM_CLOSE 路径 60s 关不掉被强杀（帧时 CSV 丢失），
   不阻塞游玩/测试，关停路径待查。
2. **eden-cli + TOTK**：shader 编译段 `CollectStorageBuffers` segfault（任何版本，官方同期版一样）。
3. **docs/wsl-windows-build.md 所述 WSL 交叉构建产物已不存在**，文档仅存档；构建一律 VS2022+shim。

## Profile 工作流（通用流程）

本机 profiling 工具（均已确认安装）：

| 工具 | 位置 | 用途 |
|---|---|---|
| wpr.exe + **ETW MCP** | `C:\Windows\system32\wpr.exe` 采集；`.etl` 由 ETW MCP 直接分析 | **主力**：CPU 采样栈、线程调度、符号级查询，AI 可自主迭代 |
| Nsight Systems 2024.4.2 | `C:\Program Files\NVIDIA Corporation\Nsight Systems 2024.4.2\` | CPU/GPU 全景时间轴（注意：`-t wddm` 需提权启动游戏，会废掉输入自动化） |
| VS2022 Performance Profiler | VS IDE | 交互式函数级火焰图 |
| Nsight Compute 2024.3.0 | `C:\Program Files\NVIDIA Corporation\Nsight Compute 2024.3.0\` | compute kernel 级（图形场景暂用不上） |

流程要点（详细步骤与脚本用法见 PROFILE_PROGRESS.md §2/§3/§7）：

1. **先观测再采集**：区分 shader 编译卡顿（pipeline cache 二进即顺）vs 持续低帧，优化目标按后者。
2. **判 CPU/GPU-bound**：nvidia-smi 采样设备利用率 / ETW 线程饱和度；eden 线程名直接可读
   （CPUCore_*/GPU/VulkanWorker/HostTiming）。
3. **wpr 采集纪律**：`-start CPU -start GPU -filemode -recordtempto F:`（C 盘满会让会话中途自灭），
   start/stop 必须同一提权脚本；ETW MCP process_trace 30s 超时是常态，后台会继续，`list_traces` 确认后再查。
4. **基准**：`python F:\prof\bench_run.py LABEL`（自动进游戏+测 90s+解析帧时 CSV，支持 `EDEN_DIR` /
   `--tolerate-overlay`）；旋转测试 `rot_test.py`；启动前检查 `check_config.py`。
5. eden 无内建插桩（Tracy/microprofile），走采样式；RelWithDebInfo 的 PDB 直接可用；
   `/OPT:ICF` 会折叠相同函数体，火焰图符号合并属正常噪声。

## 速查

- 性能优化全流程 skill：`.agents/skills/eden-bench`
- 基准历史：`F:\prof\bench_results.csv`；trace 存档与脚本清单：PROFILE_PROGRESS.md §7
- 测试存档（水塘场景）在 build-vs22/bin/user/nand；同步自 F:\Switch\Yuzu（只读源）
