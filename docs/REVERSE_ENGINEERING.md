# jubeat_quickskip 逆向笔记

本文记录实现本插件时逆向得到的结论，方便在新版本上重新定位、移植或二次开发。
所有地址都是 **RVA**（相对模块基址；运行时用 `GetModuleHandleA("jubeat.dll") + RVA`），
不是文件偏移（文件偏移 → RVA 的换算：`rva = fileoff - section.RawPointer + section.VirtualAddress`）。

> 目标版本：`modules\jubeat.dll`，`FileHeader.TimeDateStamp = 0x6a59f658`，入口 RVA `0x210560`，
> PE32 / imagebase `0x10000000`（实际会 ASLR 重定位，例如加载到 `0x54810000`）。
> 版本不符时插件会自动禁用。

---

## 1. 相关模块

| 模块 | 作用 |
|---|---|
| `spice.exe` | spice2x 启动器；`-k <dll>` 会在启动阶段 `LoadLibrary` 指定 DLL |
| `modules\jubeat.dll` | 游戏主体（DLL 型 BEMANI 游戏；导出 `dll_entry_init` / `dll_entry_main`） |
| `modules\gftools.dll` | Konami 的通用框架：FSM、输入设备状态表、图形/文件等 |
| `modules\fsm_man.dll` | gftools FSM 管理器的薄封装（jubeat 用它注册/查询状态机） |
| `modules\input.dll` | 面板输入模块：`input_update()` 每帧把 `device.dll` 的 JAMMA 状态转成面板状态表 |
| `modules\device.dll` | 硬件 I/O 抽象；spice2x 在这里注入虚拟按键状态 |
| `modules\sound.dll` | BGM/SE；`sound_bgm_stop()` / `sound_se_stop_all()` |
| `modules\ledanim.dll` | 面板灯光动画 |

---

## 2. gftools FSM 框架

### 2.1 对象结构 `GFFSMObject`（0x58 字节，来自 gftools 反汇编）

| 偏移 | 类型 | 含义 |
|---|---|---|
| `+0x00` | u32 | 对象 id（例如 `0x23800000` = MUSIC_LOOP） |
| `+0x08` | ptr | 状态处理函数指针（`handler(obj, msg, param)`） |
| `+0x0C` | u32 | flags（bit31 = transiting、`0x02000000` = shutting 等） |
| `+0x10` | u32 | status code |
| **`+0x14`** | **u32** | **当前状态 id** |
| `+0x18` | u32 | 上一个状态 |
| `+0x1C` | u32 | next state |
| `+0x24` | u32 | next data |
| `+0x2C` | u32 | update 计数 |
| `+0x30` | u32 | render 计数 |
| `+0x34` | ptr | 所属 manager |
| `+0x44` | u32 | 该对象的 FSM 调试日志开关（写 1 打开） |
| `+0x48` | ptr | 名字字符串（如 `"MUSIC_LOOP"`），游戏初始化时写入 |

### 2.2 消息结构 `GFFSM_MESSAGE`（24 字节）

```c
struct GFFSM_MESSAGE {   // 全部 cdecl
    uint32_t msg;        // +0x00 消息 id
    uint32_t param1;     // +0x04
    uint32_t param2;     // +0x08
    int32_t  srcId;      // +0x0C 发送者对象 id（系统一般是 -13 = 0xFFFFFFF3）
    int32_t  dstId;      // +0x10 目标对象 id；-1 = 广播
    uint32_t stateId;    // +0x14 要求目标当前状态，0x80000000 = 任意
};
```

### 2.3 常用系统消息 id

| 消息 | 含义 |
|---|---|
| `0x80000001` | 框架初始化 / boot |
| `0x80000003` | 请求关闭 |
| `0x80000004` | shutting 状态（对象“不再运行”） |
| `0x80000011` | 进入 init |
| `0x80000012` | finalize |
| `0x80000013` | enter state |
| `0x8000001F` | 请求 finalize（子场景收尾） |
| `0x80000021` | 状态已进入（attach/init 子对象） |
| `0x80000022` | 离开状态（detach 子对象） |
| `0x90000024` | render tick |
| `0xA0000023` | update tick |
| `0xB0000001` | is-running 查询 |
| `0xB0000002` | is-exists-state 查询 |
| `0xB0000003` | singleton/factory 查询 |

> 约定：**给父 FSM 发送一个“等于某子场景对象 id”的消息 = 要求进入该状态**；
> 但本游戏的 `MUSIC_LOOP` 路由器在特定状态下会忽略这种消息（见 §4），因此退出本局用的是强制状态转移。

### 2.4 关键 API（gftools.dll 导出，cdecl）

```c
GFFSMMan*    GFFSMManCreate(void* buf, uint32_t bufSize, int maxObj, int maxMsg); // RVA 0x5110
GFFSMObject* GFFSMManGetObject(GFFSMMan* man, int32_t id);                        // 0x56F0
GFFSMObject* GFFSMObjectMapGetObject(void* map, int32_t id);                      // 0x6B90
bool GFFSMObjectProcessMessage(GFFSMObject* obj, GFFSM_MESSAGE* msg);             // 0x60D0
bool GFFSMObjectProcessMessageByParam(GFFSMObject* obj, uint32_t msg,
                                      uint32_t p1, int32_t src, uint32_t state);  // 0x6490
void GFFSMObjectSetNextState(GFFSMObject* obj, uint32_t state);                   // 0x6520
void GFFSMObjectSetNextData(GFFSMObject* obj, uint32_t data);                     // 0x6550
void GFFSMObjectSetTransiting(GFFSMObject* obj, bool on);                         // 0x6500
bool GFFSMObjectTransitState(GFFSMObject* obj, uint32_t state);                   // 0x6560（立即派发转移）
int  GFFSMManSendMessageByParam(GFFSMMan* man, uint32_t msg, uint32_t p1,
                                int32_t src, int32_t dst, uint32_t state);        // 0x5A70
uint32_t GFInputDDevGetStateMap(int dev, int field);                              // 0x1CA70
```

`fsm_man.dll` 是对该 manager 的封装（`fsm_man_base + 0x1316C` 处是 `GFFSMMan*`），
导出 `fsm_man_get_object(id)` / `fsm_man_get_curr_state(id)` / `fsm_man_send_message(...)` 等。

---

## 3. 场景对象表（jubeat）

| 对象 | id | 备注 |
|---|---|---|
| `GAME` | `0x20000000` | 顶层游戏流程 |
| `PLAY` | `0x23000000` | 游戏会话 |
| `PLAY_INITIALIZE` | `0x23100000` | 卡入场/加载 |
| `MUSIC_LOOP` | `0x23800000` | **选曲↔游玩↔结算 的路由器**，静态对象 `jubeat.dll + 0xFCA6A0` |
| `MUSIC_SELECT` | `0x23840000` | 选曲（静态对象 `+0x1222280`） |
| `DIFFICULTY_SELECT` | `0x23850000` | 难度选择 |
| `ONLINE_MATCHING` | `0x23860000` | 联机匹配 |
| **`MUSIC_PLAY`** | **`0x23880000`** | **游玩场景**，静态对象 `jubeat.dll + 0xFCA6F8` |
| `MUSIC_RESULT` | `0x238A0000` | 单曲结算 |
| `PLAY_FINALIZE` | `0x23E00000` | 会话结束 |

`MUSIC_LOOP` 的「状态」就是当前子场景 id；`MUSIC_PLAY` 是游玩场景自己的状态机。

### 3.1 MUSIC_PLAY 状态链

```
0x23881000  cue/初始化
0x23882000  加载 BGM（music_db_get_sound_filename + sound_bgm_load_call）
0x23883000  加载谱面（sequence_load_main / sequence_load_call ...）
0x23884000  同步等待
0x23885000  就绪/倒计时
0x23886000  ★ 正在游玩（PLAYING）
0x23887000  曲目结束
0x80000004  finalize / 不再运行
```

- `0x23886000` 进入时：`score_clear_context()`、`sound_bgm_play()`、灯光等。
- 每帧 update（消息 `0xA0000023`）：读 `sound_bgm_get_position()`，和 `sequence_get_end_sector()` 比较，
  到达末尾就 `SetNextState(0x23887000)`。
- 自然结束路径：`0x23886000` → `0x23887000` → `0x80000004`（对象不再 running）→
  `MUSIC_LOOP` 路由器发现子对象不在了 → `SetNextState(0x238A0000)` 进入 `MUSIC_RESULT`（结算场景会停 BGM）。

### 3.2 MUSIC_LOOP 路由器（handler RVA `0x9B630`）行为

对每个子场景状态 X：

| 收到消息 | 行为 |
|---|---|
| `0x80000021`（进入状态） | 查工厂并 `GFFSMManAttachAndInitObjectFrom(..., X)` 附着/初始化子场景 |
| `0x80000022`（离开状态） | 若子对象还在运行：给它发 `{0x8000001F, ...}`；然后 `fsm_man_unregister_object(X)` 注销 |
| `0xA0000023`（update） | 转发 update 给子对象；若子对象不再运行 → `SetNextState(下一个场景)` |
| `0x90000024`（render） | 转发 render |
| `0xB0000002` | 返回“已处理” |

场景链（简化）：`MUSIC_SELECT(0x23840000)` → `DIFFICULTY_SELECT(0x23850000)` →
`ONLINE_MATCHING(0x23860000)` → **`MUSIC_PLAY(0x23880000)`** → `MUSIC_RESULT(0x238A0000)` →
`PASELI_CONTINUE`/`...` → 回到汇合点 → `MUSIC_SELECT`。

---

## 4. 退出本局的做法（核心结论）

**先说结论**：触发后**只把 `MUSIC_PLAY` 推进到游戏自己的「曲终」状态 `0x23887000`，然后什么都不要做**。
游戏会自己完成结算、continue、回到选曲，本局按 FAILED（0 分）结算，credit/曲数统计和 UI 都是原生的。

```c
sound_bgm_stop();          // 本局要中断，先停音乐
sound_se_stop_all();
GFFSMObjectTransitState(playObj, 0x23887000);   // 游戏自己的「本曲结束」状态
// 之后不要再改 MUSIC_LOOP / MUSIC_PLAY 的状态
```

### 4.1 为什么不能直接跳 `MUSIC_SELECT`

`MUSIC_LOOP` 当前状态是 `0x23880000`（游玩中）时，消息 switch 只接受
`{0xB0000002, 0x80000021, 0x80000022, 0xA0000023, 0x90000024}`，其它消息被忽略，
所以只能走状态转移。但直接 `TransitState(MUSIC_LOOP, 0x23840000)`（旧版做法）有两个问题：

1. **选曲画面残留游玩 HUD**：游玩时的曲目信息 / `1ST TUNE` / LEVEL / 小节条属于**共享的 AEP UI
   数据集合**（见 §4.3），游戏只在原生的「结算之后」状态链里才把它们重新排布成选曲布局，
   直接跳就会把游玩布局留在选曲画面上。
2. **吃掉本局剩下的曲数**：本局的曲数/credit 记账只在游戏原生的结算流程里完成，
   强行跳转会破坏它，之后游戏会认为整次游玩已经结束（弹 CONTINUE? 然后回 Start CREDIT）。

### 4.2 也试过但不可行的方案（均在本机 L44 `0x6a59f658` 实测）

| 方案 | 选曲 HUD | credit/下一首 |
|---|---|---|
| `play -> MUSIC_SELECT`（旧版 mode 1） | **残留** | 被吃掉 |
| `play -> MUSIC_RESULT -> MUSIC_SELECT` | 残留 | 被吃掉 |
| `play -> MUSIC_RESULT -> 0x238D0000 continue -> junction -> select`（mode 15） | 干净 | **被吃掉**（CONTINUE? → Start CREDIT） |
| `play -> junction 0x23830000` | 残留 | 被吃掉 |
| 先强行 playing->song_end，再由我们自己强推后续状态 | 不可靠 | 不可靠 |
| 强行把结果场景 child finalize（mode 30） | 结算页干净，回不到 select | 最终仍被吃掉 |
| **只强制 `play -> 0x23887000`，之后不干预（mode 31，最终方案）** | **干净** | **正常，可继续下一首** |

原因：`MUSIC_PLAY` 自己的「曲终」状态 `0x23887000` 会跑游戏真正的收尾逻辑（finalize、成绩/存档、
tune 计数等）。这一步是 credit 记账的关键；跳过它（直接跳 result/select）就会丢曲数。

### 4.3 关键状态与计数（排查用）

- `MUSIC_PLAY` 状态链：`0x23886000` 游玩中 → `0x23887000` 曲终 → `0x80000004` finalize（对象不再运行）。
- `MUSIC_LOOP` 状态：`0x23880000` 游玩 → `0x238A0000` 结算 → `0x238C0000`/`0x238D0000` continue 页 →
  `0x23830000` junction → `0x23840000` 选曲；`0x238B0000` = LIGHT_CHAT_SELECT。
- 本局曲数计数 `jubeat + 0x1243BB4`（`0x11243BB4`，clamp 0..6）：
  - 只在进入 continue 页 `0x238D0000` 时 `+1`（`jubeat RVA 0x9DCAB` 调 `0xE29F0`，
    且 `env_is_build_user()` 为真时**不**加——调试/构建用户模式下不计数）；
  - junction 用它决定下一站：`>0` → 选曲，`==0` → `0x238B0000`（LIGHT_CHAT_SELECT）。
- 同时还有一个会话计数 `jubeat + 0xFC3068`（`0x10FC3068`）和 continue 页一起 `+1`。
- 可用曲数：`coin.dll!coin_get_playable_tune_num(mode)`，经 `jubeat RVA 0xE2940` 的 getter
  按 `0x11243BB0` 的模式取值（本机 3 曲 credit 时各模式返回 `[3,2,3,3]`）。
- `0x238D0000` continue 页的进入处理还会读表 `0x11287606 + min(count,5)*0xF0` 决定后续分支，
  并据此决定是回 junction 还是走 `0x238F0000` 的过渡/结束流程——这也是为什么强行跳过结算会出事。

### 4.4 退出后的表现（最终方案）

- 本局立即结束（BGM/音效停止），结算页显示 FAILED / 0 分；
- 玩家按 NEXT/面板走过原生结算和 continue 页，回到选曲；
- 选曲画面干净、本局剩余曲数正确保留，可以继续下一首、直到 FINAL TUNE。

---

## 5. 面板输入链

```
device.dll（JAMMA/虚拟按键，spice2x 在此注入）
   ↓ 每帧
input.dll!input_update()  —— 把 device 的 JAMMA 位图转成 gftools 设备 2（16 面板）状态
   ↓
gftools 输入状态表（GFInputDDev）
   ↓
jubeat.dll 通过 input.dll!input_panel_trg_on(a,b,&t) 等读取
```

- 面板位图读取：`GFInputDDevGetStateMap(dev=2, field=1)` 低 16 位；
  **bit i = 第 i+1 号面板**（行优先），四角 1/4/13/16 = bit 0/3/12/15 = 掩码 **`0x9009`**。
- `input_panel_trg_on(a, b, outTime)` 内部用 `bit = a + 4*b`（`input.dll` 0x15A0 起）；
  jubeat 的判定函数按 `a = idx % 4, b = idx / 4` 调用，因此面板号 = `1 + a + 4*b`。
- gftools 设备状态表：`gftools.dll + 0x73660`，每设备一条记录（36 字节）：
  `+0x00` 当前按住位图、`+0x0C` 上升沿、`+0x10` 下降沿、`+0x14` 去抖后的当前值……
  （`GFInputDDevGetStateMap(dev, field)` 的 field 即这里的“第几个 u32”，不是字节偏移。）

---

## 6. 插件的 hook 点

| 目标 | 位置 | 说明 |
|---|---|---|
| `input.dll!input_update` | `input.dll + 0x1040`（导出即可 `GetProcAddress`） | 每帧、游戏线程调用；在此读面板位图并执行退出（与游戏自己的帧同步，安全） |

所有进入游戏的调用都包在 VEH（`AddVectoredExceptionHandler`）+ `setjmp/longjmp` 保护里：
万一某个地址在当前版本上无效，只会记录 `SEH ...` 并跳过，不会让游戏崩溃。

**注意**：`GFFSMObjectTransitState` 是“立即派发”版本，只能在 FSM 派发**之外**调用
（例如 `input_update` 的 hook 里）；如果是在游戏自己的状态处理函数内部，应该用
`GFFSMObjectSetNextState`（排队到下一帧处理）。

---

## 7. 版本移植指南

换 `jubeat.dll` 版本后，需要重新确认以下几项：

1. **版本标识**：`jubeat.dll` 的 PE `TimeDateStamp`（`feature_quickskip.cpp` 的 `kExpectedTimeStamp`）。
2. **静态 FSM 对象地址**：`MUSIC_LOOP` / `MUSIC_PLAY` 对象在 `.data` 中的地址（当前 `0xFCA6A0` / `0xFCA6F8`），
   以及它们的 id（`0x23800000` / `0x23880000`）和 `MUSIC_SELECT` 状态 id（`0x23840000`）。
   - 定位方法：在 `jubeat.dll` 里找字符串 `"MUSIC_LOOP"` / `"MUSIC_PLAY"` 的引用，
     或找 `fsm_man_register_singleton` / `GFFSMObjectTransitState` 调用点附近的常量；
   - 运行时可以用探针 DLL 打印这些对象的 `+0x00 id`、`+0x48 name`、`+0x14 state` 来验证。
3. **游玩状态值**：`MUSIC_PLAY` 的 PLAYING 子状态（当前 `0x23886000`）。
4. **`input_update` 导出**：优先用 `GetProcAddress("input_update")`，导出名一般稳定；RVA 只作兜底。
5. **面板位图语义**：用 spice2x 的 API 依次按 1..16 号键，观察 `GFInputDDevGetStateMap(2,1)` 的位，
   确认“bit i = 第 i+1 号键”和四角掩码 `0x9009`。

当前版本用于交叉验证的字节模式（含通配符，供新版本搜索参考）：

```
MUSIC_PLAY handler (RVA 0x9E0D0):
  55 8B EC 83 E4 F0 B8 48 10 00 00 E8 ?? ?? ?? ?? A1 ?? ?? ?? ?? 33 C4
  89 84 24 44 10 00 00 8B 45 0C 56 8B 75 08 89 74 24 18 57 8B 7D 10
  89 7C 24 18 3D 01 00 00 80 0F 85

MUSIC_LOOP handler (RVA 0x9B630):
  55 8B EC 83 E4 F8 81 EC 4C 01 00 00 A1 ?? ?? ?? ?? 33 C4 89 84 24
  48 01 00 00 8B 45 0C 53 8B 5D 10 89 5C 24 10 56 57 8B 7D 08 89 7C
  24 1C 3D 01 00 00 80 0F 85

MUSIC_LOOP -> MUSIC_RESULT 的 SetNextState 调用点 (RVA 0x9D426):
  68 00 00 8A 23 57 FF 15 ?? ?? ?? ?? 83 C4 08
```

---

## 7.5 实机（p4io）输入路径

插件本身**没有任何 p4io 相关代码**，也不需要：它只读 gftools 的输入设备状态表。
而这张表在「spice2x 虚拟输入」和「实机 p4io 硬件输入」两条路径下，都是由游戏自己的
`modules\device.dll` + `modules\input.dll` 填充的，所以两条路径等价。

实机（p4io，jubeat 机台）链路（本轮补充逆向确认）：

```
libp4io.dll            p4io_get_jamma(jamma_out, ...)  ← 真实 P4IO 板（JAMMA 位图，低电平有效）
   ↑ 导入              （device.dll 导入 p4io_get_jamma / p4io_get_jamma_hist / ...）
device.dll!device_update()      device.dll RVA 0x1250
   · 调用 p4io_get_jamma 填到 device.dll + 0x131A4（32 位缓冲），并做 0x11FEFE 位取反
   · device.dll!device_get_jamma()（RVA 0x18B0）直接 return device.dll + 0x131A4
input.dll!input_update()        input.dll RVA 0x1040
   · 把 JAMMA 缓冲的每一位按固定方式写成 gftools 设备 1 的位（bit 0..0x17）
     · 空实现（没有硬件）时把这些位清 0
   · GFInputDDevUpdate(1)
   · 按 input.dll RVA 0x10AD8 的 16 项表，把设备 1 的位映射成设备 2（16 面板）的位：
        dev2 bit i  ←  dev1/jamma bit [ 8, 4,19,15,  9, 5,20,16,  10, 6,21,17,  11, 7,22,18 ][i]
   · GFInputDDevUpdate(2)
jubeat.dll 读 input.dll!input_panel_trg_on/off/short_on 判定命中；
本插件读 GFInputDDevGetStateMap(2,1) 的按住位图判定触发组合（默认四角 1/4/13/16 = bit 0/3/12/15 = 0x9009；触发组合可配置，见 INI 的 `Keys=`）。
```

关键结论：

- 无论面板输入来自 spice2x 的虚拟按键还是真实 P4IO 板，最终都汇入同一个 dev2 位图，
  位序由 `input.dll` 的固定表决定，与 IO 来源、机台固件无关。
- 因此本插件在「实机 p4io + `-exec` 启动」下同样成立；`-exec` 恰好会关掉 spice2x 的
  IO 模拟/设备钩子，让 `device.dll` 走真实硬件，这正是实机想要的行为。
- 唯一需要自己确认的是：**游戏版本**（`jubeat.dll` 的 PE 时间戳必须是 `0x6a59f658`）
  和 `-k` 是否真的执行了（看 `log.txt` 里的 `loading hook DLL`）。

---

## 8. 验收记录（本机）

- 环境：Windows 10/11，spice2x 32 位启动器，游戏 L44（`jubeat.dll` 时间戳 `0x6a59f658`），本地自建服务器。
- 加载方式：spice2x `-k`、`-z`、外部 injector 三种均验证通过。
- **最终发布构建**（`jubeat_quickskip.dll` 850,380 B，SHA256
  `BEC549AAB0A6827944D94915C85C6034697FD9BC2C8054D3ABB32E8F96D08344`，2026-10-24）实机验证：
  - 3 曲 credit（`1ST TUNE`），真实按住四角 1/4/13/16 约 0.3 秒：
    日志 `TRIGGER ... -> abort tune` → `ABORT mode=31` → `TransitState(MUSIC_PLAY->song_end, 0x23887000)`
    → `play scene ended ... waiting for the game's native MUSIC_RESULT`
    → `game entered MUSIC_RESULT natively - handing ... over to the game`，之后插件不再有任何状态操作。
  - 本局按 FAILED（0 分）结算；玩家按 NEXT 走过原生结算/continue 页 → 回到选曲；
    **选曲画面无任何游玩 HUD 残留**；本局剩余曲数正确，可正常开始第 2 首，重复触发可到 FINAL TUNE。
  - 计数核对：tune1 触发时 `count=0`，原生结算后 tune2 触发时 `count=1`；可用曲数 `[3,2,3,3]` 不变。
  - 插件自身动作耗时约 0.16 秒（触发→交接）；剩余时间是游戏原生结算/continue 的展示。
- 已知副作用：被中断的曲目按 FAILED（0 分）结算；退出后需要玩家自己按键走过结算/continue 画面。
  `AbortMode=1`（旧版直接跳选曲）会残留 HUD 且吃掉本局剩余曲数，仅保留作调试对照。

---

## 9. 参考

- [spice2x](https://spice2x.github.io/)：开源 BEMANI 启动器，本文的 `-k`/`-z` 加载方式、虚拟输入、
  日志与截图工具都来自它（本项目不包含其代码）。
- 游戏本体的相关符号/版本信息、`patches/*.json`（573 patcher 格式）可作为版本锚点。
