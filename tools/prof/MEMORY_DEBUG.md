# TOTK 内存增长排查

## 目前证据与边界

2026-10-06 三份崩溃转储显示 Eden private commit 约 18.16、18.74、19.44 GiB，系统提交量约 51.65、52.35、52.87 GiB，上限约 53.21 GiB。最后一次转储的系统剩余提交额度约 353 MiB。它们来自不同进程，不能拼成同一进程的泄漏曲线；转储也不等于失败分配发生瞬间的系统状态。

最后一次失败涉及约 111 MiB 的 pipeline cache 快照申请。失败位置说明谁最后申请不到内存，不说明谁持有了此前的内存。异步纹理解码队列当时已排空，也不能据此认定解码积压是根因。

已有补丁保护 pipeline cache 保存，并在 staging 分配 OOM 后回收已完成使用的缓存再重试一次。这些措施不证明内存泄漏已排除或修复。

## 1. 先收集同一进程的时间轴

在隔离的 portable 测试目录运行 GUI Eden。保留对应 exe、PDB、源码版本及未提交 diff，记录游戏版本、mod、配置、存档和 shader cache 初始状态。不要在进程运行期间替换 exe；`F:\Switch\Yuzu` 按日常环境只读处理。不要用 CLI 游戏结果替代 GUI 测试。

在 `eden-postmerge` 仓库根目录执行，PID 替换为已启动的测试进程 PID，输出目录必须尚不存在：

```powershell
uv run --project tools/prof python tools/prof/memory_watch.py --pid 12345 --seconds 3600 --output F:\prof\memory-totk-01
```

脚本只读取指定进程：每 5 秒采集 private bytes、working set、系统提交量和上限，每 60 秒汇总虚拟内存区域。产物包括 `memory.csv`、`regions.jsonl`、`metadata.json`。默认输出根目录按 `EDEN_PROF_DATA`、`prof.local.json` 的 data_dir、`F:\prof` 顺序选择。按 Ctrl+C 会保存已收集结果；脚本不会启动、终止或修改 Eden。

自动读档与原地旋转使用 `memory_rot_run.py`，路径、存档、输入准备和失败清理见本目录 README。
归档名或 caption 缩略图不能证明存档地点；必须实际加载检查。已验证的本地水塘快照位于
`prof-data/fixtures/kakariko-pond`，用 save_slot 选择；本轮坐标约1807 -0998，历史站位是否一致未确认。

建议从加载前开始记录，预热约 5 分钟；重复同一路线、传送或菜单操作 10–20 次，再原地静置 5–10 分钟。记录每段开始的 UTC 时间和循环次数。避开无尽探索造成的正常新资源增长。不要为了等到 OOM 耗尽整机提交额度。

重点比较预热后每轮结束的 private bytes：稳定平台符合有界缓存；相同操作反复增长且不趋稳才值得追踪。静置后未下降也可能是 allocator 高水位或仍可达缓存，不能单独证明泄漏。Working set 可被系统裁剪，不能代替 private commit。MEM_RESERVE 不等于已提交内存；映射区域可能存在别名，区域和不等于独占物理内存。客体内存、GPU 驱动映射和 VRAM 必须分别看待。

## 2. 对增长最大的内存类型抓分配栈

工具已经存在：`D:\Windows Kits\10\Debuggers\x64` 中有 UMDH/GFlags；系统有 WPR，WPA 位于 `D:\Windows Kits\10\Windows Performance Toolkit`。本次仅检查工具，没有开启全局跟踪或修改 GFlags。

- **Windows heap 增长：** 在隔离测试 exe 启动前为其独有文件名启用 GFlags `+ust`，配置 `_NT_SYMBOL_PATH` 指向准确匹配的本地 PDB 和符号缓存。分别在预热后、重复 N 轮后、静置后，用 `umdh -p:PID -f:snapshot.txt` 抓取同一进程快照，再用 `umdh before.txt after.txt` 比较。按仍未释放的净增长字节和分配栈排序，找到具体容器或对象生命周期。先记录原有 GFlags，结束后恢复原值，不能盲目清除既有设置。UST 必须在进程启动前开启，会增加开销；UMDH 无法覆盖所有自定义分配器、映射和 GPU 内存。
- **VirtualAlloc/private 区域增长：** 用 WPR 的 `VirtualAllocation` 配置抓有界时间窗口，WPA 查看 `VirtualAlloc Commit LifeTimes`，按栈和存活分配筛选。先检查 `wpr -status`，不取消别人正在进行的记录；启动和停止由同一提权会话负责，使用 `-filemode`，临时及输出文件放在 F 盘。命令形式为 `wpr -start VirtualAllocation -filemode -recordtempto F:\prof\wpr-temp`，窗口结束后 `wpr -stop F:\prof\memory-va.etl`。先创建所需目录并使用新的输出名。
- **GPU/staging/驱动增长：** CPU heap 快照不足以解释，需要结合 Vulkan 分配器的已用/保留字节、staging 各级缓存及 in-flight/deferred 数量、纹理缓存占用。现有 `gpu_watch.py` 的设备总量不能当作 Eden 单进程占用。若曲线指向这一类，再在当前 worktree 添加有针对性的计数器，不先改清理策略掩盖问题。

不要用这些带分配栈跟踪的运行测 FPS。完整转储和 ETL 可能很大，并包含进程数据；保存在本地证据目录。

## 3. 验证是否由性能优化引入

保持已有崩溃保护一致，一次只变一个性能选项或补丁。可以先比较 shared IR cache 开关（`EDEN_JIT_IRCACHE=0` 与默认配置），但不能预设其为根因。使用 AB/BA 顺序、相同可重复场景、相同客体内存配置、独立且相同初始缓存的 portable 用户目录；核对画面和操作确实一致。修正复制配置中的绝对路径，避免写入日常目录。

对比指标是预热后每循环净增长、长期平台、分配栈归属及释放时机，而不是谁更晚崩溃。若用本工具集运行其他 worktree 的构建，明确设置 `EDEN_PROF_REPO` 为对应源码根目录、`EDEN_DIR` 为该构建的隔离测试目录，避免默认运行另一 worktree 的程序。`memory_watch.py` 只读取显式指定的 PID，不使用 `EDEN_DIR` 启动程序。

只有找到持续增长的存活分配及其保留路径，并证明修复后同样操作不再增长，才可把结论升级为已定位并修复泄漏。目前没有真实游戏的这组对照结果。

## 水塘原地旋转自动采样

先按 README 配好 worktree 根目录的 `prof.local.json`；不指定分支，明确选择 GUI
exe 的目录。`memory_rot_run.py --prepare-only` 检查并修正 portable 测试配置，
然后执行 `memory_rot_run.py`（均使用 tools/prof/.venv/Scripts/python.exe）。
输入是键盘映射的虚拟手柄：A=X、B=C、WASD 移动、IJKL 镜头。
默认运行约 30 分钟，带限时读档、镜头验证、预热、基线、交替左右旋转及末尾静置。
读默认最新存档，必须目检 shots 确认水塘；HUD 检测不能鉴定存档地点。
短测可用 `--baseline-sec 15 --rot-win 3 --rot-count 2 --tail-sec 15`。
阶段和内存样本使用 UTC 对齐，result.json 列出各阶段净增长和斜率。
强制关闭可能丢帧 CSV，但已 flush 的内存样本仍可用；错误/用户干扰局不能用于结论。
如果设备持续产生 GameSir 幻影输入，在确认无人占机时才设置 EDEN_IGNORE_INPUT=1。

## 参考来源

- [Microsoft：用 UMDH 定位用户态内存泄漏](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/using-umdh-to-find-a-user-mode-memory-leak)
- [Microsoft：排查应用和服务内存泄漏](https://learn.microsoft.com/en-us/troubleshoot/windows-server/performance/troubleshoot-application-service-memory-leaks)
- [Microsoft：WPR/WPA/Xperf 排查工具](https://learn.microsoft.com/en-us/troubleshoot/windows-server/support-tools/support-tools-xperf-wpa-wpr)

脚本和过程记录统一维护在 `eden-postmerge` 的 `tools/prof/` 与 `PROFILE_PROGRESS.md`；源码修复按正在排查的分支实施。通用约束见本仓库 `AGENTS.md`、`tools/prof/README.md` 与 `.agents/skills/eden-crash-triage`。2026-10-06 的原始转储分析和构建证据仍位于 `../eden-emulator/build-vs22/crash-evidence-20261006/`，迁移工具不会移动这些证据。
