# 任务记录：AI 原生智能阅读终端渐进式落地（2026-11-19）

需求依据：[AI 原生智能阅读终端_渐进式技术实现文档](../AI原生智能阅读终端_渐进式技术实现文档.md)

评估与计划：[AI 原生智能阅读终端_可行性评估与实施计划](../AI原生智能阅读终端_可行性评估与实施计划.md)

状态：**服务端 V1（书籍入库/引用问答/同步/工作台）已完成并用黄金书 + Mock LLM 测通；设备端 1.4 段落映射/书籍身份、1.5 结构化问答（含引用跳转）、1.6 同步（outbox/push/同步页/冲突展示/笔记入队）、1.7 章末邀请、1.9.1 黄金书真机端到端、1.9.2 断网同步闭环、1.9.3 取消后停止过期语音、1.9.4 PSRAM 采样均已完成。五个阅读伴侣界面已完成 UI 重设计并真机验证，并按 `ui-ux-pro-max` 的极简/反馈原则收口文案与引用回跳交互。生产 relay 已于 2026-11-22 升级到 `20261122-125701`，黄金书在真实 DashScope 上完成入库/引用问答，设备已切回生产 token。**

## 服务端完成记录（2026-11-20）

- 数据层：`relay/relay/library_store.py`（books/chapters/passages/FTS5/annotations/ai_results/sync_ops/sync_changes；软删除；跨设备写保护）。
- 入库：`relay/relay/book_ingest.py`。EPUB 容器/OPF/spine/目录交给 `ebooklib`，章节 DOM 用 `BeautifulSoup + lxml`，TXT 编码用 `charset-normalizer`；段落计数、文本规范化与 `passage_id` 仍按冻结契约实现。
- 问答：`relay/relay/citation.py`（结构化解析、引文子串校验、`book` 无有效引用降级 `inference`）、`reader_query.py`（selection/current_passage/current_chapter 检索与预算）。
- HTTP：`/api/v1/books`、`/by-hash`、`/processing`、`/chapters`、`/passages`、`/reader/query`、`/sync/push|pull`；`/api/v1/device/config` 增加 `readerQuery/bookSync/knowledgeCards`。
- 工作台：`/admin` 增加“阅读书库”（上传/索引状态/软删除）与“笔记与引用”（只读），复用 admin token。
- 测试：`cd relay && pytest -q` → **329 passed**。另用真实 `uvicorn` 进程 + HTTP 客户端（非 TestClient）跑通：401 鉴权 → 70 MB 上限内的 multipart 上传 → 202 后台解析 → 轮询 ready → `/by-hash` → 段落 ID 对照 → 引用问答 → push 重试为 duplicate → pull → 跨设备写 rejected。
- 边界加固：软删除后重新上传同一文件会恢复原书记录（不再撞 `sha256` 唯一约束）；解析后的落库异常会标记 `failed` 而非卡在 `indexing`；新增 200,000 段落上限，防止超大 TXT 撑爆内存。
- `test_reading_chain.py` 覆盖上传 → 索引 → 引用问答 → push/pull → 重试幂等的完整链路；`test_golden_books.py` 逐段比对 5 本黄金书。
- 部署：`requirements.txt` 增加 ebooklib/beautifulsoup4/lxml/charset-normalizer；nginx 两套配置增加 `/api/v1/books`（70m body）、`/api/v1/reader/`、`/api/v1/sync/`；`run/library/` 已加入 install.sh。
- 当时后置、后续已补齐：真实云端 LLM 实测、V1 设备端固件、设备端离线 outbox 与引用跳转；服务端不保存原始语音。

## 固件进展（2026-11-21）

- 新增 `lib/Epub/Epub/PassageRef.h`：`EpubPassage::Ref{kind,index}` + `formatId()`，唯一收敛 `passage_id = <book_id>:<spine>:<p|li><index>` 规则；纯头文件、无设备依赖。列表项前缀按契约为 `li`。
- `lib/Epub/Epub/blocks/TextBlock.{h,cpp}`：每行文本新增 passage 锚点（`Ref + charStart/charEnd`，UTF-8 字节偏移），并写入/读出页缓存序列化（`TextBlock::serialize/deserialize`）；未设置段落时为空（封面、`<div>` 散排文字）。
- `lib/Epub/Epub/ParsedText.{h,cpp}`：段内字节游标（`setPassage/clearPassage/getPassage*`），逐 token 记录 `wordPassageStart/End`；focus 拆词、CJK 拆字、连字符拆词均同步拆分偏移；行级范围在 BiDi 重排前按逻辑顺序计算，装饰性 `•` 计 0 字节。
- 归一化对齐：NFC + 去零宽字符 + 空格折叠 + CJK/CJK 标点间隙删除；`&nbsp;` 作为待定空格延迟提交，行内 `<img>` 不产生字节也不额外插入空格。
- `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.{h,cpp}`：`<p>/<li>` 文档顺序 0-based 计数放在 skip 早退之前；passage 栈保存/恢复外层游标（服务端 `_block_text` 排除嵌套块，语义一致）；`<br>` 通过块同步继续同一段游标。
- 新增 `src/util/BookIdentity.{h,cpp}`：流式 SHA-256，sidecar `/.crosspoint/books/<base>-<pathhash>.id` 缓存 `"<16 hex> <size>\n"`；大小变化时重算。
- `AskAiActivity`：`LineRef` 携带 passage 与字节范围；`collectQueryPassages()` 合并当前页/选区为最多 8 个 `passage_id + [start,end)`；`ensureBookId()` 懒计算书籍身份。已接入 `reader/query`：indexed 书走引用问答，选区/页面/章节分别映射 `selection/current_passage/current_chapter`，未入库或服务端返回 `BOOK_NOT_FOUND` 时自动降级 `/chat` 并显示“未入库”提示。
- 新增 `lib/ReaderQuery/`：`ReaderQueryCodec`（请求构造、结构化响应解析、来源/引用/警告，纯 C++ 可主机测试）与 `ReaderQueryClient`（ESP-IDF 传输，PSRAM 缓冲）。响应文本按来源分段渲染，引用编号 + 引用列表；确认键可跳到原文（spine + page），`request_id` 不一致的迟到响应直接丢弃。
- 新增 `lib/Sync/`（V1-06 首段）：`SyncOutbox`（SYO1 JSONL、512 条/256 KB、loadBatch/clear）、`SyncCodec`（push 请求/响应、AI result op 序列化）、`SyncClient`（ESP HTTP，PSRAM 请求/响应）。`AskAiActivity` 在回答保存后生成 `ai_result.create` 入 outbox；串口提供 `SYNC_STATUS`/`SYNC_PUSH` 两个验证命令。
- 主机测试：新增 `test/passage_ref/`、`test/reader_query_codec/`、`test/sync_codec/`，`ctest` **312 passed**。
- 资源：`pio run -e onepage_s3` 成功；Flash 5,508,175 B（基线 5,482,512 B，+25.1 KiB），静态 RAM 141,536 B / 43.2%。
- 已知边界：图片解码失败时固件会插入 `[Image: alt]` 文本，服务端不索引该文本，该段后续偏移会整体偏移；普通图片与独占段落的图片不受影响。CSS `display:none` 行内文字、以及用作排版表格的内部 `<p>` 也可能造成同类偏移（服务端不解析 CSS/表格布局，固件会跳过）。待后续给 `characterData` 增加 `countsForPassage=false` 路径，并评估服务端是否提供 `raw_html` 偏移校验接口。

## 稳定性修复：预索引触发看门狗重启（2026-11-21）

真机验证时发现开机后反复重启，串口日志给出直接证据：

```text
task_wdt: - ActivityManager (CPU 0)     # 渲染任务未喂狗（任务名被截断）
task_wdt: CPU 0: IDLE0
task_wdt: CPU 1: preIdx
rst:0x8 (TG1WDT_SYS_RST)                # 或 INT_WDT 触发的 RST
```

预索引任务栈：`PreIndexer::processJob → Section::createSectionFile → ChapterHtmlSlimParser` 解压图片 → `ZipFile::readFileToStream → HalFile::write → SdFat SPI`。它在写 SD 图片缓存时长时间持有 `storageMutex`；渲染任务等待该锁超过看门狗窗口后复位。首页空闲把 CPU 降到 10 MHz 会放大等待时间，而频率切换与进行中的 SPI/SD 事务竞争又会触发另一类复位。

已落地修复：

- `PreIndexer::processJob` 持有 `HalPowerManager::Lock`，预索引期间保持 240 MHz。
- `HalPowerManager` 的锁从“只允许一个持有者”改为引用计数，渲染锁与后台任务可共存。
- 等待 `storageMutex` / `modeMutex` / `renderingMutex` 时每 500 ms 调 `esp_task_wdt_reset()`（仅对已订阅任务生效），慢 SD 写降级为卡顿而不是重启。
- `ZipFile` 的 stored/deflated 解压循环每 8 KB `vTaskDelay(1)`。
- `PreIndexer::suspend()` 等待从 5 s 降到 2 s。
- `main.cpp` 启动时把任务看门狗窗口从 5 s 调到 30 s（`esp_task_wdt_reconfigure`）。
- 首页空闲降频增加 `!PreIndexer::busy()` 条件：预索引工作期间不再切 10 MHz。

真机验证（`/dev/ttyACM0`，新固件）：一次启动内 5 本书 `Pre-indexed` 完成，随后进入阅读、连续翻页，**无 `task_wdt`、无 `rst:0x8`/`rst:0xc`**。删除 36 MB 图文 EPUB 的 sections/images 后冷缓存重建 `chapter 281/2086` 也在约 6 秒内完成，捕获窗口内无复位。仍需在真实 SD 卡寿命/满卡场景做长时间 soak test；若单次 SD 写超过 30 s，看门狗仍会按设计复位。

## 真机验证：V1-05 引用问答（2026-11-21）

临时把设备 `relayUrl` 指向本机 relay（只改 URL，token/deviceId 未动），上传与设备 SD 完全相同的 `Rust 程序设计语言` EPUB，使用 mock LLM 返回一条 spine 67 的合法引用。设备端结果：

- AI 书童 → “总结本章” → `reader/query` 成功；
- 回答页显示 `[书中原文]` / `[推断]` / `[外部补充]`，书中原文段带 `[1]` 引用编号；
- 确认键打开引用列表，显示 `1. 最简单直接的智能指针是 box`；
- 再确认“跳回原文”，阅读器定位到 **15.1 使用 Box<T> 指向堆上的数据 1/15 页**，即引用所在页；
- 回答已写入书签（`已存入书签`）。

测试后已把 `relayUrl` 恢复为 `ws://118.25.20.250:8080/api/v1/voice/stream`，本机 relay 与 webremote 已停止。当时远端仍是旧版（`/api/v1/reader/query`、`/api/v1/books` 返回 404）；该段仅记录当轮本机联调，生产部署已于 2026-11-22 完成。

## 续作：同步页、笔记同步与章末邀请（2026-11-21 晚）

在 opencode 留下的半成品上继续完成 V1 剩余项，并修复其构建遗留：

- 构建修复：`test/sync_codec/CMakeLists.txt` 的路径分隔符被写成 ` / `，CMake 无法生成；`SyncRunner.cpp` 的局部 `Client::Stats stats` 遮蔽了 `Sync::stats()`；`AiSyncActivity.cpp` 把 `StrId` 枚举当成了非限定常量。三处修复后主机测试与固件均恢复可构建。
- 1.6.3 同步页：`AiSyncActivity`（设置 → AI 同步）显示待同步条数/KB、服务配置、Wi-Fi 状态；确认键在 Wi-Fi 断开时先走选择页，连接后按批 push 并显示完成/部分失败；`SyncRunner::pushAll` 逐批消费最多 512 条 outbox，失败保留待重试。
- 1.6.4 冲突展示：`RunSummary` 区分 `rejected`/`conflict`，同步页显示“冲突 N · 拒绝 M”和首条失败 `op_id`；V1 按服务端策略保留双方并留在 outbox，不做自动合并。
- 1.6.2 笔记入队：语音笔记与快捷书签保存后生成 `note.create`（`SyncCodec::buildNoteCreateOp`），带 `book_id`、书名基线、当前页 excerpt、spine/page 锚点；书籍身份通过 `BookIdentity` 的 sidecar 缓存获取。AI 回答仍走 `ai_result.create`。选区“保存引用”按 D1 留在 V1.1。
- 1.7 章末邀请：新增默认关闭的设置 `chapterAiInvite`（设置 → 系统 → 章末 AI 邀请，JSON key 同名）；开启后在本章最后一页手动前翻时只画一条底栏邀请，不自动开麦/TTS。确认后直接以本章上下文发起“章节讨论”请求（`current_chapter` + `chapter_summary` + 讨论式问题），回答包含 AI 对本章观点的理解和两个引导性开放问题，不再进入 AI 书童预设列表；返回或再次前翻直接进入下一章；同一章只邀请一次。自动翻页和朗读翻页不触发。
- 文案：补充 `STR_AI_CHAPTER_INVITE*` 与 `STR_AI_SYNC_CONFLICT*`，重新生成 i18n 表。
- 测试：新增 `SyncNote.SerializesNoteCreateWithAnchorAndEscapes`、`SyncNote.RejectsMissingBookIdAndShortBuffer`，主机 `ctest` **314 passed**（当时基线）。
- 资源（`onepage_s3`）：Flash 5,514,675 B / 6,553,600 B（84.1%），静态 RAM 139,488 B / 327,680 B（42.6%）；相对基线 5,482,512 B 增加 32,163 B（约 31.4 KiB），在 150 KiB 门禁内（后续 UI 重设计后的最新数字见文末）。
- 真机验证（`/dev/ttyACM0`，2026-11-21）：烧录后启动到首页无复位；设置中“章末 AI 邀请”默认关，打开后在本章最后一页前翻出现邀请条；确认后直接进入章节讨论（Wi-Fi 选择 → 回答页），回答为本章观点 + 两个开放问题；返回后再次前翻正常进入下一章。截图存于 `.screenshots/device-verify2/`。

## 同步 outbox 首段（2026-11-21）

- 主机测试：`SyncCodec` 6 项（批请求、applied/duplicate/rejected/conflict、缺失结果、AI op 序列化与转义）全部通过。
- 固件：`onepage_s3` 构建通过，包含 `SyncOutbox`、`SyncCodec`、`SyncClient`、AI 结果入队和串口 `SYNC_STATUS`/`SYNC_PUSH`。
- 真机：outbox 文件可通过工作台写入 `/.crosspoint/sync/outbox.syo1`；`SYNC_PUSH` 在串口打开导致设备复位、Wi-Fi 断开时返回 `NO_WIFI`，当时尚未完成一次成功的设备端 push。
- 待复测：保持 Wi-Fi 连接、不触发 USB 复位的情况下执行 `SYNC_PUSH`，核对服务端 `ai_results`/`sync_ops`，然后补同步页 UI。
- 已知噪声：等待锁的喂狗循环在未订阅任务上会打印 `esp_task_wdt_reset: task not found`；不影响复位策略，后续清理守卫条件。

## 断网同步闭环与 UTF-8 修复（2026-11-22）

- 场景：本机 relay（`mock` ASR/LLM、设备 token、全新 `library.db`）+ 真机 `onepage_s3`，Wi-Fi 关闭时在阅读器内保存一条快捷书签。
- 断网：`SYNC_STATUS lines=1 bytes=456`；`SYNC_PUSH` 返回 `NO_WIFI`，outbox 保留。
- 联网：从设置进入 Wi-Fi 选择并连接，随后执行 `SYNC_PUSH`：`POST http://192.168.1.25:8000/api/v1/sync/push (1 ops, 540 body bytes)` → `HTTP 200`、`pushed 1 ops: 1 results, allAcked=1`、`remaining=0`。
- 幂等：再次 `SYNC_PUSH` 为 `OK batches=0`、`lines=0`；把同一 `op_id` 重放给服务端返回 `duplicate`，`sync_ops` 仍为 1 条、`annotations` 仍为 1 条，没有重复落库。
- 发现并修复：第一次推送返回 `HTTP 400 There was an error parsing the body`。原因是页面摘录按字节预算截断，末尾留下不完整 UTF-8 序列，整段 JSON 被判为非法 UTF-8。`lib/Sync/SyncCodec.cpp` 的 `appendEscaped()` 现在校验 UTF-8，遇到非法/截断序列写入 U+FFFD（`EF BF BD`），保证 outbox 中的任何文本都能生成合法 JSON；新增 `SyncNote.ReplacesInvalidOrTruncatedUtf8InJson`，主机 `ctest` **315 passed**。
- 说明：本轮用快捷书签覆盖“断网笔记”路径；语音笔记依赖 ASR，未在断网场景单独复测。最终烧录会清空设备 NVS，测试后已把 `relayUrl` 恢复为 `ws://118.25.20.250:8080/api/v1/voice/stream`、`deviceId` 恢复为 `onepage-s3`；当时 token 为空；最终部署后已通过 `CMD:RELAY` 写入生产 token。

## UI 重设计与阅读语气（2026-11-21）

用户反馈三个界面问题：五个阅读伴侣界面缺少统一视觉语言；章末互动不应使用“问题/回答”这类考试化措辞；讨论题应左右选择后直接进入语音。已完成：

- 新增 `src/components/CompanionUi.{h,cpp}`：`drawSectionLabel`（细线分区标题）、`drawSelectRow`（序号方块 + 浅灰焦点卡 + 左侧书签缺口）、`drawChip`（状态胶囊）、`drawStepDots`（4 个讨论步骤点）。全部尺寸取自 `ThemeMetrics`，只用黑/白/浅灰，无动画、无新字体。
- AI 书童：预设列表改为“想聊点什么”编辑部式列表，问法与范围分开呈现；回答页改为“你刚刚问的”灰色引文卡 + “书童说”正文，保存态是胶囊；引用页增加分区标题。预设文案改成第一人称口语（“这里我没读明白”“这和我有什么关系”），不再像练习册。
- 语音书签：空闲页改为“当前页”卡片（页码 + 书名）+ “存一条书签”说明 + “开始说话”主行动条；快捷书签改为 6 行焦点列表 + “将存入”预览卡；书签列表/已保存页使用同一套分区与纸卡。
- 查字典：字头改为方形灰底字印；释义区使用“字条”分区，带标签的释义悬挂缩进；失败态改为灰底提示卡；正文预留右侧上/下键宽度。
- 英语复习：新增顶部进度条；正面单词放灰色词卡；评级改为“刚才想起来了吗？”+ “没想起来 / 有点模糊 / 想起来了”三张焦点卡；背页正文预留侧键宽度；按钮从“提交”改为“记下”。
- 章末 AI 对话：回答页不再展示冗长的 relay prompt，改为“聊聊这一章 / 这一章，读完了”；底部讨论卡白底清屏后再铺浅灰，避免正文透出；4 个系统话题用步骤点表示，底部提示改为“左 / 右”，确认后直接进入语音；讨论模式不自动 TTS。
- 语气统一：移除界面中的“问题/回答/观点/请选择/提交/查看答案”等考试化措辞；章末邀请改为“这一章，读完了 / 聊两句”；提问相关改为“你刚刚问的 / 书童说”“就聊这一段”。
- 真机验证（`/dev/ttyACM0`）：章末邀请 → 讨论页（左/右切到第 2 问）→ 语音“说说你的想法” → `CMD:VOICE_TX` 注入观点 → 书童返回针对观点的回应并保留讨论话题；截图存于 `.screenshots/ui-redesign-3/`。
- 当时的构建：Flash 5,520,923 B / 6,553,600 B（84.2%），静态 RAM 139,488 B / 327,680 B（42.6%）；相对 5,482,512 B 基线增加 38,411 B（约 37.5 KiB），仍在 150 KiB 门禁内。主机 `ctest` 315 passed，`pio check` 0 high/0 medium，改动文件已 clang-format，i18n 已重新生成。最终数字见 1.9.4。

## UI skill 复核与交互收口（2026-11-22）

用项目自带 `ui-ux-pro-max` 做了一次针对墨水屏的复核：`--design-system "e-reader reading companion monochrome minimal"` 返回 Minimalism & Swiss Style（高对比、网格、克制装饰），与现有“纸面编辑式”方向一致；`--domain ux` 的进度反馈、空状态、确认与焦点规范用于检查五个界面。没有引入颜色、动画、新字体或新布局组件，只做文案与回跳交互收口：

- 来源标签从 `[书中原文]/[推断]/[引用]` 改为 `[书里写着]/[我的理解]/[原句]`，英文同步为 `In the book / My reading / Passages`，减少术语感。
- “原句”页新增一行说明：“这些是回答里提到的原句。确认可回到书页。”，并把原来的截断列表行改成整张浅灰 pull-quote 卡：序号 `01 / 总数` 独立成页眉行，正文用 UI 12 号字完整换行，`上/下` 切换多句，底部“跳回原文”形成明确因果。
- 修复引用回跳后同一次 Confirm release 误开阅读菜单：`EpubReaderActivity::launchAskAi()` 的结果回调设置 `ignoreNextConfirmRelease`，真机复测回跳后停留原文页。
- 新增 i18n key `STR_AI_CITATION_HINT`（777 keys / 28 languages，其他语言回退英文）。

## 边界（先看再动手）

- 设备端绝不跑大模型；只做采集、排版、渲染、缓存、同步状态机。
- AI 回答必须带来源分层：`[书里写着] / [别处读到] / [我的理解]`，且原文引用必须通过服务端校验。
- 自主侵入式功能默认关闭；章末只出邀请条，不自动开麦/TTS；V1 只留开关与事件钩子。
- 同步只在前台动作触发（保存后、AI 回合结束、打开同步页、手动重试），不做后台常驻任务。
- V1 不碰知识卡片、学习计划、Agent、完整章末闲聊、跨书检索。
- 资源门禁：Flash 增量 ≤ 150 KB；大缓冲走 PSRAM；每次里程碑复测 Flash/RAM。

---

## 阶段 0：基线与协议冻结（进行中；0.3–0.5 已完成）

- [ ] 0.1 输出《硬件能力清单 + 已知问题清单》
  - 内容：S3R8/16MB Flash/8MB PSRAM、800×480 SSD1677、按键、PDM/I2S、SD、RTC、IMU、SHT30、电源；已知问题与规避。
  - 落点：`docs/` 新文档或并入本任务附录。
- [ ] 0.2 记录构建与资源基线
  - `pio run -e onepage_s3` 的 Flash/RAM；`firmware.bin` 大小；OTA 余量。
  - 当前基线：5,482,512 B / 6,553,600 B；静态 RAM 42.6%。
- [x] 0.3 冻结 `passage_id` 规则
  - `<book_sha256_16>:<spine>:<p|li><index>`；选区字符偏移格式。
  - 明确 `<p>/<li>` 计数规则、非正文块降级规则。
- [x] 0.4 冻结 V1 API Schema
  - `/api/v1/books`、`/by-hash`、`/processing`、`/reader/query`、`/sync/push|pull`。
  - 错误码、鉴权头、分页、幂等语义。
- [x] 0.5 准备黄金测试书
  - 中文 2 本、英文 2 本、图文混排 1 本；每本抽取 20 段做 ID 对照。
  - 落点：`relay/tests/fixtures/`（注意体积，必要时用脚本生成/裁剪）。

**阶段 0 验收：** 文档评审通过；同一段落设备端/服务端 ID 对照一致；Schema 不再改动。

---

## 阶段 1：V1.0 MVP（服务端已完成，设备端进行中）

目标：导入一本书 → 离线阅读 → 联网对当前段落提问 → 回答带定位 → 笔记保存并只同步一次。

### 1.1 relay：书籍入库与索引

- [x] 1.1.1 新增书库存储（落点 `library_store.py`，合并 books/passages/annotations/sync）
  - `books/chapters/passages` 表；`sync_ops` 表；幂等迁移。
  - 书籍状态：`pending/parsing/indexing/ready/failed`。
- [x] 1.1.2 新增 `book_ingest.py`
  - 上传落盘、SHA-256、大小/格式校验、异步解析任务（FastAPI BackgroundTasks，同步函数在线程池执行）。
  - EPUB：ebooklib 读容器/元数据/spine/目录 + BeautifulSoup 解析正文；TXT：单章 + 段落切分。
  - 输出 `chapters`、`passages`；`<p>/<li>` 计数与固件规则一致。
- [x] 1.1.3 FTS5 trigram 索引
  - `passages_fts`；1–2 字查询回退 `LIKE`；索引重建脚本 `tools/rebuild_fts.py`。
- [x] 1.1.4 接口
  - `POST /api/v1/books`（multipart）
  - `GET /api/v1/books/{id}`、`GET /api/v1/books/by-hash/{sha256}`
  - `GET /api/v1/books/{id}/processing`
- [x] 1.1.5 测试
  - 上传成功/重复上传/坏文件/超大文件/解析失败/状态轮询。
  - 黄金书章节数、段落数、抽样 ID 对照。

### 1.2 relay：结构化问答与引用校验

- [x] 1.2.1 检索
  - 当前段落 + 前后邻段；章节总结走整章检索；上下文 token 上限。
- [x] 1.2.2 Prompt 与 JSON 输出
  - 强制 `segments[].source ∈ {book, external, inference}`；禁止模型自造标签。
  - 非 JSON/解析失败 → 整段 `inference`。
- [x] 1.2.3 引用校验
  - `passage_id` 存在且属于当前书/章节；`quote` 归一化后必须是原文子串。
  - 失败：删除 citation；`book` 段无 citation 则降级 `inference`。
- [x] 1.2.4 接口
  - `POST /api/v1/reader/query`；兼容旧 `/api/v1/chat` 不动。
  - 请求/响应日志脱敏；指标 `reader_queries_total` / `reader_query_ms`，日志只带 request_id/book_id/scope，不记录问题与正文。
- [x] 1.2.5 测试
  - 正常引用、伪造引用、跨书引用、空上下文、模型超时、取消。
  - 说明：上游超时映射为 `MODEL_UNAVAILABLE`；HTTP 断连不作为服务端任务取消，V1 由设备用 `request_id` 丢弃过期响应。

### 1.3 relay：同步

- [x] 1.3.1 `library_store.py` 同步段
  - `sync_ops(op_id PRIMARY KEY)`；`applied/duplicate/rejected/conflict`。
  - `annotations` 表；软删除；字段级 `updated_at`。
- [x] 1.3.2 `POST /api/v1/sync/push`
  - 批量操作；逐条幂等；部分成功返回每条结果。
- [x] 1.3.3 `GET /api/v1/sync/pull`
  - 增量游标；只拉确认状态/服务端变更。
- [x] 1.3.4 测试
  - 重复 `op_id`、乱序、冲突保留双方、断线重试、批量部分失败。

### 1.4 固件：passage 映射

- [x] 1.4.1 书籍身份
  - 首次打开计算 SHA-256，缓存 sidecar（`/.crosspoint/books/<base>-<pathhash>.id`）。
  - 服务端同一文件算出的 `sha256[:16]` 与设备端一致；Web 上传路径无需额外 sidecar。
- [x] 1.4.2 排版期记录段落
  - [x] 新增 `PassageRef.h`；`TextBlock` 行级锚点（`Ref + charStart/charEnd`）+ 页缓存序列化。
  - [x] `ParsedText` 段内字节游标（`setPassage/getPassage/clearPassage`，含 `<br>` 断行串联与 NBSP/行内图片对齐）。
  - [x] `ChapterHtmlSlimParser` 在 `<p>/<li>` 处接线，文档顺序 0-based 计数在 skip 早退之前，passage 栈处理嵌套。
  - [x] `test/passage_ref/` 主机单测并注册到 `test/CMakeLists.txt`。
  - [x] 内存增量评估：段落锚点常驻每行，接线后复测 Flash/RAM（段落映射 + 身份模块合计 +15.6 KiB Flash，静态 RAM 不变）。
- [x] 1.4.3 选区块映射
  - `AskAiActivity::LineRef` 增加段落号；跨段选区生成 `passage_ids[]` + 字符范围。
  - 页面/章节 scope 也带上当前 `passage_id` 与 `spine`。
  - 注：请求体发送在 1.5.1 接入。
- [x] 1.4.4 引用跳转
  - `passage_id → spine + paragraph → page`；复用 `ProgressMapper` / xpath 解析。
  - 真机已验证：引用列表 → 跳回原文页。返回到跳转前阅读位置的显式栈留到 V1.1（2.2）。

### 1.5 固件：结构化问答 UI

- [x] 1.5.1 请求
  - 新增 `reader/query` 客户端；保留 `/chat` 给语音助手与未入库降级。
  - 请求体在 PSRAM 组装；`request_id` 用于取消/去重。
- [x] 1.5.2 响应
  - 整包 JSON 流式解析到 PSRAM 模型；超过 12 KB 文本或 32 KB 报文时截断并标记。
  - 分段渲染 + 固定来源标签 + 引用编号；长文分页滚动。
- [x] 1.5.3 引用列表
  - 确认键打开；显示引文；确认后按 spine + passage 扫描页缓存定位并跳回原文。
  - 定位是逐页扫描（页缓存没有 passage 索引），超长章节会慢；V1.1 可在 section 头加 passage→page 索引。
- [~] 1.5.4 取消
  - `request_id` 不一致的迟到响应丢弃。
  - 阻塞请求进行中按 Back 仍会等本轮结束（与 `/chat` 相同），后续再做取消。
- [x] 1.5.5 记录
  - AI 结果摘要写 NoteStore；来源标签与引用保留在正文中。

### 1.6 固件：离线 outbox 与同步页

- [x] 1.6.1 `lib/Sync/SyncOutbox.*`
  - SYO1 JSONL；`append/stats/loadBatch/clear`；512 条/256 KB 上限；append 拒绝超限而不是静默丢弃。
- [x] 1.6.2 记录接入
  - [x] AI 结果：`ai_result.create`，包含 passage_ids/question/segments/citations，保存回答后入 outbox。
  - [x] 语音笔记与快捷书签：`note.create`（book_id、excerpt、spine/page 锚点）。
  - 选区“保存引用”随 V1.1（2.1）实现；AI 回答中的引用已随 `ai_result` 同步。
- [x] 1.6.3 同步执行
  - [x] `SyncCodec`（push 请求/响应、AI/笔记 op 序列化）+ `SyncClient`（ESP HTTP，PSRAM 缓冲）。
  - [x] 串口验证命令 `SYNC_STATUS` / `SYNC_PUSH`。
  - [x] 同步页 UI（待同步数、错误、立即同步、分批消费 outbox）；按 D3 只在前台动作触发，不做后台常驻。
- [x] 1.6.4 冲突展示
  - 服务端 `rejected/conflict` 已能解析；同步页显示两类计数与首条失败 `op_id`，条目保留在 outbox。V1 不做自动合并选择（服务端保留双方），冲突解决留到 V1.1。

### 1.7 固件：章末钩子与开关（V1 只预留）

- [x] 1.7.1 设置项 `AI 自主邀请`，默认关闭；文案走 `tr()`。
- [x] 1.7.2 阅读器检测“本章最后一页翻过”，发出内部 `chapter_completed` 日志事件。
- [x] 1.7.3 关闭时零打扰；开启时只画一条章末邀请条，不自动开麦/TTS，也不打开全屏。
- [x] 1.7.4 确认键直接发起“本章讨论”：`current_chapter` 范围 + `chapter_summary` intent，问题要求 AI 给出本章观点、必要时引用原文，并留下两个可接话的话题；不再打开通用 AI 书童预设列表。回答页底部给出 4 条系统讨论话题，左/右切换，确认后直接进入语音输入；转写作为“我的想法”回传。讨论模式不自动 TTS。完整多轮闲聊仍属 V3。

### 1.8 Web 最小工作台

- [x] 1.8.1 上传页（EPUB/TXT）、进度、错误。
- [x] 1.8.2 书籍列表/索引状态/删除（软删除）。
- [x] 1.8.3 笔记/引用列表（只读即可）。
- [x] 1.8.4 认证复用 relay admin token；文档注明反向代理 TLS。
- [x] 1.8.5 隐私开关占位（是否保存原始语音默认关）。

### 1.9 验证与交付

- [x] 1.9.1 黄金书端到端：上传 → 索引 → 设备问答 → 引用跳转。
  - 真机证据（2026-11-22，`relay/tests/fixtures/golden_books/zh_prose.epub`，本机 relay + Mock LLM）：同一文件经 Web 文件管理器写入设备 SD、经 `/api/v1/books` 上传 relay；索引 `ready`（2 章 / 20 段，`book_id=9e08598fd2076fdb`）。设备打开《山间书店》后选“这一章在说什么”，回答页显示“书里写着 / 我的理解”，结构化引用 `9e08598fd2076fdb:0:p4` 通过服务端子串校验，`warnings=[]`；按确认进入“原句”页，再确认跳回 `spine 0 page 0`，页面内容与引文一致。回跳后曾误触发阅读菜单，已在 `launchAskAi()` 结果回调中补 `ignoreNextConfirmRelease`，复测回跳后停留原文页。
- [x] 1.9.2 断网笔记 → 联网同步 → 重复同步不重复落库。
  - 真机证据：断网 outbox 1 条/456 B，`SYNC_PUSH` 返回 `NO_WIFI`；联网后 HTTP 200、`acked=1`、`remaining=0`；再次推送 0 批次；同一 `op_id` 重放返回 `duplicate`，服务端 `sync_ops`/`annotations` 均为 1 条。测试中还修复了摘录截断导致的非法 UTF-8 请求体（HTTP 400），并补 `SyncNote.ReplacesInvalidOrTruncatedUtf8InJson`。
- [x] 1.9.3 取消提问 → 不播放过期 TTS。
  - 真机证据：AskAi 回答开始 TTS（375 字符，`[SPK] start`）后按 Back，日志出现 `[TTS] stopped by caller`、`[TTS] socket closed`，返回阅读器后无新的 `SPK start`/TTS 帧；`AnswerVoice::stop()` 在 `onPause()` 中同步取消播放并等待任务结束。
- [x] 1.9.4 资源复测：最终固件 5,520,967 B / 6,553,600 B（84.2%），静态 RAM 139,488 B / 327,680 B（42.6%），相对 5,482,512 B 基线 +38,455 B。真机 PSRAM 总 8,388,608 B，阅读页空闲约 7.44 MB；黄金书问答 + 引用回答期间最低 `PsramF=7,364,168 B`，即峰值占用约 1,024,440 B（≈1.0 MB，12.2%），TTS 朗读期间最低约 7.45 MB。采样来自 `[MEM]` 周期日志，未捕获毫秒级瞬时峰值。
- [x] 1.9.5 门禁：`pio run -e onepage_s3`、`pio run -e onepage`、主机单测 315、改动文件 clang-format、web core 同步、`pio check` 0 high/0 medium、relay pytest 329 均通过。
- [x] 1.9.6 更新本任务文档中的实际数字。

**V1.0 验收：** 见可行性计划第 8 节；未通过不开 V1.1。

### 生产 relay 部署（2026-11-22 完成）

- 安装包：`relay/dist/voice-relay-20261122-122749.tar.gz`，SHA-256 `5996b1a2…cbfeac`；上传后远端校验一致。
- 部署账号 `ubuntu@118.25.20.250`，执行 `deploy/install.sh --source … --mode dev --public-url ws://118.25.20.250:8080/api/v1/voice/stream`，生成 release `20261122-125701`，`current` 已原子切换。
- 保留现有 `/etc/reading-companion/voice-relay.env` 与 device token（升级前备份为 `voice-relay.env.bak-20261122`）；install.sh 自动补齐 `RELAY_LIBRARY_DIR=/opt/reading-companion-relay/run/library`、`RELAY_DEVICE_DIR` 等新键。
- nginx 8080 配置更新，新增 `/api/v1/books`（70m body）、`/api/v1/reader/`、`/api/v1/sync/`；`nginx -t` 通过，服务 active。
- 验证：`/healthz` 返回 `version=20261122-125701`；`/readyz` 为 `asr=configured, llm=configured, dict=local`；带 token 请求 `/api/v1/books` 返回 200、`/api/v1/reader/query` 对不存在书籍返回 404 `BOOK_NOT_FOUND`、`/api/v1/sync/push` 空批次返回 400 `BAD_REQUEST`，三条路由均已暴露且鉴权生效。
- 真实链路：生产环境上传黄金书《山间书店》，`book_id=9e08598fd2076fdb`，2 章 / 20 段，`ready`；`reader/query` 由真实 `qwen-flash` 返回 `book` 段及两条引用 `9e08598fd2076fdb:0:p4`、`:0:p7`，`warnings=[]`（input 638 / output 188 tokens）。设备写入生产 token 后真机复测通过，回答显示 `[书里写着]` 与两条可回跳引用。
- 说明：为验证上传的《山间书店》保留在生产书库；设备当前指回生产 URL、`deviceId=onepage-s3`、token 已写入。
- 回滚：`ln -sfn /opt/reading-companion-relay/releases/20261116-133333 /opt/reading-companion-relay/current && systemctl restart reading-companion-relay`。

---

## 阶段 2：V1.1 加固（进行中）

- [x] 2.1 选区跨段 → 多 `passage_id`；保存引用/语音批注。选区确认后先把选中原文作为 `highlight` 引用写入 outbox，再发 AI 请求；同一选区重试只写一次。
- [x] 2.2 引用编号 → 页码跳转；返回阅读位置。
- [x] 2.3 章末邀请条（仅开启时）：`[坐下聊会儿] [先往下读]`；不自动语音/全屏。
- [ ] 2.4 同一章“先往下读”后不重复纠缠；最多一次“下次提醒”。
- [ ] 2.5 正文常驻划线渲染（单独立项，需用户确认是否进 V1.1）。
- [ ] 2.6 取消/超时/弱网回归；同步失败恢复。

### V1.1 进展：选区引用落库（2026-11-22）

- `AskAiActivity` 的 Range 选区原本已能跨段收集最多 8 个 `passage_id` 并发送 `selected_text`；本轮补上持久化：确认范围后、AI 请求前，把选区作为 `highlight` 引用写入 `SyncOutbox`，AI 回答仍按原路径写 `ai_result.create`。
- `selectionQuoteSaved_` 保证同一选区在失败重试时不重复写引用；新选区进入 `State::Selecting` 时重置。
- 真机验证：黄金书第 1 章选 2 段（`p0`、`p1`），日志出现 `selection quote queued: an_… (419 bytes, 2 passages)` 与 `ai_result queued: ai_… (668 bytes)`；`SYNC_STATUS lines=2`，`SYNC_PUSH` 返回 `2 results, allAcked=1`、`remaining=0`，服务端两条 op 均为 `applied`。
- 服务端 `annotations` 实际落库：`passage_id=9e08598fd2076fdb:0:p0`、`kind=highlight`、引文为“山间的书店有一盏灯。\n灯下是一张旧木桌，桌面被许多人的手磨得发亮。”
- 联调中发现并修复 CJK 选段拼接：布局把汉字拆成独立 token，原先用空格连接会得到“山 间 的 书 店”；现在复用服务端 normalization 的 CJK/标点间隔规则，并把同一段内的排版换行合并，只在跨段时保留换行。
- 验证后已清理生产库中 4 条测试引用（`annotations` 计数回到 0），黄金书保留。
- 门禁：`pio run -e onepage_s3` 通过（5,522,495 B / 84.3%，静态 RAM 139,488 B / 42.6%）、`pio run -e onepage` 通过、`pio check` PASSED、`ctest` 315 passed、`sync_web_core.py --check` 与 `build_html.py` 通过。

---

## 阶段 3：V2 知识库（backlog）

- [ ] 3.1 章节摘要/全书导读（服务端生成、缓存、设备只展示）。
- [ ] 3.2 `KnowledgeCard` CRUD、确认状态、来源定位。
- [ ] 3.3 混合检索 BM25 + 向量；“只检索已确认知识”开关。
- [ ] 3.4 Web 知识工作台：卡片确认/编辑/删除、检索。
- [ ] 3.5 删书可选保留个人笔记/卡片。
- [ ] 3.6 多书检索带来源；候选卡片确认后才入正式库。

---

## 阶段 4：V3 学习与章末闲聊（backlog）

- [ ] 4.1 学习目标/计划。
- [ ] 4.2 FSRS 或 SM-2 复习；可替换。
- [ ] 4.3 理解检测以启发式为主，不做伪装精确测评。
- [ ] 4.4 章末“智者闲聊”完整会话：先分享、后接话、轻收束。
- [ ] 4.5 禁止题库 UI；关闭自主模式时零邀请。
- [ ] 4.6 记忆查看/编辑/删除/导出。

---

## 阶段 5：V4 受控 Agent（backlog）

- [ ] 5.1 事件总线 + 任务队列。
- [ ] 5.2 权限 L0–L4；L0/L1 自动，L2 可撤销，L3 每次确认，L4 禁止。
- [ ] 5.3 生命周期：触发 → 计划 → 展示 → 批准 → 执行 → 审计。
- [ ] 5.4 拒绝后不自动重试同一动作；失败可恢复且无重复副作用。

---

## 已确认决策（2026-11-19）

- [x] D1 V1 采用“选区保存引用 + 语音批注”，正文常驻划线渲染延后到 V1.1。
- [x] D2 SD 直接拷贝、未入库的书在 V1 降级为无引用问答，并明确提示。
- [x] D3 同步仅前台动作触发：保存后、AI 回合结束、打开同步页、手动重试；不做后台轮询。
- [x] D4 服务端 V1/V2 基线用 SQLite + FTS5；向量检索后置。
- [x] D5 Web 工作台复用 relay admin token；V1 不做账号体系。
- [x] D6 批准启动 V1-01；实施顺序为服务端先行、模拟测试通过后再做 S3 固件。

## 预期变更文件（批准后）

### relay（服务端 V1 已落地）

- `relay/relay/main.py`（新增 `/api/v1/books*`、`/reader/query`、`/sync/*`，`device/config` 加 features）
- `relay/relay/library_store.py`（新；合并原计划的 books_store/sync_store）
- `relay/relay/book_ingest.py`（新）
- `relay/relay/library_api.py`（新；上传/去重/启动解析）
- `relay/relay/reader_query.py`（新）
- `relay/relay/citation.py`（新）
- `relay/relay/admin_page.py`（扩展“阅读书库/笔记与引用”，替代原计划的 workbench_page.py）
- `relay/relay/config.py`、`metrics.py`、`relay/deploy/*`、`relay/requirements.txt`
- `relay/tests/`（新增用例与 `fixtures/golden_books/`）

### 固件

- `lib/Epub/Epub/PassageRef.h`（新；已落地）
- `lib/Epub/Epub/blocks/TextBlock.{h,cpp}`（已落地）
- `lib/Epub/Epub/ParsedText.{h,cpp}`（已落地，无调用方）
- `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.{h,cpp}`（待接线）
- `test/passage_ref/*`（新，待建）
- `lib/BookIdentity/*`（新）
- `lib/Passage/*`（新）
- `lib/Sync/*`（新）
- `src/activities/reader/AskAiActivity.{h,cpp}`
- `src/activities/reader/EpubReaderActivity.{h,cpp}`
- `src/activities/settings/*`（同步页/开关）
- `lib/Notes/*`（可选扩展）
- `lib/I18n/translations/*.yaml`（新文案）

### 文档

- `docs/ai-native/api-v1.md`（新；已落地）
- 本任务文件
- `docs/AI原生智能阅读终端_可行性评估与实施计划.md`（已回填实施进展与实际数字）
- `docs/README.md`（索引）

## 验证命令（每个里程碑）

```bash
pio run -e onepage_s3
pio run -e onepage
cmake -S test -B build/test -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/test && ctest --test-dir build/test --output-on-failure -j
./bin/clang-format-fix
python3 scripts/sync_web_core.py --check
python3 scripts/build_html.py
pio check
cd relay && pytest
```

> 本目录不是 git 仓库，按现有约定不做本地 commit；变更以文件与构建结果为准。
