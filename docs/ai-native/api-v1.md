# AI 原生阅读 · V1 接口与数据契约

- 版本：1.0（2026-11-19 冻结）
- 状态：V1-01 交付物；服务端与固件均以此为准
- 基础路径：`/api/v1`
- 认证：`Authorization: Bearer <device-token>` + `X-Device-Id: <device-id>`
- 兼容：现有 `/api/v1/chat`、`/api/v1/voice/stream`、`/api/v1/tts` 不变；新能力只走本文接口。

---

## 0. 约定

| 项目 | 约定 |
|---|---|
| 字符编码 | UTF-8 |
| 时间 | UTC ISO-8601，秒级，例如 `2026-11-19T12:34:56Z` |
| 请求 ID | 客户端生成，设备内唯一；重试必须复用 |
| 书籍 ID | 原始文件 SHA-256 的前 16 个十六进制字符（64 bit） |
| 完整哈希 | 64 个十六进制字符，用于去重和 `by-hash` 查询 |
| 分页 | 游标为不透明字符串；客户端不得解析 |
| 幂等 | 写操作带 `op_id` 或 `request_id`；服务端唯一约束去重 |
| 内容类型 | 请求/响应 `application/json`，上传 `multipart/form-data` |
| 错误响应 | 统一 `{"error": {"code","message","request_id","retry_after_ms"}}` |

---

## 1. `passage_id` 规则（冻结）

```text
passage_id = <book_id>:<spine_index>:<kind><block_index>
kind       = p | l
```

示例：

```text
a1b2c3d4e5f60718:12:p37
a1b2c3d4e5f60718:12:l5
```

### 1.1 计数规则

- `spine_index`：EPUB `<spine>` 中 `<itemref>` 的 0-based 序号；TXT/XTC 固定为 0。
- `p`：该 spine 文档内**每一个** `<p>` 元素按文档顺序计数，从 0 开始；嵌套 `<p>` 也各计一次。
- `l`：该 spine 文档内**每一个** `<li>` 元素按文档顺序计数，从 0 开始；嵌套 `<li>` 也各计一次。
- 计数与设备端 `ChapterHtmlSlimParser` 的 `xpathParagraphIndex` / `xpathListItemIndex` 保持一致。
- 标题（`h1`–`h6`）、`div` 直排文字、表格单元格不单独发 ID；不要为了覆盖率改变计数规则。
- 空段落（规范化后少于 1 个可见字符）仍占用计数，但不写入 `passages` 表；后续查询遇到该 ID 返回 `PASSAGE_NOT_FOUND`。

### 1.2 文本提取

- `p`：取该 `<p>` 内、排除嵌套 `p/li` 的可见文本。
- `li`：取该 `<li>` 的直接/内联文本，排除嵌套 `p/li`；`<li><p>…</p></li>` 只产生 `p` 段落，`li` 段落为空并跳过。
- 跳过 `script`、`style`、`head`。
- 空白折叠为单个空格；移除 CJK 字符之间的空白；保留中英文标点。
- `quote` 校验与 `text` 使用同一规范化函数。

### 1.3 字符偏移

- `char_start`、`char_end`：**UTF-8 字节偏移**，相对该条 `passages.text`，左闭右开。
- 整段引用：`0 .. len(text.encode("utf-8"))`。
- 跨段选区：每个 `passage_id` 各自带一对偏移；不跨段拼接偏移。
- 设备端选区如果只有行号，先映射到段落，再计算该段内的字节范围。

### 1.4 不可变性与版本

- `passage_id` 只由原始文件内容决定；同一文件在任何设备/服务端必须得到同一 ID。
- 书籍文件被替换但文件名相同：哈希变化 → 新 `book_id`，旧笔记保留在旧 `book_id` 下。
- 解析规则变更（例如新增块类型）必须提升接口版本，不得静默改变已有 ID。

---

## 2. 数据对象

### 2.1 Book

```json
{
  "book_id": "a1b2c3d4e5f60718",
  "sha256": "a1b2c3d4e5f60718...64hex",
  "title": "山间书店",
  "author": "示例作者",
  "language": "zh",
  "format": "epub",
  "size": 123456,
  "status": "ready",
  "progress": 1.0,
  "chapters": 12,
  "passages": 1840,
  "error": null,
  "created_at": "2026-11-19T12:00:00Z",
  "updated_at": "2026-11-19T12:00:10Z"
}
```

`status`：`pending` → `parsing` → `indexing` → `ready`；任一步失败为 `failed`。
`progress`：0.0–1.0；`ready` 时为 1.0。

### 2.2 Passage

```json
{
  "passage_id": "a1b2c3d4e5f60718:1:p0",
  "book_id": "a1b2c3d4e5f60718",
  "spine_index": 1,
  "chapter_title": "第一章 山间",
  "kind": "p",
  "block_index": 0,
  "text": "山间的书店有一盏灯。",
  "char_start": 0,
  "char_end": 33
}
```

### 2.3 Citation

```json
{
  "passage_id": "a1b2c3d4e5f60718:1:p0",
  "spine_index": 1,
  "quote": "山间的书店有一盏灯。"
}
```

`quote` 必须能在对应 passage 的 `text` 中匹配；否则服务端丢弃该 citation。

### 2.4 AnswerSegment

```json
{
  "source": "book",
  "text": "作者先用一盏灯把空间点亮，再让对话发生。",
  "citations": [
    {
      "passage_id": "a1b2c3d4e5f60718:1:p0",
      "spine_index": 1,
      "quote": "山间的书店有一盏灯。"
    }
  ]
}
```

`source` 只能是：

| 值 | 设备标签（中文 / 英文） | 含义 |
|---|---|---|
| `book` | `[书中原文]` / `[Book]` | 有通过校验的书中引用 |
| `external` | `[外部补充]` / `[External]` | 模型外部知识，可能不准确 |
| `inference` | `[推断]` / `[Inference]` | 模型推断，非原文 |

---

## 3. 书籍接口

### 3.1 `POST /api/v1/books`

上传并登记一本书。`multipart/form-data`：

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `file` | file | 是 | EPUB / TXT；最大 64 MiB（可配置） |
| `title` | string | 否 | 缺省从元数据取 |
| `author` | string | 否 | 缺省从元数据取 |

响应 `202`（新书）或 `200`（重复）：

```json
{
  "book_id": "a1b2c3d4e5f60718",
  "sha256": "a1b2c3d4e5f60718...64hex",
  "status": "pending",
  "duplicate": false
}
```

- 重复上传（同一完整哈希）：返回已有记录，`duplicate: true`，不重复解析；已有状态为 `failed` 时重新置为 `pending` 并启动解析，便于设备确认键重试。
- 格式不支持：`415 UNSUPPORTED_FORMAT`。
- 超过大小：`413 BOOK_TOO_LARGE`。
- 解析在后台进行；客户端轮询 `processing` 或 `/books/{id}`。

### 3.2 `GET /api/v1/books/{book_id}`

返回 Book 对象；不存在 `404 BOOK_NOT_FOUND`。

### 3.3 `GET /api/v1/books/by-hash/{sha256}`

按完整 SHA-256 查询。用于设备登记：

- `200`：返回 Book；
- `404 BOOK_NOT_FOUND`：本地书未入库。设备摘要与引用问答会自动上传当前 EPUB，再轮询 `processing`，就绪后重试原请求；自动入库失败会显示错误，重试后仍缺失或引用段落缺失时才明确说明并降级为无引用问答。

### 3.4 `GET /api/v1/books/{book_id}/processing`

```json
{
  "book_id": "a1b2c3d4e5f60718",
  "status": "parsing",
  "progress": 0.42,
  "chapters": 12,
  "passages": 480,
  "error": null
}
```

失败时：

```json
{
  "book_id": "a1b2c3d4e5f60718",
  "status": "failed",
  "progress": 0.0,
  "chapters": 0,
  "passages": 0,
  "error": {"code": "EPUB_PARSE_FAILED", "message": "spine item 4: ..."}
}
```

### 3.5 `GET /api/v1/books/{book_id}/chapters`

调试/工作台用，按 `ordinal` 升序：

```json
{
  "book_id": "a1b2c3d4e5f60718",
  "chapters": [
    {"spine_index": 0, "title": "封面", "ordinal": 0, "passages": 0},
    {"spine_index": 1, "title": "第一章 山间", "ordinal": 1, "passages": 42}
  ]
}
```

### 3.6 `GET /api/v1/books/{book_id}/passages?spine_index=1&limit=200&after=`

调试/引用核对用；按 `block_index` 升序。`after` 为上一页最后一个 `passage_id`。

```json
{
  "book_id": "a1b2c3d4e5f60718",
  "passages": [
    {
      "passage_id": "a1b2c3d4e5f60718:1:p0",
      "spine_index": 1,
      "kind": "p",
      "block_index": 0,
      "text": "山间的书店有一盏灯。",
      "char_start": 0,
      "char_end": 33
    }
  ],
  "next": null
}
```

### 3.7 `GET /api/v1/books/{book_id}/summary`

生成或读取章节摘要 / 全书导读。接口同步返回，设备只展示结果；书籍必须已 `ready`。

- `scope=chapter`：必须带 `spine_index`，摘要当前章节。
- `scope=book`：不带 `spine_index`，均匀抽样最多 80 章并取每章首段生成全书导读。
- 缓存键：`book_id + scope + spine_index + content_revision + prompt_version + model`。命中缓存不调用模型。
- 章节正文超过 `READER_CONTEXT_CHAR_BUDGET` 时保留头尾；模型失败或超时不写缓存。

```json
{
  "book_id": "a1b2c3d4e5f60718",
  "scope": "chapter",
  "spine_index": 1,
  "title": "第二章 灯下",
  "text": "本章讲述……",
  "status": "ready",
  "cache_key": "9f86d081884c7d659a2feaa0c55ad015",
  "generated_at": "2026-11-27T00:00:00Z",
  "model": "qwen-flash",
  "cached": false
}
```

错误码：`BAD_REQUEST`（400）、`BOOK_NOT_FOUND`（404）、`BOOK_NOT_READY`（409）、`PASSAGE_NOT_FOUND`（404）、`SUMMARY_TIMEOUT`（504）、`MODEL_UNAVAILABLE` / `MODEL_EMPTY_ANSWER`（502）。

---

## 4. 阅读问答

### 4.1 `POST /api/v1/reader/query`

请求：

```json
{
  "request_id": "onepage-s3:00001234",
  "book_id": "a1b2c3d4e5f60718",
  "spine_index": 1,
  "passage_ids": ["a1b2c3d4e5f60718:1:p0"],
  "selected_text": "",
  "question": "这句话在整章里起什么作用？",
  "intent": "explain",
  "context_scope": "current_passage",
  "response_style": "concise",
  "language": "zh",
  "max_output_tokens": 512
}
```

| 字段 | 必填 | 说明 |
|---|---|---|
| `request_id` | 是 | 客户端唯一；用于取消/去重/日志 |
| `book_id` | 是 | 必须已 `ready` |
| `spine_index` | 否 | 当前章节；缺省取第一个 `passage_id` 的 spine |
| `passage_ids` | 否 | 当前段落/选区；最多 8 个 |
| `selected_text` | 否 | 用户显式选区文本；最多 4000 UTF-8 字节 |
| `question` | 否 | 自定义问题；为空时按 `intent` 生成默认问题 |
| `intent` | 是 | 见下表 |
| `context_scope` | 是 | `selection` / `current_passage` / `current_chapter` |
| `response_style` | 否 | `concise`（默认）/ `detailed` |
| `language` | 否 | `zh`（默认）/ `en` |
| `max_output_tokens` | 否 | 64–1024，默认 512 |

`intent`：

```text
explain | summarize | compare | apply | translate | chapter_summary | chapter_background | custom
```

V1 不支持 `context_scope=current_book`：返回 `400 SCOPE_NOT_SUPPORTED`。

响应 `200`：

```json
{
  "request_id": "onepage-s3:00001234",
  "book_id": "a1b2c3d4e5f60718",
  "segments": [
    {
      "source": "book",
      "text": "作者先用一盏灯把空间点亮，再让对话发生。",
      "citations": [
        {
          "passage_id": "a1b2c3d4e5f60718:1:p0",
          "spine_index": 1,
          "quote": "山间的书店有一盏灯。"
        }
      ]
    },
    {
      "source": "inference",
      "text": "这也暗示了书店在故事里是一个避风港。",
      "citations": []
    }
  ],
  "warnings": [],
  "usage": {"input_tokens": 1200, "output_tokens": 180},
  "model": "qwen-flash"
}
```

规则：

1. `segments` 最多 12 段；每段 `citations` 最多 8 条。
2. `book` 段至少一条有效 citation；否则服务端把它降级为 `inference`，并追加 warning `CITATION_DROPPED`。
3. `external` / `inference` 段不要求 citation；即使带 citation 也只在通过校验时保留。
4. 模型输出不是合法 JSON：返回单段 `inference`（原文截断到 8 KB），warning `MODEL_OUTPUT_NOT_JSON`。
5. 任一段 `text` 为空则丢弃；全部为空返回 `502 MODEL_EMPTY_ANSWER`。
6. `request_id` 重复：V1 不缓存答案，直接重新生成；客户端用 `request_id` 丢弃过期响应。

### 4.2 检索范围

| `context_scope` | 送入模型的内容 |
|---|---|
| `selection` | `passage_ids` 对应段落 + 前后各 1 段 |
| `current_passage` | 当前段 + 前后各 1 段 |
| `current_chapter` | 当前 spine 的全部可用段落，按字符预算截断 |

- 字符预算默认 8000 字符（含段落标记）；超出时优先保留当前段/选区，再向两侧扩展。
- 没有 `passage_ids` 但有 `question`：用 FTS5 在当前书检索 top 6 段。
- 所有送入模型的段落带 `passage_id`，提示词要求引用时逐字复制 `quote`。

### 4.3 引用校验（服务端强制）

对每个 citation：

1. `passage_id` 必须存在于本次允许的段落集合中（当前书；当前章节 scope 还必须在当前 spine）。
2. `quote` 非空。
3. 规范化后 `quote` 必须是对应 `passage.text` 的子串。
4. `spine_index` 必须与 passage 一致；不一致以 passage 为准。

任一条不满足：丢弃 citation。丢弃后 `book` 段无 citation → 降级 `inference`。

规范化函数：

```text
Unicode NFC
→ 将 \r\n / \r 转为 \n
→ 所有空白（含全角空格）折叠为单个半角空格
→ 去掉首尾空白
→ 移除零宽字符（U+200B–U+200D、U+FEFF）
```

不做大小写折叠；英文引用必须保持原大小写。设备端展示时不得自行把 `inference` 标成 `book`。

### 4.4 错误码

| HTTP | code | 场景 |
|---|---|---|
| 400 | `BAD_REQUEST` | 字段缺失/非法 |
| 400 | `SCOPE_NOT_SUPPORTED` | V1 不支持整书问答 |
| 401 | `UNAUTHORIZED` | token 缺失/错误 |
| 403 | `DEVICE_NOT_ALLOWED` | 设备不在允许列表 |
| 404 | `BOOK_NOT_FOUND` | 书籍不存在 |
| 409 | `BOOK_NOT_READY` | 书籍仍在解析/索引 |
| 404 | `PASSAGE_NOT_FOUND` | passage_id 不在本书 |
| 413 | `BOOK_TOO_LARGE` | 上传超限 |
| 415 | `UNSUPPORTED_FORMAT` | 非 EPUB/TXT |
| 422 | `UNPROCESSABLE_BOOK` | 解析失败/空书 |
| 429 | `RATE_LIMITED` | 限流 |
| 502 | `MODEL_UNAVAILABLE` | 上游模型失败 |
| 502 | `MODEL_EMPTY_ANSWER` | 模型空回答 |

---

## 5. 同步

### 5.1 本地操作类型（V1）

| `type` | 说明 |
|---|---|
| `annotation.create` | 新建引用/批注/语音批注 |
| `annotation.delete` | 删除引用/批注 |
| `note.create` | 新建短笔记 |
| `note.delete` | 删除短笔记 |
| `ai_result.create` | 保存一次 AI 回答摘要 |

`progress.update` 不在 V1 同步范围；进度继续走现有 KOReader 同步。

### 5.2 `POST /api/v1/sync/push`

```json
{
  "device_id": "onepage-s3",
  "batch_id": "01J...",
  "operations": [
    {
      "op_id": "01J...",
      "type": "annotation.create",
      "client_ts": 1763550000000,
      "book_id": "a1b2c3d4e5f60718",
      "payload": {
        "id": "an_01J...",
        "passage_id": "a1b2c3d4e5f60718:1:p0",
        "quote": "山间的书店有一盏灯。",
        "note": "这里像故事的入口。",
        "kind": "highlight",
        "anchor": {
          "spine_index": 1,
          "kind": "p",
          "block_index": 0,
          "char_start": 0,
          "char_end": 33
        }
      }
    }
  ]
}
```

限制：每批最多 50 条；`operations` 不能为空；`op_id` 在设备内唯一。

响应 `200`：

```json
{
  "batch_id": "01J...",
  "results": [
    {
      "op_id": "01J...",
      "status": "applied",
      "server_id": "an_01J...",
      "error": null
    },
    {
      "op_id": "01J...",
      "status": "duplicate",
      "server_id": "an_01J...",
      "error": null
    }
  ]
}
```

`status`：

| 值 | 含义 | 设备处理 |
|---|---|---|
| `applied` | 已落库 | 标记 acked |
| `duplicate` | `op_id` 已处理 | 标记 acked |
| `rejected` | 字段非法/类型未知 | 保留在 outbox，展示错误 |
| `conflict` | 同实体版本冲突 | 保留，走冲突 UI |

V1 冲突策略：同一 `annotation.id` 已存在且 `updated_at` 更新，返回 `conflict`，服务端保留双方版本（`annotations` 加热历史行或 `conflict_json` 字段）；不静默覆盖。

### 5.3 `GET /api/v1/sync/pull`

```text
GET /api/v1/sync/pull?device_id=onepage-s3&cursor=0&limit=100
```

响应：

```json
{
  "changes": [
    {
      "seq": 42,
      "entity_type": "annotation",
      "entity_id": "an_01J...",
      "op": "upsert",
      "updated_at": "2026-11-19T12:00:00Z",
      "entity": {
        "id": "an_01J...",
        "book_id": "a1b2c3d4e5f60718",
        "passage_id": "a1b2c3d4e5f60718:1:p0",
        "quote": "山间的书店有一盏灯。",
        "note": "这里像故事的入口。",
        "kind": "highlight",
        "deleted": false
      }
    }
  ],
  "next_cursor": "42",
  "has_more": false
}
```

- `cursor` 缺省为 `0`；`next_cursor` 原样回传。
- V1 只返回该 `device_id` 自己的变更；多设备合并后置。
- `limit` 1–200，默认 100。

---

## 6. 兼容与版本

- 新增字段必须可选，旧客户端忽略。
- 删除/改名字段、改变 `source` 语义、改变 `passage_id` 规则 → `/api/v2`。
- `GET /api/v1/device/config` 增加 `features`（可选）：

```json
{
  "features": {
    "reader_query": true,
    "sync": true,
    "knowledge_cards": false
  }
}
```

设备按 feature flag 决定是否显示入口；缺省视为 `false`。

---

## 7. V1 验收对应

| 契约点 | 验收 |
|---|---|
| `passage_id` | 5 本黄金书，设备/服务端抽样一致 |
| `reader/query` | 当前段回答至少一段 `book` + 有效 citation |
| 引用校验 | 伪造 quote 被丢弃；`book` 段降级 `inference` |
| 同步 | 同一 `op_id` 重试返回 `duplicate`，不重复落库 |
| 离线 | 断网写 outbox，恢复后 push 成功 |
| 来源标签 | 设备只按 `source` 渲染，不由模型生成 |

---

## 8. 附录：设备端最小实现顺序

1. 计算并缓存书籍 SHA-256；`GET /books/by-hash/{sha256}`。
2. 排版时记录 `spine + p/li + block_index`；选区生成 `passage_ids[]`。
3. `POST /reader/query`；按 `segments[].source` 渲染标签。
4. 引用列表展示 `quote`；确认键跳回 `spine + block_index`。
5. `SYO1` outbox + `POST /sync/push` / `GET /sync/pull`。
