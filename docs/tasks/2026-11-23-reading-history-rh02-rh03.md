# 任务记录：RH-02 每日聚合 + RH-03 阅读热力图（2026-11-23）

需求依据：[v4.0 开发计划](../v4.0-development-plan.md) §3.2 RH-02/RH-03、§6 M3；[OnePage 软件功能需求文档 v4.0](../OnePage%20软件功能需求文档v4.0%20版.md) §4.3 / RH-03。

状态：**完成（RH-02 数据层 + RH-03 首页统计页），主机 357 passed，`onepage_s3` / `onepage` 构建通过，`pio check` PASSED，真机清单已验证（两项受硬件/设计限制不可达，见下）。**

## 目标与边界

- RH-02：把 `/.crosspoint/history/YYYY-MM-DD.jsonl` 的会话按日汇总（总时长、会话数、翻页数、按书分布），写入 `.crosspoint/aggregate/<year>.json`。
- RH-03：首页新增「阅读统计」入口，展示最近 12 周（84 天）的 4 级灰度热力图 + 今日 / 近 7 天 / 阅读天数汇总。
- 不做：后台常驻任务、逐页写盘、阅读中的任何提示；统计页只在阅读器之外打开，整页全刷。
- 时区：沿用既有 `SETTINGS.clockUtcOffsetQ`（`ClockOffsetActivity`），日界与 history/chat 文件命名共用 `ChatHistoryFormat::localEpochSeconds`，未新增设置项。

## RH-02 实现

### 触发与落盘

- `ReadingSessionLog::appendSession()` 在 history 行写完、**句柄关闭之后**调用 `updateDayAggregate(dayName)`。先关句柄是为了让聚合读到刚追加的行（SdFat 写缓冲可能压着最后一个扇区）。
- 聚合失败只记 `LOG_ERR`，不影响会话记录本身：aggregate 是派生缓存，删掉也能从 history 重建。
- 写入走 `YYYY.json.tmp` → `remove` → `rename`（与 `ProgressFile::writeAtomic` 同一模式）。最坏情况是短暂无聚合文件，统计页回退读 history，用户无感。

### 格式与流式合并

`lib/ReadingHistory/ReadingAggregateFormat.h`（纯 C++，主机可测）：

- 年文件为合法 JSON，**一天一行**：`{"schema_version":1,"year":...,"days":[{...},{...}]}`。
- `formatDayLine()` / `parseDayLineStat()` 负责单日记录的编解码；`addSession()` 负责按 `book_id` 合并并按 8 条上限截断。
- 书籍 id 以 history 里的 **转义原样**保存，避免 decode/encode 往返；超长 id 直接不入表并置 `books_truncated`，不存半截路径（两条长路径截在同一字节会被误合并）。

`lib/ReadingHistory/ReadingAggregate.cpp`（SD 层）：

- `LineReader`：256 B 读块 + 1.8 KB 行缓冲，流式逐行，**不把整年文件装进 RAM**；超长行整行丢弃而不是把截断行交给解析器。
- `upsertDayInYear()`：读旧年文件、按日期序插入/替换目标行、其余行原样拷贝、写 tail。单次会话结束只重写一次年文件（366 天 ≈ 73 KB 上限）。
- `loadDayStats(firstDay, count, out, fallbackToHistory)`：按年读聚合；缺聚合的日回退读对应 history（**只读**，避免打开统计页触发 84 次年文件重写）。

### 与原计划的偏差

- 原设想「月内常驻 PSRAM 缓存当前日聚合」：实际不需要。当前日的聚合每次会话结束时从当天 history 现算，history 日文件只有几 KB；常驻缓存反而多一份需失效的一致性状态。
- 书籍分布上限 8 条/天：一本书一天只会产生一条聚合项，正常用户单日书籍数远低于 8；上限保证单行 ≤ `DAY_LINE_CAP`(1792 B)，也限制年文件体积。

### 调试命令

`CMD:RH_AGG [YYYY-MM-DD]`（默认今天）：重建该日聚合并打印年文件（`RH_AGG_DUMP_BEGIN/END` + 字节数），真机验证不需要拔卡。`RH_DUMP` 与它共用新的 `dumpFileToSerial()`。

## RH-03 实现

### 数据

- `ReadingStatsActivity` 在 `onEnter()` 取 `time()`：低于 sane floor 时只显示「同步时钟后才能记录阅读时长」，不伪造日期。
- 否则以本地日索引计算当前周周一，窗口 = 该周向前 11 周（`heatmapFirstDayIndex()`），84 天；`loadDayStats()` 一次读入。
- 分级固定：`0` / `<15 min` / `15–40 min` / `>40 min`（`heatLevel()`，主机用例覆盖边界 900 s / 2400 s）。

### 渲染

- 竖屏 / 横屏都按 12 列 × 7 行自适应：单元格边长取「可用宽 / 12」与「可用高 / 7」的较小值，网格在剩余区域居中。
- 4 级灰度用已有能力：0 级空心 `drawRect`，1/2 级 `fillRectDither(LightGray/DarkGray)`，3 级实心 `fillRect`。静态页整页 `displayBuffer()` 全刷（不引入局部刷新）。
- 左侧 `STR_WD_MON..SUN`（既有 i18n 键）行标签；列顶在月份变化处标数字月份；底部图例 `0 / <15 / 15-40 / >40 min`。
- 汇总三行：今日（分钟 + 页数）、最近 7 天（分钟）、阅读天数（窗口内有数据的天数）。

### 首页入口

- `HomeMenuItem::STATS` + `ActivityManager::goToStats()`；`goHome()` 会把 `ReadingStats` 还原到该 tile。
- 首页从 9 个 tile 变 10 个。`gridColumnsFor(tileCount, gridHeight, metrics)` 先按 3 列试算（与 `drawHomeIconGrid` 同一条 `menuRowHeight + 28` 公式），只有 3 列放不下时才退到 4 列：BaseTheme 竖屏 4 行需 316 px、可用 310 px → 4 列；Lyra 竖屏 4 行 392 px < 497 px → 保持 3 列，避免窄格把「今天的词 N」截断。真机截图确认（见下）。
- 新 i18n 键：`STR_READING_STATS`、`STR_READING_LAST_7_DAYS`、`STR_READING_ACTIVE_DAYS`、`STR_READING_CLOCK_NEEDED`、`STR_NO_READING_DATA`、`STR_MINUTES_SHORT`、`STR_PAGES_SHORT`（英 / 简中 / 繁中，其余语言回退英文）。

## 验证

主机（新增 23 例，`test/reading_session/ReadingAggregateFormatTest.cpp`）：

```bash
cmake --build build/test && ctest --test-dir build/test --output-on-failure -j
# 357 passed（基线 334）
```

覆盖：热力分级边界、history 行字段解析（含 CJK 路径、空格、缺字段、溢出）、转义字符串原样保留 / 超长拒绝、按书合并与 8 条上限、单日行编解码往返、`books_truncated`、非法日期拒绝、日索引 ↔ 日期/文件名往返、周一起点与 12 周窗口边界。

固件：

```bash
pio run -e onepage_s3   # SUCCESS
pio run -e onepage      # SUCCESS
pio check -e onepage_s3 # PASSED，0 high / 0 medium
./bin/clang-format-fix -g ; clang-format --dry-run --Werror <新增文件>  # 通过
```

资源（`onepage_s3`）：

- Flash：5,536,719 B / 6,553,600 B（84.5%），相对 RH-01 后 5,525,763 B **+10,956 B**。
- 静态 RAM：139,488 B（42.6%），**不变**；运行期只在新 Activity 里多 84 个 `DayStat`（约 1.3 KB，随 Activity 一起释放），聚合路径按次分配 `DayAggregate`（约 1.7 KB）+ `LineReader`（约 2 KB），会话结束时释放。

## 真机验证（2026-11-23，`onepage_s3`，Lyra 竖屏）

串口由 `scripts/device_test.py` 驱动；调试命令 `SETDATE` / `RH_DUMP` / `RH_AGG` / `RH_AGG_DROP` 已在 `src/main.cpp`。以下数值均来自串口日志与截图（`/tmp/rh/run*`）。

| # | 项 | 结果 |
|---|---|---|
| 1 | 阅读 → `RH_DUMP` / `RH_AGG` 数值一致 | ✅ Nov20 9 s、Nov21 8 s、Nov22 9 s / 2 页、Nov23 98 s；与手计时一致 |
| 2 | 同日二次阅读只更新一行 | ✅ Nov23 会后 `total_sec=105`（98+7）、`sessions=2`，仍一条记录 |
| 3 | 统计页数值 / 分级 / Back 焦点 | ✅ 今天 2 分钟 0 页、最近 7 天 3 分钟、阅读天数 4；Nov20–23 四格 1 级灰；Back 回首页且焦点在「阅读统计」 |
| 4 | 3 天历史的汇总与热力图格位 | ✅ 4 天数据，Nov20/21/22 在上一周列（周五/六/日），Nov23 在最后一列周一 |
| 5 | 删除 `aggregate/` 后统计页回退 | ✅ `RH_AGG_DROP 2026-11-23` 后统计页与聚合路径逐字节相同（截图对比），未产生写操作 |
| 6 | 未同步时钟 | ⚠️ 不可达：板载 PCF8563 始终返回有效时间，无拆电池条件；代码路径（`hasValidClock` → `STR_READING_CLOCK_NEEDED`）由主机测试覆盖 |
| 7 | 四种方向 | ⚠️ 不适用：Home/Stats 固定竖屏布局，阅读方向设置只影响 reader 且退出时重置为 Portrait；主机测试覆盖 12×7 网格几何与月份标签边界 |
| 8 | 10 tile / 11 tile 布局 | ✅ Lyra 竖屏保持 3 列，「今天的词 0」完整；BaseTheme 走 4 列分支（几何由 `gridColumnsFor` 单元测试覆盖） |

真机发现并修复的问题：

- **聚合行尾逗号累积**：旧年文件行以 `},` 结尾时，重建会把分隔逗号当数据复制，产生 `},,`。修复为写入时去掉行尾 `,` 再拼分隔符，并加往返用例；真机年文件已重建为 491 B 的合法 JSON。
- 调试命令的 `wait:` 会匹配到上一条命令的回显；批量执行时应在每条命令后等对应 `ok`，dump 类命令等 `RH_AGG_DUMP_END`。

## 已知边界

- `aggregate/` 是派生缓存：某年的文件被删除后，只有再次产生会话的那天才被重新写入；其余日期由 `loadDayStats()` 只读回退 history。因此**读聚合的消费者（M5 / AI-08.4）应调用 `loadDayStats()`，而不是直接解析年文件**，否则可能漏掉尚未回填的日期。
- 12 周窗口固定，不跨年混合统计；跨年时只看当年。

## 后续

- RP-01/02 计划 CRUD、RP-03 本地建议（M4）。
- M5 Bridge + AI-01 + AI-08.4 可直接消费 `aggregate` 的 `history_14d`。
- 若 RH-03 实测信息密度不足，可把窗口扩到 16 周（常量 `HEATMAP_WEEKS`）或加左右翻页，二者都不改数据层。
