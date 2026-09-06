# Eden 本地开发笔记（Windows，仅本地使用）

## 项目简介

Eden 是开源（GPLv3）Nintendo Switch 模拟器，yuzu 血统的社区延续项目。
C++20 + CMake（≥3.31）构建，支持 Windows / Linux / macOS / Android 等平台。
本仓库：`https://git.eden-emu.dev/eden-emu/eden`（GitHub 镜像：`https://github.com/eden-emulator/mirror`）。
构建文档见 `docs/Build.md`、依赖说明见 `docs/Deps.md`。

## 本机构建环境（2026-09-06 验证通过）

- Windows 11 x64，12 逻辑核
- Visual Studio Community **2026**（18.9）：MSVC 14.51（cl 19.51）
  - CMake 4.3.1 / Ninja 均为 VS 自带，**不在系统 PATH**，路径：
    - `C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin`
    - `C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja`
- Git for Windows（Git Bash）+ Python 3.12（CPM 下载依赖需要）
- **glslang 16.5.0 独立版** @ `G:\Tools\glslang`（提供 `glslangValidator.exe`，构建期编译 shader 用。
  这是完整 Vulkan SDK 唯一被构建用到的组件；LunarG 官方安装器静默安装需要 UAC 提权，故未使用）
- 其余依赖（Qt 6.11.1 静态版、SDL3、FFmpeg、OpenSSL、Boost 等）由 CPM 在配置期自动下载到
  `.cache/cpm`（约 1 GB），无需手动安装；下载走 http_proxy 环境变量

## 快速编译（Git Bash）

```bash
cd /f/devel/opensource/eden-emulator

# 1. 加载 MSVC 环境并把 CMake/Ninja/glslang/MSVC-bin 加进 PATH
source tools/windows/load-msvc-env.sh
export PATH="/g/Tools/glslang/bin:$(dirname "$(command -v cl.exe)"):/c/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin:/c/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja:$PATH"

# 2. 配置（仅首次；RelWithDebInfo = /O2 /Ob1 /Zi，性能版带调试符号，profile 用）
cmake.exe -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DYUZU_TESTS=OFF \
  "-DCMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO=/DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF"

# 3. 编译（首次全量约 13 分钟，之后增量很快）
cmake.exe --build build
```

注意：`/usr/bin/link.exe` 会遮挡 MSVC 链接器，务必保持 MSVC 的 bin 目录在 PATH 前面（上面的 export 已处理）。

## 产物

| 文件 | 说明 |
|---|---|
| `build/bin/eden.exe` (+ `.pdb`) | Qt GUI 主程序（静态 Qt，无外部 DLL 依赖） |
| `build/bin/eden-cli.exe` (+ `.pdb`) | 命令行版，跑游戏做 profile 时可避开 GUI 干扰 |
| `build/bin/eden-room.exe` | 独立联机房间工具（LDN 多人），单机 profile 用不到 |

## 运行说明

- exe 旁边放 `user/` 目录即进入 portable 模式；否则使用 `%APPDATA%\eden`
- 密钥/固件/游戏路径均在数据目录配置；可从现有官方安装复制 `user` 目录（保持原安装不动）
- Vulkan / OpenGL 由运行时从显卡驱动加载，exe 无需随附任何 DLL

## 纪律

- 本笔记及一切 AI 生成内容仅限本地使用，**不要**以任何形式提交到上游仓库、issue、PR 或社区
