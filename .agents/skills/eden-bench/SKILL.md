---
name: eden-bench
description: Eden 模拟器（TOTK）性能基准与 profiling 工作流——构建 v0.2.1 worktree、跑自动 FPS 基准（内建帧时间 CSV）、wpr+ETW 微 profile 采集分析。用于 Eden 性能优化相关任务。
---

# Eden TOTK 性能基准与 Profiling

完整背景读 `F:\prof\HANDOFF.md`（交接文档，含所有坑）。本 skill 是速查卡。

## 关键路径

- 工作基线（改代码、跑测试都在这）：`F:\devel\opensource\eden-v0.2.1`，分支 `local-profiling`
- 游戏硬链接：`F:\prof\TOTK.nsp`；数据目录：`build\bin\user\`（portable）
- **一切 AI 生成内容仅限本地，严禁 push 上游 / 提 issue / PR**

## 构建（Git Bash，增量 ~1-5 分钟）

```bash
cd /f/devel/opensource/eden-v0.2.1
source /f/devel/opensource/eden-emulator/tools/windows/load-msvc-env.sh
export PATH="/g/Tools/glslang/bin:$(dirname "$(command -v cl.exe)"):/d/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin:/d/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja:$PATH"
cmake.exe --build build 2>&1 | tail -15
```

产物 `build/bin/eden.exe`。**勿用 eden-cli 跑 TOTK（shader 段必崩）**。

## FPS 基准（一条命令，~4 分钟）

```bash
python F:/prof/bench_run.py <LABEL> --measure 90
```

自动完成：杀残留 → 启动 eden → 自动 A 键进游戏（~110s）→ 静置测量 90s →
优雅关闭（WM_CLOSE）→ 解析 eden 内建 `record_frame_times` 的逐帧 CSV → 追加到
`F:\prof\bench_results.csv`，stdout 打一行 `RESULT <label>: frames=... fps=... med=...ms`
（另有 tail_fps = 丢掉窗口头 15s 后的均值，规避读档后追赶帧）。

- 前提：qt-config.ini 已设 `record_frame_times=true`、`confirmStop=2`（都已配好）
- **跑基准期间用户不能碰键盘/鼠标**（前台锁会吃掉注入按键）
- 每个构建至少跑 2 次看方差；该场景（卡卡利科村）中位帧时极稳（24.99ms 量化），
  但 fps_mean 噪声 ±1.5——**小收益（<4%）优化不要用 FPS 判定，用下面的微 profile**
- 强杀游戏会丢帧时间 CSV（PerfStats 析构才落盘）；改 qt-config 前先确认游戏没在跑
- 历史结果：`cat F:\prof\bench_results.csv`（含 git commit 列，可追溯）

## 微 profile（wpr 采集 + ETW MCP 分析）——小收益优化的判定手段

### 采集（~6 分钟）

1. **hold 模式起游戏**（进游戏后保持不关，供采集窗口用）：
   ```bash
   cd /f/prof && python bench_run.py wpr-hold --hold 260   # 后台跑
   ```
2. **等进游戏 + 帧率稳定再采样**：进游戏约在 t+110s，**再等 ≥30s**（帧率从爬升到稳态）
   后才触发采集——直接 `sleep 145` 再执行下一步。
3. 触发提权一体化采集（100s，起停必须在同一脚本里）：
   ```bash
   powershell.exe -NoProfile -Command "Start-Process -Verb RunAs -WindowStyle Minimized -FilePath 'F:\prof\wpr_run.cmd'"
   ```
   产物 `F:\prof\totk_cpugpu.etl`（~6GB；**会覆盖**，先 `mv` 保住旧 trace）。
4. 轮询 `F:\prof\wpr_run.log` 出现 `WPR_DONE`。

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

## 渲染正确性检查（改了状态跟踪类代码后必做）

基准测量窗口期间用 computer-use 全屏截图（`mcp__computer-use__screenshot`），
比对场景是否正常（卡卡利科村：帐篷/树/人物/HUD 齐全，无黑块花屏）。

## 配置 A/B 注意（qt-config.ini）

- `key\default=true` 表示用编译默认值（value 行被忽略）；改配置必须同时改
  `\default=false` + 值两行。用 python 按行改（反斜杠会被 shell 吃）。
- CPU 精度：`cpu_accuracy` 0=Auto 1=Accurate 2=Unsafe（TOTK 上 Auto 即 Accurate）。
- 游戏正常退出会重写此文件；强杀不会。

## 已知纪律

- master 构建产物跑不了 TOTK（卡 launching）——实验全在 v0.2.1 worktree
- 采集/基准时确认 Lossless Scaling 已退出（`F:\prof\cleanup.cmd` 提权强杀）
- 强杀游戏进程前记得 `fsp_srv` 补丁必须在（已在分支里，勿 revert）
