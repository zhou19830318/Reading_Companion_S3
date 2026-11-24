#pragma once

#include "ReadingPlanFormat.h"

// SD-backed store for RP-01/02. Holds both files in RAM (8 plans + 8 slots,
// ~2 KB) for the lifetime of the process: the UI reads and edits them field by
// field, and a reload from the card on every keystroke would be both slow and
// a write-amplification risk. Every mutation persists immediately through a
// tmp+rename write, so a power loss leaves either the old or the new file.
//
// The store is not thread-safe; like the rest of the settings/history code it
// is only touched from the activity (main) task.
class ReadingPlanStore {
 public:
  bool load();

  const ReadingPlan::Plans& plans() const { return plans_; }
  const ReadingPlan::Slots& slots() const { return slots_; }
  bool loaded() const { return loaded_; }

  // Returns the new index, or -1 when the list is full or the write failed.
  int addPlan(const ReadingPlan::Plan& plan);
  bool updatePlan(int index, const ReadingPlan::Plan& plan);
  bool savePlans();

  int addSlot(const ReadingPlan::FreeSlot& slot, const char* updatedAtIso = nullptr);
  bool updateSlot(int index, const ReadingPlan::FreeSlot& slot, const char* updatedAtIso = nullptr);
  bool removeSlot(int index, const char* updatedAtIso = nullptr);
  bool saveSlots(const char* updatedAtIso = nullptr);

  static ReadingPlanStore& getInstance() { return instance; }

 private:
  static ReadingPlanStore instance;

  ReadingPlan::Plans plans_{};
  ReadingPlan::Slots slots_{};
  bool loaded_ = false;

  bool readInto(const char* path, char* buffer, size_t cap, size_t& len);
  bool writeAtomic(const char* path, const char* data, size_t len);
};

#define PLAN_STORE ReadingPlanStore::getInstance()
