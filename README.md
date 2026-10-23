# jubeat_quickskip

jubeat Ave. (L44) 的「快速退出」注入插件：
**游玩中同时按住一组可配置的按键（默认 1、4、13、16 四个角落）约 0.3 秒，立即结束本局**：
BGM/音效停止、当前曲目按 FAILED（0 分）结算，然后由游戏自己走原生的「结算 → continue → 选曲」流程，
玩家按 NEXT/面板继续到下一首即可。
因为完全交给游戏原生处理，**选曲画面不会残留**游玩时的曲目信息 / `1ST TUNE` / LEVEL / 小节条等 HUD，
本局剩余的曲数（credit）也会正确保留，可以继续下一首。

A tiny injected DLL for the arcade rhythm game **jubeat Ave. (L44)**: while a tune is being
played, hold a configurable set of panels (default: the four corners 1, 4, 13, 16) for
~0.3 seconds to immediately end the current tune (it settles as FAILED / 0 score) and let
the game run its own native result -> continue -> song-select flow; the player walks the
settlement screens to the next tune.  Because the post-tune flow is fully native, the
credit/tune bookkeeping stays correct and no play HUD is left over the select screen.

- 32 位 DLL，通过 spice2x 自带的 `-k`（Inject DLL Hooks）加载；也附带备用 `injector.exe`。
- 触发按键**可配置**（`Keys=`，面板 1~16 任意组合）。
- 只在「真正游玩中」生效；选曲 / 结算 / 加载 / 倒计时按下不会触发（只作为普通面板输入）。
- 不修改任何游戏文件；版本不符会自动禁用，不会让游戏崩溃。
- MIT License。

---

## 快速开始（Windows）

目录已经包含编译好的文件，最短路径如下：

1. 先启动你的本地服务器（例如 `localhost:8083` 上的那个）。
2. 在 `contents\` 目录下执行（把 `-k` 指向本目录的 DLL）：

   ```bat
   spice.exe -k "jubeat_quickskip\bin\jubeat_quickskip.dll" <你平时的启动参数>
   ```

   或者直接双击本目录的 **`launch_mod.bat`**（`-k` 已写好；你平时的参数可以加在后面）。
3. 进曲目、开始游玩后，按住配置的按键约 0.3 秒 → 回到选曲界面。

> 也可以把 `jubeat_quickskip\bin\jubeat_quickskip.dll` 填进
> `spicecfg.exe` → DLL Hooks → **Inject DLL Hooks**，以后正常启动游戏就会自动加载。

---

## 文件说明

| 路径 | 作用 |
|---|---|
| `bin\jubeat_quickskip.dll` | 插件本体（32 位 DLL，已编译好） |
| `bin\jubeat_quickskip.ini` | 配置（触发按键、按住时长、冷却、开关、日志） |
| `bin\injector.exe` | 备用注入器（只有 `attach_mod.bat` 用到；`-k` 方式不需要） |
| `launch_mod.bat` | 用 `spice.exe -k ...` 启动游戏并加载插件 |
| `attach_mod.bat` | 备用：给**已经在运行**的游戏注入 |
| `src/` | 全部源码 |
| `config/jubeat_quickskip.ini` | 配置模板（`build.ps1` 会复制到 `bin\`） |
| `build.ps1` | 重新编译（输出到 `bin\`） |
| `docs/REVERSE_ENGINEERING.md` | 逆向笔记：FSM 结构、地址/状态、移植方法、实机 p4io 路径 |
| `LICENSE` | MIT License |

> `bin/`、`logs/`、`*.dll`、`*.exe` 都在 `.gitignore` 里：仓库只跟踪源码；编译产物由
> `build.ps1` 生成（本目录里已附带一份可用的编译产物）。

---

## 兼容版本

| 项目 | 值 |
|---|---|
| 游戏 | jubeat Ave. / L44，`modules\jubeat.dll` 时间戳 **`0x6a59f658`**（573 补丁清单里的 2026-07-22 版本） |
| 启动器 | spice2x（32 位 `spice.exe`），已验证 `-k` / `-z` / 外部注入三种加载方式 |

> 换版本（`jubeat.dll` 时间戳变化）后偏移会变，插件会在启动时检测到并**自动禁用**。
> 移植方法见 [docs/REVERSE_ENGINEERING.md](docs/REVERSE_ENGINEERING.md) §7。

---

## 功能

游玩中（`MUSIC_PLAY` 状态机处于 `0x23886000`「正在游玩」）：

> **同时按住 `Keys=` 配置的按键（默认 1、4、13、16）约 0.3 秒**
> → 本局立即结束（BGM/音效停止，按 FAILED/0 分结算）→ 游戏原生结算 → continue → 选曲，
> 玩家按键继续到下一首（`AbortMode=31`，默认）。

- 需要先松开再按才能再次触发；两次触发最小间隔 1.5 秒（可配置）。
- 被中断的曲目按 **FAILED（0 分）** 结算；因为走的是游戏自己的收尾流程，
  本局剩余曲数（credit）、下一首的选择、成绩/次数统计都由游戏原生逻辑处理。
- 中断后可以正常继续选曲、开始下一首。
- 退出后 **不会** 残留游玩时的曲目信息/HUD，也不会吃掉本局剩下的曲数。
- **非游玩状态**（选曲、难度选择、结算、加载/倒计时、标题/演示）按下配置的键**不会触发**，
  只会当作普通面板输入操作游戏；插件不拦截、不改写输入。

---

## 配置 `bin\jubeat_quickskip.ini`

放在 DLL 旁边；全部可选，改完重启游戏（或重新加载插件）生效：

```ini
[quickskip]
Enabled=1
Keys=1,4,13,16
HoldMs=300
CooldownMs=1500
MapField=1
StopSfx=1
AbortMode=31
Verbose=0
```

| 键 | 默认 | 说明 |
|---|---|---|
| `Enabled` | `1` | 总开关；`0` = 只写日志不动作 |
| `Keys` | `1,4,13,16` | **触发组合**：要同时按住的面板编号（1~16），逗号/空格分隔；也可写十六进制位掩码（bit0=1号键 …，如 `0x9009`） |
| `HoldMs` | `300` | 这些键需要同时按住的毫秒数（容易误触调大，想更灵敏调小） |
| `CooldownMs` | `1500` | 两次触发的最小间隔（毫秒） |
| `MapField` | `1` | 读取面板状态用的字段（本版本实测值，一般不改） |
| `StopSfx` | `1` | 是否同时停止音效 |
| `AbortMode` | `31` | 退出方式：`31`=默认（只快速结束当前曲目，之后由游戏原生结算→continue→选曲，玩家按键继续）；`1`=旧版直接切选曲（最快，但会残留游玩 HUD 且会吃掉本局剩余曲数，仅调试用） |
| `Verbose` | `0` | 记录更多状态变化的日志（排查用） |

`Keys` 示例：

| 写法 | 含义 |
|---|---|
| `Keys=1,4,13,16` | 四个角落（默认） |
| `Keys=2,15` | 只按 2 号和 15 号 |
| `Keys=16` | 单个 16 号键 |
| `Keys=0x9009` | 十六进制掩码，等价于 1,4,13,16 |

> 目前只支持**一组**组合（列出的键必须同时按住）。INI 注释符 `;` / `#` 之后的内容会被忽略。

---

## 日志与排查

- 默认日志：`bin\logs\jubeat_quickskip.log`（DLL 目录下的 `logs` 子目录，自动创建，**追加写入**；
  想要干净报告可先删除）。
- 环境变量 `JUBEAT_HOOK_LOG=<绝对路径>` 可指定日志文件（游戏进程会继承）。
- spice2x 的 `contents\log.txt` 里可确认 `-k` 已加载：`I:launcher: loading hook DLL ...`。

正常日志：

```
quickskip: config enabled=1 keys=0x9009 panels=[1,4,13,16] holdMs=300 ...
quickskip: READY - input_update hooked at ... keys=0x9009 panels=[1,4,13,16] holdMs=300
quickskip: FSM MUSIC_LOOP ok: obj=... id=0x23800000 name="MUSIC_LOOP"
quickskip: FSM objects READY (play detection active)
```

触发日志：

```
quickskip: TRIGGER keys held 313 ms mask=0x9009 need=0x9009 panels=[1,4,13,16] loop=0x23880000 play=0x23886000 -> abort tune
quickskip: ABORT mode=31 bgmStopped=1 loopState=0x23880000 playState=0x23886000
quickskip: TUNE(abort-start) count=0 mode=1
quickskip: TransitState(MUSIC_PLAY->song_end, 0x23887000) called=1 ret=...
quickskip: play scene ended (loop=0x23880000), waiting for the game's native MUSIC_RESULT
quickskip: game entered MUSIC_RESULT natively - handing the rest of the settlement flow over to the game
```

常见情况：

| 日志 / 现象 | 含义 |
|---|---|
| `ERROR version mismatch` | 游戏版本与插件不匹配（只支持时间戳 `0x6a59f658`），自动禁用，不影响游戏 |
| `ERROR MUSIC_LOOP object id mismatch` | 同上，版本不符，自动禁用 |
| `ERROR unexpected input_update prologue` | 同一进程里已经加载过本插件（或输入模块被别的插件改过），本次不生效 |
| `log.txt` 里 `failed to load hook` | `-k` 路径不对（路径相对 `contents\`，带空格要加引号） |
| 完全没有日志 | 先看 `contents\log.txt` 是否有 `loading hook DLL ...`；没有就是 `-k` 没生效（可能被 spicecfg 覆盖，可试 `-cmdoverride`） |
| 按住没反应 | 确认是「游玩中」；看日志有没有 `TRIGGER`；可把 `Verbose=1` 后重启再看 |

进阶调试导出（`injector.exe --call`）：

| 导出 | 作用 |
|---|---|
| `QuickSkipStatus` | 把「是否就绪 / 是否在游玩 / FSM 状态」写入日志 |
| `QuickSkipTriggerNow` | 不等按键，下一帧直接尝试中断本局（仅游玩中有效） |

---

## 与 `-exec` 启动参数的兼容性（实测）

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
  spice.exe -exec jubeat.dll -modules modules -io -jb -k "jubeat_quickskip\bin\jubeat_quickskip.dll" <其余参数>
  ```

  该组合下开机自动插卡可能不生效，需要在 ENTRY 画面手动插卡（或通过 spice2x API 插卡）后走正常 PIN 流程。
- 除非有特殊理由（例如排查/硬件环境），建议直接用正常的自动识别启动，`-k` 加载插件即可。

---

## 实机（p4io）环境能用吗？

能。插件不依赖 spice2x 的虚拟输入：面板状态最终来自 gftools 输入状态表，
而实机路径是 `libp4io.dll`（真实 P4IO 板）→ `device.dll` 的 JAMMA 缓冲 → `input.dll`
每帧按固定表生成 16 面板位图，和 PC 上虚拟输入的汇合点完全相同（详见
[docs/REVERSE_ENGINEERING.md](docs/REVERSE_ENGINEERING.md) §7.5）。

实机（`spice.exe -exec jubeat.dll ... -k <dll>`）验证清单：

1. `contents\log.txt` 出现 `I:launcher: loading hook DLL ...`（说明 `-k` 执行了；游戏没成功加载的话这一行不会出现）。
2. 插件日志（默认 `bin\logs\jubeat_quickskip.log`）出现
   `quickskip: READY - input_update hooked`、`FSM MUSIC_LOOP ok`、`FSM objects READY`。
3. 游玩中真实按住配置的按键约 0.3 秒 → 日志出现 `TRIGGER ... panels=[...]` 并回到选曲。
4. 如果第 2 步出现 `ERROR version mismatch`：你的机台游戏版本与 `0x6a59f658` 不同，
   插件会自动禁用（不会改坏游戏），需要重新按 [docs/REVERSE_ENGINEERING.md](docs/REVERSE_ENGINEERING.md)
   §7 的方法定位偏移后重编。

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

## 实现原理（简述）

完整逆向笔记见 [docs/REVERSE_ENGINEERING.md](docs/REVERSE_ENGINEERING.md)。

- 游戏内部使用 gftools 的有限状态机框架。`MUSIC_LOOP`（id `0x23800000`）是场景路由器，
  其状态字节就是当前场景；`MUSIC_PLAY`（id `0x23880000`）状态 `0x23886000` = 正在游玩。
- 面板位图：`input.dll!input_update()` 每帧刷新 gftools 设备状态表，
  `GFInputDDevGetStateMap(2, 1)` 低 16 位，bit i = 第 i+1 号键；默认四角 = `0x9009`。
- 退出本局：`sound_bgm_stop()` + `sound_se_stop_all()`，然后只把 `MUSIC_PLAY` 推进到游戏自己的
  「曲终」状态 `0x23887000`，**之后什么都不做**——结算、continue、回到选曲全部由游戏原生状态机完成。
  这样本局按 FAILED/0 分正常结算、credit/曲数统计正确、共享 AEP UI 也会按原生流程重排，
  不会残留游玩 HUD，也不会吃掉本局剩下的曲数。
  **不要**强行把 `MUSIC_LOOP` 直接切到 `MUSIC_SELECT`，也不要强行跳过结算/continue：
  前者会把游玩 HUD 留在选曲画面上，后者会破坏 credit 统计（游戏会直接结束整次游玩）。
  细节与失败方案记录见 [docs/REVERSE_ENGINEERING.md](docs/REVERSE_ENGINEERING.md) §4。
- 插件 inline hook `input.dll!input_update`，在游戏线程上判断并执行；
  所有游戏调用都有 VEH + `setjmp/longjmp` 保护，出错只跳过并写日志。
- 容错：中断后约 0.6 秒复查 `MUSIC_LOOP` 状态，未离开游玩则自动改走游戏原生的
  `MUSIC_RESULT → 汇合点 → MUSIC_SELECT` 路径。

---

## 目录结构

```
bin/                        编译产物（git 忽略；本目录已附带一份可用产物）
  jubeat_quickskip.dll      插件本体
  jubeat_quickskip.ini      配置
  injector.exe              备用注入器
launch_mod.bat              用 spice.exe -k 启动并加载插件
attach_mod.bat              备用：注入到已运行的游戏
src/feature_quickskip.cpp   插件全部功能逻辑（输入、FSM、容错、配置、日志）
src/dllmain.cpp, worker.cpp DLL 入口与后台线程
src/common/                 日志 / 模块 / 内存保护 / inline hook / IAT hook 等
src/probe/                  探针接口头文件
tools/injector/injector.cpp 备用注入器源码
config/jubeat_quickskip.ini 默认配置模板
build.ps1                   构建脚本（输出到 bin\）
docs/REVERSE_ENGINEERING.md 逆向笔记与版本移植说明
```

---

## 更新记录

- **2026-10**：
  1. 修复「退出后选曲画面上方残留游玩曲目信息/HUD」的问题。
  2. 修复退出后「整次游玩直接结束、无法进入下一首」的问题（credit/曲数被吃掉）。
  最终方案：触发后**只快速结束当前曲目**（把 `MUSIC_PLAY` 推进到游戏自己的曲终状态 `0x23887000`，
  本局按 FAILED/0 分结算），之后完全交给游戏原生的「结算 → continue → 选曲」流程；
  玩家按键继续到下一首。这样 HUD 不残留、credit 正确、下一首可正常开始（`AbortMode=31`，默认）。
  旧的「直接跳选曲」行为仍可用 `AbortMode=1` 复现（仅调试用，会残留 HUD 且会吃掉本局剩余曲数）。
- 更早：可配置触发按键（`Keys=`）、`-k` 加载方式、实机 p4io 输入路径验证、项目合并进本仓库。

---

## 免责声明 / Disclaimer

- 仅供个人学习、离线/自建服务器环境下的练习与研究使用；请勿用于商业用途。
- 本项目不含任何游戏本体文件、资源或密钥，只包含自行编写的代码与逆向得到的地址/状态常量。
- 使用第三方服务器或修改版启动器可能违反游戏服务条款，后果自负。
- 作者不对使用本软件造成的任何损失负责。

---

## License

MIT License，见 [LICENSE](LICENSE)。
