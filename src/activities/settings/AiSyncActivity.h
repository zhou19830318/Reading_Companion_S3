#pragma once

#include "SyncRunner.h"
#include "activities/Activity.h"

// AI sync screen (V1-06): shows how many operations the outbox holds, whether
// the relay and Wi-Fi are usable, and drains the queue through
// /api/v1/sync/push. Triggered only from the foreground (opening this screen or
// pressing "Sync now"); there is no background polling.
class AiSyncActivity final : public Activity {
 public:
  explicit AiSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("AiSync", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return true; }

 private:
  enum class State { Idle, Syncing, Done };

  State state_ = State::Idle;
  uint16_t pendingLines_ = 0;
  uint32_t pendingBytes_ = 0;
  bool relayConfigured_ = false;
  bool wifiConnected_ = false;
  char message_[96] = {};
  Sync::RunSummary summary_;

  void refreshStatus();
  void startSync();
  void launchWifi();
  void onWifiSelected(bool connected);
};
