#include "FreeTimeListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>
#include <memory>
#include <string>

#include "FreeTimeEditActivity.h"
#include "MappedInputManager.h"
#include "ReadingPlanStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr StrId WEEKDAY_LABELS[7] = {StrId::STR_WD_SUN, StrId::STR_WD_MON, StrId::STR_WD_TUE, StrId::STR_WD_WED,
                                     StrId::STR_WD_THU, StrId::STR_WD_FRI, StrId::STR_WD_SAT};

std::string daysText(const uint8_t mask) {
  std::string text;
  for (int day = 0; day < 7; ++day) {
    if (!ReadingPlan::daySelected(mask, day)) continue;
    // Spaces, not commas: a comma separator would need a locale-specific
    // full-width variant for Chinese and reads as a run-on in 5-day slots.
    if (!text.empty()) text += " ";
    text += I18N.get(WEEKDAY_LABELS[day]);
  }
  return text;
}

}  // namespace

int FreeTimeListActivity::itemCount() const { return PLAN_STORE.slots().count + 1; }

void FreeTimeListActivity::onEnter() {
  Activity::onEnter();
  PLAN_STORE.load();
  selectedIndex = 0;
  requestUpdate();
}

void FreeTimeListActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int count = itemCount();
  buttonNavigator.onNext([this, count] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, count);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, count] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, count);
    requestUpdate();
  });
}

void FreeTimeListActivity::handleSelection() {
  auto reload = [this](const ActivityResult&) {
    PLAN_STORE.load();
    if (selectedIndex >= itemCount()) selectedIndex = itemCount() - 1;
    requestUpdate();
  };

  if (selectedIndex < PLAN_STORE.slots().count) {
    startActivityForResult(std::make_unique<FreeTimeEditActivity>(renderer, mappedInput, selectedIndex), reload);
  } else {
    startActivityForResult(std::make_unique<FreeTimeEditActivity>(renderer, mappedInput, -1), reload);
  }
}

void FreeTimeListActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FREE_TIME));

  const int slotCount = PLAN_STORE.slots().count;
  int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  if (slotCount == 0) {
    GUI.drawSubHeader(renderer, Rect{0, contentTop, pageWidth, metrics.tabBarHeight}, tr(STR_NO_FREE_SLOTS));
    contentTop += metrics.tabBarHeight + metrics.verticalSpacing;
  }

  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const int count = itemCount();

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, count, selectedIndex,
      [slotCount](int index) -> std::string {
        if (index >= slotCount) return std::string(tr(STR_NEW_SLOT));
        const auto& slot = PLAN_STORE.slots().items[index];
        return slot.name[0] != '\0' ? std::string(slot.name) : std::string(tr(STR_NOT_SET));
      },
      [slotCount](int index) -> std::string {
        if (index >= slotCount) return std::string();
        const auto& slot = PLAN_STORE.slots().items[index];
        char start[8];
        char stop[8];
        ReadingPlan::formatHhMm(start, sizeof(start), slot.startMin);
        ReadingPlan::formatHhMm(stop, sizeof(stop), slot.endMin);
        char range[24];
        snprintf(range, sizeof(range), "%s-%s", start, stop);
        return daysText(slot.daysMask) + " " + range;
      },
      nullptr,
      [slotCount](int index) -> std::string {
        if (index >= slotCount) return std::string();
        return PLAN_STORE.slots().items[index].flexible ? std::string(tr(STR_SLOT_FLEXIBLE)) : std::string();
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
