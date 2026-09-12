# 本地优化跨平台复审

仅用于本地实验，不推送上游。功能测试通过不代表帧率提升，也不代表全平台无回退。

后续交叉构建进展见 [wsl-windows-build.md](wsl-windows-build.md)：配套重编 OpenSSL
和 Qt 后 Windows GUI/CLI 均已成功链接。下文的 SDK/链接阻碍保留为前一阶段的调查记录。

## 适用范围与风险

| 方向 | 适用范围 | 跨平台判断 |
| --- | --- | --- |
| RegAlloc 反向表 | x64 dynarmic 后端，含 Windows/Linux x64 | 减少发码时查找，不直接优化稳态游戏机器码；表初始化也有成本。ARM64 后端/NCE 不使用它 |
| 共享 A64 IR | x64 后端的 A64 JIT | 减少重复翻译/优化，但增加锁、代码校验及最多 512 MiB 的缓存 payload；内存开销不止 payload 上限 |
| 寄存器冗余写、LRU 同帧去重、buffer_id 复用 | 通用 GPU 命令/缓存路径 | Android ARM64/NCE 仍可受益，但瓶颈、缓存命中和新增对象字段的成本依设备而异 |
| uniform alignment 缓存 | OpenGL/Vulkan | 对齐值是设备生命周期常量；原 getter 很便宜，不应将收益描述成避免驱动调用 |
| transition hash | Vulkan | 哈希会完整扫描 key，memcmp 可能早退；单候选已改为直接比较，多候选仍需各 CPU 的基准 |
| TBO 初始化、FSP 验证 | 对应通用代码路径 | 正确性修复，不应按 Windows 平台限制；驱动/服务集成验证仍欠缺 |
| JIT 计时统计 | x64 后端诊断 | 原子计数和时钟读取有成本，不是性能优化；关闭后仍有分支及 live-size gauges |

本轮保留 Windows 默认开启共享 IR/活动统计，其他平台改为显式 opt-in。
这是一项保守的本地实验策略，不是测得 Linux 负收益的结论；Windows 低内存设备也不保证正收益。
`EDEN_JIT_IRCACHE=1`、`EDEN_JIT_STATS=1` 分别开启，`=0` 分别关闭。
原环境变量解析语义不变。机器码缓存保持原 tag 的 512 MiB，不能沿用旧 2 GiB 实验描述。
按用户要求，不改 IR 快满时可能重复序列化再丢弃的逻辑。

Debug 反向表对照检查改用 `!NDEBUG`，避免 GCC/Clang Debug 未定义 `_DEBUG` 时漏检。
新增平台默认值/显式开关测试，以及 CTest 的两个独立进程 on/off 配置。

## 验证结果与边界

| 环境/测试 | 结果 |
| --- | --- |
| Linux GCC 13.3 Debug，无手工 `_DEBUG`，IR/stats 显式开启 | 14 cases / 569 assertions 通过 |
| Linux 两开关关闭；另一次均未设置 | 各 13 cases 通过、1 跳过，570 assertions 通过 |
| Linux 原有 A32 ARM/Thumb | 28 cases / 170 assertions 通过 |
| Linux TBO | 1 case / 6 assertions 通过 |
| Linux IR ASan/UBSan，IR 开启 | 6 cases / 409 assertions 通过 |
| LLVM-MinGW Windows x64 PE 测试，Wine 默认配置 | 42 cases / 739 assertions 通过 |
| 同上，两开关关闭 | 41 cases 通过、1 跳过，740 assertions 通过 |
| Windows PE TBO，Wine | 1 case / 6 assertions 通过 |
| Android NDK 29，arm64-v8a/API 28 | dynarmic ARM64 静态库、TBO ELF 编译通过，未在设备运行 |
| 真实 Vulkan Next 头文件调用 TU | Linux GCC、Android ARM64、LLVM-MinGW x64 语法编译通过，未运行 GPU 集成测试 |
| 截图结构 padding 布局 TU | Linux x64、Windows x64、Android ARM64 的大小、偏移、平凡类型及零初始化编译期检查通过 |
| LLVM-MinGW Windows CLI 全部源码 | 编译通过，最终链接因预编译 OpenSSL 的 CRT 符号失败；未生成可交付 exe |

Windows 测试由独立工程链接真实 dynarmic 和目标平台重新编译的 Catch2，只替换宿主 logger。
Wine 结果不是原生 Win11/TOTK 验证。此后按用户要求，构建与代码生成仅使用 WSL 原生工具，
Windows exe 仅为目标产物，不用于生成代码。Android 编译不证明手机功能或性能。
新增 CTest 配置尚未通过完整项目 dynarmic_tests 目标执行；上述矩阵来自独立测试工程。

## WSL 交叉构建归档

依赖、构建和日志均保留在此 worktree 的 gitignored `.local-review/` 与 `.cache/cpm/`，
不依赖 `/tmp` 中的临时库，不修改原 Windows 安装或游戏配置。

- `cross-tests/`：跨平台独立测试工程；`build-llvm-mingw-tests/` 内含 Windows 测试 exe。
- `build-android-tests/`：ARM64 构建；NDK 使用 WSL 的 `/home/carton/Android/SDK/ndk/29.0.14206865`。
- `mingw-toolchain.cmake`：系统 GCC 13 POSIX 线程版交叉编译器，不能误用 win32 线程版。
- `llvm-mingw-toolchain.cmake`、`cross-deps/llvm-mingw-20260908-ucrt-ubuntu-22.04-x86_64/`：Linux LLVM-MinGW。
- `qt-host/`：Linux Qt 6.9.3 工具；Windows Qt 库仍来自 `.cache/cpm/qt6/mingw-amd64-6.9.3/`。
- `windows-prefix/`：从源码交叉构建的 iconv、double-conversion。
- `host-tools/usr/bin/glslangValidator`：WSL 原生 shader 编译器。
- `eden-windows/`：GCC + Windows Qt GUI 构建；`eden-windows-llvm/`：LLVM-MinGW 无 Qt CLI 构建。
- `platform-*.log`、`eden-windows*-build.log`：测试及主程序编译日志。

不能将 Linux 静态库链接进 Windows 程序，也不能混用 GCC/libstdc++、LLVM-MinGW/libc++、
MSVC 的 C++ 依赖。现成 Windows Qt 使用 GCC ABI，故 LLVM 路线先验证不含 Qt 的主体。
CLI 不是 TOTK 基准替代品；必须使用 GUI 进行最终游玩和帧时验证。

Qt 的 `QT_HOST_PATH` 单独设置不足以覆盖此构建已有的 Windows Tools 包缓存。
重新配置时须显式指定 `Qt6CoreTools_DIR`、`Qt6GuiTools_DIR`、`Qt6WidgetsTools_DIR`
到 `.local-review/qt-host/lib/cmake/` 下对应目录。构建前检查 `AutogenInfo.json` 的
`QT_MOC_EXECUTABLE` 以及 uic/rcc 命令，使用 `file` 确认它们是 Linux ELF。
曾错误选择 Windows moc.exe，三个 QuaZip moc 源码出现 E-SafeNet/LOCK 标记；
修正工具路径后已由 Linux moc 重新生成为 ASCII 源码，没有尝试解密或关闭安全软件。

libc++ 编译暴露的缺失标准头文件采用显式 include 修复。sirit 的 `<cstdlib>`
通过 `.patch/sirit/` 和 CPM recipe 保留；已经存在的 CPM 缓存不会自动重打新补丁，
本次缓存已同步并用 `git apply --reverse --check` 验证。

GUI 路线在恢复 Linux moc 后遇到系统 MinGW SDK 兼容性阻碍：Xbyak 7.35.2
需要 `PROCESSOR_RELATIONSHIP::EfficiencyClass` 以及
`CACHE_RELATIONSHIP::GroupMasks/GroupCount`，当前系统头文件缺少这些字段。
没有伪造结构体布局或混用另一套运行库的系统头文件。继续 GUI 路线需要升级兼容的
GCC/MinGW 工具链，或为 LLVM-MinGW 重新编译同 ABI 的 Windows Qt。

LLVM-MinGW CLI 已完成全部源码编译，最终链接停在预编译 `libcrypto.a`：
缺少导入符号 `_vsnprintf`、`_vsnprintf_s`。当前依赖与 UCRT 工具链不能直接完成链接，
下一步应以同一工具链/CRT 从源码构建 OpenSSL，而不是临时混入另一套 CRT 或伪造符号。
本轮没有可交付的 `eden.exe` 或 `eden-cli.exe`，成功的 Windows PE 产物仅为独立测试程序。
所有本轮主构建进程均已结束；复现增量 CLI 构建为：

```bash
cmake --build .local-review/eden-windows-llvm --target yuzu-cmd -j6
```

此命令仍会遇到上述 OpenSSL 链接问题，不能作为已打通的发布构建配方。

新 libc++ 还将 `std::exchange` 标为 nodiscard。呈现线程现在显式丢弃返回的旧锁，
保持其在同一个完整表达式结束时析构；调度线程及纹理初始化的两处同类调用也显式丢弃结果。
不修改锁交接次序或项目警告策略。

补齐截图服务 `<type_traits>` 时发现已有指定初始化器引用 `pad163`/`pad179`，
但这些名字由 padding 宏的 `__LINE__` 生成，新增 include 即会破坏编译。
现将这两个被引用的 padding 改成显式 `std::array` 成员 `padding` 并同步初始化器，
类型、大小及初始化方式不变。`.local-review/caps_layout_test.cpp` 验证其偏移
分别为 `0x10`、`0x50`，三套编译器均通过；不等同于实际截图功能测试。

## 下一步实机验收

分别在 Linux x64、Windows x64 做 IR on/off 的冷启动墙钟、编译 CPU、峰值内存及稳态帧时 A/B；
在 Android ARM64 做 GPU 命令优化的设备温度受控对照，关注内存和长期旋转场景。
所有性能结论需区分启动编译与稳态执行，解锁帧时基准与实际呈现体验也要分开。
本机 RTX 2060 的 ASTC CPU 异步/BC3 配置不能推广到所有 Android GPU：
支持原生 ASTC 的设备不走同样的转换路径。Unsafe CPU 精度也不能作为通用默认修复。
