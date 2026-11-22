# AI 原生智能阅读终端 · 可行性评估与实施计划

- 日期：2026-11-20（初稿 2026-11-19）
- 状态：**服务端 V1 与设备端 V1 均已完成并真机验证（段落映射、引用问答、引用回跳、章末讨论、outbox/同步页、五界面 UI 重设计、断网同步闭环、黄金书端到端、PSRAM 采样）；生产服务器已部署 `20261122-125701` 并完成真实 DashScope 引用问答；V1.1 选区引用落库/同步已完成**
- 依据：[AI 原生智能阅读终端_渐进式技术实现文档](AI原生智能阅读终端_渐进式技术实现文档.md)、[SCOPE.md](../SCOPE.md)、[api-v1.md](ai-native/api-v1.md)、现有固件与 `relay/` 实现
- 结论：**硬件与现有固件基础可行。三条关键协议（稳定 `passage_id`、带强制来源标签的结构化回答、离线可用的幂等同步）已冻结在 [api-v1.md](ai-native/api-v1.md)，服务端 V1（书库/入库解析/引用问答/同步/工作台）与设备端 V1（解析器接线、结构化问答 UI、引用跳转、outbox、同步页、章末讨论）均已落地；断网保存、联网推送、重复同步不重复落库、黄金书真机问答与引用回跳均已闭环验证，PSRAM 峰值已采样；生产 relay 已升级并跑通真实 `qwen-flash` 引用问答。** 按 V1 → V1.1 → V2 → V3 → V4 推进。

---

## 0. 实施进展（2026-11-20 更新）

第 11 节的决策已按建议全部确认（见[任务记录](tasks/2026-11-19-ai-native-implementation.md)“已确认决策”），实施顺序为**服务端先行**。当前实际进度：

| 范围 | 状态 | 落点 / 说明 |
|---|---|---|
| 协议冻结（`passage_id` / `/reader/query` / `/sync/*`） | ✅ 完成 | [api-v1.md](ai-native/api-v1.md) v1.0，冻结后未再改动 |
| relay 书库 + 入库解析 | ✅ 完成 | `library_store.py`、`book_ingest.py`、`library_api.py`；EPUB/TXT，FTS5 trigram |
| relay 结构化问答 + 引用校验 | ✅ 完成 | `reader_query.py`、`citation.py`；`book` 段无有效引用降级 `inference` |
| relay 同步 `push`/`pull`（`op_id` 幂等） | ✅ 完成 | `sync_ops` / `sync_changes`，跨设备写保护 |
| Web 工作台（上传/状态/软删除/只读笔记） | ✅ 完成 | `admin_page.py`，复用 admin token |
| 部署与网关 | ✅ 完成 | nginx 增 `/api/v1/books`、`/api/v1/reader/`、`/api/v1/sync/`；依赖增 ebooklib/bs4/lxml/charset-normalizer |
| 设备端段落模型 | ✅ 完成 | `PassageRef`、`TextBlock`/`ParsedText` 段落锚点、解析器接线与页缓存序列化均已落地；`test/passage_ref` 覆盖 |
| 设备端问答 UI / 引用跳转 / outbox / 同步页 | ✅ 完成 | AI 书童/章末讨论五界面重设计、引用列表与回跳、outbox/push、同步页与冲突展示、断网同步闭环、黄金书端到端均已真机验证 |
| 真实云端 LLM、真机端到端 | ✅ 完成 | 生产 relay `20261122-125701` + 真实 `qwen-flash`：黄金书 2 章/20 段，两条引用通过校验、`warnings=[]`，设备真机复测通过 |
| Flash/RAM/PSRAM 复测 | ✅ 已完成 | 5,522,495 B / 139,488 B（42.6%），增量 39,983 B；PSRAM 峰值约 1.0 MB |

> **设备端现状：** S3 固件已按当前 `src/` 重新构建并烧录，黄金书问答/引用回跳已在 `/dev/ttyACM0` 复测。服务端全量单测通过（329 passed，2026-11-20，见任务记录）。

---

## 1. 结论摘要

1. **硬件完全满足目标，且有余量。** ESP32-S3R8、16 MB QIO Flash、8 MB OPI PSRAM、800×480 墨水屏、PDM 麦克风、MAX98357A 扬声器、6 个按键、SD 卡、RTC、IMU、温湿度传感器均已在本机验证。设备端只做采集、排版、渲染、缓存和状态机，不跑任何模型，符合文档的端云硬边界。
2. **V0 实际上已经完成。** 独立 `onepage_s3` 目标、PSRAM、SD、显示全刷/局刷、按键、休眠、断网阅读、进度恢复都已落地；缺的只是一份正式的《硬件能力清单 + 已知问题清单》。
3. **V1 设备端已落地。** EPUB/TXT/XTC 阅读、书架/目录/书签、中文排版、SD 字体、Ask AI、语音助手、朗读模式、本地笔记、Web 上传/设置、OTA 之外，本轮已接入 `PassageRef`/`TextBlock`/`ParsedText`、解析器分段、结构化问答 UI、引用列表与跳回原文、章末讨论、outbox 与同步页，并完成真机验证。
4. **服务端已不是主要缺口——V1 服务端已完成。** relay（FastAPI + SQLite）新增 `library_store.py`、`book_ingest.py`、`reader_query.py`、`citation.py`、`library_api.py` 与工作台页，落地书籍解析、章节/段落索引、检索问答、引用校验、通用同步。基线仍是 **SQLite + FTS5 trigram**，未引入 PostgreSQL + Qdrant。
5. **资源够用，且 V1 已复测。** 当前固件 5,522,495 B（2026-11-22 构建，含段落锚点、引用问答、同步、章末章节讨论、五界面 UI 重设计、UI 文案收口、UTF-8 修复与选区引用落库），双 OTA 单槽 6,553,600 B，余量约 1.01 MiB（15.7%）。静态 RAM 139,488 / 327,680 B（42.6%）。相对 5,482,512 B 基线增加 39,983 B（约 39.0 KiB），低于 **150 KB** 预算。PSRAM 8 MB，黄金书问答期间最低空闲 7,364,168 B，峰值占用约 1.0 MB。大文本、索引、向量一律留在服务端。
6. **最大风险不是硬件，而是“引用可信”。** 协议与服务端引用校验器已就位；剩余风险转到**设备端分段计数与字节偏移必须与服务端完全一致**，以及 UI 不得自行把 `inference` 标成 `book`。

---

## 2. 硬件核对

| 文档要求 | 实物 / 固件 | 结论 |
|---|---|---|
| ESP32-S3 N16R8 | ESP32-S3R8，16 MB QIO Flash（PY25Q128HA），8 MB OPI PSRAM | ✅ 匹配 |
| 墨水屏 800×480，支持全刷/局刷 | SSD1677，800×480，FULL/HALF/FAST，单 framebuffer 48,000 B | ✅ 匹配 |
| PDM 麦克风 | 已接入，语音助手/朗读打断可用 | ✅ |
| I2S 扬声器 / 功放 | MAX98357A，朗读模式已验证 | ✅ |
| 实体按键 | 前 4 键 + 侧 2 键 + 电源键（`BTN_BACK/CONFIRM/LEFT/RIGHT/UP/DOWN/POWER`） | ✅ 足够做 PTT、翻页、确认/取消 |
| SD 卡 | 已挂载，书籍、字体、缓存、笔记均在 SD | ✅ |
| RTC | PCF8563 | ✅ |
| IMU | MSA311，倾斜翻页/手势 | ✅ |
| 温湿度 | SHT30 | ✅ |
| 电池/电源管理 | 已有 HAL 电源管理与休眠 | ✅ |
| 离线阅读 | EPUB/TXT/XTC、进度 A/B 双槽、书签、断网可读 | ✅ 已超过 V0 验收 |

**资源现状（2026-11-22 最终固件）**

| 项目 | 数值 | 说明 |
|---|---|---|
| 固件大小 | 5,522,495 B | `.pio/build/onepage_s3/firmware.bin`（2026-11-22） |
| OTA 单槽 | 6,553,600 B（0x640000） | 双 OTA |
| Flash 余量 | 1,032,633 B（约 1.01 MiB） | 新增功能必须克制 |
| 静态 RAM | 139,488 / 327,680 B（42.6%） | 2026-11-22 复测 |
| PSRAM | 8 MB；实测峰值占用约 1.0 MB | 文本缓冲、页面缓存、JSON 回复的优先落点；黄金书问答最低空闲 7,364,168 B |
| 屏幕缓冲 | 48,000 B | 单缓冲，不适合再开全屏副本 |

**结论：** 设备端可以承担 V1–V3 的交互与展示；不能承担整书索引、向量、模型推理，也不应在 Flash 里放大字典/大字体/大表。

---

## 3. 现有能力对照

图例：✅ 已有并可复用　🟡 部分具备，需要改造　❌ 缺口

### 3.1 阅读与本地数据

| 能力 | 现状 | 结论 |
|---|---|---|
| EPUB2/3、TXT、XTC | 已有阅读器、目录、书签、进度 | ✅ |
| 中文排版、SD 字体 | 已有 | ✅ |
| 章节/页码定位 | `spineIndex + pageNumber`，KOReader xpath 解析器已有段落计数基础 | 🟡 缺稳定 `passage_id` |
| 选区 | Ask AI 按行选区已映射到多 `passage_id`，确认后作为 `highlight` 引用入 outbox 并同步 | ✅ 已完成 |
| 划线/批注 | 无正文常驻划线渲染，无独立 AnnotationStore | ❌ |
| 笔记 | `NoteStore` NTF1，按书追加，支持语音笔记/问答记录 | 🟡 缺 passage_id、缺同步状态 |
| 进度同步 | KOReader 进度同步已有 | 🟡 不是通用变更同步 |
| 断网可用 | 阅读、笔记、书签均本地 | ✅ |

### 3.2 AI 与语音

| 能力 | 现状 | 结论 |
|---|---|---|
| 当前页/章节/选区问答 | `AskAiActivity`，8 预设 + 自定义 | ✅ 直接基础 |
| 语音提问 | PTT → ASR → LLM → TTS | ✅ |
| 回答展示 | 纯文本，滚动窗口 | 🟡 缺分段来源标签与引用跳转 |
| 回答朗读/取消 | `AnswerVoice`，可停止 | ✅ 需复验“取消后不播过期 TTS” |
| 章节上下文 | 只取当前页向后最多 3 页（`MAX_CONTEXT_PAGES=3`） | 🟡 不是整章，需服务端检索补足 |
| 章末事件 | 无“章节完成”事件总线 | ❌ V1 只预留钩子 |
| 自主闲聊 | 无 | ❌ 默认关闭，V3 再做 |

### 3.3 网络、Web 与系统

| 能力 | 现状 | 结论 |
|---|---|---|
| Wi-Fi / relay 配置 | 已有，设备 token 认证 | ✅ |
| relay 文本问答 | `POST /api/v1/chat` 保留；已新增 `POST /api/v1/reader/query` 结构化接口 | ✅ 服务端与设备端均已接入 |
| relay 语音/TTS | `/api/v1/voice/stream`、`/api/v1/tts` | ✅ |
| 设备 Web | 上传、设置、文件管理、OTA | ✅ |
| relay 管理页 | `admin_page.py` 已含“阅读书库”“笔记与引用”工作台 | ✅ 服务端就绪 |
| 通用同步 | 已实现 `/api/v1/sync/push|pull`、`op_id` 幂等、跨设备写保护 | ✅ 服务端与设备端 outbox 均已就绪 |
| 知识卡片 | 无 | ❌ V2 |

### 3.4 relay 现状

| 模块 | 现状 | V1 复用方式 |
|---|---|---|
| 框架 | FastAPI，单进程，SQLite | 继续使用 |
| 认证 | `Authorization: Bearer` + `X-Device-Id`，常量时间比较，支持设备注册表 | 已复用（书籍/问答/同步同样校验） |
| 文本问答 | `/api/v1/chat`；新增 `/api/v1/reader/query` | 语音助手用 `/chat`；Ask AI 引用问答走 `/reader/query` |
| 书籍/索引 | `library_store.py`（books/chapters/passages/FTS5/annotations/ai_results/sync）、`book_ingest.py`、`library_api.py` | 已落地 |
| 引用校验 | `citation.py` + `reader_query.py` | 已落地 |
| 语音/TTS | WebSocket | 不动 |
| 学习/番茄钟/飞鸽 | 各自 SQLite store，已有幂等/唯一约束范式 | 作为新 store 的写法参考 |
| 数据库 | SQLite（WAL；FTS5 trigram） | V1 已用 SQLite + FTS5；向量后置 |
| 部署依赖 | 增 ebooklib/beautifulsoup4/lxml/charset-normalizer | 仍无 Postgres/Redis/Celery |

---

## 4. 关键差距（按优先级）

> 状态：✅ 已解决（服务端）　🟡 部分（设备端未接）　❌ 未开始

### P0 — V1 必须解决

1. **稳定段落 ID**：设备端和服务端必须能用同一规则定位“第几章第几段”，否则引用不可信。
   - ✅ 协议冻结（[api-v1.md](ai-native/api-v1.md) 第 1 节）+ 服务端 `book_ingest.py` 已实现；✅ 设备端解析器分段、`PassageRef` 与引用跳转已接并真机验证。
2. **书籍入库与索引**：上传 → 校验 → 解析 → 章节 → 段落 → FTS5 → 状态查询。→ ✅ 服务端完成（`/api/v1/books*`）。
3. **结构化问答**：请求带 `book_id + passage_ids + selected_text + intent + context_scope`；响应带分段来源与引用。→ ✅ 服务端完成（`/api/v1/reader/query`）；✅ 设备端 UI、引用列表与跳转已接并真机验证。
4. **引用校验**：服务端必须验证引用段落存在、引文匹配；不匹配就降级为“推断”，不能伪装成原文。→ ✅ 服务端完成（`citation.py`）。
5. **离线 outbox + 幂等同步**：笔记、批注、AI 结果摘要在断网时可写，恢复后只同步一次。→ ✅ 服务端完成（`/api/v1/sync/*`）；✅ 设备端 outbox/同步页已完成，并用断网笔记→联网推送→同 `op_id` 重放验证不重复落库。
6. **设备端 passage 映射**：把现有按行选区映射到 `spine + paragraph`，并支持“跳回原文”。→ ✅ 已完成（解析器分段、选区映射、引用列表与跳转均已真机验证）。

### P1 — V1.1 / V2

7. 正文常驻划线渲染（先有数据，后做渲染）。
8. 知识卡片 CRUD、确认入库、来源定位。
9. 章节摘要/全书导读（服务端生成，设备只展示）。
10. Web 知识工作台：上传、处理状态、笔记列表、卡片确认、隐私开关。
11. 混合检索（BM25 + 向量）与“只检索已确认知识”。

### P2 — V3 / V4

12. 学习目标、FSRS/SM-2 复习。
13. 章末“智者闲聊”完整会话（默认关闭、只出邀请条）。
14. 受控 Agent：事件总线、任务队列、L0–L4 权限、审计。

---

## 5. 建议架构

```text
┌──────────────────────────── 设备端（ESP32-S3） ────────────────────────────┐
│  阅读器（EPUB/TXT/XTC）                                                     │
│    ├─ 排版时记录 spine + paragraph + 行范围                                 │
│    ├─ 选区 → passage 范围（可含字符偏移）                                   │
│    └─ 引用跳转：passage → spine/paragraph → 页码                             │
│  Ask AI                                                                     │
│    ├─ 用户调用式：页面/章节/选区                                            │
│    ├─ 结构化请求 → relay                                                    │
│    └─ 分段渲染：[书中原文] [外部补充] [推断] + 引用编号                      │
│  NoteStore / AnnotationStore（本地）                                        │
│    └─ 追加写 + SyncOutbox（SYO1，幂等 op_id）                               │
│  同步页 / 设置页                                                             │
│    └─ 仅用户动作触发：保存后、AI 回合结束、打开同步页、手动重试              │
└──────────────────────────────────┬─────────────────────────────────────────┘
                                   │ HTTPS/WSS（设备 token）
┌──────────────────────────────────▼─────────────────────────────────────────┐
│ relay（FastAPI，单进程）                                                     │
│  /api/v1/chat                 现有语音助手，保持兼容                         │
│  /api/v1/books                上传/登记/解析状态                             │
│  /api/v1/reader/query         结构化问答 + 引用校验                          │
│  /api/v1/sync/push|pull       幂等同步                                       │
│  /api/v1/knowledge/*          V2 知识卡片                                    │
│  /api/v1/learning/*           现有生词；V3 扩展目标/复习                     │
│                                                                              │
│  BookIngest（zipfile/ebooklib + BeautifulSoup）                              │
│  PassageIndex（SQLite + FTS5 trigram）                                       │
│  RAG（FTS5 top-k + 邻接段落 + 章节摘要缓存）                                 │
│  CitationValidator（存在性 + 引文归一化匹配）                                │
│  SyncStore（op_id UNIQUE）                                                   │
│  Workbench（复用 admin 认证的最小 Web 页）                                   │
└──────────────────────────────────────────────────────────────────────────────┘
```

### 5.1 端云职责

| 设备端 | 服务端 |
|---|---|
| 排版、页码、当前页/章节文本 | EPUB 解析、章节/段落切分、索引 |
| 选区、passage 范围、引用跳转 | 检索、RAG、引用校验、模型调用 |
| 笔记/批注/outbox、同步状态 UI | 同步落库、冲突处理、审计 |
| 语音采集、TTS 播放、取消 | ASR / LLM / TTS |
| 设置开关、章末事件钩子 | 知识卡片、学习计划、Agent 调度 |

**硬边界：** 设备端不下载整书索引，不做 embedding，不跑模型；服务端不依赖设备在线状态完成阅读。

---

## 6. 关键设计决策

### 6.1 稳定段落 ID（V1 的核心）

建议格式：

```text
passage_id = "<book_sha256_16>:<spine_index>:<block_kind><block_index>"
例：a1b2c3d4e5f60718:12:p37
    a1b2c3d4e5f60718:12:l5
```

- `book_sha256_16`：书籍原始文件 SHA-256 的前 16 个十六进制字符（64 bit）。设备首次登记时计算一次并缓存；relay 上传时计算完整 SHA-256。`book_id` 默认取这 16 位；数据库保留完整 `sha256` 作为唯一键，`GET /books/by-hash/{sha256}` 负责映射。
- `spine_index`：EPUB spine 序号；TXT/XTC 固定为 0。
- `block_kind`：`p` = `<p>`，`l` = `<li>`。V1 只承诺正文段落和列表项。
- `block_index`：该 spine 文档内 `<p>` 或 `<li>` 的**文档顺序**计数，从 0 开始。设备端已有 `xpathParagraphIndex` / `xpathListItemIndex`，可直接复用；服务端必须用同一规则计数。
- 选区跨段时传 `passage_ids[]` + 每段的字符偏移（UTF-8 字节，左闭右开，相对该段 `text`）。
- 标题（h1–h6）、`<div>` 直排文字、表格单元格**不单独发 ID、也不占用计数**（与冻结契约 [api-v1.md](ai-native/api-v1.md) 第 1.1 节一致）；不要为了覆盖率把规则复杂化。

**为什么不用页码：** 页码依赖字体、字号、行距、屏幕，换设备就变。`spine + paragraph` 是排版无关的，和现有 KOReader xpath 段落计数也能对齐。

**验收方式：** 准备 5 本黄金测试书（中文 2、英文 2、图文混排 1），设备端与服务端对同一段落输出的 `passage_id` 完全一致；抽样 100 段引文匹配。

**实现现状（2026-11-20）：** 服务端计数与文本规范化已在 `book_ingest.py` 完成，`test_golden_books.py` 对 5 本黄金书逐段比对通过。设备端把规则收敛到 `lib/Epub/Epub/PassageRef.h`，`TextBlock`/`ParsedText` 已能携带 `Ref + charStart/charEnd` 并纳入页缓存序列化；但 `ChapterHtmlSlimParser` 尚未在 `<p>/<li>` 处调用 `setPassage`，`test/passage_ref` 主机单测（`PassageRef.h` 注释已引用）尚未创建，S3 也未重新构建。

### 6.2 书籍身份与索引生命周期

```text
上传/登记 → sha256 → books(status=pending)
         → 解析元数据/目录/正文 → chapters/passages
         → FTS5 索引 → status=ready
         → 失败：status=failed + error_code
```

- 设备查询 `GET /api/v1/books/by-hash/{sha256}`：
  - `ready`：带引用问答；
  - `pending/failed`：提示“索引中/失败”，可退回无引用问答；
  - `404`：本地书未入库，提示通过 Web 工作台上传，或使用无引用问答。
- **V1 不要求设备自动上传整本书。** 设备通过 Web/OPDS 上传是主路径；SD 直接拷贝的书先降级为无引用问答。这样避免在固件里做 multipart 流式上传和断点续传。
- 删书：服务端软删除书籍；用户可选保留个人笔记/卡片（V2 验收项）。

### 6.3 结构化回答与来源标签

请求（`POST /api/v1/reader/query`）：

```json
{
  "request_id": "dev-01:00001234",
  "book_id": "a1b2c3d4e5f60718",
  "spine_index": 12,
  "passage_ids": ["a1b2c3d4e5f60718:12:p37"],
  "selected_text": "……",
  "intent": "explain",
  "context_scope": "current_passage",
  "response_style": "concise",
  "language": "zh"
}
```

响应：

```json
{
  "request_id": "dev-01:00001234",
  "segments": [
    {
      "source": "book",
      "text": "作者在这里把……",
      "citations": [
        {
          "passage_id": "a1b2c3d4e5f60718:12:p37",
          "spine_index": 12,
          "quote": "原文片段……"
        }
      ]
    },
    {
      "source": "external",
      "text": "背景补充……",
      "citations": []
    },
    {
      "source": "inference",
      "text": "因此可以推断……",
      "citations": []
    }
  ],
  "usage": {"prompt_tokens": 0, "completion_tokens": 0}
}
```

规则：

1. `source` 只能是 `book / external / inference`，缺省或非法值一律按 `inference` 处理。
2. `book` 段必须至少有一个通过校验的 citation；否则整段降级为 `inference`。
3. 引文校验：按空白归一化后，`quote` 必须是 passage 文本的子串；不匹配则拒绝该 citation。
4. 设备端渲染时给每段加固定标签：`[书中原文]`、`[外部补充]`、`[推断]`，不把标签交给模型生成。
5. 模型返回非 JSON 或解析失败：整段作为 `inference` 展示，并标注“未通过引用校验”，绝不冒充原文。
6. 现有 `/api/v1/chat` 保持不变，语音助手继续使用；只有 Ask AI 的“带引用问答”走新接口。

### 6.4 离线 outbox 与幂等同步

**本地格式（SYO1，JSONL，一行一条）：**

```json
{"v":1,"op_id":"01J...","type":"annotation.create","book":"a1b2...","passage":"a1b2...:12:p37","ts":1763...,"payload":{...}}
```

- 文件：`/.crosspoint/sync/outbox.jsonl`；上限建议 512 条或 256 KB，满了先提示用户同步，不静默丢数据。
- `op_id`：设备生成 128 bit 随机 ID（硬件 RNG），服务端 `UNIQUE`；重试永远带同一个 `op_id`。
- 同步触发（遵守 SCOPE，不做后台常驻）：
  - 用户保存笔记/批注后，若在线，立即尝试一次；
  - AI 回合结束后，若在线，推送该回合结果摘要；
  - 用户打开“同步”页或手动点“立即同步”；
  - 不做定时器、不做后台轮询、不在阅读中偷偷联网。
- push 成功：服务端返回 `applied / duplicate / rejected`；`applied` 和 `duplicate` 都算成功，设备把该条标记为 acked 并在下次同步时压缩文件。
- pull：按 `updated_at` 增量拉取服务端变更；V1 只拉“笔记/批注/AI 结果”的确认状态，不做多端实时合并。
- 冲突：V1 用“字段级更新时间 + 保留双方”策略，禁止最后写入覆盖导致笔记丢失；冲突在同步页可见。

### 6.5 服务端存储

**V1：SQLite + FTS5，单文件，WAL。** 已实现于 `relay/relay/library_store.py`（建表 + `schema_version` 落在 `meta` 表）。

实际表：`meta / books / chapters / passages / passages_fts / annotations / ai_results / sync_ops / sync_changes`；`books` 保留完整 `sha256 UNIQUE`，软删除用 `deleted` 标记（重复上传同文件会恢复原记录，不撞唯一约束）。`ai_results` 存 AI 回答摘要，`sync_changes` 提供 `pull` 增量游标。

```sql
books(id TEXT PRIMARY KEY, sha256 TEXT UNIQUE, title TEXT, author TEXT,
      format TEXT, file_path TEXT, size INTEGER, status TEXT, error TEXT,
      created_at TEXT, updated_at TEXT);

chapters(id INTEGER PRIMARY KEY, book_id TEXT, spine_index INTEGER,
         title TEXT, href TEXT, ordinal INTEGER);

passages(id TEXT PRIMARY KEY, book_id TEXT, chapter_id INTEGER,
         spine_index INTEGER, block_kind TEXT, block_index INTEGER,
         text TEXT, char_start INTEGER, char_end INTEGER);

CREATE VIRTUAL TABLE passages_fts USING fts5(
    text, content='passages', content_rowid='rowid', tokenize='trigram');

annotations(id TEXT PRIMARY KEY, device_id TEXT, book_id TEXT,
            passage_id TEXT, quote TEXT, note TEXT, kind TEXT,
            created_at TEXT, updated_at TEXT, deleted INTEGER DEFAULT 0);

sync_ops(op_id TEXT PRIMARY KEY, device_id TEXT, applied_at TEXT, result TEXT);

conversations / messages / knowledge_cards / learning_*  -- 后续阶段
```

**中文检索注意：** 默认 `unicode61` 对中文几乎不切词。V1 用 FTS5 `trigram`（SQLite 3.34+），支持 3 字以上子串检索；1–2 字查询退回 `LIKE`。如果部署环境的 SQLite 没有 trigram，再退回“入库时在 CJK 字符间插空格 + unicode61”的方案，但需要重建索引。

**V2 向量：** 先评估 `sqlite-vec` 或独立 Qdrant；不要在 V1 同时引入 embedding 服务和向量库。混合检索的接口可以先预留，实现后置。

### 6.6 网络与触发模型

- 保持“用户调用式 AI 始终可用（需网络）”。
- 自主侵入式默认关闭；V1 只落设置项与 `chapter_completed` 事件钩子，不弹邀请、不自动开麦。
- 同步只在前台动作触发；阅读页不因同步而阻塞翻页。
- 所有网络请求可取消；取消后不得再播放过期 TTS（复用现有 `AnswerVoice` 停止路径并补测试）。

### 6.7 章末“智者闲聊”边界

- 默认关闭；导师模式可建议开启，但仍可关。
- 开启后章末只出底部邀请条，不自动全屏、不自动语音。
- 会话是“先分享、后邀请接话”，禁止“问题 1/3、正确率”题库结构。
- 用户选“先往下读”后，同一章不反复纠缠；最多提供一次“下次提醒”。
- V1 只做开关、事件钩子、文案占位；完整会话放 V3。

---

## 7. 分阶段实施计划

> 工期按“单人 + AI 辅助”估算，单位为工作日；不含硬件返工和模型供应商故障。
> **进展（2026-11-22）：** 服务端 V1（V1-01~V1-05、V1-09）与设备端 V1（V1-06~V1-08、V1-10）均已完成；断网保存→联网推送→同 `op_id` 重放不重复落库、黄金书问答与引用回跳、PSRAM 峰值采样均已在真机验证；生产 relay 已升级并跑通真实云端 LLM。

### 阶段总览

| 阶段 | 工期 | 目标 | 主要交付 | 验收 | 进展 |
|---|---|---|---|---|---|
| V0 收尾 | 2–3 天 | 补齐基线文档 | 硬件能力清单、已知问题清单、构建/资源基线 | 可重复构建；资源数字有记录 | ❌ 待做 |
| **V1.0 MVP** | **35–45 天（约 7–9 周）** | 带引用的段落问答 + 离线同步 | 书籍入库、passage 索引、结构化问答、outbox、最小 Web 工作台 | 断网可记；联网只同步一次；回答引用当前段落；来源标签正确 | ✅ 服务端与设备端完成，黄金书真机端到端通过 |
| V1.1 加固 | 5–8 天 | 引用跳转与选区保存 | 选区→引用、跳回原文、笔记/语音批注、章末开关与钩子 | 引用可跳转；关闭自主模式时章末零打扰 | 🟡 选区→多 `passage_id`、引用落库/同步已完成；剩余弱网回归与正文常驻划线（待确认） |
| V2 知识库 | 30–40 天（6–8 周） | 多书检索与知识卡片 | 章节摘要、卡片 CRUD/确认、BM25+向量、Web 工作台 | 检索带来源；候选卡片确认后才入库；删书可保留笔记 | ❌ 未开始 |
| V3 学习与闲聊 | 30–40 天（6–8 周） | 学习闭环与章末闲聊 | 目标/计划/FSRS、完整章末会话、记忆管理 | 计划可执行；闲聊非题库；可彻底删除记忆 | ❌ 未开始 |
| V4 受控 Agent | 40+ 天（8+ 周） | 事件驱动、分级权限 | 事件总线、任务队列、L0–L4、审计 | 权限不足只建议；拒绝后不重试；失败可恢复 | ❌ 未开始 |

### V1.0 拆解

| 编号 | 工作项 | 端 | 状态 | 关键文件 / 落点 |
|---|---|---|---|---|
| V1-01 | 冻结 `passage_id`、请求/响应 Schema、同步协议；准备黄金测试书 | 全 | ✅ | `docs/ai-native/api-v1.md`、`relay/tests/fixtures/golden_books/` |
| V1-02 | 书籍上传/登记、SHA-256、解析任务、状态查询 | relay | ✅ | `relay/relay/library_store.py`、`library_api.py`、`book_ingest.py`、`main.py` |
| V1-03 | EPUB 解析：元数据、spine、章节、`<p>/<li>` 段落、FTS5 | relay | ✅ | `relay/relay/book_ingest.py`（ebooklib + BeautifulSoup/lxml + charset-normalizer） |
| V1-04 | 结构化问答 + 检索 + 引用校验 + JSON 兜底 | relay | ✅ | `relay/relay/reader_query.py`、`citation.py` |
| V1-05 | `sync/push`、`sync/pull`、`op_id` 幂等 | relay | ✅ | `relay/relay/library_store.py`（`sync_ops`/`sync_changes`，合并入书库 store） |
| V1-06 | 设备端 book hash 缓存、spine/paragraph 映射 | 固件 | ✅ | `lib/Epub/Epub/PassageRef.h`、`blocks/TextBlock.*`、`ParsedText.*`、`parsers/ChapterHtmlSlimParser.*` |
| V1-07 | Ask AI 结构化请求、分段来源渲染、引用列表 | 固件 | ✅ | `src/activities/reader/AskAiActivity.*`、`lib/VoiceRelay/VoiceRelayChatClient.*` |
| V1-08 | outbox 追加写、同步页、手动同步、状态展示 | 固件 | ✅ | `lib/Sync/*`、`src/activities/settings/AiSyncActivity.*` |
| V1-09 | 最小 Web 工作台：上传、状态、笔记列表 | relay/Web | ✅ | `relay/relay/admin_page.py`（“阅读书库 / 笔记与引用”） |
| V1-10 | 真机端到端、黄金书回归、资源复测、文档 | 全 | ✅ | `docs/tasks/2026-11-19-ai-native-implementation.md` |

**V1.0 明确不做：** 跨书检索、知识卡片、学习计划、完整章末闲聊、Agent、正文常驻划线渲染、设备自动上传整本书。

### V1.1 拆解

- ✅ 选区跨段 → 多个 `passage_id`；保存为“引用/语音批注”。（2026-11-22：确认选区即写 `highlight` 引用；CJK 拼接与段内换行已修正，真机推送验证）
- ✅ 引用编号 → `spine + paragraph` → 页码，支持“跳回原文”。
- 章末完成事件：在阅读器检测“本章最后一页翻过”时发出内部事件；默认只更新状态，不开 UI。
- 设置项：`AI 自主邀请` 默认关闭；关闭时章末零打扰。
- 复验取消请求后的 TTS 不播放。
- 正文常驻划线渲染若用户要求，单独立项（涉及 Page 渲染、注解查找、刷新策略），不混入 V1.0。

---

## 8. V1 验收清单

> 标注 **(服务端)** 的项已在 relay 单测 / 真实 HTTP 链路中通过；其余为真机端到端项，待固件完成。

### 功能

- [x] (服务端) 用户通过 Web 工作台上传 EPUB，状态从 `pending` 变为 `ready`。
- [x] 设备打开同一本书，能取到 `book_id` 与索引状态。（黄金书 `zh_prose.epub` → `book_id=9e08598fd2076fdb`，2 章 / 20 段，`ready`）
- [x] (服务端) 当前段落提问，回答至少一段 `[书中原文]`，引用指向当前段或同章相邻段。
- [x] 引用编号可跳回原文页码。
- [x] (服务端) 问“本章总结”时，服务端检索整章而不是只取设备端 3 页。
- [x] (服务端) 超出书籍内容时，明确出现 `[外部补充]` 或 `[推断]`，不冒充原文。
- [x] 断网保存笔记/批注/AI 结果，恢复网络后同步，服务端只落一次。（真机：离线 outbox 1 条 → 联网 HTTP 200/acked=1 → 同 `op_id` 重放 `duplicate`，库中仍 1 条）
- [x] (服务端) 重复点“同步”不产生重复记录（`op_id` 返回 `duplicate`）。
- [x] 关闭自主模式时，章末没有任何邀请；开启后只出邀请条（V1.1）。
- [x] 取消提问后，不再播放该次回答的 TTS。

### 质量门禁

- [x] `pio run -e onepage_s3` 通过，Flash 增量 ≤ 150 KB。（5,522,495 B，+39,983 B）
- [x] `pio run -e onepage` 通过（不回归 C61 目标）。
- [x] 主机单测 315 passed；改动文件 clang-format 无 diff。
- [ ] `python3 scripts/sync_web_core.py --check`、`python3 scripts/build_html.py` 通过。
- [x] `pio check` 无新增高优先级问题。（0 high / 0 medium）
- [x] relay 单测通过（329 passed，2026-11-20）；新增接口有幂等/失败路径测试。
- [x] 黄金测试书 5 本，段落 ID 一致率 100%（`test_golden_books.py`）；引用抽样匹配由 `test_reader_api.py`/`test_citation.py` 覆盖。

---

## 9. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| 设备端与服务端段落计数不一致 | 引用错位，产品核心失效 | 同一规则文档化；黄金书回归；失败时降级为页码/无引用 |
| LLM 编造原文 | 信任崩塌 | 引用存在性 + 引文子串校验；非法即降级 `inference`；标签由固件渲染 |
| EPUB 结构差异（EPUB2/3、脚注、表格、图片） | 解析失败/漏段 | 5 本黄金书 + 失败状态可见；V1 只承诺正文 p/li |
| 中文 FTS5 不分词 | 搜不到 | trigram；1–2 字用 LIKE；预留重建索引脚本 |
| 整章上下文过长 | 成本/延迟 | 章节摘要缓存；分段 map-reduce；上下文 token 上限 |
| Flash 增长 | OTA 失败 | 150 KB 预算；复用现有 UI/JSON/字体；每次构建记录大小 |
| 堆碎片/大缓冲 | 崩溃 | PSRAM 分配、固定缓冲、流式解析；禁止整书进内存 |
| 同步重复/丢笔记 | 数据事故 | `op_id UNIQUE`；ack 后压缩；冲突保留双方；同步页可见失败原因 |
| 后台同步违反 SCOPE | 耗电/隐私 | 只在前台动作触发；无定时器/轮询 |
| 单进程解析阻塞 | 问答超时 | 单 worker 线程池 + 状态表；上传后异步解析；限制书大小 |
| 语音/回答过期播放 | 交互错乱 | 取消令牌 + 播放前校验 request_id；真机测试 |
| 范围蔓延 | 延期 | V1 不碰卡片/学习/Agent；每项功能过 SCOPE 与资源门禁 |

---

## 10. 与 `SCOPE.md` 的对齐建议

| 现有 SCOPE | 新需求 | 建议 |
|---|---|---|
| 不做复杂打字笔记 | 划线、个人批注、知识卡片 | 改为“短批注/语音批注 + 引用保存；不做长文打字笔记”。知识卡片以“确认入库”为边界。 |
| 不做后台 Wi-Fi 任务/轮询 | 恢复网络后自动同步、主动服务 | 改为“同步只在用户动作或前台事件触发；不做常驻后台任务”。 |
| AI 为被动调用 | 章末自主闲聊 | 保留“默认关闭、只出邀请条、不自动开麦/TTS”；V1 只留开关与钩子。 |
| relay 是唯一后端 | 新增书籍/RAG/同步 | 不新增第二后端；全部落在现有 relay。 |
| 设备端轻量 | 知识库/学习 | 索引、向量、模型全部在服务端；设备只做展示与缓存。 |

> `SCOPE.md` 的修改需要用户确认后再动，本文只给建议。**目前 `SCOPE.md` 尚未改写**（其正文仍写“不做复杂打字笔记”“不做后台 Wi-Fi 任务”），建议随 V1 设备端落地时一并按本表口径更新。

---

## 11. 需要用户拍板的决策（已确认）

> 以下 6 项已于 2026-11-19 按“建议”口径确认，见[任务记录](tasks/2026-11-19-ai-native-implementation.md)“已确认决策”；实施顺序为**服务端先行**。

1. **V1 的“划线/批注”做到什么程度？** → 已定：V1 做“选区 → 保存引用/语音批注 + 跳回原文”，正文常驻划线渲染延后 V1.1。
2. **SD 直接拷贝的书怎么处理？** → 已定：仅对经 Web/relay 上传并索引的书提供带引用问答，未入库的书降级为无引用问答并明确提示。
3. **同步触发是否接受“仅前台动作”？** → 已定：保存后、AI 回合结束、打开同步页、手动重试；不做定时器/后台轮询。
4. **服务端是否接受 SQLite + FTS5 作为 V1/V2 基线？** → 已定：接受；已落地，V2 再评估 `sqlite-vec`/Qdrant。
5. **Web 工作台的认证方式？** → 已定：复用 relay admin token（反向代理加 TLS），V1 不做账号体系；已落地。
6. **是否确认按本文阶段与工期推进？** → 已确认；V1-01 已完成，服务端 V1 已实现。

---

## 12. 启动 V1 的“零业务代码”第一步（已完成）

1. [x] 新建 `docs/ai-native/api-v1.md`：固化 `passage_id`、`/reader/query`、`/sync/push|pull`、错误码。
2. [x] 准备 `relay/tests/fixtures/golden_books/` 黄金测试书与段落对照表（5 本）。
3. [x] 在 relay 建表（`library_store.py`，含 `schema_version` 与 WAL）。
4. [ ] 在固件加 passage 映射的只读日志（不改 UI），用真机对照服务端结果。
   - 数据模型已加，但**解析器尚未接线**、无 `test/passage_ref`；下一步是接线并补主机单测。
5. [ ] 对照通过后，再进入 V1-07 问答 UI 与 V1-08 outbox。

---

## 附录 A：V1 接口（已实现，以 [api-v1.md](ai-native/api-v1.md) 为准）

| 方法 | 路径 | 用途 | 状态 |
|---|---|---|---|
| `POST` | `/api/v1/books` | multipart 上传；返回 `book_id/sha256/status` | ✅ |
| `GET` | `/api/v1/books` | 书籍列表 | ✅ |
| `GET` | `/api/v1/books/{book_id}` | 书籍档案 | ✅ |
| `GET` | `/api/v1/books/by-hash/{sha256}` | 设备按文件哈希查索引状态 | ✅ |
| `GET` | `/api/v1/books/{book_id}/processing` | 解析/索引进度 | ✅ |
| `GET` | `/api/v1/books/{book_id}/chapters` | 章节列表（调试/工作台） | ✅ |
| `GET` | `/api/v1/books/{book_id}/passages` | 段落列表（调试/核对） | ✅ |
| `POST` | `/api/v1/reader/query` | 结构化问答 + 引用校验 | ✅ |
| `POST` | `/api/v1/sync/push` | 上传变更，`op_id` 幂等 | ✅ |
| `GET` | `/api/v1/sync/pull` | 增量拉取 | ✅ |
| `GET` | `/api/v1/device/config` | 增加 `features.readerQuery/bookSync` | ✅ |
| `POST/GET` | `/api/v1/knowledge/cards` | V2 | ❌ |
| `POST` | `/api/v1/learning/goals` | V3 | ❌ |
| `POST` | `/api/v1/agent/tasks` | V4 | ❌ |

错误码（实现值）：`BAD_REQUEST`、`SCOPE_NOT_SUPPORTED`、`UNAUTHORIZED`、`DEVICE_NOT_ALLOWED`、`BOOK_NOT_FOUND`、`BOOK_NOT_READY`、`PASSAGE_NOT_FOUND`、`BOOK_TOO_LARGE`、`UNSUPPORTED_FORMAT`、`UNPROCESSABLE_BOOK`、`BOOK_ID_COLLISION`、`RATE_LIMITED`、`MODEL_UNAVAILABLE`、`MODEL_EMPTY_ANSWER`。

## 附录 B：设备端新增模块（含当前状态）

```text
lib/Epub/Epub/PassageRef.h   passage_id 构造（Ref + formatId）        ✅ 已加（仅头文件）
lib/Epub/.../blocks/TextBlock.*  行级 passage 锚点 + 页缓存序列化     ✅ 已加
lib/Epub/.../ParsedText.*    段内字节游标（setPassage/getPassage…）   ✅ 已加（无调用方）
lib/Epub/.../parsers/ChapterHtmlSlimParser.*  <p>/<li> 接线 setPassage ❌ 待做
lib/BookIdentity/            书籍 SHA-256 + 缓存（sidecar）            ❌ 待做
lib/Passage/                 passage_id 解析/跳转（spine/paragraph）  ❌ 待做
lib/Sync/                    SYO1 outbox、ack 压缩、push/pull 客户端  ❌ 待做
src/activities/reader/AskAiActivity.*   结构化请求 + 分段渲染 + 引用列表 ❌ 待做
src/activities/reader/EpubReaderActivity.*  章节完成事件钩子           ❌ 待做
src/activities/settings/     AI 自主邀请开关、同步页入口               ❌ 待做
test/passage_ref/            主机单测（PassageRef.h 注释已引用）       ❌ 待建
```

## 附录 C：来源标签 UI 建议

- 每段首行固定标签，反色或加粗：`[书中原文]`、`[外部补充]`、`[推断]`。
- `[书里写着]` 段尾给引用编号 `[1]`，按确认键打开“原句”列表；选中引用可“跳回原文”。
- 标签文本走 `tr()`，不硬编码中文；英文为 `[In the book]`、`[Elsewhere]`、`[My reading]`。
- 引用列表是独立半屏/整页，不在正文里插入脚注排版，避免破坏阅读位置。
- 解析失败时显示“未通过引用校验，以下为推断”，不要静默隐藏。

---

**实施状态：** 第 11 节决策已确认；第 12 节第 1–3 步已完成，服务端 V1 与设备端 V1（解析器接线、问答 UI/引用回跳、outbox/同步页、章末讨论与五界面重设计）均已落地并完成主机/真机验证；断网同步闭环、黄金书端到端与 PSRAM 峰值采样均已通过。生产 relay 已升级到 `20261122-125701`，真实 `qwen-flash` 引用问答与设备真机链路均已通过，详见[任务清单](tasks/2026-11-19-ai-native-implementation.md)。
