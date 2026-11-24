#pragma once

#include <cstddef>

// Rendering helpers for model replies shown on a 1-bit screen. The relay
// returns whatever the LLM wrote, which is often Markdown: `**8:00**` used to
// reach the panel verbatim (v4.0 plan §3.4 bug B). This is not a Markdown
// renderer — it only removes the inline markers that look like noise in a
// monochrome, fixed-font reader, and it never allocates.
namespace PlainText {

// Strip emphasis (`**`, `__`, `*`, `_`, backticks, `~~`), leading `#` heading
// markers, and fence lines from `text` in place; collapse runs of blank lines
// to one. Returns the new length and always NUL-terminates within `len`
// (requires text[len] to be writable, i.e. the usual +1 buffer).
//
// Emphasis is only removed where it reads as a marker: `2 * 3` and
// `file_name` are left alone. UTF-8 bytes are copied unchanged.
size_t stripMarkdown(char* text, size_t len);

}  // namespace PlainText
