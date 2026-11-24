# 任务记录：RH-01 阅读会话追踪（2026-11-22）

需求依据：[v4.0 开发计划](../v4.0-development-plan.md) §3.2 RH-01；[OnePage 软件功能需求文档 v4.0](../../docs/OnePage%20软件功能需求文档v4.0%20版.md) §3.3 / §4.2。

状态：**完成（EPUB + TXT），RH-01 新增 19 个主机用例（当前合并套件 373 passed），`onepage_s3` / `onepage` 构建通过；2026-11-24 真机走查 5/6 项通过（朗读自动翻页未覆盖，见下）。**

## 目标

在阅读器内记录“有效阅读时长”和“向前翻页数”，追加写入 SD 卡 JSONL，为 RH-02 每日聚合与 RH-03 热力图提供数据源。

边界：

- 设备端只做计时与落盘，不做聚合、不做热力图（RH-02/03）。
- 不做后台常驻任务；所有写入都在阅读活动的前台生命周期内完成。
- 时钟未同步（1970）时不写记录，避免把会话归到 1970-01-20。
- 每次会话只在结束时写一行，不做逐页写盘。

## 实现

### 会话状态机

`lib/ReadingHistory/ReadingSessionTracker.h`（纯头文件，主机可测）：

- `start(now)`：时钟低于 sane floor（`1600000000`）时保持 inactive；否则开始计时。
- `noteActivity(now)`：任意按键按下/释放都为活动中的会话续期；会话被空闲关闭后，单独的按键不会开启新会话（避免“按一下退出”产生 1 秒假记录）。
- `notePageAdvance(now)`：向前翻页 +1，并续期；空闲关闭后第一次向前翻页会开启新会话（覆盖朗读自动翻页）。
- `tick(now)`：距最后一次活动 ≥ 90 s 时关闭会话，记录的结束时间是 `lastActivity + 90 s`，不是发现超时的时刻，避免主循环卡顿放大时长。
- `finish(now)`：离开阅读器或压入菜单时按当前时间关闭；零长度会话返回 false，不写盘。
- 时钟回拨（SNTP 校正）只重设活动标记，不产生负时长；时钟跌回 1970 时 `tick()` 忽略，不会把会话起点改到 1970。

### 接入点

EPUB（`src/activities/reader/EpubReaderActivity.cpp`）：

- `onEnter()` 启动会话；`loop()` 每帧处理输入续期与空闲关闭。
- `onPause()`（菜单 / AI / 语音等子活动压栈）关闭会话；返回后的第一帧重新开始，菜单时间不计入阅读。
- `onExit()` 兜底关闭；`finish()` 幂等，push+pop 不会重复写。
- `pageTurn(true)` 只在页面确实前进时计数：章节内前进、跨章前进、长按跳章前进；后退不计。朗读自动翻页和自动翻页都走同一条路径。
- 章末 AI 邀请条：`continuePastChapterInvite()` 也是向前跨章，显式调用 `notePageAdvance`；否则读者点“继续”跨章时会漏计 1 页。
- 朗读期间每个主循环都续期：TTS 一页可能朗读数分钟，不能因为无按键就在 90 s 处切断会话。

TXT（`src/activities/reader/TxtReaderActivity.cpp`）：

- `initializeReader()` 在渲染任务里建页索引，完成后用原子标志通知主循环；会话从“第一页已就绪”开始，索引构建时间不计入阅读。
- 翻页计数在主循环 `currentPage++` 分支；`onPause()` / `onExit()` 与 EPUB 相同。

### 落盘格式

`/.crosspoint/history/YYYY-MM-DD.jsonl`，一行一条会话，按会话开始日归档：

```json
{"schema_version":1,"ts_start":"2026-11-22T15:30:00+08:00","ts_end":"2026-11-22T15:52:30+08:00","book_id":"/Books/山间书店.epub","position":{"spine":0,"page":4},"duration_sec":1350,"pages_advanced":12}
```

- `book_id` 在 V1 使用设备侧 SD 路径作为书键；后续 RH-02 可换成 AI 书库的 `sha256[:16]`，或在记录中补 `book_path` 做兼容。
- `position` 用 `spine + page` 代替 v4.0 的 CFI（当前引擎没有 CFI）。
- 时间戳按 `SETTINGS.clockUtcOffsetQ` 输出本地时间与偏移。
- 字符串转义复用 `ReadingSessionFormat::escapeJsonString`；路径超长/截断时整条丢弃并 `LOG_ERR`，不写半条 JSON。

### 时钟来源

RH-01 读 `time()`，而 ESP32 启动时 `time()` 为 0，原先只有 NTP 会设置它。现在 `lib/hal/HalClock.cpp` 的 `seedSystemTimeFromRTC()` 在启动和 `setDateTime()` 成功后把 RTC（UTC）写入系统时钟；RTC 低于 `1600000000` 时拒绝播种。联网后 NTP 仍可校正，时钟回拨由状态机重设活动标记处理。

## 验证

主机：

```bash
cmake --build build/test && ctest --test-dir build/test --output-on-failure -j
# 373 passed（RH-01 新增 19 例：ReadingSessionTracker 13 + ReadingSessionFormat 6；
# 其余为 RH-02/03、RP-01/02 与既有套件）
```

固件：

```bash
pio run -e onepage_s3   # SUCCESS
pio run -e onepage      # SUCCESS
```

资源（`onepage_s3`）：

- RH-01 单独增量：Flash 5,525,763 B（+1,392 B，相对 v1.11.0 基线 5,524,371 B），静态 RAM 139,488 B（不变）；会话状态机只有 3 个整数 + 1 个 bool，无堆分配。
- 当前未提交工作树合并构建（含 RH-02/03、RP-01/02、时钟修复、`CLOCK_CLEAR`）：Flash 5,561,975 B / 6,553,600 B（84.9%），静态 RAM 141,344 B / 327,680 B（43.1%）。

代码质量：

- `./bin/clang-format-fix -g` 通过。
- `pio check -e onepage_s3`：**PASSED**，0 high / 0 medium（69 条 low 为既有风格告警）。

## 真机验证（2026-11-24，`onepage_s3`）

RTC 已设为 2026-11-24；系统时钟由 `HalClock::seedSystemTimeFromRTC()` 在启动时从 RTC 播种，因此离线也能落盘。设备端口 `/dev/ttyACM0`。

| # | 检查项 | 结果 | 证据 |
|---|---|---|---|
| 1 | EPUB 连续向前 5 页 | ✅ | `{"ts_start":"2026-11-24T07:53:33+08:00","ts_end":"2026-11-24T07:53:55+08:00",...,"duration_sec":22,"pages_advanced":5}`，position `spine=67 page=13`，`book_id="/Rust 程序设计语言 简体中文版.epub"` |
| 2 | 停留 >90 s 后翻页切分 | ✅ | 第一条 `08:47:48→08:49:33`（105 s，1 页），结束时间 = 最后一次活动 `08:48:03` + 90 s；第二条 `08:49:40→08:49:43`（3 s，1 页）由下一次向前翻页重启 |
| 3 | 阅读菜单停留不计时 | ✅ | `onPause` 写入 `08:00:54→08:01:02`（8 s，1 页）；菜单停留 14 s；返回后 `08:01:16→08:01:20`（4 s，0 页） |
| 4 | TXT 翻页与索引构建排除 | ✅ | 58,799 B / 86 页；索引构建 `[34297]→[40883]` 6.6 s；首页 `[42319]`；记录 `17:23:40→17:24:02`（22 s），`pages_advanced=5`，position `page=5` |
| 5 | 时钟未同步不落盘 | ✅ | `CLOCK_CLEAR` → `time()=0`；`RH_DUMP no sane clock`；翻 2 页后退出无 `session saved`，文件保持 2,866 B；`SETDATE` 恢复后文件不变。另：RTC 设 2001 并复位后启动日志为 `RTC time not sane (2001-01-01), system clock not seeded` |
| 6 | 朗读自动翻页 | ⚠️ 未覆盖 | 需要 relay TTS 音频并等待整页朗读（数分钟）。代码路径已核对：朗读期间每帧 `noteActivity` 续期（`EpubReaderActivity.cpp:1230`），自动翻页走 `pageTurn(true)` → `notePageAdvance` |

### 走查中发现并修复的问题

1. **章末 AI 邀请条跨章不计页**：`continuePastChapterInvite()` 只推进 spine，没有调用 `notePageAdvance`，读者点“继续”跨章时少计 1 页。已在 `src/activities/reader/EpubReaderActivity.cpp:2017` 补上计数；用 15 次章内翻页 + 1 次邀请条跨章验证 `pages_advanced=16`。
2. **离线系统时钟停留在 1970**：`time()` 启动时为 0，只有 NTP 才设置，导致 RTC 已设置时仍不写历史。已在 `lib/hal/HalClock.cpp` 增加 `seedSystemTimeFromRTC()`：启动和 `setDateTime()` 成功后从 RTC 播种；RTC 低于 `1600000000` 时拒绝播种。设备验证 `SETDATE 2026-11-23 23:53:00` 后日志出现 `System clock seeded from RTC`。
3. **验证工具**：新增 `CMD:CLOCK_CLEAR`（仅调试）把 `time()` 归零而不动 RTC。软件复位会保留 ESP32 的 RTC 计时域，仅把 RTC 设到 2001 再复位无法清空系统时钟，必须用该命令验证未同步路径。

### 未覆盖项

- 朗读自动翻页（上表第 6 项）：需要在设备上连接 relay 并等待整页 TTS。会话续期与自动翻页计数均复用已验证的 `noteActivity` / `notePageAdvance` 路径，风险低；后续做 TTS 回归时补测。

## 后续

- RH-02：按日聚合 `history/*.jsonl` 到 `.crosspoint/aggregate/<year>.json`，提供今日/累计时长、会话数、翻页数、按书分布。**已完成（2026-11-23，见 [RH-02/03 任务记录](2026-11-23-reading-history-rh02-rh03.md)）**。
- RH-03：12–16 周 4 级灰度热力图（全刷，首页统计入口）。**已完成（同上）**。
- 如果 RH-02 需要跨设备/跨书稳定身份，再在记录中补 `book_id=sha256[:16]`，保留 `book_path` 兼容旧文件。
