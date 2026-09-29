# PGO 构建管线（MSVC，local-only）

在 master（VS2022 + Ninja + RelWithDebInfo）工具链上对 eden.exe 做
Profile-Guided Optimization：插桩构建 → TOTK 训练（pgosweep 落盘）→
pgomgr 合并 → /LTCG:PGOPTIMIZE 重链接 → bench_ab 交错对验证。
首轮结果与全部坑见 PROFILE_PROGRESS.md §35；本文只维护流程本身。

## 首轮结论速览（2026-09-29）

- 3/3 对 AB/BA 交错对有效，**B/A 中位 1.0527（+5.3%）**，区间 1.014~1.064；
- p95 帧时长：PGO 臂三对全部稳定在 25.0ms 档，基线臂在 25/33.3ms 档间跳；
  进程退出时间也快约 2s（17.3-18.2s → 15.6-15.9s）；
- 与瓶颈画像一致（uProf：GPU 线程分支误预测仅 0.40% cycles，PGO 分支布局
  红利有限）；官方 nightly 声称的 10-30% 未在本机复现，不建议外推；
- 混杂变量：PGO 臂含 /GL（LTCG 本身可能有贡献），未做 LTCG-only 对照。

## 管线

### 1. 插桩构建目录（build-pgo）

```bash
cd /f/devel/opensource/eden-emulator          # 源码 worktree（master）
source tools/windows/load-msvc-env.sh
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'   # robocopy/参数不被路径转换
export PATH="/g/Tools/glslang/bin:$(dirname "$(command -v cl.exe)"):<VS CMake bin>:<VS Ninja>:$PATH"

cmake.exe -S . -B build-pgo -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DYUZU_TESTS=OFF \
  -DCPM_SOURCE_CACHE=F:/devel/opensource/eden-emulator/.cache/cpm \
  '-DCMAKE_C_FLAGS=/DWIN32 /D_WINDOWS /GL' \
  '-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /EHsc /GL' \
  '-DCMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO=/LTCG:PGINSTRUMENT F:/prof/crtvec/crtvec_shim.obj /DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF'
cmake.exe --build build-pgo --target yuzu      # 产物是 bin/eden.exe（目标名就叫 yuzu）
```

要点：
- **PGO 链接标志必须走每配置变量**（`..._RELWITHDEBINFO`）。只设总的
  `CMAKE_EXE_LINKER_FLAGS` 时 CMake 会追加默认 `/INCREMENTAL`，与
  `/LTCG:PGINSTRUMENT` 冲突（LNK4075/直接失败），且丢掉 `/OPT:REF,ICF`；
- `crtvec_shim.obj` 必须保留（CPM 预编译 Qt 6.11.1 需要它的 `__std_*` 垫片），
  与 PGO 无冲突（已排除嫌疑）；
- portable user 目录从 build-vs22 镜像一份（含着色器缓存与水塘存档）：

```bash
robocopy "F:\...\build-vs22\bin\user" "F:\...\build-pgo\bin\user" /E /R:1 /W:1 /NFL /NDL /NP
```

- 拷运行时到 bin：`pgort140.dll`、`pgodb140.dll`（MSVC 工具集
  `bin/Hostx64/x64/` 下）；`pgosweep.exe` 同目录，见 EDEN_PGOSWEP 缺省。

### 2. 训练（pgo_train.py）

```bash
cd /f/devel/opensource/eden-premerge/tools/prof
EDEN_DIR=F:/devel/opensource/eden-emulator/build-pgo/bin python pgo_train.py
```

时序按插桩版校准（慢 2-5 倍，勿照搬到普通构建）：t=100s boot sweep →
t=150s 起 6×A 键（间隔 12s）→ +120s 读档 → 300s 游戏内（45s 间隔转镜头，
均分 3 次 sweep）→ final sweep → taskkill。全部参数可 `--help`。

**为什么必须 pgosweep**：eden 的退出路径不执行 pgort 的 atexit dump——
干净退出（rc=0）也不产生任何 .pgc；WM_CLOSE 挂死（AGENTS 已知问题 1）时
更没有。pgosweep 是唯一可靠落盘手段，输出 `pgo_*.pgc` 在 bin 下。

### 3. 合并与重链接

```bash
PGOMGR="<MSVC>/bin/Hostx64/x64/pgomgr.exe"
cd build-pgo/bin && "$PGOMGR" /merge pgo_boot.pgc pgo_game1.pgc pgo_game2.pgc \
  pgo_game3.pgc pgo_final.pgc eden.pgd      # 输出应显示 0.0% 计数因溢出放弃

cmake.exe -B build-pgo \
  '-DCMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO=/LTCG:PGOPTIMIZE F:/prof/crtvec/crtvec_shim.obj /DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF'
cmake.exe --build build-pgo --target yuzu   # 只重链接，对象全保留
```

验证：`dumpbin /DEPENDENTS build-pgo/bin/eden.exe` 不再依赖 pgort140.DLL；
裸启动 12s + taskkill 烟测（rc=0）。体积参考：插桩 69MB → 优化后 ~50MB。

### 4. 基准验证（bench_ab 两臂 EDEN_DIR）

```bash
# 公平性：先把 build-pgo/bin/user 从 build-vs22 重新 /MIR（消除训练期着色器漂移）
# 两臂各跑 patch_input.py 打测试键（bench 前置），测完 --restore
cd /f/devel/opensource/eden-premerge/tools/prof
python bench_ab.py --a base --b pgo --pairs 3 --measure 90 \
  --aenv "EDEN_DIR=F:/devel/opensource/eden-emulator/build-vs22/bin" \
  --benv "EDEN_DIR=F:/devel/opensource/eden-emulator/build-pgo/bin"
```

## 坑清单（按杀伤力排序）

1. **退出不落盘 .pgc**（见上，pgosweep 兜底是管线的地基）。
2. **/INCREMENTAL 冲突**：每配置链接标志必须显式 `/INCREMENTAL:NO`。
3. **幻影输入**：GameSir 手柄（VID_3537）固件心跳持续刷新全局输入时钟，
   GetLastInputInfo 恒为"刚有输入"→ bench 全 VOID、按键注入的 LastInput 校验
   必抛。对策：拔手柄；或 `EDEN_IGNORE_INPUT=1`（本套件已内置开关，代价是
   用户干扰检测完全失效，只在无人值守时用）。空闲探针：
   采样两次 `GetLastInputInfo` 间隔 20s，`seconds-since-input` 恒 0.0 即中招。
4. **MSYS 路径转换**：robocopy 的 `/E` 被转成 `E:/`、cmake 的 `/DWIN32` 同理；
   一律 `MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'`（注意：此模式下
   `cmd //c` 双斜杠写法失效）。
5. **bash `$!` 不是 Windows PID**：驱动 GUI 进程用 python `subprocess.Popen`
   （`.pid` 即 Win PID），别用 bash 后台 `$!` 传给 taskkill。
6. **插桩版时序**：所有"到主菜单/读档"等待按正常版 2-5 倍放大，进游戏用
   暴力连点（多按 A 无害），别信单点定时。
7. 训练/基准全程的 zombie 巡检：taskkill 后要用 tasklist 确认真死了，
   WM_CLOSE 挂死的实例继续吃 CPU（曾残留 200MB 僵尸）。

## 产物与数据

| 位置 | 内容 |
|---|---|
| `build-pgo/` | PGO 构建目录（configure 即含全部标志） |
| `build-pgo/bin/eden.exe` | PGO 优化版（现役验证产物） |
| `build-pgo/bin/*.pgc` + `eden.pgd` | 训练 profile（可继续增量 merge） |
| `F:\prof\runs\base-*\, pgo-*` | 首轮 A/B 全部 result.json / frames.csv / shots |

## 后续方向

- **LTCG-only 对照臂**：只 /GL 不喂 profile 的第三臂，分离 LTCG 与 PGO 的
  各自贡献（build-pgo 克隆改链接标志即可，编译产物不通用需全编）；
- 更长/多场景训练（村庄、战斗、其他游戏）——预期边际收益有限（瓶颈画像）；
- 训练 profile 跨源码版本的有效性窗口未测：大改 src 后应重训。
