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

**为什么不能直接给 `MUSIC_LOOP` 发 `0x23840000` 消息**：
当 `MUSIC_LOOP` 当前状态是 `0x23880000`（游玩中）时，它的消息 switch 只接受
`{0xB0000002, 0x80000021, 0x80000022, 0xA0000023, 0x90000024}`，其余消息被忽略。

**可行做法：强制状态转移**（`GFFSMObjectTransitState`）：

```c
if (*(uint32_t*)(jubeat + 0xFCA6A0 + 0x14) == 0x23880000 &&   // MUSIC_LOOP 在游玩场景
    *(uint32_t*)(jubeat + 0xFCA6F8 + 0x14) == 0x23886000) {   // MUSIC_PLAY 正在游玩
    sound_bgm_stop();        // 正常流程里由 MUSIC_RESULT 场景做
    sound_se_stop_all();
    GFFSMObjectTransitState((void*)(jubeat + 0xFCA6A0), 0x23840000); // -> MUSIC_SELECT
}
```

`GFFSMObjectTransitState` 会设置 `flags |= 0xC0000000`、`next = state`，然后**同步**派发框架转移消息：

1. 给 `MUSIC_LOOP` 派发 `0x80000022`（离开旧状态）→ 路由器给 `MUSIC_PLAY` 发 `0x8000001F`
   并 `fsm_man_unregister_object(0x23880000)`（子场景原生收尾）；
2. 给 `MUSIC_LOOP` 派发 `0x80000021`（进入新状态）→ 附着并初始化 `MUSIC_SELECT` 子场景。

这与游戏自己「游玩 → 结算」的转移是同一条代码路径（`jubeat.dll RVA 0x9D426` 处就是
`SetNextState(MUSIC_LOOP, 0x238A0000)`），只是目标状态换成 `MUSIC_SELECT`。

**兜底**：若转移后约 0.6 秒仍发现 `MUSIC_LOOP` 停在 `0x23880000`，插件会改走游戏原生路径：
`SetNextState(0x238A0000)`（结算，会自己停 BGM）→ 1.5 秒后 `SetNextState(0x23830000)`（汇合点）→ 选曲。

其他候选（本需求不用）：把 `MUSIC_PLAY` 的 next state 设为 `0x23887000` = 自然曲终；
`0x80000004` = 直接 finalize；`0x23883000`/`0x23885000` = 重新加载/快速重开（如果以后想做“重开本曲”）。

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

## 8. 验收记录（本机）

- 环境：Windows 10/11，spice2x 32 位启动器，游戏 L44（`jubeat.dll` 时间戳 `0x6a59f658`），本地自建服务器。
- 加载方式：spice2x `-k`、`-z`、外部 injector 三种均验证通过。
- 结果：游玩中按住四角约 0.3 秒 → 约 0.6 秒后回到选曲界面；连续两次触发正常，无黑屏/卡死/崩溃；
  之后可正常继续选曲并开始下一首。日志中 `abort verify OK - MUSIC_LOOP state now 0x23840000`。
- 已知副作用：中断的曲目不产生成绩/结算、不上传服务器；选曲界面顶部可能短暂残留“1ST TUNE”HUD 文字。

---

## 9. 参考

- [spice2x](https://spice2x.github.io/)：开源 BEMANI 启动器，本文的 `-k`/`-z` 加载方式、虚拟输入、
  日志与截图工具都来自它（本项目不包含其代码）。
- 游戏本体的相关符号/版本信息、`patches/*.json`（573 patcher 格式）可作为版本锚点。
