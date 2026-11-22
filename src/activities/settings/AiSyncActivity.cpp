#include "AiSyncActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdio>

#include "CloudConfig.h"
#include "MappedInputManager.h"
#include "SyncOutbox.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void AiSyncActivity::onEnter() {
  Activity::onEnter();
  state_ = State::Idle;
  message_[0] = '\0';
  refreshStatus();
  requestUpdate();
}

void AiSyncActivity::refreshStatus() {
  const Sync::Stats stats = Sync::stats();
  pendingLines_ = stats.lines;
  pendingBytes_ = stats.bytes;
  CLOUD_CFG.load();
  relayConfigured_ =
      CLOUD_CFG.relayUrl[0] != '\0' && CLOUD_CFG.relayToken[0] != '\0' && CLOUD_CFG.relayDeviceId[0] != '\0';
  wifiConnected_ = WiFi.status() == WL_CONNECTED;
}

void AiSyncActivity::launchWifi() {
  LOG_INF("ASYNC", "Sync requested with Wi-Fi down, opening selection");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelected(!result.isCancelled); });
}

void AiSyncActivity::onWifiSelected(const bool connected) {
  if (!connected) {
    requestUpdate();
    return;
  }
  startSync();
}

void AiSyncActivity::startSync() {
  refreshStatus();
  if (!relayConfigured_) {
    state_ = State::Done;
    std::snprintf(message_, sizeof(message_), "%s", tr(STR_AI_SYNC_NOT_CONFIGURED));
    requestUpdate();
    return;
  }
  if (pendingLines_ == 0) {
    state_ = State::Done;
    std::snprintf(message_, sizeof(message_), "%s", tr(STR_AI_SYNC_NOTHING));
    requestUpdate();
    return;
  }

  // Paint the "syncing" frame before the blocking HTTP round so the panel does
  // not sit on the idle screen for the duration of the push.
  state_ = State::Syncing;
  requestUpdateAndWait();

  summary_ = Sync::pushAll(CLOUD_CFG.relayUrl, CLOUD_CFG.relayToken, CLOUD_CFG.relayDeviceId);
  refreshStatus();
  state_ = State::Done;

  if (summary_.result != Sync::Client::Result::Ok) {
    const StrId reason = summary_.result == Sync::Client::Result::NoWifi ? StrId::STR_AI_SYNC_OFFLINE
                         : summary_.result == Sync::Client::Result::Auth ? StrId::STR_AI_SYNC_AUTH_FAILED
                         : summary_.result == Sync::Client::Result::Oom  ? StrId::STR_MEMORY_ERROR
                                                                         : StrId::STR_AI_SYNC_FAILED;
    std::snprintf(message_, sizeof(message_), "%s", I18N[reason]);
  } else if (summary_.failed > 0) {
    // Both outcomes stay queued (the server deduplicates by op_id), so the bar
    // tells the reader what needs attention instead of claiming success.
    const StrId label = summary_.conflict > 0 ? StrId::STR_AI_SYNC_CONFLICT : StrId::STR_AI_SYNC_PARTIAL;
    std::snprintf(message_, sizeof(message_), "%s (%u)", I18N[label], static_cast<unsigned>(summary_.failed));
  } else if (summary_.acked > 0) {
    std::snprintf(message_, sizeof(message_), "%s (%u)", tr(STR_AI_SYNC_OK), static_cast<unsigned>(summary_.acked));
  } else {
    std::snprintf(message_, sizeof(message_), "%s", tr(STR_AI_SYNC_NOTHING));
  }
  requestUpdate();
}

void AiSyncActivity::loop() {
  if (state_ == State::Syncing) return;

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (pendingLines_ == 0) {
      state_ = State::Done;
      std::snprintf(message_, sizeof(message_), "%s", tr(STR_AI_SYNC_NOTHING));
      requestUpdate();
      return;
    }
    if (!wifiConnected_) {
      launchWifi();
      return;
    }
    startSync();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
  }
}

void AiSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_AI_SYNC));

  if (state_ == State::Syncing) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, tr(STR_AI_SYNCING));
    renderer.displayBuffer();
    return;
  }

  const int left = pageWidth / 8;
  const int valueX = pageWidth / 2;
  int y = metrics.topPadding + metrics.headerHeight + 40;
  const int rowGap = 38;

  char value[64];
  auto row = [&](const char* label, const char* text) {
    renderer.drawText(UI_10_FONT_ID, left, y, label);
    renderer.drawText(UI_10_FONT_ID, valueX, y, text, true, EpdFontFamily::BOLD);
    y += rowGap;
  };

  const unsigned kb = (pendingBytes_ + 1023) / 1024;
  std::snprintf(value, sizeof(value), tr(STR_AI_SYNC_PENDING_FORMAT), static_cast<unsigned>(pendingLines_), kb);
  row(tr(STR_AI_SYNC_PENDING), value);

  row(tr(STR_AI_SYNC_RELAY), relayConfigured_ ? tr(STR_AI_SYNC_CONFIGURED) : tr(STR_AI_SYNC_NOT_CONFIGURED));
  row(tr(STR_AI_SYNC_WIFI), wifiConnected_ ? tr(STR_AI_SYNC_CONNECTED) : tr(STR_AI_SYNC_OFFLINE));

  if (message_[0] != '\0') {
    y += 14;
    renderer.drawCenteredText(UI_10_FONT_ID, y, message_, true, EpdFontFamily::BOLD);
    y += rowGap;
  }

  // rejected/conflict detail: V1 keeps both versions on the server and leaves
  // the operation queued locally for a later retry, so the screen shows the
  // split and the first offending op id rather than pretending it is done.
  if (state_ == State::Done && summary_.failed > 0) {
    char issues[64];
    std::snprintf(issues, sizeof(issues), tr(STR_AI_SYNC_CONFLICT_FORMAT), static_cast<unsigned>(summary_.conflict),
                  static_cast<unsigned>(summary_.rejected));
    renderer.drawCenteredText(UI_10_FONT_ID, y, issues);
    y += rowGap;
    if (summary_.firstFailedOpId[0] != '\0') {
      char first[64];
      std::snprintf(first, sizeof(first), tr(STR_AI_SYNC_CONFLICT_OP), summary_.firstFailedOpId);
      renderer.drawCenteredText(UI_10_FONT_ID, y, first);
    }
  }

  const char* confirmLabel = pendingLines_ > 0 ? tr(STR_AI_SYNC_NOW) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
