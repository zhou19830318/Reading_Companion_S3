#include "WifiIdleManager.h"

#include <Arduino.h>
#include <Logging.h>
#include <WiFi.h>

WifiIdleManager& WifiIdleManager::getInstance() {
  static WifiIdleManager manager;
  return manager;
}

void WifiIdleManager::arm() {
  if (!armed_) {
    LOG_INF("WIFI", "idle shutdown armed (%us of quiet)", static_cast<unsigned>(IDLE_TIMEOUT_MS / 1000));
  }
  armed_ = true;
  lastActivityMs_ = millis();
}

void WifiIdleManager::noteActivity() {
  if (armed_) lastActivityMs_ = millis();
}

void WifiIdleManager::hold() {
  uint8_t holds = holds_.load(std::memory_order_relaxed);
  while (holds < UINT8_MAX && !holds_.compare_exchange_weak(holds, holds + 1, std::memory_order_relaxed)) {
  }
  lastActivityMs_.store(millis(), std::memory_order_relaxed);
}

void WifiIdleManager::release() {
  uint8_t holds = holds_.load(std::memory_order_relaxed);
  while (holds > 0 && !holds_.compare_exchange_weak(holds, holds - 1, std::memory_order_relaxed)) {
  }
  lastActivityMs_.store(millis(), std::memory_order_relaxed);
}

void WifiIdleManager::poll() {
  if (!armed_ || holds_.load(std::memory_order_acquire) > 0) return;
  if (WiFi.getMode() == WIFI_MODE_NULL) return;  // radio already off
  if (static_cast<uint32_t>(millis() - lastActivityMs_.load(std::memory_order_acquire)) < IDLE_TIMEOUT_MS) return;
  shutdown();
}

void WifiIdleManager::shutdown() {
  armed_ = false;
  const uint32_t freeBefore = static_cast<uint32_t>(ESP.getFreeHeap());
  // Park sockets before the radio goes: the shared gateway session would
  // otherwise keep retrying a dial it cannot complete until its reconnect
  // budget empties into Failed.
  if (hookFn_ != nullptr) hookFn_(hookCtx_);
  WiFi.disconnect(true);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  LOG_INF("WIFI", "idle %us: radio off, free %u -> %u", static_cast<unsigned>(IDLE_TIMEOUT_MS / 1000),
          static_cast<unsigned>(freeBefore), static_cast<unsigned>(ESP.getFreeHeap()));
}
