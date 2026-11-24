#include "ReadingPlanEditActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <ReadingAggregate.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <variant>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "PlanTime.h"
#include "ReadingPlanStore.h"
#include "RecentBooksStore.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/CompanionUi.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

const StrId ROW_LABELS[ReadingPlanEditActivity::ROW_COUNT] = {
    StrId::STR_TARGET_BOOK,  StrId::STR_TARGET_DATE, StrId::STR_WEEKLY_MINUTES,
    StrId::STR_WEEKLY_PAGES, StrId::STR_PRIORITY,    StrId::STR_PLAN_STATUS,
};

// The editor's value column shows the file name; the full path is one tap
// away in the keyboard screen and would otherwise be truncated mid-directory.
std::string bookName(const char* path) {
  if (path == nullptr || path[0] == '\0') return std::string(tr(STR_NOT_SET));
  const char* slash = strrchr(path, '/');
  return std::string(slash != nullptr && slash[1] != '\0' ? slash + 1 : path);
}

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

}  // namespace

void ReadingPlanEditActivity::onEnter() {
  Activity::onEnter();

  isNew_ = planIndex_ < 0;
  selectedRow_ = 0;
  errorId_ = StrId::STR_NONE_OPT;

  if (!isNew_) {
    const auto& plans = PLAN_STORE.plans();
    if (planIndex_ < plans.count) {
      plan_ = plans.items[planIndex_];
    } else {
      // The record vanished between entering the list and opening the editor.
      isNew_ = true;
      planIndex_ = -1;
    }
  }

  if (isNew_) {
    plan_ = ReadingPlan::Plan{};
    plan_.weeklyMinutes = 180;
    plan_.priority = 3;
    plan_.tzOffsetQ = static_cast<int8_t>(SETTINGS.clockUtcOffsetQ);
    PlanTime::dateInDays(30, plan_.targetDate, sizeof(plan_.targetDate));
    const auto& books = RECENT_BOOKS.getBooks();
    if (!books.empty()) {
      snprintf(plan_.bookId, sizeof(plan_.bookId), "%s", books.front().path.c_str());
    }
  }

  refreshAdvice();
  requestUpdate();
}

void ReadingPlanEditActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    saveAndFinish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  buttonNavigator.onNext([this] {
    selectedRow_ = (selectedRow_ + 1) % ROW_COUNT;
    errorId_ = StrId::STR_NONE_OPT;
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selectedRow_ = (selectedRow_ + ROW_COUNT - 1) % ROW_COUNT;
    errorId_ = StrId::STR_NONE_OPT;
    requestUpdate();
  });
}

void ReadingPlanEditActivity::handleSelection() {
  switch (selectedRow_) {
    case 0: {
      auto handler = [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        const auto* keyboard = std::get_if<KeyboardResult>(&result.data);
        if (keyboard == nullptr) return;
        snprintf(plan_.bookId, sizeof(plan_.bookId), "%s", keyboard->text.c_str());
        requestUpdate();
      };
      startActivityForResult(
          std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_TARGET_BOOK), plan_.bookId,
                                                  ReadingPlan::BOOK_CAP - 1, InputType::Text),
          handler);
      break;
    }
    case 1: {
      auto handler = [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        const auto* keyboard = std::get_if<KeyboardResult>(&result.data);
        if (keyboard == nullptr) return;
        if (!keyboard->text.empty() && !ReadingPlan::isValidDate(keyboard->text.c_str())) {
          errorId_ = StrId::STR_INVALID_DATE;
          requestUpdate();
          return;
        }
        snprintf(plan_.targetDate, sizeof(plan_.targetDate), "%s", keyboard->text.c_str());
        refreshAdvice();
        requestUpdate();
      };
      startActivityForResult(
          std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_TARGET_DATE), plan_.targetDate,
                                                  ReadingPlan::DATE_CAP - 1, InputType::Text),
          handler);
      break;
    }
    case 2: {
      int initial = plan_.weeklyMinutes;
      if (initial < 30) initial = 30;
      if (initial > 1260) initial = 1260;
      auto handler = [this](const ActivityResult& result) {
        const auto* value = std::get_if<IntervalResult>(&result.data);
        if (value == nullptr) return;
        plan_.weeklyMinutes = static_cast<uint16_t>(value->value);
        refreshAdvice();
        requestUpdate();
      };
      startActivityForResult(
          std::make_unique<IntervalSelectionActivity>(renderer, mappedInput, "PlanMinutes", StrId::STR_WEEKLY_MINUTES,
                                                      StrId::STR_NONE_OPT, initial, 30, 1260, 30, 120),
          handler);
      break;
    }
    case 3: {
      int initial = plan_.weeklyPages;
      if (initial > 1000) initial = 1000;
      auto handler = [this](const ActivityResult& result) {
        const auto* value = std::get_if<IntervalResult>(&result.data);
        if (value == nullptr) return;
        plan_.weeklyPages = static_cast<uint16_t>(value->value);
        refreshAdvice();
        requestUpdate();
      };
      startActivityForResult(
          std::make_unique<IntervalSelectionActivity>(renderer, mappedInput, "PlanPages", StrId::STR_WEEKLY_PAGES,
                                                      StrId::STR_NONE_OPT, initial, 0, 1000, 10, 50),
          handler);
      break;
    }
    case 4: {
      const int initial = plan_.priority < 1 ? 1 : (plan_.priority > 5 ? 5 : plan_.priority);
      auto handler = [this](const ActivityResult& result) {
        const auto* value = std::get_if<IntervalResult>(&result.data);
        if (value == nullptr) return;
        plan_.priority = static_cast<uint8_t>(value->value);
        requestUpdate();
      };
      startActivityForResult(
          std::make_unique<IntervalSelectionActivity>(renderer, mappedInput, "PlanPriority", StrId::STR_PRIORITY,
                                                      StrId::STR_NONE_OPT, initial, 1, 5, 1, 1),
          handler);
      break;
    }
    case 5:
      cycleStatus();
      break;
    default:
      break;
  }
}

void ReadingPlanEditActivity::cycleStatus() {
  switch (plan_.status) {
    case ReadingPlan::PlanStatus::Active:
      plan_.status = ReadingPlan::PlanStatus::Paused;
      break;
    case ReadingPlan::PlanStatus::Paused:
      plan_.status = ReadingPlan::PlanStatus::Archived;
      break;
    case ReadingPlan::PlanStatus::Archived:
      plan_.status = ReadingPlan::PlanStatus::Active;
      break;
  }
  refreshAdvice();
  requestUpdate();
}

void ReadingPlanEditActivity::refreshAdvice() {
  advice_ = ReadingPlan::Advice{};
  hasAdvice_ = false;
  // Paused/archived plans are not nudged; only the active plan gets advice.
  if (plan_.status != ReadingPlan::PlanStatus::Active) return;

  int32_t todayDay = 0;
  int weekday = -1;
  const bool clockOk = PlanTime::todayDayIndex(todayDay, weekday);

  ReadingPlan::PaceWindow window;
  if (clockOk) {
    ReadingHistory::DayStat stats[14];
    ReadingHistory::loadDayStats(todayDay - 13, 14, stats, /*fallbackToHistory=*/true);
    // The window can be sparse (days with no reading), so every slot is
    // inspected; the return value only counts present days.
    for (int i = 0; i < 14; ++i) {
      if (!stats[i].present) continue;
      window.totalSec += stats[i].totalSec;
      window.pagesAdvanced += stats[i].pagesAdvanced;
      ++window.daysWithData;
    }
  }

  int32_t targetDay = 0;
  const bool haveTarget = clockOk && PlanTime::dayIndexFromDate(plan_.targetDate, targetDay);
  advice_ = ReadingPlan::buildAdvice(plan_, PLAN_STORE.slots(), window, weekday, haveTarget ? targetDay : 0,
                                     clockOk ? todayDay : 0);
  hasAdvice_ = advice_.dailyMinutes > 0;
}

void ReadingPlanEditActivity::saveAndFinish() {
  if (plan_.targetDate[0] != '\0' && !ReadingPlan::isValidDate(plan_.targetDate)) {
    errorId_ = StrId::STR_INVALID_DATE;
    requestUpdate();
    return;
  }

  // A brand-new plan that never got a book or a deadline carries no intent.
  if (isNew_ && plan_.bookId[0] == '\0' && plan_.targetDate[0] == '\0') {
    finish();
    return;
  }

  if (plan_.createdAt[0] == '\0') {
    char iso[ReadingPlan::ISO_CAP];
    if (PlanTime::nowIso(iso, sizeof(iso))) {
      snprintf(plan_.createdAt, sizeof(plan_.createdAt), "%s", iso);
    }
  }
  plan_.tzOffsetQ = static_cast<int8_t>(SETTINGS.clockUtcOffsetQ);

  const bool saved = isNew_ ? (PLAN_STORE.addPlan(plan_) >= 0) : PLAN_STORE.updatePlan(planIndex_, plan_);
  if (!saved) {
    LOG_ERR("PLAN", "save failed (index %d, new=%d)", planIndex_, isNew_ ? 1 : 0);
    errorId_ = StrId::STR_ERROR_GENERAL_FAILURE;
    requestUpdate();
    return;
  }
  finish();
}

void ReadingPlanEditActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 isNew_ ? tr(STR_NEW_PLAN) : tr(STR_EDIT_PLAN));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int listHeight = ROW_COUNT * metrics.listRowHeight;

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, listHeight}, ROW_COUNT, selectedRow_,
      [](int index) -> std::string { return std::string(I18N.get(ROW_LABELS[index])); }, nullptr, nullptr,
      [this](int index) -> std::string {
        char value[48];
        switch (index) {
          case 0:
            return bookName(plan_.bookId);
          case 1:
            return plan_.targetDate[0] != '\0' ? std::string(plan_.targetDate) : std::string(tr(STR_NOT_SET));
          case 2:
            snprintf(value, sizeof(value), "%u %s", static_cast<unsigned>(plan_.weeklyMinutes), tr(STR_MINUTES_SHORT));
            return value;
          case 3:
            // 0 means the plan tracks minutes only; "0 pages" would read as a
            // goal of zero instead of "not tracked".
            if (plan_.weeklyPages == 0) return std::string(tr(STR_NO_LIMIT));
            snprintf(value, sizeof(value), "%u %s", static_cast<unsigned>(plan_.weeklyPages), tr(STR_PAGES_SHORT));
            return value;
          case 4:
            snprintf(value, sizeof(value), "%u", static_cast<unsigned>(plan_.priority));
            return value;
          case 5:
            return statusLabel(plan_.status);
          default:
            return std::string();
        }
      },
      true);

  // RP-03 advice sits in the blank band under the six field rows. All values
  // come from RAM except the aggregate window, which refreshAdvice() folded
  // into advice_ already.
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int adviceTop = contentTop + listHeight + metrics.verticalSpacing;
  if (hasAdvice_ && adviceTop + lineH * 4 <= contentBottom) {
    const int left = metrics.contentSidePadding;
    const int width = pageWidth - 2 * left;
    int y = adviceTop;
    y += CompanionUi::drawSectionLabel(renderer, left, y, width, tr(STR_PLAN_ADVICE)) + 4;

    char line[160];
    snprintf(line, sizeof(line), tr(STR_PLAN_ADVICE_DAILY), static_cast<unsigned>(advice_.dailyMinutes));
    renderer.drawText(UI_10_FONT_ID, left, y, line, true, EpdFontFamily::BOLD);
    y += lineH;

    if (advice_.hasPace) {
      snprintf(line, sizeof(line), tr(STR_PLAN_ADVICE_PACE), static_cast<unsigned>(advice_.pacePagesPerMinX100 / 100),
               static_cast<unsigned>(advice_.pacePagesPerMinX100 % 100));
    } else {
      snprintf(line, sizeof(line), "%s", tr(STR_PLAN_ADVICE_FROM_GOAL));
    }
    renderer.drawText(UI_10_FONT_ID, left, y, line);
    y += lineH;

    if (advice_.pagesPerDay > 0) {
      snprintf(line, sizeof(line), tr(STR_PLAN_ADVICE_PAGES), static_cast<unsigned>(advice_.pagesPerDay));
      renderer.drawText(UI_10_FONT_ID, left, y, line);
      y += lineH;
    }

    if (advice_.segmentCount > 0) {
      char slots[112];
      size_t used = 0;
      for (uint8_t i = 0; i < advice_.segmentCount; ++i) {
        char start[8];
        char end[8];
        ReadingPlan::formatHhMm(start, sizeof(start), advice_.segments[i].startMin);
        ReadingPlan::formatHhMm(end, sizeof(end), advice_.segments[i].endMin);
        const int n = snprintf(slots + used, sizeof(slots) - used, "%s%s-%s", i == 0 ? "" : " / ", start, end);
        if (n <= 0 || static_cast<size_t>(n) >= sizeof(slots) - used) break;
        used += static_cast<size_t>(n);
      }
      snprintf(line, sizeof(line), tr(STR_PLAN_ADVICE_SLOTS), slots);
    } else {
      snprintf(line, sizeof(line), "%s", tr(STR_PLAN_ADVICE_NO_SLOTS));
    }
    std::string slotsLine(line);
    if (renderer.getTextWidth(UI_10_FONT_ID, slotsLine.c_str()) > width) {
      slotsLine.resize(renderer.fitPrefixLen(UI_10_FONT_ID, slotsLine, width));
    }
    renderer.drawText(UI_10_FONT_ID, left, y, slotsLine.c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_SAVE), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (errorId_ != StrId::STR_NONE_OPT) {
    GUI.drawPopup(renderer, I18N.get(errorId_));
  }

  renderer.displayBuffer();
}
