#pragma once

#include <I18n.h>
#include <ReadingAdvice.h>

#include "ReadingPlanFormat.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// RP-01 editor. Fields are saved when leaving the screen (Back = save), so a
// half-edited new plan that never got a book or deadline is discarded instead
// of leaving an empty record behind.
class ReadingPlanEditActivity final : public Activity {
 public:
  ReadingPlanEditActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int planIndex)
      : Activity("ReadingPlanEdit", renderer, mappedInput), planIndex_(planIndex) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

  static constexpr int ROW_COUNT = 6;

 private:
  ButtonNavigator buttonNavigator;
  int planIndex_;
  bool isNew_ = false;
  ReadingPlan::Plan plan_{};
  int selectedRow_ = 0;
  StrId errorId_ = StrId::STR_NONE_OPT;
  // RP-03: recomputed on entry and whenever a field that feeds the formula
  // changes, never per repaint (the aggregate read touches the SD card).
  ReadingPlan::Advice advice_{};
  bool hasAdvice_ = false;

  void handleSelection();
  void saveAndFinish();
  void cycleStatus();
  void refreshAdvice();
};
