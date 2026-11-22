#include "SyncOutbox.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

namespace Sync {
namespace {

bool ensureDir() {
  if (Storage.exists(OUTBOX_DIR)) return true;
  return Storage.ensureDirectoryExists(OUTBOX_DIR);
}

}  // namespace

Stats stats() {
  Stats out;
  HalFile file = Storage.open(OUTBOX_PATH, O_RDONLY);
  if (!file) return out;

  char buf[1024];
  int got;
  while ((got = file.read(buf, sizeof(buf))) > 0) {
    for (int i = 0; i < got; ++i) {
      if (buf[i] == '\n' && out.lines < 0xFFFF) out.lines++;
    }
    if (out.bytes + static_cast<uint32_t>(got) >= MAX_BYTES + MAX_OP_BYTES) {
      out.bytes += static_cast<uint32_t>(got);
      break;
    }
    out.bytes += static_cast<uint32_t>(got);
  }
  return out;
}

bool append(const char* opJson, const size_t len) {
  if (opJson == nullptr || len == 0 || len > MAX_OP_BYTES) return false;
  if (!ensureDir()) {
    LOG_ERR("SYO", "cannot create %s", OUTBOX_DIR);
    return false;
  }

  const Stats before = stats();
  if (before.lines >= MAX_LINES || before.bytes + len + LINE_PREFIX_LEN + 1 > MAX_BYTES) {
    LOG_ERR("SYO", "outbox full: %u lines / %u bytes", static_cast<unsigned>(before.lines),
            static_cast<unsigned>(before.bytes));
    return false;
  }

  HalFile file = Storage.open(OUTBOX_PATH, O_WRITE | O_APPEND | O_CREAT);
  if (!file) {
    LOG_ERR("SYO", "append open failed: %s", OUTBOX_PATH);
    return false;
  }
  const size_t written = file.write(LINE_PREFIX, LINE_PREFIX_LEN);
  const size_t body = file.write(opJson, len);
  const size_t nl = file.write("\n", 1);
  if (written != LINE_PREFIX_LEN || body != len || nl != 1) {
    LOG_ERR("SYO", "short append: %u/%u + %u/%u", static_cast<unsigned>(written),
            static_cast<unsigned>(LINE_PREFIX_LEN), static_cast<unsigned>(body), static_cast<unsigned>(len));
    return false;
  }
  LOG_INF("SYO", "queued op: %u bytes (%u lines before)", static_cast<unsigned>(len),
          static_cast<unsigned>(before.lines));
  return true;
}

bool clear() {
  if (!Storage.exists(OUTBOX_PATH)) return true;
  const bool ok = Storage.remove(OUTBOX_PATH);
  if (!ok) LOG_ERR("SYO", "clear failed: %s", OUTBOX_PATH);
  return ok;
}

bool removeFront(const size_t lines) {
  if (lines == 0) return true;
  if (!Storage.exists(OUTBOX_PATH)) return true;

  static constexpr const char* TMP_PATH = "/.crosspoint/sync/outbox.tmp";
  if (!ensureDir()) return false;

  HalFile in = Storage.open(OUTBOX_PATH, O_RDONLY);
  if (!in) {
    LOG_ERR("SYO", "removeFront open failed: %s", OUTBOX_PATH);
    return false;
  }
  Storage.remove(TMP_PATH);  // leftover from an interrupted rewrite
  HalFile out = Storage.open(TMP_PATH, O_WRONLY | O_CREAT | O_TRUNC);
  if (!out) {
    LOG_ERR("SYO", "removeFront temp open failed: %s", TMP_PATH);
    return false;
  }

  char buf[1024];
  size_t skip = lines;
  bool skipping = true;
  bool wroteAny = false;
  bool ok = true;
  int got;
  while ((got = in.read(buf, sizeof(buf))) > 0) {
    size_t i = 0;
    if (skipping) {
      while (i < static_cast<size_t>(got)) {
        if (buf[i++] == '\n' && --skip == 0) {
          skipping = false;
          break;
        }
      }
    }
    const size_t keep = static_cast<size_t>(got) - i;
    if (keep > 0) {
      if (out.write(buf + i, keep) != keep) {
        ok = false;
        break;
      }
      wroteAny = true;
    }
  }
  in.close();
  out.flush();
  out.close();

  if (!ok) {
    LOG_ERR("SYO", "removeFront rewrite failed");
    Storage.remove(TMP_PATH);
    return false;
  }
  if (!wroteAny) {
    // Every queued line was acked; drop the file instead of leaving it empty.
    Storage.remove(TMP_PATH);
    return clear();
  }
  if (!Storage.remove(OUTBOX_PATH)) {
    LOG_ERR("SYO", "removeFront cannot replace %s", OUTBOX_PATH);
    Storage.remove(TMP_PATH);
    return false;
  }
  if (!Storage.rename(TMP_PATH, OUTBOX_PATH)) {
    LOG_ERR("SYO", "removeFront rename failed: %s", TMP_PATH);
    return false;
  }
  return true;
}

size_t loadBatch(char* buf, const size_t cap, const char* ops[], const size_t maxOps) {
  if (buf == nullptr || cap < LINE_PREFIX_LEN + 2 || ops == nullptr || maxOps == 0) return 0;

  HalFile file = Storage.open(OUTBOX_PATH, O_RDONLY);
  if (!file) return 0;

  size_t total = 0;
  while (total < cap - 1) {
    const int got = file.read(buf + total, cap - 1 - total);
    if (got <= 0) break;
    total += static_cast<size_t>(got);
  }
  buf[total] = '\0';
  if (total == 0) return 0;

  size_t count = 0;
  size_t pos = 0;
  while (pos + LINE_PREFIX_LEN < total && count < maxOps) {
    if (std::memcmp(buf + pos, LINE_PREFIX, LINE_PREFIX_LEN) != 0) {
      const char* nl = static_cast<const char*>(std::memchr(buf + pos, '\n', total - pos));
      if (nl == nullptr) break;
      pos = static_cast<size_t>(nl - buf) + 1;
      continue;
    }
    char* start = buf + pos + LINE_PREFIX_LEN;
    char* nl = static_cast<char*>(std::memchr(start, '\n', total - (start - buf)));
    if (nl == nullptr) break;  // partial trailing line, leave for next push
    *nl = '\0';
    ops[count++] = start;
    pos = static_cast<size_t>(nl - buf) + 1;
  }
  return count;
}

}  // namespace Sync
