#pragma once
#include <cstdint>

#include "activities/Activity.h"

class BootActivity final : public Activity {
 public:
  explicit BootActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Boot", renderer, mappedInput) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool isBootAnimationComplete() const override { return animationComplete; }

 private:
  uint8_t frameIndex = 0;
  unsigned long nextFrameMs = 0;
  bool animationComplete = false;
};
