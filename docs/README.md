# 文档索引 / Docs Index

## 产品与需求
- [OnePage 软件功能需求文档 v4.0](<OnePage 软件功能需求文档v4.0 版.md>) — 历史 PRD
- [OnePage 补充 PRD](<OnePage_additional _PRD.md>) — 历史增补需求（其中的 OpenClaw 集成已废弃）
- [comparison.md](comparison.md) — 与参考实现的界面对比
- 项目愿景与范围见根目录 [SCOPE.md](../SCOPE.md)

## 开发计划
- [AI 原生智能阅读终端 · 可行性评估与实施计划](AI原生智能阅读终端_可行性评估与实施计划.md) — V1–V4 端云分工、passage_id、结构化引用、离线同步（服务端 V1 已完成，设备端进行中）
- [AI 原生智能阅读终端渐进式落地任务清单](tasks/2026-11-19-ai-native-implementation.md) — 分阶段任务、验收与待决策项
- [V1 接口与数据契约](ai-native/api-v1.md) — passage_id、/reader/query、/sync 冻结契约
- [v4.0-development-plan.md](v4.0-development-plan.md) — v4.0 实施计划（历史，含构建/烧录流程）
- [RH-01 阅读会话追踪任务记录](tasks/2026-11-22-reading-history-rh01.md) — 会话状态机、90 s 空闲、JSONL 落盘
- [RH-02/03 每日聚合与阅读热力图任务记录](tasks/2026-11-23-reading-history-rh02-rh03.md) — 年聚合文件、流式合并、首页 12 周热力图
- [AI 界面吉祥物与空状态任务记录](tasks/2026-11-24-ai-companion-mascot.md) — 1-bit「书灵」角色、七种姿势、各 AI 屏的放置与验证
- [RP-01/02 阅读计划与空余时段任务记录](tasks/2026-11-23-reading-plan-rp01-rp02.md) — plans.json / slots.json、列表与编辑页、空余时段
- [RP-03 本地阅读建议任务记录](tasks/2026-11-24-reading-plan-rp03.md) — 周目标 + 近 14 天均速 → 每日分钟与今日时段（离线、纯公式）
- [AI 回复纯文本化任务记录](tasks/2026-11-24-ai-plain-text.md) — relay 回复就地剥离 Markdown 标记（零分配，覆盖回答/词典/书签/朗读）
- [AI-08.5 温和提醒任务记录](tasks/2026-11-24-ai-gentle-reminder.md) — 睡眠页一行进度、阅读提醒开关、今日跳过（纯本地）
- [小白泽主题任务记录](tasks/2026-11-24-baize-theme.md) — 首页问候区 + 1-bit 吉祥物 + 四列快捷入口（第一版）
- [UI 走查与优化（2026-11-23）](ui/2026-11-23-ui-audit.md) — 计划/统计/首页的焦点、空状态与文案修正；保留项说明
- [voice-openclaw-port-plan.md](voice-openclaw-port-plan.md) — 语音移植方案（历史；OpenClaw 路径已移除，现全部走 voice relay）

## 架构与模块
- [activity-manager.md](activity-manager.md) — 活动（页面）栈与生命周期
- [file-formats.md](file-formats.md) — 书籍/缓存文件格式
- [hyphenation-trie-format.md](hyphenation-trie-format.md) — 断字 trie 格式
- [i18n.md](i18n.md) — 国际化与字体字符集
- [sd-card-fonts.md](sd-card-fonts.md) — SD 卡字体安装与注册
- [focus-reading.md](focus-reading.md) — 专注阅读模式
- [reading-page-turn-baseline.md](reading-page-turn-baseline.md) — 翻页性能基线

## Web 服务器
- [webserver.md](webserver.md) — Web 界面使用指南：传输模式、四个页面、设置分类面板与保存/恢复出厂/重启、安全说明
- [webserver-endpoints.md](webserver-endpoints.md) — 全部 HTTP / WebSocket / WebDAV / UDP 端点

## 贡献与排障
- [contributing/](contributing/README.md) — 开发环境、架构、测试调试工作流
- [translators.md](translators.md) — 翻译指南
- [troubleshooting.md](troubleshooting.md) — 常见问题
- [images/](images) — 文档配图
