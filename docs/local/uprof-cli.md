# AMD uProf CLI 使用手册（本机实测版）

> 本机环境：AMD uProf **5.3.521** @ `D:\Program Files\AMD\AMDuProf\`，Windows 11，
> Ryzen 5 5600（Zen 3）。用法来源：2026-09-20 本机错误驱动探针（`collect -h` 在本机
> 挂起，选项表靠非法参数报错逐个挖出）+ codex-research 查官方 5.3 用户指南
> （全文与来源 URL 存 `F:\prof\codex_uprof_cli_research.md`）。
> 本文档随 PROFILE_PROGRESS.md §28.21 首次落地。

## 1. 前置条件与当前拦截状态

- CLI 主程序：`D:\Program Files\AMD\AMDuProf\bin\AMDuProfCLI.exe`。
- **普通采样不需要管理员**（TBS 定时采样本机非提权实测 rc=0，IBS 文档同样不要求）。
  仅 thread-concurrency 等附加功能明确要求管理员；`AMDProfilerService` 只是远程
  profiling 的服务，本机采集不需要。
- **VBS/Hyper-V 拦截（当前状态）**：本机 HypervisorPresent=True 且 VBS running
  （Windows"内存完整性"开着）时，uProf 的 **EBP/IBS（PMU 硬件计数器采样）被禁用**，
  实测报 `ERROR: IBS counters are not available`；TBS 不受影响。状态检查：

  ```powershell
  (Get-CimInstance Win32_ComputerSystem).HypervisorPresent   # 应为 False
  Get-CimInstance -ClassName Win32_DeviceGuard -Namespace root\Microsoft\Windows\DeviceGuard |
    Select-Object -ExpandProperty VirtualizationBasedSecurityStatus   # 0=off
  ```

  解锁 = Windows 安全中心关闭"内核隔离→内存完整性"+ 重启（用户决策；
  用户已计划后续关闭以重试 IBS，见 §6 重试配方）。

## 2. 命令速查

### 顶层命令

| 命令 | 用途 |
|---|---|
| `collect` | 采集样本数据（核心命令） |
| `profile` | 采集+分析+直接出报告的组合命令 |
| `report` | 从已采集数据生成报告 |
| `translate` | 原始数据转数据库文件 |
| `timechart` | 系统 power/thermal/frequency 时序 |
| `info` | 系统与 CPU 信息 |
| `compare` / `diff` | 多份 profile 数据对比 |

注意：**`collect -h` / `collect --help` 在本机挂起且无输出**（该 CLI 的坑）。
顶层用法用无参调用可打出来；子命令选项用"传非法参数看报错"来探，例如
`collect -zzz` → "not a valid short option"。

### collect 选项（本机逐个验证过）

| 选项 | 语义 | 备注 |
|---|---|---|
| `-a` | 全系统采集（标志，无参） | 与启动程序、`-p` 三选一 |
| `-p <pid>` / `--pid <pid>` | 附加到运行中进程 | 可逗号分隔多个；**无进程名过滤**，先自己拿 PID |
| `-d <秒>` | 采集时长 | `--duration` |
| `-o <目录>` | 输出目录 | 会话目录建在其下 |
| `-e <事件>` | 事件/EBS/IBS 规格 | 见 §3；裸写 `ibs_op` 会被拒，要 `event=ibs-op` 键值式 |
| `-t <间隔>` | 采样间隔 | TBS 用 |
| `-f <格式>` | 输出文件格式 | 报告格式 text/csv 等 |
| `-c/--cpu <规格>` | 限定 CPU/核 | IBS 数据量大时官方建议加上 |
| `-s/--sort-by <键>` | 报告排序 | |
| `-m` | `--data-buffer-count` | 内部缓冲数，一般不动 |
| `-w <目录>` | 被测程序工作目录 | |
| `-b` | 仅限"启动程序"模式的标志 | |
| `-i` | `--input-dir`（report 侧用） | |

### 附加上限（Windows）

只能附加**同用户**的**原生程序**（不能是 SYSTEM/其他账户），最多 512 个 PID；
`--pid` 是过滤器不是独占。eden.exe（非提权 GUI）从非提权终端附加没有问题。

## 3. IBS 采集（解锁后的目标用法）

```powershell
# 简写配置（推荐起点）
AMDuProfCLI.exe collect --config ibs --pid <EDEN_PID> -d 60 -o F:\prof\uprof\ibs-<label>

# 等价的显式写法（可调参数）
AMDuProfCLI.exe collect `
  -e event=ibs-op,interval=250000,ibsop-count-control=0,user=1,os=0 `
  -e event=ibs-fetch,interval=250000,user=1,os=0 `
  --pid <EDEN_PID> -d 60 -o F:\prof\uprof\ibs-<label>
```

参数含义：

| 参数 | 含义 |
|---|---|
| `interval` | IBS FETCH=fetch 计数；IBS OP 由 count-control 决定按 cycle 还是 dispatch；默认 250000 |
| `ibsop-count-control=0/1` | 0=按 cycle 采样，1=按 dispatch 采样 |
| `user=1,os=0` | 仅用户态；`user=1,os=1`（默认）加内核 |

要点与坑：

- **IBS 实际按系统级采样，`--pid` 只是过滤**；官方警告 rawdata 可能很大，建议加
  `--cpu` 限定核以减量（eden 的 GPU 线程可先任务管理器看常驻核）。
- **Zen 3 没有 `ibsop-l3miss` 过滤**（Zen 4 专属）；逐函数缓存归因看报表的
  `IBS_LD_L2_MISS`、`IBS_L1_DC_MISS_LAT`（L1 miss 总延迟）、`IBS_LD_LOCAL_CACHE_HIT`、
  `IBS_LD_LOCAL_DRAM_HIT` 等派生指标族。部分 Northbridge/remote-cache 指标 Zen 3 不支持。
- TBS（默认模式，当前唯一可用）：`collect -a -d 60 -o DIR`（全系统）或加 `--pid`；
  产物为 `session.uprof` + `cpu\CpuProfile_*.{prd,ri,ti}`。

## 4. 报告生成

```powershell
AMDuProfCLI.exe report -i <会话目录> --detail -s event=ibs-op
# 产物：<会话目录>\report.csv
```

- 函数级归因要求**目标带调试信息（PDB）**——eden 的 RelWithDebInfo 构建即满足
  （`eden.pdb` 在 exe 旁）。
- 报告可按 process/module/thread 聚合；`--detail` 出明细。
- 另有 cache-line/false-sharing 专用的 `--config memory` 采集 + 
  `report --sort-by event=l3-miss`，可作为 IBS 的补充视角。

## 5. 本机实测记录（2026-09-20）

- TBS 全系统 3s：rc=0，产物正常；TBS `--pid` 附加 sleep 进程 3s：rc=0。
- IBS `--config ibs --pid` 附加 5s：`ERROR: IBS counters are not available`
  → §1 的 VBS 拦截实锤。
- 探针方法：bash 直跑 CLI 会因 conio 挂起，**用 python subprocess（关 stdin）跑**；
  选项表靠 `collect -<字母> junk` 的报错长名逐个还原（`-m` 报错暴露
  `--data-buffer-count` 等）。
- 探针的输出目录会建在**当前工作目录**（当时在仓库根留下 `AMDuProf-SWP-*` 目录，
  已清理）——正式采集务必 `-o` 指到 F:\prof 下。
- 附带发现：`bin\AMDuProfPcm.exe` 是独立的核级计数器监视器（时序/累计指标
  dc/fp/ipc/l1/l2/tlb，`-l` 可列全部原始 PMU 事件名），与 CLI collect 是两个工具；
  若要看"掉帧窗口内 L2 miss 率时序"可用它，但它也是 PMU 系，同样被 VBS 拦。

## 6. 解锁后的重试配方（水塘场景 IBS 归因）

1. 确认 VBS 已关（§1 两条 powershell；HypervisorPresent 应为 False）。
2. 照常起 bench/rot（`python tools/prof/bench_run.py LABEL` 或 rot_capture），
   游戏进场景后拿 eden.exe PID。
3. 采集（IBS 有采样开销，**该局帧时只做参考，不进宏观 A/B 结论**）：

   ```
   collect --config ibs --pid <PID> -d 60 -o F:\prof\uprof\ibs-<label>
   ```

4. 报告：`report -i <会话目录> --detail -s event=ibs-op`。
5. 分析目标（§28.21 遗留问题）：按函数聚合的 miss/延迟排名 vs ETW CPU 采样排名
   交叉，验证"散布成本=cache miss"假设——重点看 `HostMemory::Impl::Protect`
   （页保护 churn）、描述符表读（DescriptorTable::Read）、`GetContinuousSizeFrom`/
   `IsContinuousRange`（内存连续性遍历）、hash/memcmp 族的 miss 份额；与 codex
   串行路径提案（`F:\prof\codex_serialpath_proposals_20260920.md`）里的切口
   优先级互相校准。
6. 结果与脚本输出归档到 `F:\prof\runs\<label>-<id>\`（或 `F:\prof\uprof\`），
   结论写 PROFILE_PROGRESS.md。
