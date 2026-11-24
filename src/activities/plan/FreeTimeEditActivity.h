#pragma once

#include <I18n.h>

#include "ReadingPlanFormat.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// RP-02 editor: name, weekday mask, start/end clock and the flexible flag.
// Back saves; a new slot the user never touched is discarded instead of
// creating a default record.
class FreeTimeEditActivity final : public Activity {
 public:
  FreeTimeEditActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int slotIndex)
      : Activity("FreeTimeEdit", renderer, mappedInput), slotIndex_(slotIndex) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr int ROW_NAME = 0;
  static constexpr int ROW_FIRST_DAY = 1;  // seven weekday toggles, Monday first
  static constexpr int ROW_START = 8;
  static constexpr int ROW_END = 9;
  static constexpr int ROW_FLEXIBLE = 10;
  static constexpr int ROW_DELETE = 11;

  ButtonNavigator buttonNavigator;
  int slotIndex_;
  bool isNew_ = false;
  bool dirty_ = false;
  ReadingPlan::FreeSlot slot_{};
  int selectedRow_ = 0;
  StrId errorId_ = StrId::STR_NONE_OPT;

  int rowCount() const { return isNew_ ? ROW_DELETE : ROW_DELETE + 1; }
  void handleSelection();
  void editTime(bool start);
  void saveAndFinish();
  void deleteSlot();
};
