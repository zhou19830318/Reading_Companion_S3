#include "BookIdentity.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_task_wdt.h>
#include <freertos/task.h>
#include <mbedtls/sha256.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>

#include "NoteFormat.h"

namespace {

constexpr const char* BOOK_ID_DIR = "/.crosspoint/books";
constexpr size_t ID_HEX_LEN = 16;     // first 8 SHA-256 bytes, as the relay's sha256[:16]
constexpr size_t HASH_CHUNK = 16384;  // one SD read per 16 KB; keeps a 70 MB book's first hash tolerable

bool isHexId(const char* text, const size_t len) {
  if (len != ID_HEX_LEN) return false;
  for (size_t i = 0; i < len; ++i) {
    const char c = text[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

std::string sidecarPathFor(const std::string& bookPath) {
  char base[Notes::NoteFormat::NAME_PATH_SIZE];
  if (!Notes::NoteFormat::baseNameForBook(bookPath.c_str(), base, sizeof(base))) return {};
  // The base name alone collides for same-named books in different folders,
  // and an id mix-up would point citations at the wrong book. The path hash
  // (the same key the EPUB cache uses) disambiguates them.
  const auto pathHash = static_cast<unsigned>(std::hash<std::string>{}(bookPath));
  char suffix[16];
  std::snprintf(suffix, sizeof(suffix), "-%08x.id", pathHash);
  std::string out = BOOK_ID_DIR;
  out += '/';
  out += base;
  out += suffix;
  return out;
}

// Reads "<16 hex> <size>\n". Returns the id and writes the recorded size.
std::string readSidecar(const std::string& sidecar, size_t& recordedSize) {
  char buf[64];
  const size_t len = Storage.readFileToBuffer(sidecar.c_str(), buf, sizeof(buf));
  if (len < ID_HEX_LEN + 2 || !isHexId(buf, ID_HEX_LEN) || buf[ID_HEX_LEN] != ' ') return {};

  size_t value = 0;
  size_t pos = ID_HEX_LEN + 1;
  if (buf[pos] < '0' || buf[pos] > '9') return {};
  while (pos < len && buf[pos] != '\n' && buf[pos] != '\r') {
    const char c = buf[pos];
    if (c < '0' || c > '9') return {};
    value = value * 10 + static_cast<size_t>(c - '0');
    ++pos;
  }
  recordedSize = value;
  return std::string(buf, ID_HEX_LEN);
}

void writeSidecar(const std::string& sidecar, const char* id, const size_t size) {
  if (!Storage.ensureDirectoryExists(BOOK_ID_DIR)) {
    LOG_ERR("BID", "cannot create %s", BOOK_ID_DIR);
    return;
  }
  HalFile out;
  if (!Storage.openFileForWrite("BID", sidecar, out)) {
    LOG_ERR("BID", "cannot write %s", sidecar.c_str());
    return;
  }
  char line[48];
  const int written = std::snprintf(line, sizeof(line), "%s %u\n", id, static_cast<unsigned>(size));
  if (written > 0) out.write(line, static_cast<size_t>(written));
}

std::string hashFile(HalFile& file, const size_t fileSize, size_t& sizeOut, const HashProgress onProgress,
                     void* const ctx) {
  auto buf = makeUniqueNoThrow<uint8_t[]>(HASH_CHUNK);
  if (!buf) {
    LOG_ERR("BID", "OOM: hash buffer %u bytes", static_cast<unsigned>(HASH_CHUNK));
    return {};
  }

  mbedtls_sha256_context ctxHash;
  mbedtls_sha256_init(&ctxHash);
  mbedtls_sha256_starts(&ctxHash, /*is224=*/0);

  uint8_t lastPercent = 0;
  if (onProgress != nullptr) onProgress(ctx, 0);

  size_t total = 0;
  while (true) {
    const int got = file.read(buf.get(), HASH_CHUNK);
    if (got < 0) {
      LOG_ERR("BID", "hash read failed after %u bytes", static_cast<unsigned>(total));
      mbedtls_sha256_free(&ctxHash);
      return {};
    }
    if (got == 0) break;
    mbedtls_sha256_update(&ctxHash, buf.get(), static_cast<size_t>(got));
    total += static_cast<size_t>(got);
    // The first ask on an SD-copied book can hash tens of MB. This runs on the
    // Arduino loop task (subscribed to the task watchdog) and previously held
    // it for >30 s on a 36 MB EPUB, tripping a WDT reboot that looked like a
    // hang. Yield + feed per 16 KB chunk so the UI and the watchdog stay alive.
    if (onProgress != nullptr && fileSize > 0) {
      uint64_t scaled = static_cast<uint64_t>(total) * 100 / fileSize;
      if (scaled > 100) scaled = 100;
      const uint8_t percent = static_cast<uint8_t>(scaled);
      // Coarse steps keep the e-ink repaint cost off the hash loop while the
      // screen still moves on a multi-second book.
      if (percent >= lastPercent + 10 || (percent == 100 && lastPercent < 100)) {
        lastPercent = percent;
        onProgress(ctx, percent);
      }
    }
    if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset();
    vTaskDelay(1);
  }

  uint8_t digest[32];
  mbedtls_sha256_finish(&ctxHash, digest);
  mbedtls_sha256_free(&ctxHash);

  if (onProgress != nullptr && lastPercent < 100) onProgress(ctx, 100);

  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  char id[ID_HEX_LEN + 1];
  for (size_t i = 0; i < ID_HEX_LEN / 2; ++i) {
    id[i * 2] = HEX_DIGITS[digest[i] >> 4];
    id[i * 2 + 1] = HEX_DIGITS[digest[i] & 0x0F];
  }
  id[ID_HEX_LEN] = '\0';
  sizeOut = total;
  return std::string(id, ID_HEX_LEN);
}

}  // namespace

std::string readCachedBookId(const std::string& path) {
  const std::string sidecar = sidecarPathFor(path);
  if (sidecar.empty()) return {};

  HalFile book;
  if (!Storage.openFileForRead("BID", path, book) || !book) return {};
  const size_t bookSize = book.size();

  size_t recordedSize = 0;
  const std::string id = readSidecar(sidecar, recordedSize);
  if (id.empty() || recordedSize != bookSize) return {};
  return id;
}

std::string getBookId(const std::string& path, const HashProgress onProgress, void* const ctx) {
  const std::string sidecar = sidecarPathFor(path);
  if (sidecar.empty()) return {};

  HalFile book;
  if (!Storage.openFileForRead("BID", path, book) || !book) {
    LOG_ERR("BID", "open failed: %s", path.c_str());
    return {};
  }
  const size_t bookSize = book.size();

  size_t recordedSize = 0;
  const std::string cached = readSidecar(sidecar, recordedSize);
  if (!cached.empty() && recordedSize == bookSize) return cached;

  size_t hashedSize = 0;
  const std::string id = hashFile(book, bookSize, hashedSize, onProgress, ctx);
  if (id.empty()) return {};

  // Use the byte count actually hashed: if the file changed between the size
  // query and the read, the sidecar records the size that produced this id.
  writeSidecar(sidecar, id.c_str(), hashedSize);
  LOG_DBG("BID", "%s -> %s (%u bytes)", path.c_str(), id.c_str(), static_cast<unsigned>(hashedSize));
  return id;
}
