#include "ReadingPlanListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "CrossPointSettings.h"
#include "FreeTimeListActivity.h"
#include "MappedInputManager.h"
#include "PlanTime.h"
#include "ReadingPlanEditActivity.h"
#include "ReadingPlanStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

const char* statusLabel(const ReadingPlan::PlanStatus status) {
  switch (status) {
    case ReadingPlan::PlanStatus::Paused:
      return tr(STR_PLAN_PAUSED);
    case ReadingPlan::PlanStatus::Archived:
      return tr(STR_PLAN_ARCHIVED);
    case ReadingPlan::PlanStatus::Active:
    default:
      return tr(STR_PLAN_ACTIVE);
  }
}

// The plan's identity is the book, not its deadline: the list row leads with
// the file name and keeps the date/goal as the supporting line. Full SD paths
// are long enough to be truncated in every value column.
std::string bookName(const char* path) {
  if (path == nullptr || path[0] == '\0') return std::string(tr(STR_NOT_SET));
  const char* slash = strrchr(path, '/');
  const char* name = (slash != nullptr && slash[1] != '\0') ? slash + 1 : path;
  return std::string(name);
}

// AI-08.5: the skip row only exists while the reminder is enabled. The skip
// itself is a local day index in settings, so it lapses by itself tomorrow.
bool reminderRowVisible() { return SETTINGS.planReminder != 0; }

bool reminderSkippedToday() {
  int32_t todayDay = 0;
  int weekday = -1;
  if (!PlanTime::todayDayIndex(todayDay, weekday)) return false;
  return SETTINGS.planReminderSkipDay == static_cast<uint32_t>(todayDay);
}

std::string planSubtitle(const ReadingPlan::Plan& plan) {
  char buf[64];
  if (plan.targetDate[0] != '\0') {
    snprintf(buf, sizeof(buf), "%s  %u %s%s", plan.targetDate, static_cast<unsigned>(plan.weeklyMinutes),
             tr(STR_MINUTES_SHORT), tr(STR_PER_WEEK));
  } else {
    snprintf(buf, sizeof(buf), "%u %s%s", static_cast<unsigned>(plan.weeklyMinutes), tr(STR_MINUTES_SHORT),
             tr(STR_PER_WEEK));
  }
  return buf;
}

}  // namespace

int ReadingPlanListActivity::itemCount() const { return PLAN_STORE.plans().count + 2 + (reminderRowVisible() ? 1 : 0); }

void ReadingPlanListActivity::onEnter() {
  Activity::onEnter();
  PLAN_STORE.load();
  selectedIndex = 0;
  requestUpdate();
}

void ReadingPlanListActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::PLAN);
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

void ReadingPlanListActivity::handleSelection() {
  auto reload = [this](const ActivityResult&) {
    PLAN_STORE.load();
    if (selectedIndex >= itemCount()) selectedIndex = itemCount() - 1;
    requestUpdate();
  };

  const int planCount = PLAN_STORE.plans().count;
  if (selectedIndex < planCount) {
    startActivityForResult(std::make_unique<ReadingPlanEditActivity>(renderer, mappedInput, selectedIndex), reload);
  } else if (selectedIndex == planCount) {
    startActivityForResult(std::make_unique<ReadingPlanEditActivity>(renderer, mappedInput, -1), reload);
  } else if (selectedIndex == planCount + 1) {
    startActivityForResult(std::make_unique<FreeTimeListActivity>(renderer, mappedInput), reload);
  } else if (reminderRowVisible()) {
    int32_t todayDay = 0;
    int weekday = -1;
    if (!PlanTime::todayDayIndex(todayDay, weekday)) {
      // Without a sane clock there is no day to skip; leave settings alone.
      requestUpdate();
      return;
    }
    const uint32_t day = static_cast<uint32_t>(todayDay);
    const uint32_t next = SETTINGS.planReminderSkipDay == day ? 0u : day;
    if (next != SETTINGS.planReminderSkipDay) {
      SETTINGS.planReminderSkipDay = next;
      SETTINGS.saveToFile();
    }
    requestUpdate();
  }
}

void ReadingPlanListActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READING_PLANS));

  const int planCount = PLAN_STORE.plans().count;
  int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  if (planCount == 0) {
    GUI.drawSubHeader(renderer, Rect{0, contentTop, pageWidth, metrics.tabBarHeight}, tr(STR_NO_PLANS));
    contentTop += metrics.tabBarHeight + metrics.verticalSpacing;
  }

  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const int count = itemCount();
  const bool skipped = reminderSkippedToday();
  const StrId skipLabel = skipped ? StrId::STR_PLAN_RESUME_TODAY : StrId::STR_PLAN_SKIP_TODAY;
  const StrId skipHint = skipped ? StrId::STR_PLAN_RESUME_TODAY_HINT : StrId::STR_PLAN_SKIP_TODAY_HINT;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, count, selectedIndex,
      [planCount, skipLabel](int index) -> std::string {
        if (index < planCount) return bookName(PLAN_STORE.plans().items[index].bookId);
        // The primary action comes before the secondary one even when the list
        // is empty, so the first row is never "Free Time" by default.
        if (index == planCount) return std::string(tr(STR_NEW_PLAN));
        if (index == planCount + 1) return std::string(tr(STR_FREE_TIME));
        return std::string(I18N.get(skipLabel));
      },
      [planCount, skipHint](int index) -> std::string {
        // Action rows also get a second line: the list uses one row height for
        // every entry, and an empty half-row would read as a rendering gap.
        if (index < planCount) return planSubtitle(PLAN_STORE.plans().items[index]);
        if (index == planCount) return std::string(tr(STR_NEW_PLAN_HINT));
        if (index == planCount + 1) return std::string(tr(STR_FREE_TIME_HINT));
        return std::string(I18N.get(skipHint));
      },
      nullptr,
      [planCount](int index) -> std::string {
        if (index < planCount) return statusLabel(PLAN_STORE.plans().items[index].status);
        return std::string();
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
