#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// RP-02 list: periodic free-time windows (weekday mask + clock range + the
// "flexible" flag). Reached from the reading-plan list; Back returns there.
class FreeTimeListActivity final : public Activity {
 public:
  FreeTimeListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("FreeTimeList", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;

  int itemCount() const;
  void handleSelection();
};
