#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// RP-01 entry point: active/paused/archived plans, plus the free-time editor
// and "new plan". Archiving is a status on the plan (not a delete), so the row
// keeps its data and can be re-activated from the editor.
class ReadingPlanListActivity final : public Activity {
 public:
  ReadingPlanListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ReadingPlans", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;

  int itemCount() const;
  void handleSelection();
};
