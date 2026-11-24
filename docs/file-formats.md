# File Formats

These formats describe the SD-card cache files under `/.crosspoint/epub_<hash>/`.
All POD fields are written in the ESP32 little-endian representation used by
`Serialization.h`; strings are length-prefixed UTF-8.

## `book.bin`

### Version 7

`book.bin` stores EPUB metadata plus lookup tables for spine and TOC entries.
The current firmware writes this version from `BookMetadataCache`.

**Atomic write:** `book.bin` is written to `book.bin.tmp` and renamed over the
final path, so a power cut or a cancelled build (background `PreIndexer`, which
writes this file while the device sits idle on Home/FileBrowser) never leaves a
partial file at the final path. Presence of `book.bin` therefore means the
metadata cache is complete; readers treat a missing file as "rebuild". The same
tmp+rename applies to `css_rules.cache`.

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 7
#define MAX_STRING_LENGTH 65535

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

struct Metadata {
    String title [[comment("Book title")]];
    String author [[comment("Book author")]];
    String language [[comment("Book language code")]];
    String coverItemHref [[comment("Path to cover image")]];
    String textReferenceHref [[comment("Path to guided first text reference")]];
};

struct SpineEntry {
    String href [[comment("Resource path")]];
    u32 cumulativeSize [[comment("Cumulative uncompressed spine size through this entry")]];
    s16 tocIndex [[comment("Index into TOC, or inherited/previous TOC index when no direct entry exists")]];
};

struct TocEntry {
    String title [[comment("Chapter/section title")]];
    String href [[comment("Resource path")]];
    String anchor [[comment("Fragment identifier")]];
    u8 level [[comment("Nesting level")]];
    s16 spineIndex [[comment("Index into spine (-1 if none)")]];
};

struct BookBin {
    u8 version;
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }

    u32 lutOffset [[comment("Offset to lookup tables")]];
    u16 spineCount;
    u16 tocCount;

    Metadata metadata;

    u32 currentOffset = $;
    if (currentOffset != lutOffset) {
        std::warning(std::format("LUT offset mismatch: expected 0x{:X}, got 0x{:X}", lutOffset, currentOffset));
    }

    u32 spineLut[spineCount] [[comment("Spine entry offsets")]];
    u32 tocLut[tocCount] [[comment("TOC entry offsets")]];

    SpineEntry spines[spineCount];
    TocEntry toc[tocCount];
};

BookBin book @ 0x00;

u32 fileSize = std::mem::size();
u32 parsedSize = $;
if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

## `section.bin`

### Version 25

Each file in `sections/*.bin` stores one laid-out spine section. The header is
also the cache-busting key: if any layout-affecting setting differs from the
current reader settings, the section is discarded and rebuilt.

Version 25 includes:

- cache-busting fields for paragraph alignment, hyphenation, embedded CSS,
  image rendering mode, and Focus Reading
- page offset LUT
- anchor-to-page map for fragment and footnote navigation
- paragraph and list-item LUTs used by KOReader sync page refinement
- optional per-word Focus Reading split metadata
- per-page footnote entries

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 28
#define MAX_STRING_LENGTH 65535
#define FOOTNOTE_NUMBER_LEN 32
#define FOOTNOTE_HREF_LEN 96

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

enum PageElementTag : u8 {
    TAG_PageLine = 1,
    TAG_PageImage = 2,
    TAG_PageHorizontalRule = 3
};

enum WordStyle : u8 {
    REGULAR = 0,
    BOLD = 1,
    ITALIC = 2,
    BOLD_ITALIC = 3,
    UNDERLINE = 4,
    STRIKETHROUGH = 8,
    SUP = 16,
    SUB = 32
};

enum TextAlign : u8 {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    NONE = 4
};

struct BlockStyle {
    TextAlign alignment;
    bool textAlignDefined;
    s16 marginTop;
    s16 marginBottom;
    s16 marginLeft;
    s16 marginRight;
    s16 paddingTop;
    s16 paddingBottom;
    s16 paddingLeft;
    s16 paddingRight;
    s16 textIndent;
    bool textIndentDefined;
    bool isRtl;
    bool directionDefined;
};

struct TextBlock {
    u16 wordCount;
    String words[wordCount];
    s16 wordXPos[wordCount];
    WordStyle wordStyle[wordCount];

    u8 hasFocus;
    if (hasFocus != 0) {
        u8 wordFocusBoundary[wordCount] [[comment("UTF-8 byte boundary between bold prefix and suffix")]];
        u16 wordFocusSuffixX[wordCount] [[comment("Suffix x offset from word start")]];
    }

    BlockStyle blockStyle;

    // Citation anchor (AI-native reader query contract). kind 0 = no passage,
    // 1 = <p>, 2 = <li>; index is 0-based within the spine document; the byte
    // range indexes the relay's normalized passage text.
    u8 passageKind;
    u16 passageIndex;
    u32 passageCharStart;
    u32 passageCharEnd;
};

struct ImageBlock {
    String imagePath;
    s16 width;
    s16 height;
};

struct PageLine {
    s16 xPos;
    s16 yPos;
    TextBlock block;
};

struct PageImage {
    s16 xPos;
    s16 yPos;
    ImageBlock image;
};

struct PageHorizontalRule {
    s16 xPos;
    s16 yPos;
    u16 width;
    u8 thickness;
};

struct PageElement {
    PageElementTag pageElementType;
    if (pageElementType == TAG_PageLine) {
        PageLine pageLine [[inline]];
    } else if (pageElementType == TAG_PageImage) {
        PageImage pageImage [[inline]];
    } else if (pageElementType == TAG_PageHorizontalRule) {
        PageHorizontalRule horizontalRule [[inline]];
    } else {
        std::error(std::format("Unknown page element type: {}", pageElementType));
    }
};

struct FootnoteEntry {
    char number[FOOTNOTE_NUMBER_LEN];
    char href[FOOTNOTE_HREF_LEN];
};

struct Page {
    u16 elementCount;
    PageElement elements[elementCount] [[inline]];

    u16 footnoteCount;
    FootnoteEntry footnotes[footnoteCount];
};

struct AnchorEntry {
    String anchor;
    u16 page;
};

struct AnchorMap {
    u16 count;
    AnchorEntry entries[count];
};

struct ParagraphLut {
    u16 count;
    u16 paragraphIndex[count];
};

struct SectionBin {
    u8 version;
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }

    s32 fontId;
    float lineCompression;
    bool extraParagraphSpacing;
    u8 paragraphAlignment;
    u16 viewportWidth;
    u16 viewportHeight;
    bool hyphenationEnabled;
    bool embeddedStyle;
    u8 imageRendering;
    bool focusReadingEnabled;

    u16 pageCount;
    u32 pageLutOffset;
    u32 anchorMapOffset;
    u32 paragraphLutOffset;
    u32 listItemLutOffset;

    Page pages[pageCount];

    u32 currentOffset = $;
    if (currentOffset != pageLutOffset) {
        std::warning(std::format("Page LUT offset mismatch: expected 0x{:X}, got 0x{:X}", pageLutOffset, currentOffset));
    }

    u32 pageLut[pageCount] [[comment("Page data offsets")]];

    if (anchorMapOffset != 0) {
        AnchorMap anchorMap @ anchorMapOffset;
    }

    if (paragraphLutOffset != 0) {
        ParagraphLut paragraphLut @ paragraphLutOffset;
    }

    if (listItemLutOffset != 0 && paragraphLutOffset != 0) {
        u16 listItemIndex[paragraphLut.count] @ listItemLutOffset;
    }
};

SectionBin section @ 0x00;

u32 fileSize = std::mem::size();
u32 parsedSize = $;
if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

## Reading session log (`/.crosspoint/history/YYYY-MM-DD.jsonl`)

RH-01 writes one JSON object per line, appended when a reading session ends.
The file is named by the session's **start** date in the user's configured
offset (`SETTINGS.clockUtcOffsetQ`); a session that crosses midnight stays in
the start day's file. There is no header line and no version byte — read it as
JSONL.

```json
{"schema_version":1,"ts_start":"2026-11-22T15:30:00+08:00","ts_end":"2026-11-22T15:52:30+08:00","book_id":"/Books/山间书店.epub","position":{"spine":0,"page":4},"duration_sec":1350,"pages_advanced":12}
```

| Field | Meaning |
|---|---|
| `schema_version` | `1` |
| `ts_start` / `ts_end` | ISO-8601 local time with the UTC offset, `YYYY-MM-DDTHH:MM:SS+HH:MM` |
| `book_id` | V1 device-side book key: the SD path of the book (escaped JSON string) |
| `position` | Last visible position: `spine` (0-based spine index) and `page` (0-based page in that spine) |
| `duration_sec` | Session duration; an idle-closed session ends at `last activity + 90 s` |
| `pages_advanced` | Forward page turns in the session (backward turns do not decrement) |

Rules enforced by the writer:

- Sessions whose clock is below `1600000000` (never synced) are not written.
- `time()` is seeded from the RTC at boot and after a manual `SETDATE`; NTP may
  correct it later. An RTC below the sane floor does not seed the system clock,
  so the session is dropped rather than filed under 1970.
- `pages_advanced` counts every accepted forward turn, including the
  chapter-invite "continue" press that crosses a spine. Backward turns and
  chapter jumps from the TOC/menu do not count.
- Zero-length sessions are not written.
- One line is appended per session; a truncated line is never written (the
  record is dropped and logged instead).
- `duration_sec` is capped by the idle boundary, so a blocked main loop or a
  long menu visit cannot inflate it.

Consumers (RH-02 daily aggregate, RH-03 heatmap) should tolerate unknown
fields: `book_id` may become the AI library's `sha256[:16]` in a later version,
with `book_path` added for backward compatibility.

## Reading aggregate (`/.crosspoint/aggregate/YYYY.json`)

RH-02 rolls each day's history sessions up into one record inside a per-year
document. The file is rewritten through `YYYY.json.tmp` + rename, and only when
a session ends (`ReadingSessionLog::appendSession`) or the `CMD:RH_AGG` debug
command asks for it.

The document is valid JSON laid out one day per line, so the writer can
stream-merge one line and copy the rest without holding the year in RAM:

```json
{"schema_version":1,"year":2026,"days":[
{"date":"2026-11-22","total_sec":1840,"sessions":3,"pages_advanced":27,"books":[{"id":"/Books/a.epub","sec":1720}]},
{"date":"2026-11-23","total_sec":600,"sessions":1,"pages_advanced":4,"books":[]}
]}
```

| Field | Meaning |
|---|---|
| `schema_version` | `1` (per document) |
| `year` | Local year the file covers |
| `days[].date` | Local date (`YYYY-MM-DD`) the sessions started on |
| `days[].total_sec` | Sum of `duration_sec` for that day |
| `days[].sessions` | Number of valid session lines folded in |
| `days[].pages_advanced` | Sum of forward page turns |
| `days[].books[]` | Per-book seconds; `id` is the escaped history `book_id` |
| `days[].books_truncated` | `true` when a share was dropped (see caps below) |

Rules enforced by the writer:

- Day records are sorted by date; rewriting a day replaces its line in place.
- Book shares are capped at 8 per day (`MAX_BOOKS_PER_DAY`). Totals always stay
  exact; a ninth book, or a `book_id` that would not fit the 160-byte field,
  sets `books_truncated` instead of being stored cut in half.
- An unparsable existing day line is dropped on rewrite; an existing line for
  the same date is replaced, never duplicated.
- The aggregate is derived data: deleting `aggregate/` loses nothing, and the
  stats view recomputes a missing day from `history/` (read-only fallback, no
  writes).

Consumers should ignore unknown fields. RH-03 reads this file through
`ReadingHistory::loadDayStats()`; the heatmap levels are fixed at
`0 / <15 min / 15–40 min / >40 min` per v4.0 §RH-03.

## 阅读计划 `/.crosspoint/plans/plans.json`

RP-01（v4.0 §4.4）。单文件、一行一条记录，`schema_version` 当前为 1。

```json
{"schema_version":1,"plans":[
{"id":"plan_20261223_001","book_id":"/books/示例.epub","status":"active","created_at":"2026-11-23T11:51:47+08:00","target_date":"2026-12-23","goal":{"type":"minutes_per_week","value":180,"pages_per_week":0},"priority":3,"timezone_offset_q":80,"paused_until":""}
]}
```

- `status`：`active` / `paused` / `archived`。归档只改状态、不删记录（归档不丢数据）。
- `goal.type` 固定 `minutes_per_week`；`value` 为每周分钟；`pages_per_week` 为 0 表示不跟踪页数。
- `timezone_offset_q` 与 `SETTINGS.clockUtcOffsetQ` 同一编码（48 = UTC+0）。固件没有 IANA 时区库，因此不存 `Asia/Hong_Kong` 字符串。
- 最多 8 条，超出的记录读取时丢弃；缺 `id` 或 `target_date` 非法的行整行跳过。
- 写入先写 `<file>.tmp` 再 rename；断电只会留下旧文件或新文件。

## 空余时段 `/.crosspoint/plans/slots.json`

RP-02（v4.0 §4.5）。同样一行一条，`schema_version` 为 1。

```json
{"schema_version":1,"updated_at":"2026-11-23T11:52:32+08:00","slots":[
{"name":"阅读时段","days":[2,3,4,5],"start":"21:00","end":"22:00","flexible":false}
]}
```

- `days`：0=周日 … 6=周六；`start` / `end` 为本地 `HH:MM`，必须 `start < end`。
- 最多 8 条；空 `days`、非法时间或 `end <= start` 的行会被丢弃。
