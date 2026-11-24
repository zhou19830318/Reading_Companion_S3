# RP-01/02 阅读计划与空余时间（2026-11-23）

需求依据：v4.0 §4.4/§4.5、§3.2 RP-01/02、§6 M4。状态：**RP-01/02 完成；RP-03 已完成（2026-11-24，见 [RP-03 记录](2026-11-24-reading-plan-rp03.md)）**。

## 范围

- RP-01：计划创建/编辑/状态（进行中 / 已暂停 / 已归档），多计划并行（上限 8）。
- RP-02：周期性空余时段（星期掩码 + 起止钟点 + 灵活标记），上限 8。
- 明确不做：计划删除（归档替代）、通知策略、AI 建议（RP-03 / AI-08）。

## 数据契约

`/.crosspoint/plans/plans.json`、`/.crosspoint/plans/slots.json`，`schema_version=1`，一行一条；字段与示例见 [file-formats.md](../file-formats.md)。写入 `<file>.tmp` + rename；缺文件 = 空列表；损坏行整行跳过。

## 实现

- `lib/ReadingPlan/ReadingPlanFormat`：纯编解码 + 日期/时间/状态校验 + 稳定 id（`plan_YYYYMMDD_NNN`），主机可直接测试。
- `lib/ReadingPlan/ReadingPlanStore`：单例、8+8 上限、add/update、slot add/update/remove；写入失败回滚内存。常驻约 2 KB。
- UI：`ReadingPlanListActivity`、`ReadingPlanEditActivity`、`FreeTimeListActivity`、`FreeTimeEditActivity`；首页第 6 个 tile「阅读计划」。
- Back = 保存：新计划若书与日期都为空则丢弃；新时段未做任何修改则丢弃。
- 字段编辑复用 `KeyboardEntryActivity` 与 `IntervalSelectionActivity`；错误用 `GUI.drawPopup` 给出具体原因（日期/时间格式、至少一天）。
- i18n：英 / 简中 / 繁中；所有文案走 `tr()`。
- 调试：`CMD:PLAN_DUMP` 打印两个文件，真机验证不必拔卡。

## 资源

- 静态 RAM：Plans 8 × ~180 B + Slots 8 × ~48 B + store ≈ 2 KB，常驻；会话期无堆分配。
- Flash：`onepage_s3` 待填（相对 RH-03 的 5,536,719 B）。
- 主机：373 passed（新增 16 例 `test/reading_plan`）。

## 真机验证（onepage_s3，Lyra 竖屏）

- 新建计划：默认目标日期 today+30、书籍取最近一本；Back 保存后 `PLAN_DUMP` 显示 1 条，`id` / `created_at` / `target_date` / `goal` / `timezone_offset_q` 正确。
- 状态：切到「已暂停」后列表值列与 JSON 的 `status:"paused"` 一致；再次进入编辑仍是同一条 id（未新增）。
- 空余时段：新建默认「阅读时段」周一–周五 21:00–22:00；关闭周一并保存 → JSON `days:[2,3,4,5]`，列表副标题与之一致。
- 两个文件均为合法 JSON（349 B / 162 B），重新进入页面读取正常。
- 未覆盖：归档状态的真机切换（状态解析/序列化有主机用例）、跨年/时区切换、计划满 8 条。
- 截图：`/tmp/ui/run2`（优化后）。

## 边界与偏差

- 时区只存偏移量（固件无 IANA 时区库）；`paused_until`、`notification_policy` 保留在 schema，UI 未暴露。
- 目标类型仅 `minutes_per_week`；`pages_per_week=0` 表示不跟踪页数，编辑页显示「不限」。
- 归档计划仍在列表中可见（不丢数据），暂无单独筛选。
- 日期/时间用键盘输入并校验，尚无日期选择器。

## 后续

- RP-03 本地规则建议：输入计划 + aggregate 均速，纯公式；AI-08.4 经 Bridge 复用。
- 读取聚合请走 `ReadingHistory::loadDayStats()`，不要直接解析年文件（见 RH-02 任务记录）。
