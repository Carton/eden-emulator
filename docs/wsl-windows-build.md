# WSL 原生工具交叉构建 Windows Eden

仅用于本地实验，不是发布构建或原生 Windows 游玩验收。所有依赖、临时工程和产物
保存在 worktree 的 gitignored `.local-review/`、`.cache/cpm/`；不调用 Windows host
程序进行配置、代码生成或编译，不修改原 Windows 安装。

## 官方资料与参考项目

- [OpenSSL Windows 构建说明](https://github.com/openssl/openssl/blob/master/NOTES-WINDOWS.md)
  明确支持从 Linux 使用 `mingw64` 目标交叉构建。其 WSL hosted 小节描述的是 Linux
  程序，不能把该小节直接当作 Windows 目标配方。
- [OpenSSL 交叉构建的 CMake 查找问题](https://github.com/openssl/openssl/issues/21428)
  有同类依赖发现问题，但不是本次两个 CRT 导入符号错误的完全相同复现。
- [MinGW-w64 CRT 兼容说明](https://github.com/mingw-w64/mingw-w64/blob/master/mingw-w64-doc/howto-build/ucrt-vs-msvcrt.txt)
  建议切换 CRT 时重编静态依赖，而不是默认它们可混用。
- [Qt 交叉构建说明](https://doc.qt.io/qt-6/cross-compiling-qt.html)
  要求宿主和目标使用同版本 Qt；moc/uic/rcc 等由宿主构建提供。

只读参考 `/home/carton/work/projects/remote-link-host`：
`cmake/llvm-mingw-toolchain.cmake` 使用 Linux LLVM-MinGW 驱动配合 MSYS2 UCRT
GCC 16.2.0 的 libstdc++ 头文件、静态库；不运行 MSYS2 包中的 Windows GCC。
`scripts/ensure_msys2_ucrt_gcc.sh` 固定包版本 `16.2.0-3` 及 SHA256。
本次将其已缓存的 UCRT 依赖复制到 Eden 内，原项目未修改。

该方案避免默认 libc++ 与 GCC C++ ABI 的差异，但不是任意现成库均兼容的保证。
最小 Qt Core 工程采用此组合后仍遇到 `swprintf_s` 未定义，来自现成
`libQt6Core.a(qsystemerror.cpp.obj)`。未通过伪造符号或混入另一套 CRT 绕过。
实验保存在 `.local-review/qt-ucrt-smoke*`，其工具链为
`.local-review/llvm-ucrt-libstdcxx-toolchain.cmake`。

因此，只升级旧系统 MinGW SDK 能解决 Xbyak 缺字段的问题，但不能保证同时解决
C++ ABI 和 CRT 静态依赖兼容问题。最终推进路线统一使用 LLVM-MinGW/libc++/UCRT，
从源码构建匹配的 OpenSSL 和 Windows Qt。

## OpenSSL

源码：[OpenSSL 3.6.0 官方发布](https://github.com/openssl/openssl/releases/tag/openssl-3.6.0)。
源码包与官方 SHA256 校验一致：
`b6a5f44b7eb69e3fa35dbf15524405b44837a481d43d81daddde3ff21fcbb8e9`。

源码目录 `.local-review/cross-deps/openssl-3.6.0`，安装目录
`.local-review/openssl-ucrt`。使用 `perl Configure mingw64 no-shared no-tests`，
显式指定当前 LLVM-MinGW 的 clang、llvm-ar、llvm-ranlib、windres，再运行
`make -j6 build_libs` 和 `make install_dev`。没有构建或运行 Windows OpenSSL 命令行工具。
配置、构建、安装日志为 `.local-review/openssl-ucrt-*.log`。

Eden 使用 `YUZU_USE_BUNDLED_OPENSSL=OFF`，匹配的 `OPENSSL_INCLUDE_DIR`，以及
`LIB_EAY`/`SSL_EAY` 指向新静态库。本机 CMake 3.28 的 MinGW 查找分支会从这两个变量
设置 `OPENSSL_CRYPTO_LIBRARY`/`OPENSSL_SSL_LIBRARY`，只传后两项不足以覆盖查找。
旧预编译库的 `_vsnprintf`、`_vsnprintf_s` 导入符号问题由统一 CRT 的库替换处理。

## Qt

源码来自 [Qt 6.9.3 官方归档](https://download.qt.io/archive/qt/6.9/6.9.3/submodules/)：
Qt Base、SVG、Charts。宿主工具为 `.local-review/qt-host` 的 Linux Qt 6.9.3。
目标安装前缀 `.local-review/qt-ucrt-libcxx`，使用
`.local-review/llvm-mingw-toolchain.cmake`，Release、静态库、关闭 tests/examples。
Qt Base 的 OpenSSL 为 linked 模式，指向上述 UCRT 库。

Base/SVG/Charts 均已完成构建安装。SVG/Charts 使用目标 Qt 安装生成的
`lib/cmake/Qt6/qt.toolchain.cmake`；它继续调用原 LLVM-MinGW 工具链，并添加正确
搜索路径。该文件解释了 CMake 的 root/prefix 相等时重复拼接问题：根路径应为
Qt 安装前缀，而 `CMAKE_PREFIX_PATH` 应指向其 `lib/cmake` 子目录。

Eden 设置 `ENABLE_QT=ON`、`YUZU_USE_BUNDLED_QT=OFF`，`Qt6_DIR` 指向新前缀；
Core/Gui/Widgets Tools 包显式指向 Linux host Qt。保持 `YUZU_STATIC_BUILD=OFF`：
该全局选项会强制开启旧的 bundled OpenSSL；Qt 本身仍为显式指定的静态库。

构建前核对 `AutogenInfo.json` 中的 moc 路径，并用 `file` 确认工具为 Linux ELF。
Windows 目标目录中的 `.exe` 不用于生成代码。当前使用 WSL GNU Make；Qt 官方推荐
Ninja，配置对此有警告，但允许 GNU Make。WinRT 支持缺失的配置警告仍需注意。

## 验证边界

Windows GUI 和 CLI 均已实际完成链接，位于 `.local-review/eden-windows-llvm/bin/`：
`eden.exe` 约 57 MiB、`eden-cli.exe` 约 31 MiB；`file` 均确认为 PE32+ x86-64。
`llvm-readobj --coff-imports` 检查后补齐同工具链的
`libc++.dll`、`libunwind.dll`、`libwinpthread-1.dll`；其余导入为 Windows 系统/UCRT DLL。
本轮没有启动这些 exe。OpenSSL 的原两个 CRT 符号错误在替换配套库后消失。

GUI 首次链接后曾因 `WINDEPLOYQT_EXECUTABLE-NOTFOUND` 的 POST_BUILD 步骤失败，
Make 随后删除该次 exe。已修正源码 CMake：以 `Qt6::Core` 实际库类型识别静态 Qt，
不要求 Qt DLL 部署；共享 Qt 的交叉构建明确提示单独部署，不调用目标平台工具；
原生共享 Qt 保持原部署步骤，缺工具则在配置时报错。
`.local-review/deploy-policy-test/` 从真实 CMake 文件提取该分支进行配置测试：
静态/交叉、共享/交叉均无部署命令，共享/原生且有工具保留命令，缺工具按预期失败。
这些是模拟分支配置测试，不是原生 Windows 部署测试。

当前配置为 Release；关闭翻译、更新检查、Web service、Discord presence、联机房间、
Cubeb、libusb、Qt Multimedia/WebEngine。它不是功能配置完全等同于官方包的发布版本。
特别是没有中文翻译构建，且部分可选设备/音频后端未启用。

从 worktree 根目录进行增量构建：

```bash
cmake -S . -B .local-review/eden-windows-llvm
cmake --build .local-review/eden-windows-llvm --target yuzu yuzu-cmd -j6
```

复制使用时保留 exe 同目录的三个运行时 DLL。不要覆盖原可玩版本或原游戏配置。
本目录没有复制密钥、固件、存档或原 `user/` 目录。

编译/链接成功只能证明工具链和依赖在此配置下能构建，不能证明原生 Win11 的
驱动、输入、渲染、HTTPS、截图或游戏兼容性。TOTK 仍必须使用 GUI，不使用 CLI。
本轮不会自动运行 Windows host 程序或修改游戏配置。
