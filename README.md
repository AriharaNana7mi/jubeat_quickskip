# jubeat_quickskip — 构建说明

本 README 只说明如何从源码构建 `jubeat_quickskip.dll`（32 位）。

## 依赖

- **32 位（i686）MinGW-w64 g++**，GCC 8 或更新；可执行文件名通常是
  `i686-w64-mingw32-g++`。
  - Windows：winlibs 的 i686 版本（解压后把 `bin` 加进 PATH），或任意 i686 MinGW-w64 工具链。
  - Linux：`sudo apt install g++-mingw-w64-i686`。
- 必须是 **i686（32 位）** 工具链；64 位 MinGW 编译出来的 DLL 游戏无法加载。
- 本仓库验证过的工具链：MinGW-W64 i686-ucrt-posix-dwarf，GCC 16.2.0。

## Windows

在 `jubeat_quickskip` 目录下执行（自动在 PATH 里查找 i686 版 `i686-w64-mingw32-g++`）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
```

指定编译器路径：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Gxx "C:\mingw32\bin\i686-w64-mingw32-g++.exe"
```

清理后重新构建 / 跳过可选的注入器：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Clean
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -NoInjector
```

> 注意：`-Clean` 会删除整个 `bin\` 目录（包括已有的编译产物和日志）。

构建输出：

```
bin\jubeat_quickskip.dll    插件本体
bin\injector.exe            可选的注入器（找不到 32 位工具链时可用 -NoInjector 跳过）
bin\jubeat_quickskip.ini    默认配置（bin 里没有时从 config\ 复制）
```

脚本最后会校验生成的 PE：`machine = 0x14c (i386)`、`magic = 0x10b (PE32)`，
不匹配会直接报错。

## Linux 交叉编译

```bash
i686-w64-mingw32-g++ -std=c++17 -O2 \
  -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0601 -DNOMINMAX -DHARNESS_PROBE \
  -static -static-libgcc -static-libstdc++ \
  -Isrc -Isrc/common -Isrc/probe \
  -shared -o bin/jubeat_quickskip.dll -Wl,--kill-at \
  src/common/log.cpp src/common/modules.cpp src/common/pattern.cpp \
  src/common/hook.cpp src/common/iat.cpp src/common/hotkey.cpp \
  src/dllmain.cpp src/worker.cpp src/feature_quickskip.cpp
```

## 关键编译参数

| 参数 | 作用 |
|---|---|
| `-std=c++17 -O2` | 语言标准 / 优化 |
| `-static -static-libgcc -static-libstdc++` | 静态链接运行库，产物不依赖 MinGW DLL |
| `-DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0601 -DNOMINMAX` | Windows 头文件配置 |
| `-DHARNESS_PROBE` | 使用仓库内的探针/日志接口 |
| `-Isrc -Isrc/common -Isrc/probe` | 头文件搜索路径 |
| `-shared -Wl,--kill-at` | 生成 DLL；导出名不做 `@` 修饰 |
| 目标 | 必须是 32 位 PE（i386 / PE32） |

## 目录

```
src/                   插件源码（feature_quickskip.cpp 为功能主体）
src/common/            日志、模块、内存保护、inline hook、IAT hook 等
src/probe/             探针接口头文件
tools/injector/        可选注入器源码
config/                默认 INI 模板
build.ps1              Windows 构建脚本
bin/                   构建输出（已 gitignore）
```

## 常见问题

| 现象 | 原因 / 处理 |
|---|---|
| `32-bit MinGW-w64 g++ not found` | PATH 里没有 i686 版 g++：安装 i686 工具链，或用 `-Gxx` 指定。 |
| `unexpected PE (machine=0x8664)` | 用了 64 位编译器，换 i686 版。 |
| 链接时报 `Permission denied` | `bin\jubeat_quickskip.dll` 正被游戏/其它进程占用：先关掉游戏再编译。 |
| 想确认产物是 32 位 | `dumpbin /headers bin\jubeat_quickskip.dll` 查看 machine 为 `14C`，或用任意 PE 工具看 PE32。 |
