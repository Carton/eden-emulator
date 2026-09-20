---
name: eden-crash-triage
description: Eden 崩溃取证——查 WER/Application Error 事件、提取 faulting 模块与偏移、用 tools/prof/sym.cpp 把 RVA 符号化到函数+行（PDB）、判断崩溃是否同 site / 是否与我们的改动相关。用于 eden.exe 或驱动模块崩溃、bench 局 VOID 疑似崩溃、稳定性排查。
---

# Eden 崩溃取证（WER + PDB 符号化）

工具都在仓库 `tools/prof/`（背景与坑见 PROFILE_PROGRESS §28.15-28.16）。

## 流程

### 1. 抓崩溃事件（含 faulting 模块/偏移）

```bash
powershell -NoProfile -ExecutionPolicy Bypass -File tools/prof/query_crash.ps1
# 或全量字段（含 ExceptionCode / ProcessId / ReportId）：
powershell -NoProfile -Command "Get-WinEvent -FilterHashtable @{LogName='Application'; ProviderName='Application Error'} -MaxEvents 6 | ForEach-Object { \$_.Message.Substring(0,700) }"
```

要点：
- `出错模块名称` + `错误偏移`（hex，无 0x 前缀）是符号化输入；`异常代码` c0000005=访问违例。
- **同偏移重复崩 = 确定性 site**（driver 侧也适用：无符号时偏移相同即同 site）。
- 崩溃时间对回 bench 日志（如 ~启动 30-35s = 游戏加载/shader 段）。
- WER(1001) 事件常是余波报告（P4=aaaa 占位符），真信号看 Application Error(1000)。

### 2. 符号化（eden 自身模块）

```bash
cd tools/prof && cl /O2 /EHsc sym.cpp     # 一次性编译（VS 环境下）
sym.exe <eden.exe 路径> <hexRVA>           # 例：sym.exe build-vs22/bin/eden.exe ebab1c
```

- 输出 `函数名 + 偏移` + `文件:行`（RelWithDebInfo 的 PDB 在 exe 旁）。
- **/OPT:ICF 折叠**：命中的可能是折叠兄弟 lambda——当"函数簇"看，别当字面帧。
- 非 eden 模块（nvoglv64.dll 等）也可传入：通常无符号，但能验证偏移==同 site。

### 3. 判定与记录

- 崩在 eden 代码：对回我们分支的改动面（token/epoch 全在 env 门控后，
  serial 模式不执行——golden(串行)崩而 token 过 ⇒ 优先怀疑环境/驱动而非代码）。
- 崩在驱动/系统模块：跨重启存活 = 系统层问题（记录 + 需用户决策，勿自行修系统）。
- 结论写 PROFILE_PROGRESS 对应章节（含事件时间线、偏移、符号化结果）。

## 已知案例（速查）

- §28.15 async 崩溃：`CommandChunk::Record` vk_scheduler.h:220（ICF 折叠到
  CopyImage lambda）——worker resolve 的纹理运行时与 GPU 线程竞态写当前命令块。
- §28.16 nvoglv64.dll 0xebab1c：golden(串行)环境 eden 启动 ~35s 崩、token 环境过；
  跨重启存活，未定性（系统层，待用户处理）。
