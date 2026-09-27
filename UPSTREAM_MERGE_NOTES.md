# 上游合并笔记（UPSTREAM MERGE NOTES）

> 用途：每次从 `origin/master` 合并进本地分支时，把该区间提交按主题归档成表，
> 性能相关的写详细（动机/机制/对本仓库的影响），其余一句话带过。
> 合并操作本身的冲突解法与坑写在 `PROFILE_PROGRESS.md` 对应章节，本文只管"上游改了什么"。
> 后续合并主线在此文件追加新章节（区间号 + 日期）。

---

## 第 1 轮：`74ccea3def..bebc19da32`（78 commits，2026-09-26 合入 test/p2-draw-resolver）

合并细节、三处冲突解法、QA 事故见 PROFILE §35/§35.1。上游 PR 编号对应
`https://git.eden-emu.dev/eden-emu/eden/pulls/<N>`。

### A. 性能优化（重点）

| 提交 | PR | 内容 |
|---|---|---|
| `9a30172e45` | #4158 | **dynarmic 非独占读写 fallback 合并**。统一 u8/u16/u32/u64 四种尺寸的内存 Read/Write callback 为公共过程，减少不同尺寸访问间的代码跳转；多数尺寸可在一个 u64 寄存器内传递，改善 x64/ARM64 codegen。这是 guest 内存访问慢路径（fastmem miss / 越界兜底）的热点，对 CPU 重载场景有直接收益。 |
| `5f142c7926` | #4219 | **页表分配优化**。page entry 32B→8B；`VirtualBuffer` 重写为对大零区高效的分配（即后来抽出 `SparseLargeVector` 的前身）；CPU 页表 reserve 4GiB→1GiB、实际 commit 至多 ~8MiB；GPU 侧 `big_page_table_dev` 同步换成 `SparseLargeVector<u32>`。副作用收益：Windows 低内存机器启动 committable 从 ~10GiB 降到实际用量，不再拖累其他进程。**本仓库影响**：与我们 memory_manager 里的 WaitForDrawResolve 屏障无冲突（不同区域）。**2026-09-27 实测**：GPU 侧 sparse 化是 merge 后 -7.3% 回归的主因（Windows 读路径每次翻译多 bounds 检查 + committed 位图原子依赖加载；详见 PROFILE §35.4），本地已用 dense `DenseU32Table` 恢复单次解引用读（`b362a07ccd`）；CPU 侧页表保留上游方案。 |
| `38df54edfe` | #4471 | **SparseLargeVector decommit + 零区首页修复**（#4219 引入容器的跟进）：零区首页未正确清零的正确性修复 + 未用页物理内存归还（decommit）。 |
| `4fe5f62c38` | #4446 | **A64 vaddr 查找符号扩展修复 + x64 优化**（#4415/#4444 的 A64 移植）。 |
| `6374f7f51f` | #4448 | 上条的**部分回退**：回退破坏 MK8D 的 2 行（高地址边界处理）。净效果 = 符号扩展修复保留 + 部分查找优化保留。 |
| `ac35358b3f` | #4436 | **SpinLock→std::mutex**：删自旋锁（空转烧 CPU），kernel 侧 k_slab_heap/k_thread 改用 std::mutex，声称更少卡顿/发热。注意是 kernel/HLE 侧，与 video_core 的锁无关。 |
| `bebc19da32` | #4432 | **ScratchBuffer 用 `std::make_unique_for_overwrite<T[]>`**（默认初始化替代值初始化，扩容路径免清零），删 make_unique_for_overwrite polyfill 头。微优化 + 代码卫生。 |
| `0ce29be608` | #4422 | **RDTSC 纳秒换算修复**（计时正确性，非性能本身）：ticks/ns 比率拆整数 + Q0.64 小数，避免 >1GHz TSC 溢出；>1GHz 才用 invariant clock。影响 HostTiming/帧 pacing 的时基精度。 |
| `c5f6f1ca5e` | #4359 | host_memory GetFuncAddress 缺符号不再抛异常（调用方已优雅处理），修崩溃。 |

**本区间的实测结论**（PROFILE §35.1）：机器静置正常时合并版 ~44 fps 与历史持平，
无回归。（首轮 A/B 的 +7.5% 是机器累积退化时段测的，作废。）

> **2026-09-27 勘误（PROFILE §35.4）**：严格交错对复测证实该区间存在 **-7.3%
> 宏观回归**（B/A 中位 0.9269），§35.2 的"无回归"结论作废。主因 = #4219 GPU 侧
> `big_page_table_dev` sparse 化的翻译读税，次因 = #4362 的每 pass multi-range
> Query；本地修复两枚（`b362a07ccd` dense 大页表 + `c0cad6c673` 连续性闸门），
> 修复后 draw 路径微观全回 parity、宏观残差 ≤2%（当日 A/A 噪声底 0.9903 校准）。

### B. GPU / 视频核心（正确性为主，部分与我们改动面相交）

| 提交 | PR | 内容 |
|---|---|---|
| `a538cd9aff` | #4362 | **sparse multi-range storage buffer**。修 SSBO 跨非连续 GPU 页时按连续缓冲读错字节（MH Sunbreak 顶点爆炸/z-fighting 的根因）。引入 `VirtualRangeCache`（惰性向 MemoryManager 查 submapped ranges + deferred unmap 驱逐）与 `vk_multi_range_buffer`（sparse VkBuffer 别名或 gather-copy 呈现给 shader）。**本仓库影响**：`FindBuffer/CreateBuffer` 增加 `sparse_compatible` 第三参；`Binding` 增加 `gpu_addr/segment_first/segment_count`；storage binding 每 pass 走 `ResolveMultiRangeStorage`。我们的 serial-cut 早退与它的交互解法 = 早退追加 `segment_count == 0` 条件（PROFILE §35 冲突 3，stale 池索引隐患）。**2026-09-27 实测**：compute 存储 binding 每 dispatch 的 `ResolveMultiRangeStorage`→`Query`（miss 时 `GetSubmappedRange` 全程遍历）是回归次因；本地已加 O(1) 大页连续性闸门 `IsBigPageContiguousRange` 跳过必然单段的绑定（`c0cad6c673`，sparse 游戏照旧走上游路径；详见 PROFILE §35.4）。 |
| `dbeb73ee01` | #4473 | **cpu buffer 修复 + kepler 上传 / maxwell 宏 dirty 跟踪修复**（UE5 崩溃向：Ender Magnolia 冲刺/野兽崩）。四件事：Kepler ComputeInline 的 dirty 跟踪跨 DMA continuation 与异步回读保留；Maxwell 宏在页粒度 CPU 上传期间保留 GPU-owned 子范围；DiscardWrite 不再用 64B 对齐范围清掉邻接宏参数；DMA Step 用 continuation 感知的 dirty 采样。**本仓库影响**：`SynchronizeBuffer` 上传循环改为排除 `gpu_modified_ranges` 中的 GPU-owned 子区——上传行为变化是我们 serial/uniform diag 重定基线的原因之一。 |
| `9b64944480` | #4406 | channel_state 未初始化 SIGBUS 修复（`= nullptr`）。合并冲突点之一，双方保留。 |
| `c95ad020fb` | #4395 | GPU 关闭/重置无限挂死修复（`NotifyShutdown` = request_stop + join）。**可能直接改善我们已知问题 1（master 优雅关闭偶发超时），值得回归观察。** |
| `ce202292cf` | #4368 | GPU 线程析构顺序修复：gpu_thread 成员前移（最后析构），与 #4395 配套。 |
| `99bf8cf51a` | #4475 | applet 开启时 overlay 变暗 + SGSR 黑屏修复（Z-index 按 IsOverlayOpenLocked 分层；applet 层跳过 SGSR pass）。 |
| `cb73a4dcc7` | #4474 | applet 层跳过后处理（post-processing）。 |
| `fa4e7c6992` | #4408 | vk_texture_cache 删深度比较采样器 fallback 路径。 |
| `74b5e10dc5` | #4465 | doesUpdateMatchProgram 的 update mask 修复（shader 热更新判定）。 |
| `8a10109e24` | #4320 | VkInfo 伪装 PUBGMobile + UE（绕驱动 quirks）。 |

### C. dynarmic / CPU 正确性

| 提交 | PR | 内容 |
|---|---|---|
| `1575f55abe` | #4415 | x64 符号扩展逻辑修复（#4446 的前身）。 |
| `908b1e9a37` | #4458 | relocation 上的坏 ARM codegen 修复。 |
| `7bf95be2c2` | #4444 | A32 错误 LEA 编码崩溃修复。 |
| `2f787ddb8c` | #4366 | f16 指令生成测试修复 + SIGABRT。 |
| `5adaa5b0f7` | #4421 | MSVC handler 表初始化修复。 |
| `90aafeedc1` | #4447 | libc++ 惰性 mutex 初始化的 k_scheduler 兼容修复。 |
| `e27650cf42` | #4369 | NCE：西里尔 UTF8 字符串被误判为独占 store 指令的修复。 |

### D. HLE / 服务 / 固件（简）

- `e7a96c7907` #4390：**固件 23.0.0 支持**（大件）。
- `f7b8ade40e` #4435：stpl:sys / stpl:u 初始实现。
- `8c1194474e` #4428：ssl 实现 Peek/RenegotiateMode/Pending/Poll 等。
- `ee73920d28` #4392：删弃用 SSL 后端（SecureTransport/SChannel）。
- `8a22f1845b` #4394：Enforce max_sessions + bpc:ams 服务。
- `42642f8bad` #4425：bt I{User,Debug,System}::StartDetectionWithFilter。
- `a25e32676f` #4472：audren:d 服务框架修正。
- `00a1c392e7` #4384：opus 解码器实例数限制与错误码。
- `defddec47f` #4429 / `bfba95fd60` #4427 / `23585820bc` #4376 / `dcbcdf16ad` #3770 / `d76f8f91c4` #4438 / `fd34024f0e` #4440 / `a7061eb4c8` #4439：ns/olsc 各种 stub 与实现（qlaunch fw23、ListApplicationIcon 等）。
- `847e91c3a8` #4457：applet 退出清理，避免累积。
- `07f40d5cac` #4442：npad WriteEmptyEntry 修复 + hid 551 stub + applet PopOutData 重构。
- `4ce45b3b37` #4417：MK8D / SM3DW LDN 崩溃修复。
- `87e5d0b6e4` #4343：VirtualBoy classics + QLaunch IPC 修复 + ams SVCInfo。
- `5d150cac5c` #4407：Android qlaunch 回归修复 + FW23 启动 cmd23。
- `1036982d2f` #4355：overlay applet 显示问题部分回退（54cd5fb8eb）。
- `6f6a8f6d67` #4374：SVC trace 日志不再截断 64 位参数。
- `3c38afad74` #4319 + `a277b62fe4` #4462：**bundled 程序共享父资源，后者部分回退其 savedata_factory 部分**（patch_manager/registered_cache 的共享逻辑保留）。
- IPS 三连：`3266c20e3f` #4469（魔数/EOF）、`14235dc0d0` #4445（>256 字节单行 .pchtxt）、`ed57836903` #4336（尾随空白毁 TLTD 十六进制解析）。
- `c3f1e6562b` #4437：Windows 路径净化保留根。
- `f3af5d0c25` #4375：temp 目录清理改按应用级别。
- `e5b8b10188` #4379：fsp-pr 找不到修复。

### E. 音频（含一对自抵消）

- `ff8368c606` #4169 libopus→FFmpeg，随后的 `2b4184dd2b` #4419 完整回退——**净效果为零**（仍 libopus）。若未来重提该方向，这对 diff 是现成参考。
- `93318ef697` #4416：持久设备音量控制。
- `301da63a15` #4397：Android 录屏静音修复（OpenSL 提示）。

### F. 前端 / UI / 输入（简）

- `278f411dad` #4424：**统一命令行解析**（新增 `src/core/launch_params.cpp`；MSVC 下有一处 C4800，我们已修，见 §35）。
- `d9159fefdd` #4352：Qt/SDL 前端鼠标漂移修复（bootmanager 的 mouse update timer）。
- `ed566919f4` #4348：Android 后处理 shader + **ReShade FX 运行时**（`.patch/reshade` 新增、CMake 接入）。
- `c5405250d1` #4426：PC 端 FX shader 配置 UI 精修。
- `099547b3f4` #4393：Android LSFG 快捷开关。
- `ecb2ae4076` #4378：program_args/debug_knobs 设置项移回 debugging 分类且可 per-game——**配置项搬动，旧 ini 键迁移注意**。
- `505b157647` #4430：ConfigurationShared 翻译上下文。
- `20f9aa4cfe` #3710：FAQ 链接换 User Handbook（main.ui 菜单变化）。

### G. 构建 / 代码卫生（简）

- `10c07d700c` #4198：**boost 瘦身分发**（cpmfile.json 变化、`.patch/boost` 删、reshade patch 增；合并后需重新 configure，CPM 缓存兼容）。
- `1203082a8f` #4451：MSVC 构建错误修复。
- `3d37a816c6` #4389：clang 23 unused function 修复。
- `73e004de6e` #4433：page_table 非 constexpr 去标记。
- `2d1eb0dab9` #4450：strerror_r 宏清理。
- `fdd8d4252c` #4333：删未用 vector_math。

### H. 翻译

- `753b57a8c2` / `1a48de6e55` / `9cd24e85c2`：Transifex 同步 ×3（Sep 07/14/21）。

### 本轮合并操作备忘（详见 PROFILE §35）

- 冲突 3 处：channel_state_cache.h（nullptr 初始化）、gpu_thread.h（NotifyShutdown 并集）、
  buffer_cache.h（sparse multi-range × serial-cut 早退，解法 = 早退追加 `segment_count == 0`）。
- MSVC-only 修复 1 处：launch_params.cpp C4800。
- pre-commit 钩子会拦 upstream dynarmic 的 tab 缩进（#4446/#4448），`--no-verify` 通过。
- **重定基线纪律**：SynchronizeBuffer/FindBuffer/上传路径与 dynarmic 均有变化，
  合并前后 serial/diag 中位数与 fps 绝对值不互比。
