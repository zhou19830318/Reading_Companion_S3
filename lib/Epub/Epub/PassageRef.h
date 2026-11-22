#pragma once

// Passage identity shared by the EPUB layout and the relay's citation API.
//
// The contract (docs/ai-native/api-v1.md):
//   passage_id = <book_id>:<spine_index>:<kind><block_index>
// where kind is 'p' for <p> or 'li' for <li>, block_index is 0-based within
// the spine document and every <p>/<li> in document order consumes an index
// (even hidden or empty ones). Keeping the numbering rule in one header means
// the parser, the page cache and the AI screen cannot drift from the server.
//
// Header-only and free of device dependencies so test/passage_ref compiles it
// on the host as well.

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace EpubPassage {

enum class Kind : uint8_t {
  None = 0,
  Paragraph = 1,
  ListItem = 2,
};

struct Ref {
  Kind kind = Kind::None;
  uint16_t index = 0;

  bool valid() const { return kind != Kind::None; }
  bool operator==(const Ref& other) const { return kind == other.kind && index == other.index; }
  bool operator!=(const Ref& other) const { return !(*this == other); }
};

inline const char* kindPrefix(const Kind kind) {
  switch (kind) {
    case Kind::Paragraph:
      return "p";
    case Kind::ListItem:
      return "li";
    case Kind::None:
    default:
      return "";
  }
}

// Formats "<bookId>:<spine>:<p|li><index>" into out. bookId is the 16-char
// SHA-256 prefix from the relay; an empty/undersized id still formats what is
// available rather than failing silently. Returns the length written, or 0
// when the buffer is too small.
inline size_t formatId(char* out, const size_t cap, const char* bookId, const uint16_t spine, const Ref ref) {
  if (out == nullptr || cap == 0 || bookId == nullptr || !ref.valid()) return 0;
  const int written = std::snprintf(out, cap, "%s:%u:%s%u", bookId, static_cast<unsigned>(spine), kindPrefix(ref.kind),
                                    static_cast<unsigned>(ref.index));
  if (written <= 0 || static_cast<size_t>(written) >= cap) return 0;
  return static_cast<size_t>(written);
}

}  // namespace EpubPassage
