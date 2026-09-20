# Eden profiling tools（local-only）

这些脚本是项目维护资产。代码放在本目录，实验数据默认留在 F:\prof。
Python 3.12+；通用开发约束沿用既有 Python 项目的 uv、Ruff 100 列、pytest、Poe。
本目录是独立的 Python 工具项目，不影响模拟器本身的 CMake 构建。

## 环境与质量检查

在本目录执行：

    uv sync --locked
    uv run poe format
    uv run poe check-full
    uv run pytest --cov=. --cov-report=term-missing

check-full 包括格式检查、Ruff、公共纯逻辑模块 prof_common.py 的 strict mypy、
全部 pytest。其他 Windows 边界模块目前不宣称通过 strict mypy。
测试使用临时目录和进程/API 替身，默认禁止真实 subprocess，不启动游戏、
不发送按键、不提权、不运行 WPR、不读取用户配置。真实 GUI/驱动验收另行安排。

首次修改公共行为时补回归测试；优先测错误路径、结果有效性与资源生命周期。
所有 CLI 支持 --help；模块导入无启动/采集/文件写入副作用。
在仓库根目录也可运行：

    uv run --project tools/prof python tools/prof/bench_ab.py --help

## 公共模块

- prof_common.py：路径、受保护目录、label/参数校验、原子写、帧数据和统计。
- eden_session.py：portable GUI 会话、唯一自动化锁、输入活动检测、按 PID 关闭。
- shot_capture.py：统一类型化 Win32/GDI 截图，释放前复制像素并恢复旧 GDI 对象。
- wpr_capture.py：WPR 所有权、退出码与 trace 校验、限时 JSON helper。
- window_input.ps1：向明确 PID 的主窗投递按键，finally 释放按键；不向桌面注入全局输入。

## 路径和运行前提

| 环境变量 | 默认 |
|---|---|
| EDEN_DIR | 仓库根目录/build-vs22/bin |
| EDEN_PROF_DATA | F:\prof |
| EDEN_NSP | EDEN_PROF_DATA/TOTK.nsp |
| EDEN_PYTHON | cmd/bat 包装器使用的 Python，默认 python |
| PM_EXE | G:\Tools\PresentMon\PresentMon-2.5.1-x64.exe |

F:\Switch\Yuzu 及其解析后子路径禁止写入/作为运行目录。
启动前要求 portable 配置存在且没有其他 eden.exe；不会按进程名清理游戏。
同一数据目录的 automation.lock 防止并发自动化；异常断电遗留锁须确认旧任务结束后手动删除。
不同数据目录不构成机器级并发隔离，不能同时运行两个自动化任务。

NVIDIA Overlay 只 WARN，不阻止、不关闭。LosslessScaling 仍使 preflight 失败。
自动化期间用户键鼠活动会使当前测量作废；窗口消息投递成功只代表消息入队，
不证明游戏已经接收，因此仍须检查截图和场景。
重启后先 warmup；图像验收和背靠背 A/B 纪律见根目录 AGENTS.md。

## 基准与留档

    python bench_run.py LABEL --measure 90
    python bench_ab.py --a golden --b token --pairs 3 --benv "EDEN_DRAW_TOKEN=1"
    python quiet_watch.py quiet_seq.py

每局使用 runs/<label>-<随机ID>/，内含 result.json、frames.csv、shots/、diag.txt。
result.json 区分源码 source_head 与实际 exe SHA-256/mtime；不能用源码 HEAD
声称 exe 是该版本。旧 bench_results.csv 不再改写或迁移，历史数据保持原样。
机器读取成功结果用 RESULT_JSON，失败退出非零且不输出成功 JSON。
失败局保留 valid=false 的 manifest。截图缺失、旧/歧义 CSV、时长不足、
菜单帧率签名、用户输入、强杀均不得作为有效结果。

帧 CSV 没有绝对时间戳，统计只能近似关闭前的尾窗；关闭耗时超过 5 秒拒绝出数。
不能据此声称精确窗口 ETW 对齐。默认每 15 秒截图，首次在 5 秒；
人工检查截图确为目标游戏场景仍必需。--no-shots 只适合诊断，不可用于 A/B 结论。
--hold N 只持有窗口，不输出性能结果。

A/B 交替 AB/BA；任一边失败重跑整对，未收齐指定对数退出非零。
每边清除继承的 EDEN 实验开关，仅保留路径变量，再应用明确的 aenv/benv。
要求两边有 luma、均值差不超过 1 且各自标准差不超过 1（保守初筛，不替代场景 QA）。
只报逐对 B/A 比值中位数，禁止用独立样本均值替代配对。

quiet_seq.py 是可信本地 Python 文件，可执行代码，勿载入外部不可信文件。
RUNS 使用唯一 label；cmd 必须是参数列表，构建或基准失败立即停止后续步骤。
PAIRS 是明确的完整 label 对，取消旧的前缀过滤后 zip 机制。
宏观性能对比优先使用 bench_ab.py。看门狗不再自动关闭 LosslessScaling。

## 输入配置

    python patch_input.py [INI]
    python patch_input.py --restore [INI]

仅维护六个测试键，备份到 INI.autotest.json。重复 patch 不覆盖原始值；
restore 恢复这些键后立即返回，保留其他设置，成功后删除键备份。
原始 .bak-autotest 全文件备份不自动恢复：先人工比对，以免回滚后来的其他配置。
改配置前须关闭 Eden。

## 辅助命令

| 命令 | 用途 |
|---|---|
| check_config.py [--ini INI] | 配置、按键与干扰进程检查 |
| config_diff.py INI_A INI_B | 按 section/key 比较，不混同不同 section |
| csv_timeline.py [LOGDIR] [日期前缀...] | 帧时间分析，默认尊重 EDEN_DIR |
| shot_compare.py DIR_A DIR_B | 自然时间顺序配图，空目录/数量不匹配失败；先做 golden 校准 |
| load_capture.py LABEL [--duration N] | 独立加载图像采集目录 |
| visual_run.py [--hold N] | 经同一 preflight 的 GUI 持有会话 |
| probe_windows.py PID [--outdir DIR] | 复用正式截图路径检查窗口与子窗 |
| maximize_eden.py PID | 只最大化该 PID 的 Form |
| rot_test.py LABEL [--win N] [--reps N] | 旋转窗口与原始 CSV 留档，窗口对齐为近似值 |
| gpu_watch.py [SECONDS] [--output FILE] | 每 GPU 每采样一行，拒绝覆盖现有文件 |
| query_crash.ps1 [-Count N] [-Days N] | 先按 Eden Application Error 过滤再截取数量 |
| sym.exe MODULE HEX_RVA | PDB 符号定位；编译 sym.cpp，使用加载后的实际基址 |

## ETW 与卡顿哨兵

    python tailimm_ab.py --seconds 40
    python wpr_capture.py OUTPUT_DIRECTORY --name capture --seconds 100 --with-gpu
    python stutter_watch.py --duration 300 --max-captures 4

tailimm_ab 一次 UAC 启动隐藏 helper，两个阶段准备好后才原子发布 JSON 请求。
helper 仅接受 name/seconds，不执行数据目录中的 cmd；唯一 session 目录隔离旧请求。
只在 WPR 返回成功且 ETL 非空时报告完成，helper 空闲超过 10 分钟退出。
独立 wpr_capture/stutter_watch 需从提权终端运行，cmd/bat 只是脚本相对路径包装器。
不在启动时取消别人的 WPR 或 PresentMon 会话。WPR start/stop/cancel 同一进程发出。

哨兵用独立 PresentMon session 和输出目录；触发前预留采集额度；
后台线程只 stop/写文件，主线程决定是否重新 start，退出时先等待 stop 完成再清理。
队列溢出、缺帧、采集失败返回失败；不会悄悄给出完整采集成功的结论。
WPR 子进程有超时；如果系统级 WPR 自身挂起，检查日志和系统会话后人工恢复。
历史一次性脚本保留在 F:\prof 存档，后续维护只改本目录。

## AMD uProf（缓存归因；本机暂被 VBS 拦截）

完整参数手册与本机实测记录见 `docs/local/uprof-cli.md`（含解锁后的重试配方）；
本节只留最小要点。

CLI：`D:\Program Files\AMD\AMDuProf\bin\AMDuProfCLI.exe`（5.3.521）。
`collect -h` 在本机挂起且无输出，选项表由错误驱动探针摸出；调研全文与
来源 URL 存 `F:\prof\codex_uprof_cli_research.md`。

- TBS 定时采样（默认，非提权实测可用）：`collect -a -d 60 -o DIR`（全系统）
  或 `--pid <PID>` 附加本用户原生进程；产物在 `cpu\CpuProfile_*.prd`。
- IBS 指令采样：`collect --config ibs --pid <PID> -d 60 -o DIR`，等价
  `-e event=ibs-op,interval=250000,user=1,os=1`（可再叠 `-e event=ibs-fetch,...`）。
  Zen 3 无 `ibsop-l3miss` 过滤（Zen 4 专属），逐函数缓存归因看报表的
  IBS_LD_L2_MISS / IBS_L1_DC_MISS_LAT 族指标。IBS 实际按系统级采样，
  `--pid` 只是过滤，官方建议加 `--cpu` 控制数据量。
- 报告：`report -i <会话目录> --detail -s event=ibs-op` 生成 report.csv；
  函数级归因要求目标带 PDB（eden RelWithDebInfo 构建即满足）。
- **本机现状（2026-09-20 实测）**：`ERROR: IBS counters are not available`
  ——HypervisorPresent=True、VBS running（Windows 内存完整性）时 EBP/IBS
  被禁用，TBS 不受影响。要做 IBS 须先关内存完整性并重启，属用户决策，
  不要擅自改系统虚拟化配置。`AMDProfilerService` 仅远程 profiling 需要。
