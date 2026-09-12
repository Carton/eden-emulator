# 本地性能补丁复审验证（2026-09-12）

仅用于本地实验，不是上游修复验收或性能收益声明。

后续跨平台复审见 [local-review-portability.md](local-review-portability.md)。非 Windows
现在默认关闭共享 IR 和活动统计；下面复现缓存开启测试时须显式设置环境变量。

## 本轮变更

- TBO：默认初始化 `TextureBufferBinding::format`，修复首次空绑定比较读取未初始化枚举的问题。测试在非零填充内存上默认构造，并在有效绑定销毁后重建。
- RegAlloc：直接测试 name 0、1、4095、4096、65536，39 个同时存活的 spill，以及占用寄存器交换时的 alias 重跟踪。Debug 构建同时运行原线性扫描对照断言。这些测试检查分配器记账，不执行测试生成的 spill 机器码。
- IR：round-trip 扩展到 96 条指令，跨多个池；新增多块条件分支、内存读写、FP、SIMD、AES 执行结果检查。首次编译和跨 JIT 复用均与预期值比较，另用独立进程关闭缓存重跑。
- 反序列化契约：明确仅接受本进程生成的可信 IR，不保证验证任意磁盘或外部数据。
- 诊断：启动进程前设 `EDEN_JIT_STATS=0` 可关闭 JIT 活动计数及阶段时钟读取；仍保留运行时条件分支、IR live-size gauges 和独立的 block dump。不要将其称为完全无插桩构建。
- 按用户要求，**不改动 IR 容量临界时可能重复序列化再丢弃的行为**。

## 已执行的验证

WSL/Linux x86-64，GCC 13.3，Debug + `_DEBUG`。独立工程链接真实 dynarmic；只替代宿主日志实现，不替代翻译、优化、发码或缓存逻辑。未构建完整 Eden GUI。

| 测试 | 结果 |
| --- | --- |
| JIT/IR/RegAlloc，基线 ISA 构建 | 13 cases / 567 assertions 通过 |
| 同上，`DYNARMIC_ENABLE_CPU_FEATURE_DETECTION=ON` | 13 cases / 567 assertions 通过 |
| IR cache 关闭，两种构建分别运行执行测试 | 4 cases / 30 assertions 通过；1 个只验证 cache hit 的 case 跳过 |
| JIT stats 关闭，两种构建分别运行 | 13 cases / 573 assertions 通过，包含计数器与计时保持零的检查 |
| 原有 A32 ARM/Thumb 指令测试，两种构建分别运行 | 28 cases / 170 assertions 通过 |
| TBO 默认构造测试，两种构建分别运行 | 1 case / 6 assertions 通过 |
| IR cache ASan + UBSan | 6 cases / 409 assertions 通过 |

宿主特性检测只覆盖当前 CPU 支持的路径，不代表所有 CPU/ISA 组合。Sanitizer 检查覆盖 IR 缓存实现，不等于 JIT 机器码执行的 sanitizer 覆盖。

## 可复用的本地工程

依赖、临时工程、二进制和日志存放在被 `.gitignore` 排除的 `.local-review/`：

- `CMakeLists.txt`：使用相对路径定位此 worktree 和内部依赖。
- `boost/`、`xbyak-7.35.2/`、`host-deps/`：当前测试所用头文件和 Catch2 静态库。另保存下载包及未采用的旧 Xbyak。
- `build-local/`：迁移后重新配置的构建。
- `build/`、`build-features/`：原 `/tmp` 构建归档，CMake cache 中仍有旧绝对路径，**不要直接增量构建它们**。
- `jit-dump-regression/`：之前的 block dump 验证产物。

从 worktree 根目录运行（需要 Linux CMake、GCC 和 Make；归档的静态库不是跨平台工具链）：

```bash
cmake -S .local-review -B .local-review/build-local \
  -DCMAKE_BUILD_TYPE=Debug -DDYNARMIC_ENABLE_CPU_FEATURE_DETECTION=ON
cmake --build .local-review/build-local -j 4
EDEN_JIT_IRCACHE=1 EDEN_JIT_STATS=1 .local-review/build-local/review_tests
EDEN_JIT_IRCACHE=0 .local-review/build-local/review_tests '[ir-cache-execution]'
EDEN_JIT_IRCACHE=1 EDEN_JIT_STATS=0 .local-review/build-local/review_tests
.local-review/build-local/a32_review_tests
.local-review/build-local/buffer_binding_tests
EDEN_JIT_IRCACHE=1 .local-review/build-local/ir_cache_sanitized
```

LeakSanitizer 在限制 ptrace 的沙箱中可能报运行环境错误；本轮是在允许其运行的环境中验证通过。此目录整体不随 Git 分享，重新克隆不会自动获得依赖或临时工程。

## 仍需实机/集成验证

- Windows/MSVC 完整 GUI 构建和 TOTK 游玩、旋转及退出/重启回归。
- 完整 BufferCache 的有效→空→有效绑定、跨 channel 删除/槽位重用；当前类型级测试不覆盖驱动和 channel 生命周期。
- FSP 两个打开接口的各 SaveDataSpaceId、读写/大小和重启测试；当前没有构建服务级 fixture。
- 更多 FPCR、异常/fastmem fault、SIMD 和宿主 ISA 组合；不能将有限的执行用例当作完整差分证明。
- 同一 Windows 配置、同一场景的 IR cache on/off 冷启动墙钟、完整编译时间、内存和解锁帧时 A/B；精确校验后的版本不能沿用旧版本收益数字。
- `EDEN_JIT_STATS=0/1` 的插桩开销 A/B；同时关闭 `EDEN_JIT_BLOCKDUMP` 并保持其他探针/日志设置一致。新开关只是验证手段，本轮未声称测得帧率提升。
