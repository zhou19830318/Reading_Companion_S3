# RP-03 本地规则建议（2026-11-24）

需求依据：v4.0 §5.2、§3.2 RP-03、§6 M4。状态：**完成**。前置：[RP-01/02](2026-11-23-reading-plan-rp01-rp02.md)、[RH-01/02/03](2026-11-23-reading-history-rh02-rh03.md)。

## 目标

无网络、无 AI 时，根据计划与最近阅读速度给出**每日分钟数**建议，并把它落到今天的空余时段里。纯整数公式，零网络零堆分配。

## 与 v4.0 §5.2 的差异（重要）

原文公式需要 `remaining_pages`（剩余页数）。固件没有可靠的总页数：EPUB 的分页在 section 首次排版后才存在，未读章节没有页数；因此本实现改用设备实际拥有的数据：

```
daily_minutes = ceil(weekly_minutes / 7)
pages_per_day = ceil(weekly_pages / 7)
if 近 14 天有速度 and pages_per_day > 0:
    daily_minutes = ceil(pages_per_day / pace)
pace = Σ pages_advanced / Σ total_sec   (来自 ReadingHistory::loadDayStats)
```

- 没有历史、或计划只设时长 → 直接用每周目标折算（验收要求的离线路径）。
- 有「每周页数」目标且有历史均速 → 用速度把页数换算成分钟。
- `target_date` 只作为信息（`daysLeft`），不参与加压：没有剩余页数时，用截止日期提高强度只会产生假建议。
- 每日建议钳制在 5–240 分钟。

时段映射：取今天（按 `clockUtcOffsetQ` 的本地星期）命中的空余时段，按时间顺序切成 **每段 ≤30 分钟、最多 3 段、<10 分钟的尾段丢弃**。时钟未同步时不给时段建议，但仍给分钟数。

## 实现

- `lib/ReadingPlan/ReadingAdvice.{h,cpp}`：纯 C++，无 SD / 时钟 / 网络，输入 `Plan + Slots + PaceWindow + 今天星期/日序号`，输出 `Advice`（分钟、页/天、速度、时段段数）。
- `src/activities/plan/PlanTime.h`：新增 `todayDayIndex()`（本地日序号 + 0=周日 的星期）与 `dayIndexFromDate()`。
- `ReadingPlanEditActivity`：进入页面、修改目标日期/每周时长/每周页数、切换状态时重算一次（聚合读取只在这里发生，渲染路径不再碰 SD）；**只有进行中的计划**显示建议。建议区画在六个字段行下方的空白带，复用 `CompanionUi::drawSectionLabel`。
- i18n：英 / 简中 / 繁中 7 个键（`STR_PLAN_ADVICE*`），全部 `tr()`。

## 资源

- 静态 RAM：`Advice` ≈ 40 B；`DayStat stats[14]` ≈ 280 B，仅刷新时在栈上。
- Flash：`onepage_s3` 5,580,923 B（README 的 5,536,719 B 基线为本轮 RH/RP 之前）。
- 主机：388 passed（新增 15 例 `ReadingAdviceTest`）。

## 真机验证（onepage_s3，Lyra 竖屏）

- 打开「阅读计划 → 编辑计划」，把暂停中的计划切到「进行中」：
  - `阅读建议 / 每天约 26 分钟 / 按近 14 天均速：2.36 页/分钟 / 今天可读：21:00-21:26`；
  - 6 行字段 + 4 行建议 + 按键提示在 480×800 内无重叠，建议区在字段行下方。
- 暂停/归档状态不显示建议（同一计划在暂停时不出现该区块）。
- 截图：`/tmp/ui/plan/plan_advice_p.png`。
- 未覆盖：真机修改「每周页数」后的页数→分钟换算（主机用例覆盖）；跨午夜/时区边界；计划满 8 条。

## 后续

- AI-08.4 走 Bridge 的 `reading_schedule_advice`，可直接复用本模块作为无网降级结果。
- 若要贴近 v4.0 原公式的“剩余页数”，需要先给 EPUB 缓存补一个全书页数（当前不完整），属于独立的数据层改动。
