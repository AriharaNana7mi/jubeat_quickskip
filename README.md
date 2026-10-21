# jubeat_quickskip

jubeat Ave. (L44) 的「四角快速退出」注入插件：
**游玩中同时按住 1、4、13、16（四个角落）约 0.3 秒，立即中断本局并回到选曲界面**（不经过结算）。

A tiny injected DLL for the arcade rhythm game **jubeat Ave. (L44)**: while a tune is being
played, hold the four corner panels (1, 4, 13, 16) for ~0.3 seconds to abort the tune and
jump straight back to the song select screen.

- 32-bit DLL，通过 spice2x 自带的 `-k`（Inject DLL Hooks）加载，或用附带的 `injector.exe` 注入。
- 只在「真正游玩中」生效；选曲/结算/加载/倒计时按四角不会触发。
- 不修改任何游戏文件；版本不符会自动禁用，不会让游戏崩溃。
- MIT License。

---

## 兼容版本

| 项目 | 值 |
|---|---|
| 游戏 | jubeat Ave. / L44，`modules\jubeat.dll` 时间戳 **`0x6a59f658`**（573 补丁清单里的 2026-07-22 版本） |
| 启动器 | spice2x（32 位 `spice.exe`），已验证 `-k` / `-z` / 外部注入三种加载方式 |

> 换版本（`jubeat.dll` 时间戳变化）后偏移会变，插件会在启动时检测到并**自动禁用**。
> 移植方法见 [docs/REVERSE_ENGINEERING.md](docs/REVERSE_ENGINEERING.md)。

---

## 功能

游玩中（`MUSIC_PLAY` 状态机处于 `0x23886000`「正在游玩」）：

> **同时按住 1、4、13、16 号键约 0.3 秒** → BGM/音效停止 → 回到选曲界面（实测约 0.6 秒）。

- 需要先松开四角才能再次触发；两次触发最小间隔 1.5 秒（可配置）。
- 被中断的曲目不记录成绩、不上传结算数据（练习用定位）。
- 中断后可以正常继续选曲、开始下一首。

---

## 构建

需要 **32 位 MinGW-w64 g++**（GCC 8+；例如 winlibs 的 i686 版本，或 Linux 上的
`g++-mingw-w64-i686`，可执行文件名 `i686-w64-mingw32-g++`）。

```powershell
# Windows PowerShell（自动找 PATH 里的 i686-w64-mingw32-g++）
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1

# 或者显式指定编译器
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Gxx "C:\mingw32\bin\i686-w64-mingw32-g++.exe"
```

输出（已被 `.gitignore` 忽略）：

```
bin\jubeat_quickskip.dll    插件本体
bin\injector.exe            可选的注入器（只有不能使用 -k 时才需要）
bin\jubeat_quickskip.ini    默认配置（从 config\ 复制）
```

Linux 上直接交叉编译的命令：

```bash
i686-w64-mingw32-g++ -std=c++17 -O2 -static -static-libgcc -static-libstdc++ \
  -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0601 -DNOMINMAX -DHARNESS_PROBE \
  -Isrc -Isrc/common -Isrc/probe -shared -o bin/jubeat_quickskip.dll -Wl,--kill-at \
  src/common/log.cpp src/common/modules.cpp src/common/pattern.cpp \
  src/common/hook.cpp src/common/iat.cpp src/common/hotkey.cpp \
  src/dllmain.cpp src/worker.cpp src/feature_quickskip.cpp
```

---

## 安装 / 使用

### 方式 1（推荐）：spice2x 的 `-k`

`-k` 是 spice2x 自带的「Inject DLL Hooks」（Launcher → DLL Hooks）参数：
启动时对指定 DLL 做 `LoadLibrary`。在你平时的启动命令后面加一项即可：

```bat
spice.exe -w -autofps -url http://localhost:8083/ -k "path\to\jubeat_quickskip.dll"
```

- 路径相对**游戏工作目录**（`contents\`），也可以写绝对路径；带空格要加引号。
- `-k` 可写多个；`-z`（Inject Early DLL Hooks）也可以，本 DLL 同样支持。
- 也可以写进 `spicecfg.exe` → DLL Hooks → **Inject DLL Hooks**，以后正常启动就会自动加载。

### 方式 2：injector 注入

游戏已经开着、不想重开时：

```bat
injector.exe --dll path\to\jubeat_quickskip.dll -p <spice.exe 的 PID>
```

`injector.exe` 也可以直接启动并注入：`injector.exe --dll <dll> -- <spice.exe 及参数>`。

> ⚠️ **同一个游戏进程只能加载一次插件**：不要 `-k` 和 injector 混用，也不要重复注入
> （第二次会失败并在日志里写 `ERROR unexpected input_update prologue`，不影响游戏本身）。

---

### 与 `-exec` 启动参数的兼容性（实测）

spice2x 的 `-exec <名字>` 用于手动指定游戏 DLL，但它会**跳过游戏自动识别**：
不会自动把模块目录切到 `modules\`，也不会自动打开 `-io`（IO 模拟）和 `-jb`（jubeat 模块）。
而且 `-k` 的 DLL 加载发生在 avs 核心/游戏 DLL 加载**之后**，前面一旦致命错误退出，`-k` 根本不会执行。

本机实测矩阵（`jubeat.dll` 时间戳 `0x6a59f658`）：

| 启动参数 | 游戏能启动？ | `-k` 的插件生效？ |
|---|---|---|
| 正常启动（不加 `-exec`，自动识别） | ✅ | ✅ |
| `-exec jubeat.exe` | ❌ 找不到 `avs2-core.dll`（模块目录没切到 `modules\`），进程提前退出 | ❌ `-k` 未执行，插件日志都不会生成 |
| `-exec jubeat.dll` | ❌ 同样缺少 `-modules modules` | ❌ |
| `-exec jubeat.dll -modules modules` | ⚠️ 能加载，但黑屏/无虚拟输入（缺 `-io -jb`） | ⚠️ 插件加载并 READY，但读不到面板（一直 0）→ 不会触发 |
| `-exec jubeat.dll -modules modules -io -jb` | ✅（需手动插卡，见下） | ✅ 实测完整流程可用（+约 0.6 秒回选曲） |

结论：

- **不要用 `-exec jubeat.exe`**：L44 的游戏主体是 `modules\jubeat.dll`，不是 exe。
- 确实需要 `-exec` 时，正确写法是：

  ```bat
  spice.exe -exec jubeat.dll -modules modules -io -jb -k "mod_quickskip\bin\jubeat_quickskip.dll" <其余参数>
  ```

  该组合下开机自动插卡可能不生效，需要在 ENTRY 画面手动插卡（或通过 spice2x API 插卡）后走正常 PIN 流程。
- 除非有特殊理由（例如排查/硬件环境），建议直接用正常的自动识别启动，`-k` 加载插件即可。

---

## 配置

`jubeat_quickskip.ini`（放在 DLL 旁边；全部可选）：

| 键 | 默认 | 说明 |
|---|---|---|
| `Enabled` | `1` | 总开关；`0` = 只写日志不动作 |
| `HoldMs` | `300` | 四角需要同时按住的毫秒数 |
| `CooldownMs` | `1500` | 两次触发的最小间隔（毫秒） |
| `MapField` | `1` | 读取面板状态用的字段（本版本实测值，一般不改） |
| `StopSfx` | `1` | 是否同时停止音效 |
| `Verbose` | `0` | 记录更多状态变化的日志 |

---

## 日志

- 默认：`<DLL 目录>\logs\jubeat_quickskip.log`（追加写入，自动创建）。
- 环境变量 `JUBEAT_HOOK_LOG=<绝对路径>` 可指定日志文件。
- spice2x 的 `log.txt` 里可确认加载：`I:launcher: loading hook DLL ...`。

正常日志：

```
quickskip: jubeat.dll base=... timestamp=0x6A59F658 expected=0x6A59F658
quickskip: READY - input_update hooked at ...
quickskip: FSM MUSIC_LOOP ok: obj=... id=0x23800000 name="MUSIC_LOOP"
quickskip: FSM objects READY (play detection active)
```

触发日志：

```
quickskip: TRIGGER corners held 313 ms mask=0x9009 panels=[1,4,13,16] loop=0x23880000 play=0x23886000 -> abort tune
quickskip: ABORT requested (bgmStopped=1 transitDispatched=1)
quickskip: abort verify OK - MUSIC_LOOP state now 0x23840000 (left MUSIC_PLAY)
```

进阶调试导出（`injector.exe --call`）：

| 导出 | 作用 |
|---|---|
| `QuickSkipStatus` | 把「是否就绪 / 是否在游玩 / FSM 状态」写入日志 |
| `QuickSkipTriggerNow` | 不等按键，下一帧直接尝试中断本局（仅游玩中有效） |

---

## 实现原理（简述）

完整逆向笔记见 [docs/REVERSE_ENGINEERING.md](docs/REVERSE_ENGINEERING.md)。

- 游戏内部使用 gftools 的有限状态机框架。`MUSIC_LOOP`（id `0x23800000`）是场景路由器，
  其状态字节就是当前场景；`MUSIC_PLAY`（id `0x23880000`）状态 `0x23886000` = 正在游玩。
- 面板位图：`input.dll!input_update()` 每帧刷新 gftools 设备状态表，
  `GFInputDDevGetStateMap(2, 1)` 低 16 位，bit i = 第 i+1 号键；四角 = `0x9009`。
- 退出本局：`sound_bgm_stop()` + `sound_se_stop_all()`，然后
  `GFFSMObjectTransitState(MUSIC_LOOP, 0x23840000)` 切到 `MUSIC_SELECT`。
  这是游戏自己「游玩→结算」转移所用的同一条路径，路由器会自己完成子场景的收尾与选曲场景的附着。
- 插件 inline hook `input.dll!input_update`，在游戏线程上判断并执行；
  所有游戏调用都有 VEH + `setjmp/longjmp` 保护，出错只跳过并写日志。

---

## 目录结构

```
src/feature_quickskip.cpp   插件全部功能逻辑（输入、FSM、容错、配置、日志）
src/dllmain.cpp, worker.cpp DLL 入口与后台线程
src/common/                 日志 / 模块 / 内存保护 / inline hook / IAT hook 等
tools/injector/injector.cpp 可选注入器（启动注入 / 附加到 PID / --call）
config/jubeat_quickskip.ini 默认配置模板
build.ps1                   构建脚本（输出到 bin\）
docs/REVERSE_ENGINEERING.md 逆向笔记与版本移植说明
```

---

## 免责声明 / Disclaimer

- 仅供个人学习、离线/自建服务器环境下的练习与研究使用；请勿用于商业用途。
- 本项目不含任何游戏本体文件、资源或密钥，只包含自行编写的代码与逆向得到的地址/状态常量。
- 使用第三方服务器或修改版启动器可能违反游戏服务条款，后果自负。
- 作者不对使用本软件造成的任何损失负责。

---

## License

MIT License，见 [LICENSE](LICENSE)。
