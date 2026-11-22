# OnePage V3.1 (ESP32-S3) 板载外设融入阅读功能 · 实施计划 v2

状态：**服务端已完成并上线验证；设备端待开发**
日期：2026-11-16（v2.2：relay 已部署到 118.25.20.250，release `20261116-133333`）
目标板：`env:onepage_s3`（ESP32-S3R8，240 MHz，16 MB QIO Flash，8 MB OPI PSRAM）

---

## 0. 评审结论已锁定

| # | 决策点 | 你的决定 | 本计划的落法 |
|---|---|---|---|
| 1 | 倾翻翻页 | 按建议执行 | 复用现有 `tiltPageTurn`（Off/Normal/Inverted），不加灵敏度档；S3 用加速度计跑新状态机 |
| 2 | 温湿度显示 | 首页新增「万年历」菜单 | **取消状态栏温湿度**，SHT30 读数进万年历页；黄历 + 5 日天气同页；**月历视图要做**（v2 二次评审确认） |
| 3 | 音效 | 翻页 + 按键都要 | 全局按键音（`MappedInputManager` 单点）+ 翻页专用音（阅读器内抑制通用点击音，避免一次按键两声） |
| 4 | TTS | relay 用 edge-tts，WS 送设备播放 | 新增 relay `/api/v1/tts`；设备新增 PCM 收流播放器。查字典中文发音、英文查词与复习美音同源 |
| 5 | 语音便签 | 保持现状 | `lib/Notes` 的 `NTF1` 纯文本格式不动，不加版本号 |
| 6 | 天气地点 | 按当前 IP 所在地 | relay 按访问者公网 IP 定位（ip-api → ipinfo 回退），设备不选城市、不调第三方（守 `SCOPE.md:37` 的 relay 唯一后端规则） |

---

## 1. 现状基线（证据）

### 1.1 已就绪的外设 HAL

| 器件 | HAL | 现状 |
|---|---|---|
| MSA311 三轴加速度计 | `lib/hal/HalAccelSensor.h` | 已实现，仅自检使用（`src/HardwareSelfTest.cpp`） |
| SHT30 温湿度 | `lib/hal/HalEnvSensor.h` | 已实现，仅自检使用 |
| MAX98357A 喇叭 | `lib/hal/HalAudio.h`（`SAMPLE_RATE=16000`，`HalAudio.h:20`） | 已实现 `begin/end/setEnabled/beep/writePcm`，仅自检使用 |
| PDM 麦克风 | `lib/hal/HalMicrophone.h` + `lib/voice/Recorder` | **已接入产品功能**（语音助手、语音书签） |

初始化点：`src/main.cpp:387-388`（`ENV_SENSOR.begin()` / `ACCEL.begin()`，`ONEPAGE_S3` 下编译）。

### 1.2 本轮新确认的四个缺口（计划必须覆盖）

1. **倾翻链路在 S3 上是断的**：`lib/hal/HalTiltSensor.cpp:55` 首句 `if (!gpio.deviceIsX3())` 直接返回，`src/main.cpp:381` 在 OnePage 上跳过 `begin()`。设置项 `SETTINGS.tiltPageTurn`（`src/CrossPointSettings.h:292`，设置行在 `src/SettingsList.h:282-288`，以 `halTiltSensor.isAvailable()` 为门）、主循环轮询（`src/main.cpp:621`）、阅读器消费（`src/activities/reader/ReaderUtils.h:44-45`）三处都只是"没有数据源"。
2. **RTC 只有时分，没有日期**：`lib/hal/HalClock.h:33` 只暴露 `getTime(hour, minute)`；`HalClock::writeTimeToRTC()` 只写 `0x02..0x04`（`lib/hal/HalClock.cpp:179-199`）。**万年历需要完整年月日**，PCF8563 的 `0x05..0x08`（日/星期/月/年）目前完全没读没写，`syncFromNTP()` 也不会写日期。
3. **全项目没有天气与农历代码**：`lib/` 下无 Weather/City/Almanac 模块（只有 `lib/ChatLog/ChatHistoryFormat.h:140-160` 的公历换算/星期辅助）；`relay/relay/` 无 weather 路由（现有路由见 `relay/relay/main.py:10-17`，markets 是最接近的"relay 抓上游 → 设备渲染"范式）。现有串口调试命令集中在 `src/main.cpp:646-661`（`HWTEST/SENSORS/MIC/SPKMAX/IMU`），可作为本轮调参与验收的入口。
4. **WebSocket 目前只上行二进制**：`lib/VoiceRelay/VoiceRelayClient.cpp:262` 明确"binary frames do not exist in this protocol"（设备→relay 送 PCM 给 ASR）。TTS 是**反方向**（relay→设备送 PCM），需要新的收流路径；`links2004/WebSockets` 已在内核依赖里（`platformio.ini:73`）。

### 1.3 可复用资产

- **当前页文本提取**：`AskAiActivity::extractPageLines()`（`src/activities/reader/AskAiActivity.cpp:136-217`）已经做了 `Section → loadPageFromSectionFile() → PageLine → 逐词拼行 → PSRAM 池`，TTS 朗读当前页可直接复用（建议抽成共享 helper，见 §6.3）。
- **JSON 解析**：`lib/JsonParser`（markets/learning 都在用）。
- **relay HTTP 客户端范式**：`lib/Markets/MarketClient`（带 device token 的 GET）。
- **Wi-Fi 按需接入**：`DictionaryActivity::promptForWifi()` 的 `WifiSelectionActivity` 回调范式。
- **首页图标网格**：`HomeActivity::buildMenu()`（`src/activities/home/HomeActivity.cpp:25-38`）+ `GUI.drawHomeIconGrid`，加一个 tile 就是加一行 label + icon。

### 1.4 资源基线

- 内部 DRAM：单 framebuffer 48 KB（`-DEINK_DISPLAY_SINGLE_BUFFER_MODE=1`）；PSRAM 8 MB 已用于语音链路。
- 主循环栈 24 KB（`-DARDUINO_LOOP_STACK_SIZE=24576`）。
- Flash：16 MB，当前固件远未占满，农历表 + 宜忌表的 ~1 KB `.rodata` 无压力。

---

## 2. 里程碑与工作量

| 里程碑 | 内容 | 设备侧 | relay 侧 |
|---|---|---|---|
| **M1** | 倾翻翻页（加速度计） | 1.5 d | — |
| **M2** | 全局按键音 + 翻页音 + 番茄钟提醒音 | 1.5 d | — |
| **M3** | 万年历（RTC 日期 + 农历黄历 + 月历视图 + 5 日天气 + 室内温湿度） | 6 d | ✅ 已完成（0.5 d） |
| **M4** | 朗读当前页 + 三处发音（字典中文 / 查词美音 / 复习美音） | 3 d | ✅ 已完成（1.5 d） |

设备侧合计约 **12 人天**（M1 1.5 + M2 1.5 + M3 6 + M4 3）；relay 侧 2 人天已交付上线。顺序即上表顺序：M1/M2 完全离线、可独立合入与回滚；M3/M4 所需的服务端接口已就绪。

---

## 3. M1：倾翻翻页（加速度计）

### 3.1 架构

新增门面 `lib/hal/HalTiltGesture.{h,cpp}`（单例 `TILT`），**不改** `HalTiltSensor` 与 `HalAccelSensor` 的实现：

```
TILT.begin()            X3: halTiltSensor.begin()   S3: ACCEL.begin()
TILT.isAvailable()
TILT.update(mode, orientation, inReader)     主循环每轮一次
TILT.wasTiltedForward() / wasTiltedBack()    消费型
TILT.hadActivity()                            非消费型（自动休眠）
TILT.deepSleep()
```

X3 分支透传给原陀螺仪实现（零行为变化）；S3 分支用加速度计跑角度状态机。

### 3.2 S3 算法（无陀螺仪）

1. 每 20 ms 采样一次（MSA311 已配 62.5 Hz）。
2. 低通分离重力：`g += 0.25 * (a - g)`。
3. 倾角 `atan2` 取"左右"与"上下"分量，轴向映射复用 `HalTiltSensor` 里现成的 orientation switch。
4. 角速率由相邻倾角差分得到（同低通）。
5. 触发：`|rate| > 180 °/s` 且累计转角 > 25°，冷却 600 ms，回中（< 8°、|rate| < 40 °/s）后方可再触发。
6. 唤醒后 300 ms 丢弃读数；`inReader == false` 时不采样；按键翻页后 400 ms 屏蔽（防一 gesture 翻两页）。

阈值全部 `static constexpr`，真机用现有 `CMD:IMU` 观察（`src/main.cpp:654`）调一轮。

### 3.3 改动清单

| 文件 | 改动 |
|---|---|
| `lib/hal/HalTiltGesture.h/.cpp` | 新增（S3 状态机 + X3 透传） |
| `src/main.cpp:621` | `halTiltSensor.update(...)` → `TILT.update(...)` |
| `src/main.cpp:286` | 休眠前 `TILT.deepSleep()` |
| `src/main.cpp:904` | 自动休眠活跃判断加 `TILT.hadActivity()` |
| `src/activities/reader/ReaderUtils.h:44-45` | 改调 `TILT.wasTiltedForward/Back()` |
| `src/SettingsList.h:282` | 可用性判断改 `TILT.isAvailable()` |

资源：~80 B 浮点状态，无堆分配，零新任务。

---

## 4. M2：音效

### 4.1 先改 HAL（必要）

`HalAudio::beep()` 每次 `setEnabled(true) → 写 PCM → delay(ms+30) → setEnabled(false)`，每页切换一次功放地（Q2）会有爆音并白等 30 ms。新增非阻塞接口：

```cpp
bool tone(uint16_t frequencyHz, uint16_t durationMs, int16_t amplitude);  // 只推进 I2S DMA
void drain(uint16_t timeoutMs);                                           // 等 DMA 播完
```

`beep()` 保留为自检用的阻塞封装（内部改为 `setEnabled(true); tone(); drain(); setEnabled(false)`），自检行为不变。

### 4.2 `src/util/UiSound.{h,cpp}`（产品层，不进 `lib/hal`）

- `init()` / `shutdown()`：`AUDIO.begin()` 一次；空闲 300 ms 自动 `setEnabled(false)`（保留 I2S 通道，省 DMA 重建）；离开阅读器/进休眠时 `AUDIO.end()`。
- `key()`：1200 Hz / 18 ms。`page()`：900 Hz / 30 ms（更低更钝，与按键区分）。`alertPattern()`：3 × 1800 Hz / 120 ms（番茄钟）。
- 节流：同类音效最小间隔 60 ms；`SETTINGS.soundFeedback == OFF` 时全空转。

### 4.3 全局按键音挂点（单点，不散落埋点）

在 `MappedInputManager::update()`（`src/MappedInputManager.h:17`，实现是 `gpio.update()` 的薄壳）里，`gpio.update()` 之后：

```cpp
if (gpio.wasAnyPressed() && !UiSound::pageKeysOwned()) UiSound::key();
```

`pageKeysOwned()`：阅读器 `onEnter()` 置位、`onExit()` 清零。置位期间**跳过** PageForward/PageBack/Up/Down 四键的通用点击音 —— 这样一次翻页只响一声（由阅读器在 `displayBuffer()` 前调用 `UiSound::page()`），不会"点击音 + 翻页音"叠在一起。菜单/设置等其它 Activity 不受影响，按键一律有点击音。

时序：翻页音在 `displayBuffer()` **之前**触发，刷新 1-2 s 正好与"啪"的一声同步，手感正确。

### 4.4 番茄钟

`src/pomodoro/PomodoroAlertActivity.cpp:69` 的 `blinkAlert()` 里加 `UiSound::alertPattern()`，与闪屏同时开始。

### 4.5 设置

`CrossPointSettings::soundFeedback`：`OFF / ON`，默认 `OFF`（开箱不响；用户要求"要有音效"，但默认开会在公共场合突兀，建议先关，需要时一键打开）。

---

## 5. M3：首页「万年历」

### 5.1 页面内容

一页 800×480，左右分栏：

```
┌───────────────────────────────────────────────────┐
│ 2026年11月16日 星期一            丙午年 十月初七    │  ← header（GUI 字面）
├────────────────────────┬──────────────────────────┤
│ 干支   丙午 丁亥 壬戌   │ 南京      室内 21.4°C 45% │
│ 节气   立冬 11/7        │ ┌──────────────────────┐ │
│ 下一   小雪 11/22       │ │ 今天  晴    21/13 10%│ │
│ 生肖   马               │ │ 17    多云  20/12 20%│ │
│ 建除   成               │ │ 18    小雨  18/11 80%│ │
│ 宜     祭祀 祈福 出行   │ │ 19    阴    19/10 30%│ │
│ 忌     动土 开仓        │ │ 20    晴    22/13  5%│ │
└────────────────────────┴──────────────────────────┘ │
```

- 黄历块（干支/节气/生肖/建除/宜/忌）**仅在中文界面渲染**；其它语言只显示公历 + 天气 + 室内温湿度（节气名、宜忌条目需要 CJK 字形，英文界面回落成一行 `Lunar: 10-07` 更诚实）。
- 天气图标：v1 用**文字**（晴/多云/阴/小雨/雪/雾），不做点阵图标 —— 省 8 个位图与一套映射，e-ink 上文字更清晰。
- 室内温湿度来自 SHT30，与状态栏方案的关键区别是**不在渲染路径里读传感器**（`HalEnvSensor::read()` 内含 `delay(20)`，`lib/hal/HalEnvSensor.cpp:50`）。改为页面 `onEnter()` 读一次 + 之后每 30 s 刷新，结果存 12 B 缓存。

### 5.2 按键

| 键 | 行为 |
|---|---|
| `Confirm` | 刷新天气（需 Wi-Fi，走 `WifiSelectionActivity` 回调；已新鲜则直接重画） |
| `Left` / `Right` | 切换月历视图 / 今日视图 |
| `Up` / `Down` | 月历视图里翻月份 |
| `Back` | 退出 |

### 5.3 月历视图（「万年历」的本体）

7 列 × 6 行网格，每格约 100×56 px：公历数字 + 农历小字（初一显示月名，节气日显示节气），今天反白。**已确认列入 M3**：它才是「万年历」的主体，今日视图只是它的默认页。渲染细节：

- 网格用 `GUI.getMetrics()` 行高推格子尺寸，不硬编码 480/800；只有 `Left/Right` 翻月时重绘整页（e-ink 全刷 1-2 s 可接受，不做局刷）。
- 农历小字用 `SMALL_FONT_ID`；中文界面才画（非中文只画公历与天气）。
- 跨年边界（12 月→1 月、1 月→上年 12 月）必须 `switch` 雨露均沾，这是最容易写错的一处。

### 5.4 日期来源：必须扩 `HalClock`

- 新增 `bool getDate(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& weekday) const`，读 PCF8563 `0x05..0x08`（BCDNAM/世纪位），校验 BCD 合法性。
- `writeTimeToRTC` 扩成 `writeDateTimeToRTC`，`syncFromNTP()` 一并写日期（否则 NTP 校时后日期仍是旧值）。
- 日期无效（VL 位置位 / RTC 缺失）时页面顶部显示 `请联网校时` 提示，黄历块显示 `—`，不编造。

### 5.5 农历 / 节气 / 建除（新增 `lib/Almanac/`）

| 项 | 实现 | 体积 |
|---|---|---|
| 农历 | 1900–2100 压缩表（201 × `uint32`，标准 lunarInfo 布局：闰月 + 月大小 + 闰月大小） | 804 B `.rodata` |
| 节气 | 经典 24 项分钟偏移表（24 × `int32`）+ 1900-01-06 02:05 基准 | 96 B |
| 干支 | 由公历天数对 60 取模（以已知甲子日为锚点），年月干支按节气/农历年推导 | 0 |
| 建除十二神 | 日支与月支之差 → 12 值神；每个值神带固定宜/忌短词表（12 × ~40 B） | ~600 B |
| 生肖 | 年支索引 12 项字符串 | 24 B |

参数化纯函数 + host 单测（`test/almanac`），用 2026 年春节、闰月年、节气日与已知甲子日做基准断言。

**诚实边界**：宜/忌只做**建除十二神**这一层，不是通书全量神煞。文档与 UI 都不宣称"传统黄历全量"。

### 5.6 天气：relay 端点 + 设备缓存

**relay 新增 `GET /api/v1/weather?days=5`**（device token 鉴权，与 markets 同款）。定位完全在服务端完成，设备只拿结果：

1. **取访问方公网 IP**：直接读 `request.client.host` 即可 —— 随附部署里 nginx 已写 `X-Forwarded-For` / `X-Real-IP`（`relay/deploy/nginx-voice-relay-dev.conf:26-27`），uvicorn 绑定 127.0.0.1 且 `--proxy-headers` 默认开启、信任 127.0.0.1（`relay/deploy/reading-companion-relay.service:12`），所以 Starlette 拿到的 `client.host` 已经是被 nginx 换过的真实客户端地址，**不需要手解 XFF**。手解仅作为“有人把 relay 直接暴露在公网/换了反向代理”的兼容分支。
2. **私网回退**：若该地址是私网/回环（relay 与设备在同一局域网部署），改用 relay 自己的出口公网 IP —— 此时"设备所在地"= "relay 所在地"，这就是 IP 定位的物理边界。
3. **地理编码**：`ip-api.com/json/<ip>` → 得到城市名与经纬度；失败则回退 `ipinfo.io/<ip>/json`。两家对同一 IP 的城市判定会不一致（本项目历史记录里出现过设备在常州、ip-api 判南京），所以**只信返回值、不做二次修正**。
4. **显式覆盖**：若设置了 `WEATHER_LAT` / `WEATHER_LON`，跳过 1-3（给专线/固定点位留后门）。

上游天气：open-meteo（免费、无 key）`daily=temperature_2m_max,temperature_2m_min,weather_code,precipitation_probability_max` + `current=temperature_2m,weather_code`。

缓存：天气按**解析后的城市键**缓存（不是按 IP，避免同城多 IP 反复解析），`WEATHER_TTL_SECONDS=21600`（6 h）；地理结果单独存 24 h 小表（内存 ≤ 8 条）。上游失败返回上次缓存并标 `stale:true`。

隐私：IP 仅用于本次地理解析，不落盘、不写入日志正文（只记 `geo: ok/failed` 与耗时）。

响应（紧凑 JSON，设备侧 `lib/JsonParser` 直解）：

```json
{"city":"南京","stamp":1763270400,"stale":false,
 "current":{"t":21,"code":3},
 "days":[{"d":"2026-11-16","code":2,"hi":21,"lo":13,"rain":10}]}
```

`city` 是 IP 库返回的地级市名，UI 只显示"城市 · 温度"，不承诺精确到区县；解析失败时 `city:""`，页面该处改为显示"本地"。

**设备侧 `lib/Weather/WeatherClient`**：GET + 解析 + 写 SD 缓存 `.crosspoint/weather.bin`（带 stamp 与 city）。页面进入时先画缓存（离线也有内容），再按 6 h 新鲜度决定是否提示可刷新。

### 5.7 改动清单

| 文件 | 改动 |
|---|---|
| `lib/hal/HalClock.h/.cpp` | 日期读/写；NTP 同步写日期 |
| `lib/Almanac/Almanac.{h,cpp}` | 新增：农历/节气/干支/建除纯函数 |
| `lib/Weather/WeatherClient.{h,cpp}` | 新增：HTTP + 解析 + SD 缓存 |
| `src/activities/calendar/CalendarActivity.{h,cpp}` | 新增：今日视图 + 月历视图 |
| `src/activities/ActivityManager.h` | `HomeMenuItem::CALENDAR` + `goToCalendar()` |
| `src/activities/home/HomeActivity.h:42,65` | 两张索引映射各加一行 |
| `src/activities/home/HomeActivity.cpp:25` | `buildMenu()` 加 tile（label + icon） |
| `src/components/themes/BaseTheme.h` | 若需新图标，`UIIcon` 加一项（或复用 `Market`/`Library`） |
| `relay/relay/weather.py` | 新增：IP 地理编码（ip-api → ipinfo）+ open-meteo 拉取 + 城市键缓存 |
| `relay/relay/main.py` | 新增 `/api/v1/weather` 路由 |
| `relay/relay/config.py` | `WEATHER_LAT/LON`（可选覆盖）、`WEATHER_TTL_SECONDS` |
| `relay/deploy/*` | 新 env 写入镜像与安装脚本；部署文档补一句“若不经 nginx 直接暴露，需自行确认客户端 IP 可用” |
| `relay/tests/test_weather.py` | 新增：XFF 取值、私网回退、geo 失败、上游失败回退缓存、城市键缓存命中 |
| `test/almanac/`、`test/weather_format/` | host 单测 |

资源：农历表 + 宜忌表 ≈ 1.6 KB `.rodata`（Flash，0 DRAM）；页面运行期缓存 12 B + 天气 5 天 60 B；无新增常驻任务。

---

## 6. M4：朗读与发音（edge-tts over WebSocket）

### 6.1 硬约束（决定协议形状）

- **固件没有音频解码器**：`lib/` 只有 PNG/JPEG 转换器，`HalAudio::writePcm()` 只吃 `int16_t*`（`lib/hal/HalAudio.h:40`），采样率固定 16 kHz（`HalAudio.h:20`）。
- **edge-tts 只能输出 MP3**：其 `outputFormat` 硬编码为 `audio-24khz-48kbitrate-mono-mp3`（edge-tts 上游 `communicate.py`，官方讨论 #252 明确"API 不支持更好的格式"）。
- 所以：**relay 负责 MP3 解码 + 24 kHz → 16 kHz 重采样**，向设备只送裸 PCM。这是唯一不新增设备端解码器的路径。
- 实现用 **pip 包 `miniaudio`**（自带解码器），而不是 ffmpeg：没有系统二进制依赖，`requirements.txt` 一行即可。实测中文句 2.9 s 音频从合成到 PCM 共 1.5 s。

### 6.2 WS 协议（新增 `/api/v1/tts`）

```
设备 → {"type":"tts.start","id":7,"text":"…","lang":"zh"}
relay → {"type":"tts.meta","id":7,"rate":16000,"ch":1,"bits":16,"voice":"zh-CN-XiaoxiaoNeural"}
relay → <binary, s16le, 8 KB/帧>
relay → {"type":"tts.end","id":7,"ms":8420}
relay → {"type":"tts.error","id":7,"code":"bad_text|bad_request|upstream_failed|upstream_timeout|decode_failed|unavailable"}
```

（以上为已上线实现；`tts.meta` 额外带 `truncated` 布尔，用于告知设备文本被 `TTS_MAX_CHARS` 截断。）

- relay 侧把长文本按句号/问号/换行切成 ~60 字符段（`TTS_SEGMENT_CHARS`），逐段 `edge_tts.Communicate.stream()` → `miniaudio.decode(16 kHz/单声道/s16le)` → 8 KB 帧。合成与发送用 2 段流水线重叠，段间无声；**实测首帧 1.3–2.1 s**。
- 音色：`TTS_VOICE_ZH`（默认 `zh-CN-XiaoxiaoNeural`）、`TTS_VOICE_EN`（默认 `en-US-AriaNeural`，美音女声；可选 `en-US-GuyNeural`）。
- 单次上限：`TTS_MAX_CHARS=1200`（一页中文 ≈ 600-900 字，够用；超出截断并在 `tts.end` 标 `truncated`）。

### 6.3 设备侧播放器 `lib/VoiceRelay/TtsPlayer.{h,cpp}`

- 复用 `WebSocketsClient`（`platformio.ini:73` 已有）+ 现有 CA bundle 与鉴权头范式（`VoiceRelayClient.cpp:103-127`）。**新增二进制收帧路径**（现有实现忽略二进制，`VoiceRelayClient.cpp:262`）。
- PSRAM 环形缓冲 **64 KB**（16 kHz × 2 B = 32 KB/s，即 2 s 抖动余量）；短音（单词/单字）用 32 KB 足够。
- **不用任务**：`poll()` 在 activity 的 `loop()` 里推进 —— WS `loop()` 非阻塞，每次从环取 ≤512 样本喂 `AUDIO.writePcm()`，UI 保持响应。符合本项目"能不用任务就不用"的惯例。
- 功放：首帧到 `setEnabled(true)`；播完/停止后 `drain()` + `setEnabled(false)`；`stop()` 幂等，`onExit()` 必调。
- 断流：3 s 无新帧 → 播完缓冲即停，报 `网络中断`，不自动重连。

### 6.4 朗读当前页

新增 `src/activities/reader/TtsActivity.{h,cpp}`（从阅读器菜单 `EpubReaderMenuActivity::MenuAction` 进入，携带 book/spine/page，与 `AskAiActivity` 同一套 `setBookContext` 范式）：

1. `onEnter`：先画"准备朗读…"帧；用抽出的共享 helper（从 `AskAiActivity::extractPageLines()` 提取，`AskAiActivity.cpp:136-217`）把当前页文本取进 PSRAM 池。
2. 文本清洗：去连字符换行、压缩空白、按句切分（与 relay 的切分规则一致，双端幂等）。
3. `tts.start` → 收 PCM 播放；`Confirm` 暂停/继续，`Back` 停止退出。
4. `preventAutoSleep()` = true；退出恢复。
5. 降级：relay 未配置 → 提示；无 Wi-Fi → 走 `WifiSelectionActivity`；失败 → 显示 relay 返回的 `code`，不静默。

限制（明确不做）：不跨页续播、不后台播放、不做倍速（PCM 变速需要重采样，M4 不做）。

### 6.5 三处发音

统一走 `TtsPlayer::speakShort(text, lang)`（一次取完，≤32 KB PSRAM），**卡片就绪即自动发音**，不做按键绑定 —— 三个界面的按键表已经排满（`DictionaryActivity.cpp:205-221`、`ReviewActivity.cpp:355-402`），再插键必然冲突，而"查/看即读"符合查词直觉：

| 位置 | 触发点 | 音色 |
|---|---|---|
| 查字典（汉字卡） | `DictionaryActivity` 卡片落定后读 `heroChar_` | `zh-CN-XiaoxiaoNeural` |
| 查单词（生词卡） | `WordCardActivity` 卡片落定后读 `word_` | `en-US-AriaNeural` |
| 复习 | `ReviewActivity::reveal()` 翻面时读 `currentWord()` | `en-US-AriaNeural` |

设置 `SETTINGS.autoPronounce`（默认 `ON`）可关。这几处都在阅读/学习路径上，一次 1-2 s 的音频约占 30-60 KB 流量，局域网可忽略。

### 6.6 改动清单

| 文件 | 改动 |
|---|---|
| `relay/relay/tts.py` / `weather.py` 等 | ✅ **已完成并上线**（见 §10） |
| `relay/relay/main.py` | ✅ 新增 `/api/v1/tts` WS 路由 + `/api/v1/weather` 路由 |
| `relay/relay/config.py` | ✅ `TTS_*` / `WEATHER_*` 配置 |
| `relay/requirements.txt` | ✅ `edge-tts`、`miniaudio`（无系统二进制） |
| `relay/deploy/nginx-*.conf` | ✅ 新增 `/api/v1/tts` 与 `/api/v1/weather` 代理块 |
| `lib/VoiceRelay/TtsPlayer.{h,cpp}` | 新增：WS 收流 + PSRAM 环 + 播放 |
| `lib/VoiceRelay/TtsCodec.{h,cpp}` | 新增：帧解析（host 可测） |
| `src/activities/reader/TtsActivity.{h,cpp}` | 新增 |
| `src/activities/reader/ReaderPageText.{h,cpp}` | 新增：从 AskAi 抽出的共享页文本提取 |
| `AskAiActivity.cpp:136` | 改为调用共享 helper（行为不变） |
| `src/activities/reader/EpubReaderMenuActivity.cpp:47` | 菜单加 `READ_ALOUD` |
| `DictionaryActivity.cpp` / `WordCardActivity.cpp` / `ReviewActivity.cpp` | 卡片落定点调 `speakShort` |
| `test/tts_codec/` | 帧解析单测 |

内存：PSRAM 64 KB（朗读）/ 32 KB（发音）；内部 DRAM 增量 ≈ WS 客户端 + 4 KB 环形索引，无新任务。功耗：Wi-Fi 常开 + 喇叭约 80-150 mA，是阅读的 3-6 倍；TTS 开始前弹出实测提示（已有 `drawPopup`）。

---

## 7. 横切工程项

1. **i18n**：只改 `lib/I18n/translations/*.yaml`。新增键：`STR_CALENDAR`、`STR_CAL_TODAY`、`STR_CAL_MONTH`、`STR_LUNAR`、`STR_GAN_ZHI`、`STR_SOLAR_TERM`、`STR_NEXT_TERM`、`STR_ZODIAC`、`STR_JIAN_CHU`、`STR_YI`、`STR_JI`、`STR_WEATHER`、`STR_INDOOR`、`STR_TEMPERATURE`、`STR_HUMIDITY`、`STR_REFRESH_WEATHER`、`STR_WEATHER_STALE`、`STR_SET_TIME_HINT`、`STR_SOUND_FEEDBACK`、`STR_AUTO_PRONOUNCE`、`STR_READ_ALOUD`、`STR_TTS_PREPARING`、`STR_TTS_PLAYING`、`STR_TTS_PAUSED`、`STR_TTS_FAILED`、`STR_NEED_WIFI`。中文/英文必填，其余语言回落英文。用户可见文本全部 `tr()`；节气名/宜忌词条走 `Almanac` 里的 `const char*` 表（中文界面专用，不进 i18n）。
2. **设置持久化**：`CrossPointSettings` 字段 + `SettingsList.h` 条目 + `JsonSettingsIO`（按 `SettingsList` 遍历，通常免改）三处齐改。本轮三个：`soundFeedback`、`autoPronounce`、（若加城市）`weatherCity`。
3. **自动休眠**：加速度计活动必须计入 `main.cpp:904` 的活跃判断，否则甩设备不重置休眠计时。
4. **I2C 串行化**：SHT30 / MSA311 / PCF8563 共用 `Wire`，三者都只在主循环调用，天然串行，不加锁；但 `HalEnvSensor::read()` 的 `delay(20)` 必须落在 30 s 节流内，绝不进渲染路径。
5. **缓存版本不动**：以上功能不改 `book.bin`(v7) / `section.bin`(v25)，不需要清 `.crosspoint/`。只新增 `.crosspoint/weather.bin`（独立文件，带 stamp，版本不匹配直接丢弃重抓）。
6. **不要静态 `std::string`**：状态栏之外的文本构造一律 `snprintf` 到栈缓冲（本计划新增的渲染代码都受此约束）。
7. **构建门禁**：`pio run -e onepage_s3` 与 `pio run -e onepage` 都要过 —— 新代码在非 S3 目标上必须退化为"不可用"而不是编译失败（跟 `HalEnvSensor.cpp` 的 `#else` 分支同款）。

---

## 8. 验证清单

**我可完成（无需硬件）**

- `pio run -e onepage_s3` / `-e onepage` 零错误零警告
- `./bin/clang-format-fix` + `pio check`
- host 单测：`test/almanac`（农历/节气/干支/建除，用已知日期断言）、`test/weather_format`（JSON 解析 + 缓存往返 + 脏数据拒绝）、`test/tts_codec`（帧切分 + 环缓冲边界 + 截断）
- 四个方向的 `switch/case` 覆盖检查（倾翻轴向、月历格布局）
- relay：`pytest relay/tests` —— ✅ 已全部通过（248 例，其中 weather 24 例、tts 20 例）。weather 的 ip.sb/ipinfo/ip-api/open-meteo 全部实现 monkeypatch，覆盖：XFF 解析、私网回退、地理编码顺序与全失败、`stale` 回退、城市键缓存、固定坐标；tts 的 edge-tts 与 MP3 解码同样打桩，覆盖：分段、帧上限、流水线顺序、生产端失败上抛、截断标记、四种错误码与同连接多请求。
- 串口新命令：`CMD:WEATHER`（打印设备拿到的天气）、`CMD:TTS <text>`（点播一次朗读），与现有 `CMD:IMU` / `CMD:SENSORS` 同款

**需真机（交付后由你验证）**

- `CMD:IMU` 看倾翻读数，调阈值；4 方向各 20 次手势
- 状态栏/月历在 4 个方向下不重叠
- 按键音无爆音、关设置后功放断电（万用表测 AUDIO_EN）
- SHT30 与独立温湿度计偏差 ≤ 3 °C
- ✅ 已在生产机验证：天气 200（首次 2.8 s、缓存命中 0.7 ms）、XFF 定位到杭州、WS `tts` 经 nginx 升级正常、中文 6.19 s PCM / 首帧 2.06 s、英文 1.85 s / 1.49 s
- 断电重启后 RTC 日期正确（电池在位）

---

## 9. 已确认项（v2.1，无待决问题）

1. **天气地点 = 当前 IP 所在地**：relay 用访问方公网 IP 做地理编码（私网回退到 relay 出口 IP），设备无城市设置项、无城市表、无选择器 Activity —— 节省约 2 人天与 3.9 KB Flash。
2. **万年历含月历视图**：7×6 网格 + 翻月，计入 M3（+1 d）。

两个已知限制（不阻塞开发，但交付时要当面说清）：

- **IP 定位只到地级市**，中小城市常被判到邻近地级市；无城市选择器则用户无法自行纠正。若后续反馈“城市不对”，补救方案是加一个 `WEATHER_LAT/LON` 环境变量覆盖（已预留，零设备侧改动）。
- **relay 与设备同一局域网时**，定位结果是出口网络的地址而非设备实际位置——LAN 部署本来也就在同一地点，可接受。

---

## 10. 服务端交付状态（2026-11-16）

**已上线**：`118.25.20.250`，release `20261116-133333`（`current` 软链已切），systemd `active`，`/readyz` 报 `weather: ip`、`tts: configured`。

| 项 | 内容 |
|---|---|
| 新增代码 | `relay/weather.py`（IP 定位 + open-meteo + 城市键缓存）、`relay/tts.py`（edge-tts → miniaudio → 16 kHz PCM，分段流水线） |
| 改动 | `main.py`（两条路由 + 启动期初始化 + 能力位）、`config.py`（`WEATHER_*` / `TTS_*`）、`metrics.py`（`weather_requests_total`、`tts_requests_total`、`tts_first_audio_ms`）、`requirements.txt` |
| 测试 | `tests/test_weather.py`（24）、`tests/test_tts.py`（20）；全仓 `pytest` **248 passed** |
| nginx | `sites-available/reading-companion-relay.conf` 新增 `/api/v1/weather`、`/api/v1/tts`（`Upgrade`/`Connection` + `proxy_buffering off` + 600 s 读写超时）；旧配置备份在 `/root/reading-companion-relay.conf.bak.*` |
| 依赖 | 只走 pip：`edge-tts 7.3.1`、`miniaudio 1.73`（**不需要 ffmpeg**） |
| 环境变量 | 无需新增（默认值即可）。可选：`WEATHER_LAT`/`WEATHER_LON`/`WEATHER_CITY`、`TTS_VOICE_ZH`/`TTS_VOICE_EN` |

**实机实测**（从服务器本地发起，经 nginx `:8080` 与直连 `:8000` 各一次）

- `GET /api/v1/weather`：首次 2.81 s、城市 `Shanghai`（请求来自本机，走出口 IP）；带 `X-Forwarded-For: 223.5.5.5` 时为 `Hangzhou`；第二次同城请求 **0.7 ms**（缓存命中）。
- `WS /api/v1/tts`：中文四句 → `tts.end ms=6192`、首帧 **2.06 s**；英文 `dictionary` → 1848 ms、**1.49 s**；`bad_text` 错误码正常返回。

**回滚**：`sudo ln -sfn /opt/reading-companion-relay/releases/20261112-155022 /opt/reading-companion-relay/current && sudo systemctl restart reading-companion-relay`

**设备端接入点（下一步）**：`http://118.25.20.250:8080/api/v1/weather`（GET，带设备 token）与 `ws://118.25.20.250:8080/api/v1/tts`。

---

## 11. 设备端 M1/M2 交付状态（2026-11-16，真机验证）

**已烧录到 V3.1 实机**（`env:onepage_s3`，串口 `115200`），两项均通过串口日志实测。

| 项 | 实现 |
|---|---|
| 倾翻门面 | `lib/hal/HalTiltGesture.{h,cpp}`（单例 `TILT`）：X3 透传 `HalTiltSensor`，S3 走加速度计角度状态机 |
| 音效 | `src/util/UiSound.{h,cpp}`：`key()` / `pageTurn()` / `alert()` / `poll()` / `shutdown()` |
| 挂点 | `src/main.cpp` 主循环（按键音，位于串口/BLE 注入之后）、`EpubReaderActivity::pageTurn()`、`TxtReaderActivity`/`XtcReaderActivity` 翻页分支、`PomodoroAlertActivity::blinkAlert()`、`enterDeepSleep()` |
| 翻页键归属 | `ActivityManager::isTopReaderActivity()`：只有**栈顶**是阅读器时方向键才归翻页音所有（阅读器菜单、字典、生词卡仍有点击音） |
| 设置 | `CrossPointSettings::soundFeedback`（默认关）+ `SettingsList` 控件 + `STR_SOUND_FEEDBACK`（中/英/繁体） |

### 真机踩到的两个坑（已验证，勿回退）

1. **`i2s_channel_enable()` 在 10 MHz 下永久阻塞**。实测：主循环空闲 2 s 后 CPU 降到 `LOW_POWER_FREQ`，此时 `i2s_channel_enable()` 打印到 "enabling channel" 后再无返回，设备彻底静默（同一次调用在 240 MHz 下立刻返回）。I2S 时钟源是 PLL_160M，10 MHz 设置不会保住它。`UiSound` 因此从第一个音到功放尾巴结束都持有 `HalPowerManager::Lock`（与麦克风/SPKMAX 自检同一套约定）。**M4 的 PCM 收流必须整段持锁**，否则同样会卡死。
2. **音量 40%**。MAX98357A 增益由引脚跳线固定，音量只能调 PCM 峰值。首轮真机试听 15% 满幅（5000/6000）偏吵，先按 60% 试听仍偏吵，最终定 `VOLUME_PERCENT=40`（→2000/2400）；后续 TTS 播放也应沿用这个比例。

### 已实测（串口证据）

- 开机：`[TILT] accelerometer tilt gestures enabled`、SHT30/RTC 正常，无异常复位。
- 设置页「按键」标签出现「倾斜翻页」「按键音效」，切换为「开」后写入 `settings.json`，重启后仍为开。
- 按键：`[IN] press edge` → `[SND] 1250 Hz 16 ms amp=1 queued=1` → 140 ms 后 `[SND] amp off`（功放断电）。
- 阅读器翻页：按键只产生 **880 Hz 翻页音**一次（1250 Hz 点击音被抑制），回翻同样。
- `CMD:IMU` 读数稳定（X≈20 / Y≈130 / Z≈1005 mg），阅读器内静置 30 s 无 `[TILT] gesture` 误触发。

### 待人手验证

- 四个方向各 20 次甩动的手感/误触发率（阈值 `RATE_TRIGGER_DPS=170`、`ANGLE_TRIGGER_DEG=22`）。
- 真实听感（音量 40% 是否合适、翻页音是否与刷新同步）。

### 调试入口（本轮新增）

`CMD:SOUNDTEST`（绕过设置门控依次播放三种音并打印门控值）、`CMD:GO_SETTINGS`（跳过首页网格直达设置）。原有 `CMD:KEY <back|ok|left|right|up|down|power>` 可无手驱动 UI。

---

## 12. M3 万年历交付状态（2026-11-16，真机验证）

**已烧录到 V3.1 实机**（`env:onepage_s3`），今日视图 / 月历视图 / 室内温湿度均已真机截图确认。

| 项 | 实现 |
|---|---|
| 日期 | `HalClock::getDateTime()` 读取 PCF8563（UTC 存储）；`getDate()` 沿用；新增 `setDateTime()` 供手动校时 |
| 农历 | `lib/Almanac/Almanac.{h,cpp}`：HKO 1900–2100 `LUNAR_INFO` 表、天文算法节气（视太阳黄经 + 二分 + UTC+8）、干支年/月/日柱、生肖、建除十二神 + 宜/忌 |
| 天气 | `lib/Weather/WeatherClient.{h,cpp}`：relay `GET /api/v1/weather?days=5`，缓存 `/.crosspoint/weather.bin`（magic `WXR1`） |
| 室内 | `HalEnvSensor`（SHT30）30 s 节流，仅当读数变化 >0.2 °C / 2 %RH 才重绘 |
| 页面 | `src/activities/calendar/CalendarActivity.{h,cpp}`：今日视图（农历 + 干支 + 节气 + 建除宜忌 + 室内 + 5 日预报）/ 月历视图（周一开头、节气与农历日、今日反色） |
| 入口 | 首页 tile `HomeMenuItem::CALENDAR` + `ActivityManager::goToCalendar()` |
| 时区 | RTC 存 UTC，界面按 `SETTINGS.clockUtcOffsetQ` 换算成本地日期后再算农历/干支（跨日会进位） |

### 真机踩到的坑（已验证）

1. **NTP 服务器要选国内可达的**。`configTzTime("UTC0","pool.ntp.org","time.nist.gov")` 在本网实测 5 s 超时（`[CLK] NTP sync timed out`）。改为 `ntp.aliyun.com` / `cn.pool.ntp.org` / `pool.ntp.org` 后 3 s 内完成：`[CLK] RTC set to 2026-11-16 07:53:53 UTC`。
2. **主循环里不能做阻塞式 Wi-Fi 连接**。临时加的 `CMD:WIFI`（`WiFi.begin` + 20 s 轮询）触发 `task_wdt: Task watchdog got triggered` 并复位；已删除该命令，Wi-Fi 走原有的 `WifiSelectionActivity`。
3. **relay 凭据在 NVS（`CloudConfig`），不在 SD 卡**。全新设备的 `CLOUD url= deviceId= tokenSet=0`，天气只会显示「未配置服务器」——功能正常，缺配置。
4. **`relayUrl` 必须写成 `ws://host:port`**。`VoiceRelay::parseRelayUrl()`（[lib/VoiceRelay/VoiceRelayCodec.cpp:1176](../lib/VoiceRelay/VoiceRelayCodec.cpp)）只接受 `ws://` / `wss://`，HTTP 客户端再从 `parsed.tls` 推导 `http(s)`。写成 `http://118.25.20.250:8080` 会解析失败，天气直接返回 `Result::Config`（表现为「未配置服务器」），但 `/api/v1/weather` 手工 curl 是 200——很容易误判成 token 问题。market / learning / pomodoro 三个客户端共用这段解析，同样受影响。

### 已实测（串口 + 截图证据）

- 手写 RTC 后进入万年历：农历十月初八 / 丙午年 己亥月 丙申日 / 生肖马 / 下一节气小雪 11/22 / 建除收 · 宜纳财捕捉入仓 · 忌开市出行安葬（与 tianqi.com 一致）。
- 月历视图：2026-11 与翻页到 2026-10（寒露 10/8、霜降 10/23、九月初一 10/10），今日（16 日）反色显示。
- 室内：`[ENV] SHT30 found: 20.2 C, 54 %RH`，页面 30 s 刷新且无明显闪烁。
- Wi-Fi：设置里已有 `Prefoco` 凭据，万年历按确认 → 自动连接（`-62 dBm`）→ NTP 写 RTC → 天气请求成功：`[WX] Changzhou 5 days, current 17 C, stale=0`；页面显示 `Changzhou 17°C 阴` + 5 行日期/天气/温度/降水概率。
- relay 直连校验（同一 token）：`GET /api/v1/weather?days=5` + `Authorization: Bearer` + `X-Device-Id` → 200，`city=Changzhou`。
- 截图：`docs/screenshots/m3-home-calendar-tile.png`、`m3-calendar-today.png`、`m3-calendar-today-weather.png`、`m3-calendar-month.png`、`m3-calendar-month-oct.png`、`m3-settings-keys.png`。

### 待完成

- 琴键/侧键的按键音（40%）、倾翻手势仍需人手确认手感。
- 今日视图下半屏仍空（480×800 里内容只占约一半），可考虑补 3 日星期行或大字号时钟。

### 调试入口（本轮新增）

`CMD:GO_CALENDAR`（直达万年历）、`CMD:SETDATE yyyy-mm-dd HH:MM[:SS]`（无网手写 RTC，验证日期相关界面）、`CMD:CLOUD`（打印 relay 配置，token 只报存在性）、`CMD:DEVICEID <id>`（写 `X-Device-Id`）、`CMD:RELAY <url>|<token>`（写 relay 端点与 token，走 NVS；url 用 `ws://host:port`）。
