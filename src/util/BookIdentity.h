#pragma once

#include <cstdint>
#include <string>

// Device-side book identity, matching the relay contract
// (docs/ai-native/api-v1.md): book_id = first 16 hex chars of the file's
// SHA-256, i.e. the first 8 digest bytes. The relay computes the same value
// from the uploaded file, so passage IDs and annotations agree without any
// device-server negotiation.
//
// Hashing happens once and is cached in a sidecar:
//   /.crosspoint/books/<base>-<pathhash>.id   ->  "<16 hex> <size>\n"
// The base name comes from Notes::NoteFormat::baseNameForBook (so it matches
// the note file for a book, including books outside the library folder); the
// path hash keeps same-named books in different folders apart. The size is
// re-checked on every read; moving or replacing a file with a different size
// forces a re-hash. Replacing a file with a same-size different file keeps the
// old id (acceptable for V1: the reader clears the book cache when a file is
// overwritten anyway).
//
// Returns an empty string on open/hash failure; the AI screen can then show a
// specific "could not identify this book" notice instead of pretending the
// relay does not know it.
//
// `onProgress` is optional. When set, the first fingerprint reports 0-100 so
// the caller can keep a waiting screen alive while a large SD-copied book is
// hashed. The callback runs on the caller's task inside the read loop.
using HashProgress = void (*)(void* ctx, uint8_t percent);

std::string getBookId(const std::string& path, HashProgress onProgress = nullptr, void* ctx = nullptr);

// Cache-only lookup: returns the sidecar id when it is well-formed and its
// stored size matches the current file. Never hashes, so it is safe on a UI
// path that must not block on SD reads.
std::string readCachedBookId(const std::string& path);
