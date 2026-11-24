#include "PlainText.h"

namespace PlainText {

namespace {

bool isSpace(const unsigned char c) { return c == ' ' || c == '\t' || c == '\r'; }

bool isAsciiAlnum(const unsigned char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// Characters that may sit right next to a marker for it to count as emphasis.
bool isPunct(const unsigned char c) {
  switch (c) {
    case ',':
    case '.':
    case ';':
    case ':':
    case '!':
    case '?':
    case '"':
    case '\'':
    case '(':
    case ')':
    case '[':
    case ']':
    case '<':
    case '>':
    case '/':
      return true;
    default:
      return false;
  }
}

bool isBoundary(const unsigned char c) { return c == '\0' || isSpace(c) || isPunct(c); }

// A single '*' / '_' is a marker when it opens a word (boundary before, word
// char after) or closes one (word char before, boundary after). This keeps
// "2 * 3" and "file_name" intact while dropping *italic* / _italic_.
bool opensEmphasis(const char prev, const char next, const unsigned char marker) {
  if (next == '\0' || isSpace(static_cast<unsigned char>(next))) return false;
  if (!isBoundary(static_cast<unsigned char>(prev))) return false;
  if (marker == '_') return isAsciiAlnum(static_cast<unsigned char>(next));
  return true;
}

bool closesEmphasis(const char prev, const char next, const unsigned char marker) {
  if (prev == '\0' || isSpace(static_cast<unsigned char>(prev))) return false;
  if (next != '\0' && !isBoundary(static_cast<unsigned char>(next))) return false;
  if (marker == '_') return isAsciiAlnum(static_cast<unsigned char>(prev));
  return true;
}

}  // namespace

size_t stripMarkdown(char* text, size_t len) {
  if (text == nullptr) return 0;

  size_t in = 0;
  size_t out = 0;
  while (in < len) {
    size_t lineEnd = in;
    while (lineEnd < len && text[lineEnd] != '\n') ++lineEnd;

    size_t cursor = in;
    size_t indent = 0;
    while (cursor + indent < lineEnd && isSpace(static_cast<unsigned char>(text[cursor + indent]))) ++indent;
    const size_t firstNonSpace = cursor + indent;

    // Fence line (``` or ~~~): drop the line, keep the block's content.
    if (lineEnd - firstNonSpace >= 3 && (text[firstNonSpace] == '`' || text[firstNonSpace] == '~')) {
      // A line that opens with ``` / ~~~ is a fence even when it carries an
      // info string (```cpp): drop it, keep the block's lines.
      const char fence = text[firstNonSpace];
      size_t run = 0;
      while (firstNonSpace + run < lineEnd && text[firstNonSpace + run] == fence) ++run;
      if (run >= 3) {
        in = lineEnd < len ? lineEnd + 1 : lineEnd;
        continue;
      }
    }

    const size_t lineOutStart = out;
    bool dropIndent = false;
    if (firstNonSpace < lineEnd && text[firstNonSpace] == '#') {
      size_t hashes = 0;
      while (firstNonSpace + hashes < lineEnd && text[firstNonSpace + hashes] == '#') ++hashes;
      if (hashes <= 6) {
        size_t after = firstNonSpace + hashes;
        if (after == lineEnd || text[after] == ' ') {
          cursor = after < lineEnd ? after + 1 : after;
          dropIndent = true;
        }
      }
    }
    if (!dropIndent) cursor = in;

    for (; cursor < lineEnd; ++cursor) {
      const char c = text[cursor];
      const char prev = cursor > in ? text[cursor - 1] : '\0';
      const char next = cursor + 1 < lineEnd ? text[cursor + 1] : '\0';
      if (c == '`') continue;
      if (c == '*' || c == '_' || c == '~') {
        if (next == c) {
          // ** / __ / ~~ run (also ***bold italic***): drop the whole run.
          while (cursor + 1 < lineEnd && text[cursor + 1] == c) ++cursor;
          continue;
        }
        if (c == '~') {
          // A lone tilde is prose.
        } else if (opensEmphasis(prev, next, static_cast<unsigned char>(c)) ||
                   closesEmphasis(prev, next, static_cast<unsigned char>(c))) {
          continue;
        }
      }
      text[out++] = c;
    }

    while (out > lineOutStart && isSpace(static_cast<unsigned char>(text[out - 1]))) --out;

    if (lineEnd < len) {
      // At most one blank line; no leading blank lines.
      const bool collapse = out == 0 || (text[out - 1] == '\n' && (out < 2 || text[out - 2] == '\n'));
      if (!collapse) text[out++] = '\n';
      in = lineEnd + 1;
    } else {
      in = lineEnd;
    }
  }

  while (out > 0 && (text[out - 1] == '\n' || isSpace(static_cast<unsigned char>(text[out - 1])))) --out;
  text[out] = '\0';
  return out;
}

}  // namespace PlainText
