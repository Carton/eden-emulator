---
name: eden-bench
description: Eden 模拟器（TOTK）本地 GUI 性能基准、输入配置、原地旋转内存增长采样与 profiling；支持配置路径后跨 worktree 运行。
---

# Eden TOTK 性能基准与 Profiling

完整背景读仓库根目录 `PROFILE_PROGRESS.md`（性能进度记录，含所有坑）；
**方法论与规则以 `AGENTS.md` 为准（"图形验收与性能评估规程" 章 + 工作规则与纪律），
本 skill 是速查卡**。

## 关键路径

- 脚本根 = 此 skill 所在仓库的 `tools/prof`；目标源码/构建以调用者配置和
  PROFILE_PROGRESS 最新验证为准，不硬编码 worktree、分支或 build 目录。
  `prof.local.json` 放目标 worktree 根，示例 `tools/prof/prof.local.example.json`；
  填 eden_dir/data_dir/nsp，可选 repo。相对路径以 JSON 所在目录为准，env 优先。
  跨 worktree 调用已有脚本时设置 EDEN_PROF_CONFIG 的绝对路径。
  没有配置才回退脚本仓库/build-vs22/bin；不要把此回退当作最新有效构建。
- **脚本集：`tools/prof/`**（清单/淘汰名单/路径约定见 `tools/prof/README.md`；
  数据归档 `EDEN_PROF_DATA` 默认 `F:\prof`——bench_results.csv / shots / diag /
  trace 都在那，**新脚本不再放 F:\prof**）
- 游戏由 nsp/EDEN_NSP 选择；数据目录 = eden_dir/user（portable）。
- **一切 AI 生成内容仅限本地，严禁 push 上游 / 提 issue / PR**（推自己 fork 允许）

## 构建（Git Bash，增量 ~1-5 分钟）

```bash
cd "$EDEN_PROF_REPO"  # 明确选定目标源码 worktree
source tools/windows/load-msvc-env.sh
export PATH="/g/Tools/glslang/bin:$(dirname "$(command -v cl.exe)"):/d/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin:/d/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja:$PATH"
cmake.exe --build "$EDEN_BUILD_DIR"  # 明确选定该 worktree 的 build 目录
# 直接检查退出码与产物时间戳，不接吞掉失败状态的管道
```

产物以选定 build 的 bin/eden.exe 为准。**勿用 eden-cli 跑 TOTK（shader 段必崩）**。

## 准备与内存旋转测试

先读 tools/prof/README.md 和 MEMORY_DEBUG.md。用 uv sync --locked --python
明确选择已安装 Python，避免 .venv 引用已移除解释器。运行 memory_rot_run.py
--prepare-only 检查 exe/游戏/portable NAND，并备份修正键级配置和 portable 路径。
所有共享 preflight 均自动准备输入；可用 prepare_test.py --restore 还原。
映射 A=X、B=C、移动=WASD、镜头=IJKL，player 0 显式连接，TAS 关闭，
custom 输入使用全局映射。消息必须发到同 PID 可见 Form 渲染窗；input_ok
仅证明投递，红心 HUD、镜头响应及截图目检才证明游戏与场景正确。

memory_rot_run.py 默认限时读最新存档、双向预热、120s 基线、60x20s 交替
左右旋转、300s 静置；启动到退出全程 memory_watch 采样。先短测
--baseline-sec 15 --rot-win 3 --rot-count 2 --tail-sec 15 验证管线。
检查 shots 确为水塘，再使用同 PID 预热后的 private bytes 曲线判断平台/增长。
内存局无须套历史 FPS 预设；不要用菜单增长、加载增长、working set 或静置不降
单独宣布泄漏。失败必须保留 result/phases/watcher.log，清理拥有的进程。

## FPS 基准（单局 ~4 分钟；A/B 结论用交错对）

```bash
python tools/prof/bench_run.py <LABEL> --measure 90       # 单局
python tools/prof/bench_ab.py --pairs 3 --a golden --b LABEL \
    --benv "EDEN_XXX=1,EDEN_YYY=1"                        # A/B 结论只认这个
```

**无人值守轮（用户可能在机器旁时）**：写 sequence 文件（模板
`tools/prof/quiet_seq_example.py`：RUNS 列表 + 可选 PAIRS 比值汇总），分离启动
静置看门狗——等空闲（默认 180s 无输入）自动跑，用户回来自动静默：
```bash
powershell.exe -NoProfile -Command "Start-Process -WindowStyle Hidden -FilePath 'python' -ArgumentList 'tools/prof/quiet_watch.py','tools/prof/myseq.py' -RedirectStandardOutput 'F:/prof/myseq.log' -RedirectStandardError 'F:/prof/myseq.err'"
```
（sequence 文件是每轮实验的脚本，放仓库 `tools/prof/` 下、不必提交；日志/结果照旧
进 F:\prof。）sequence 里的 cmd（如局间构建）必须用完整路径 Git Bash（裸 `bash`
会解析到 WSL，/f/ 路径必失败——§28.16 实坑）。**重启后首个 bench 局只作 warmup
丢弃**（AGENTS 规则）。

自动完成：检查没有其他 Eden → 启动指定 portable exe → 自动 A 键进游戏 → 静置测量与截图 → 按 PID 优雅关闭 → 校验新 CSV → 独立目录留档。
新局归档位于 F:\prof\runs\<label>-<id>\，含 result.json、frames.csv、shots/、diag.txt。
实际 exe SHA-256 与源码 HEAD 分开记录；旧 bench_results.csv 不改写。
失败退出非零；机器读取成功数据用 RESULT_JSON。整对失败须重试整对；quiet sequence 的 PAIRS 使用完整标签。

- preflight 自动准备 record_frame_times/confirmStop 和测试输入，检查读回；不假设已配好。
- **跑基准期间用户不能碰键盘/鼠标**（前台锁吃掉注入按键；测试时段有人=数据作废）
- **VOID 签名**（RESULT VOID，不进 CSV）：陈旧 CSV / fps>52 或 med<20（60fps
  菜单栅格；游戏内 med 最低 21.67）
- **场景档位**：水塘有快/慢两档（med 22.50 vs 25.00+ms，跨档 ~11%，档位持续数小时）
  ——单局绝对值只在同档内可比；**A/B 用 bench_ab 交错对，报逐对比值中位数**，
  阈值 ±2%；med 落 0.83ms 栅格整数倍警惕量化栅格（PROFILE §23.4）
- 小收益（<4%）优化用 FPS 不可判——用微 profile 或逐 draw diag 归因
- 强杀游戏会丢帧时 CSV（PerfStats 析构才落盘）；改 qt-config 前确认游戏没在跑

## 微 profile（wpr 采集 + ETW MCP 分析）——小收益优化的判定手段

### 采集

优先运行 python tools/prof/tailimm_ab.py --seconds 40：一次 UAC，阶段进入游戏后发布原子 JSON 请求，唯一目录隔离旧请求，不取消其他 WPR 会话。
独立采集在提权终端运行 python tools/prof/wpr_capture.py OUTPUT_DIRECTORY --name capture --seconds 100 --with-gpu；拒绝覆盖已有 ETL，校验退出码与非空产物。
不再依赖 WPR_DONE 字符串或数据目录中的可执行 cmd。命令参数与迁移见 tools/prof/README.md。

### 分析（本会话 ETW MCP，30s 超时但后台继续）

```
mcp__etw__process_trace(filePath="F:\\prof\\totk_cpugpu.etl",
    categoryNames=["Sampled CPU Usage Data","CPU Scheduling Data","Ready Thread Data"])
→ 超时是正常的，等 2-3 分钟后 mcp__etw__list_traces() 确认拿到 traceId
```

已验证的查询套路（先建后执行）：
1. **线程总账**：Process.ImageName==eden.exe，按 Thread.Name 分组，Sum(Duration)。
   盯 GPU / CPUCore_0/1/2 / VulkanWorker 的相对变化（判断瓶颈线程是否变化）。
2. **单线程函数热点**：加 Thread.Name==GPU 条件，按 FunctionName 分组（Sum+Count）。
   eden 线程名直接可读；全量 FunctionName 分组大 trace 必超时，**带线程过滤后再按函数分组**。
3. **内核归因**（如 fastmem 缺页排查）：Thread.Name in CPUCore_*，按 ModuleName 或
   FunctionName 分组，看 ntoskrnl 的 MmAccessFault/MiUserFault 占比。
注意：`/OPT:ICF` 折叠 + 内联会让函数归因在版本间偏移，**对比要按"函数簇合计"看**；
每次 perform_query 超时可直接重发（queryId 保留）。

## 图像 QA（改渲染/图形路径的必做验收；完整规程见 AGENTS.md）

- bench 测量窗自动截 6 张到 `F:\prof\runs\<LABEL>-<id>\shots\`（PrintWindow 抓 "Form" 渲染窗，
  遮挡免疫）；**每局确认截图内容是游戏画面**——内容=桌面/其他应用 ⇒ 该局作废
  （用户占机铁证）。截不到=游戏窗不可用，查 tap 日志。
- 对比：`python tools/prof/shot_compare.py DIR_A DIR_B`（PASS/WARN/FAIL+坏点%）；
  先跑 golden vs golden 校准噪声地板；参照局与测试局**背靠背**。
- 水塘干净局 luma 60–61（result.json 有字段），离带=档位不同或污染。
- 加载画面只比右侧/右下（左侧每次加载随机）；细节问题用 load_capture.py 放大窗口。

## 渲染正确性检查（改了状态跟踪类代码后必做）

基准测量窗口期间比对 QA 截图（同上）：水塘场景水面/植被/HUD 齐全，无黑块花屏、
无右缘 HUD 元素渲染异常（P2 token 模式的已知敏感位，PROFILE §28.4）。

## 配置 A/B 注意（qt-config.ini）

- `key\default=true` 表示用编译默认值（value 行被忽略）；改配置必须同时改
  `\default=false` + 值两行。用 python 按行改（反斜杠会被 shell 吃）。
- CPU 精度：`cpu_accuracy` 0=Auto 1=Accurate 2=Unsafe（TOTK 上 Auto 即 Accurate）。
- 游戏正常退出会重写此文件；强杀不会。

## 已知纪律

- 构建身份以 exe hash/PDB 及 PROFILE_PROGRESS 最新验证为准；分支 HEAD 不代表磁盘 exe。
- 采集/基准前 `python tools/prof/check_config.py`（LosslessScaling 等会被拦；
  **NVIDIA Overlay 一律不杀不拦**，2026-09-19 用户定规，med 实测无扰）
- 缓存归因（"成本摊匀在所有函数"类）：AMD uProf @ `D:\Program Files\AMD\AMDuProf`
  （IBS 按函数归因 L2/L3 miss），share 采样看不见这类
- 强杀游戏进程前记得 `fsp_srv` 补丁必须在（已在分支里，勿 revert）
