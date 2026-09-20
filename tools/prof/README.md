# tools/prof — Eden 性能/图像 QA 工具集（本地维护）

从 `F:\prof` 整理入库的 profiling 工具（2026-09-20；来历与淘汰清单见
PROFILE_PROGRESS.md §7/§28.16）。**脚本在此维护；数据仍归 `F:\prof`**
（bench_results.csv / shots/ / diag/ / trace / 日志——历史数据不动）。

## 路径约定（全部脚本一致）

| 变量 | 含义 | 默认 |
|---|---|---|
| `EDEN_PROF_DATA` | 数据归档目录（结果 CSV/shots/diag/trace） | `F:\prof` |
| `EDEN_DIR` | eden 可执行目录 | `<repo>\build-vs22\bin`（脚本位置推导） |
| `EDEN_NSP` | 游戏 NSP | `$EDEN_PROF_DATA\TOTK.nsp` |

同目录脚本互相引用（focus_test.ps1 / check_config.py / shot_capture.py），
从仓库任意 cwd 运行均可。**不要往 F:\prof 放新脚本**（包括 quiet_watch 的
sequence 文件——放本目录下、不必提交）；新实验写新的 sequence 文件给
`quiet_watch.py`，或直接加/改这里的脚本。

## 基准与 A/B（宏观结论唯一来源，规则见 AGENTS.md）

| 脚本 | 用途 |
|---|---|
| `bench_run.py LABEL [--measure 90] [--hold N]` | 单局基准：自动进游戏→静置测量→优雅关闭→解析帧时 CSV→追加 `bench_results.csv`（含 luma 列）；VOID 检测（陈旧 CSV / 60fps 菜单签名）；QA 截图到 `shots/<label>/`；diag 留档 `diag/<label>.txt` |
| `bench_ab.py --pairs N --a A --b B [--aenv/--benv K=V,...]` | 交错 A/B 对（AB/BA），报逐对比值中位数；**无参数会真跑基准，别裸跑** |
| `quiet_watch.py SEQ.py` | 静置看门狗：等机器空闲→按 SEQ 序列跑局/命令，用户回来自动静默等待；取代历轮 `*_watch.py`（模板 `quiet_seq_example.py`） |
| `check_config.py` | 跑前守卫：qt-config 档位/按键自动化/干扰进程（LosslessScaling FAIL；NVIDIA Overlay 只 WARN——用户定规不杀） |
| `patch_input.py [--restore]` | 切换/还原自动测试按键映射（A=X code 88，rstick J/L） |

## 图像 QA（改渲染路径必做，规程见 AGENTS.md「图形验收」）

| 脚本 | 用途 |
|---|---|
| `shot_capture.py` | PrintWindow(PW_RENDERFULLCONTENT) 抓 "Form" 渲染窗（遮挡免疫；GDI 句柄坑见 §28.11，勿回退 ctypes 裸调） |
| `shot_compare.py DIR_A DIR_B` | 降采样逐对比较，PASS/WARN/FAIL + 坏点%；先 golden-vs-golden 校准噪声地板 |
| `load_capture.py LABEL` | 加载画面采集（Form 放大到 1280×720 再截） |
| `probe_windows.py` | 窗口层级/PrintWindow 目标探针（截图路径再坏时的诊断） |
| `maximize_eden.py` | 等待并最大化 eden 的 Qt 主窗 |
| `visual_run.py` | 截图专用局（跳过干扰进程检查，--hold 形态） |

## ETW / 微 profile（三层法第 3 层；采集纪律见 AGENTS.md）

| 脚本 | 用途 |
|---|---|
| `wpr_run.cmd` | 提权一体化 wpr 采集（start+100s+stop 同一脚本，产物 `F:\prof\totk_t3.etl`；**会覆盖，先保旧**） |
| `tailimm_ab.py` | 同脚本背靠背 ETW 对（一次 UAC，helper 轮询相位文件；编辑文件头 `PHASES` 适配实验） |
| `gpu_watch.py [秒]` | nvidia-smi 采样到 `gpu_watch.csv`（判 GPU-bound） |
| `csv_timeline.py [logdir] [日期前缀...]` | 全天帧时 CSV 时间线（档位漂移/回归起点定位） |

## 崩溃取证（WER + PDB）

| 工具 | 用途 |
|---|---|
| `sym.cpp` | `cl /O2 /EHsc sym.cpp` 编译；`sym.exe <exe> <hexRVA>` 解析崩溃偏移到函数+行（ICF 折叠警告见文件头） |
| `query_crash.ps1` | 列出 eden 相关 Application Error 事件（含 faulting 模块/偏移） |

## 场景工具（有效休眠，按需启用）

| 脚本 | 用途 |
|---|---|
| `rot_test.py LABEL` / `rot_hold.ps1` | 相机旋转卡顿测试（idle/rotL/rotR 窗口化帧分析） |
| `stutter_watch.py` + `stutter_watch_start.bat` | 手动游玩卡顿哨兵 + wpr 环形缓冲自动抓现场（PresentMon；坑见文件头） |
| `config_diff.py INI_A INI_B` | qt-config 键值对比 |

## 已淘汰（留在 F:\prof 存档，勿再用）

`rot_probe/rot_probe2/analyze_rot`（被 rot_test 替代）、`p2_trace/p2_profile_off/wpr_p2.cmd`、
`pond_trace/wpr_pond.cmd`（被 tailimm_ab 模式替代）、`parse_dmp/dmp_stack`（被 sym.cpp 替代）、
`ninja_windows`（v0.2.1 冻结期一次性）、`live_probe/reanalyze_good/fixverify_run`（一次性）、
`auto_play.ps1/master_run*`、`cap_p1/cleanup/wpr_start/wpr_stop/wpr_run2/wpr_run_boot/wpr_t3/pm_session_cleanup`
（早期 wpr 变体，保留 `wpr_run.cmd`+`wpr_helper.cmd`）、历轮 `*_watch.py`（被 `quiet_watch.py` 取代）。
