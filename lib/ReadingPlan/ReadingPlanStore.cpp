#include "ReadingPlanStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>

namespace {
constexpr size_t PLANS_FILE_CAP = 4096;
constexpr size_t SLOTS_FILE_CAP = 2048;
constexpr size_t TMP_PATH_CAP = 64;
}  // namespace

ReadingPlanStore ReadingPlanStore::instance;

bool ReadingPlanStore::readInto(const char* path, char* buffer, size_t cap, size_t& len) {
  len = 0;
  if (buffer == nullptr || cap == 0) return false;
  buffer[0] = '\0';
  if (!Storage.exists(path)) return true;  // no file yet: empty store, not an error
  const size_t read = Storage.readFileToBuffer(path, buffer, cap);
  if (read == 0) {
    LOG_ERR("PLAN", "unreadable or empty file: %s", path);
    return false;
  }
  len = read;
  return true;
}

bool ReadingPlanStore::load() {
  loaded_ = false;
  plans_ = ReadingPlan::Plans{};
  slots_ = ReadingPlan::Slots{};

  auto plansBuf = makeUniqueNoThrow<char[]>(PLANS_FILE_CAP);
  auto slotsBuf = makeUniqueNoThrow<char[]>(SLOTS_FILE_CAP);
  if (!plansBuf || !slotsBuf) {
    LOG_ERR("PLAN", "OOM: load buffers");
    return false;
  }

  size_t plansLen = 0;
  if (!readInto(ReadingPlan::PLANS_PATH, plansBuf.get(), PLANS_FILE_CAP, plansLen)) {
    LOG_ERR("PLAN", "plans read failed");
  } else {
    const int count = ReadingPlan::parsePlansFile(plansBuf.get(), plansLen, plans_);
    LOG_DBG("PLAN", "loaded %d plan(s)", count);
  }

  size_t slotsLen = 0;
  if (!readInto(ReadingPlan::SLOTS_PATH, slotsBuf.get(), SLOTS_FILE_CAP, slotsLen)) {
    LOG_ERR("PLAN", "slots read failed");
  } else {
    const int count = ReadingPlan::parseSlotsFile(slotsBuf.get(), slotsLen, slots_);
    LOG_DBG("PLAN", "loaded %d free slot(s)", count);
  }

  loaded_ = true;
  return true;
}

bool ReadingPlanStore::writeAtomic(const char* path, const char* data, size_t len) {
  if (path == nullptr || data == nullptr || len == 0) return false;
  if (!Storage.ensureDirectoryExists(ReadingPlan::DIR)) {
    LOG_ERR("PLAN", "mkdir failed: %s", ReadingPlan::DIR);
    return false;
  }

  char tmpPath[TMP_PATH_CAP];
  const int n = snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(tmpPath)) {
    LOG_ERR("PLAN", "tmp path too long: %s", path);
    return false;
  }

  bool ok = false;
  {
    HalFile file;
    if (!Storage.openFileForWrite("PLAN", tmpPath, file)) {
      LOG_ERR("PLAN", "open for write failed: %s", tmpPath);
      return false;
    }
    const size_t written = file.write(data, len);
    file.flush();
    ok = written == len;
  }  // handle closes before the rename (DESTRUCTOR_CLOSES_FILE)

  if (!ok) {
    Storage.remove(tmpPath);
    LOG_ERR("PLAN", "short write: %s", tmpPath);
    return false;
  }

  // SdFat's rename does not overwrite; the remove/rename pair leaves a window
  // with no file, which load() treats as an empty store rather than an error.
  Storage.remove(path);
  if (!Storage.rename(tmpPath, path)) {
    LOG_ERR("PLAN", "rename failed: %s", path);
    return false;
  }
  return true;
}

bool ReadingPlanStore::savePlans() {
  auto buf = makeUniqueNoThrow<char[]>(PLANS_FILE_CAP);
  if (!buf) {
    LOG_ERR("PLAN", "OOM: plans buffer");
    return false;
  }
  const size_t len = ReadingPlan::formatPlansFile(buf.get(), PLANS_FILE_CAP, plans_);
  if (len == 0) {
    LOG_ERR("PLAN", "plans file format failed (%d plans)", plans_.count);
    return false;
  }
  return writeAtomic(ReadingPlan::PLANS_PATH, buf.get(), len);
}

bool ReadingPlanStore::saveSlots(const char* updatedAtIso) {
  auto buf = makeUniqueNoThrow<char[]>(SLOTS_FILE_CAP);
  if (!buf) {
    LOG_ERR("PLAN", "OOM: slots buffer");
    return false;
  }
  const size_t len = ReadingPlan::formatSlotsFile(buf.get(), SLOTS_FILE_CAP, slots_, updatedAtIso);
  if (len == 0) {
    LOG_ERR("PLAN", "slots file format failed (%d slots)", slots_.count);
    return false;
  }
  return writeAtomic(ReadingPlan::SLOTS_PATH, buf.get(), len);
}

int ReadingPlanStore::addPlan(const ReadingPlan::Plan& plan) {
  if (plans_.count >= ReadingPlan::MAX_PLANS) {
    LOG_ERR("PLAN", "plan list full (%d)", plans_.count);
    return -1;
  }
  ReadingPlan::Plan stored = plan;
  if (stored.id[0] == '\0') {
    ReadingPlan::makePlanId(stored.id, sizeof(stored.id), stored.targetDate, plans_.count + 1);
  }
  plans_.items[plans_.count] = stored;
  ++plans_.count;
  if (!savePlans()) {
    --plans_.count;
    return -1;
  }
  return plans_.count - 1;
}

bool ReadingPlanStore::updatePlan(int index, const ReadingPlan::Plan& plan) {
  if (index < 0 || index >= plans_.count) return false;
  const ReadingPlan::Plan previous = plans_.items[index];
  plans_.items[index] = plan;
  if (plans_.items[index].id[0] == '\0') {
    memcpy(plans_.items[index].id, previous.id, sizeof(plans_.items[index].id));
  }
  if (!savePlans()) {
    plans_.items[index] = previous;
    return false;
  }
  return true;
}

int ReadingPlanStore::addSlot(const ReadingPlan::FreeSlot& slot, const char* updatedAtIso) {
  if (slots_.count >= ReadingPlan::MAX_SLOTS) {
    LOG_ERR("PLAN", "slot list full (%d)", slots_.count);
    return -1;
  }
  slots_.items[slots_.count] = slot;
  ++slots_.count;
  if (!saveSlots(updatedAtIso)) {
    --slots_.count;
    return -1;
  }
  return slots_.count - 1;
}

bool ReadingPlanStore::updateSlot(int index, const ReadingPlan::FreeSlot& slot, const char* updatedAtIso) {
  if (index < 0 || index >= slots_.count) return false;
  const ReadingPlan::FreeSlot previous = slots_.items[index];
  slots_.items[index] = slot;
  if (!saveSlots(updatedAtIso)) {
    slots_.items[index] = previous;
    return false;
  }
  return true;
}

bool ReadingPlanStore::removeSlot(int index, const char* updatedAtIso) {
  if (index < 0 || index >= slots_.count) return false;
  const ReadingPlan::FreeSlot previous = slots_.items[index];
  for (int i = index; i + 1 < slots_.count; ++i) slots_.items[i] = slots_.items[i + 1];
  --slots_.count;
  if (!saveSlots(updatedAtIso)) {
    for (int i = slots_.count; i > index; --i) slots_.items[i] = slots_.items[i - 1];
    slots_.items[index] = previous;
    ++slots_.count;
    return false;
  }
  return true;
}
