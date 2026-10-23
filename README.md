# jubeat_quickskip

32 位 DLL。用 **i686** MinGW-w64 g++（GCC 8+；已验证 i686-ucrt-posix-dwarf 16.2.0）构建。
产物必须是 PE32 / i386。

## Windows

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Gxx "C:\mingw32\bin\i686-w64-mingw32-g++.exe"
```

选项：`-Clean`（先删除 `bin\`）、`-NoInjector`。
输出：`bin\jubeat_quickskip.dll`、`bin\injector.exe`；`bin\jubeat_quickskip.ini` 缺失时从 `config\` 复制。
脚本会校验 PE 的 i386/PE32 标识。

## Linux

```bash
i686-w64-mingw32-g++ -std=c++17 -O2 -static -static-libgcc -static-libstdc++ \
  -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0601 -DNOMINMAX -DHARNESS_PROBE \
  -Isrc -Isrc/common -Isrc/probe -shared -o bin/jubeat_quickskip.dll -Wl,--kill-at \
  src/common/log.cpp src/common/modules.cpp src/common/pattern.cpp \
  src/common/hook.cpp src/common/iat.cpp src/common/hotkey.cpp \
  src/dllmain.cpp src/worker.cpp src/feature_quickskip.cpp
```
