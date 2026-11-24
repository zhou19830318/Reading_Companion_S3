#include "FreeTimeEditActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <variant>

#include "MappedInputManager.h"
#include "PlanTime.h"
#include "ReadingPlanStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr StrId WEEKDAY_LABELS[7] = {StrId::STR_WD_SUN, StrId::STR_WD_MON, StrId::STR_WD_TUE, StrId::STR_WD_WED,
                                     StrId::STR_WD_THU, StrId::STR_WD_FRI, StrId::STR_WD_SAT};

// Rows 1..7 are Monday..Sunday; the on-disk mask is Sunday-first.
int weekdayForRow(const int row) {
  const int index = row - 1;
  return index < 6 ? index + 1 : 0;
}

}  // namespace

void FreeTimeEditActivity::onEnter() {
  Activity::onEnter();

  isNew_ = slotIndex_ < 0;
  dirty_ = false;
  selectedRow_ = 0;
  errorId_ = StrId::STR_NONE_OPT;

  if (!isNew_) {
    if (slotIndex_ < PLAN_STORE.slots().count) {
      slot_ = PLAN_STORE.slots().items[slotIndex_];
    } else {
      isNew_ = true;
      slotIndex_ = -1;
    }
  }

  if (isNew_) {
    slot_ = ReadingPlan::FreeSlot{};
    // A named default is what the list will show if the user only adjusts the
    // days/time; an empty name would render as "Not Set" for a perfectly valid
    // slot. Leaving without touching anything still discards the record.
    snprintf(slot_.name, sizeof(slot_.name), "%s", tr(STR_DEFAULT_SLOT_NAME));
    slot_.daysMask = static_cast<uint8_t>(ReadingPlan::dayBit(1) | ReadingPlan::dayBit(2) | ReadingPlan::dayBit(3) |
                                          ReadingPlan::dayBit(4) | ReadingPlan::dayBit(5));
    slot_.startMin = 21 * 60;
    slot_.endMin = 22 * 60;
    slot_.flexible = false;
  }

  requestUpdate();
}

void FreeTimeEditActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    saveAndFinish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int rows = rowCount();
  buttonNavigator.onNext([this, rows] {
    selectedRow_ = (selectedRow_ + 1) % rows;
    errorId_ = StrId::STR_NONE_OPT;
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, rows] {
    selectedRow_ = (selectedRow_ + rows - 1) % rows;
    errorId_ = StrId::STR_NONE_OPT;
    requestUpdate();
  });
}

void FreeTimeEditActivity::handleSelection() {
  if (selectedRow_ == ROW_NAME) {
    auto handler = [this](const ActivityResult& result) {
      if (result.isCancelled) return;
      const auto* keyboard = std::get_if<KeyboardResult>(&result.data);
      if (keyboard == nullptr) return;
      snprintf(slot_.name, sizeof(slot_.name), "%s", keyboard->text.c_str());
      dirty_ = true;
      requestUpdate();
    };
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SLOT_NAME), slot_.name,
                                                                   ReadingPlan::NAME_CAP - 1, InputType::Text),
                           handler);
  } else if (selectedRow_ >= ROW_FIRST_DAY && selectedRow_ < ROW_START) {
    slot_.daysMask ^= ReadingPlan::dayBit(weekdayForRow(selectedRow_));
    dirty_ = true;
    requestUpdate();
  } else if (selectedRow_ == ROW_START) {
    editTime(true);
  } else if (selectedRow_ == ROW_END) {
    editTime(false);
  } else if (selectedRow_ == ROW_FLEXIBLE) {
    slot_.flexible = !slot_.flexible;
    dirty_ = true;
    requestUpdate();
  } else if (selectedRow_ == ROW_DELETE && !isNew_) {
    deleteSlot();
  }
}

void FreeTimeEditActivity::editTime(const bool start) {
  char initial[8];
  ReadingPlan::formatHhMm(initial, sizeof(initial), start ? slot_.startMin : slot_.endMin);

  auto handler = [this, start](const ActivityResult& result) {
    if (result.isCancelled) return;
    const auto* keyboard = std::get_if<KeyboardResult>(&result.data);
    if (keyboard == nullptr) return;
    uint16_t minutes = 0;
    if (!ReadingPlan::parseHhMm(keyboard->text.c_str(), minutes)) {
      errorId_ = StrId::STR_INVALID_TIME;
      requestUpdate();
      return;
    }
    if (start) {
      slot_.startMin = minutes;
    } else {
      slot_.endMin = minutes;
    }
    dirty_ = true;
    requestUpdate();
  };

  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, start ? tr(STR_SLOT_START) : tr(STR_SLOT_END),
                                              initial, 5, InputType::Text),
      handler);
}

void FreeTimeEditActivity::saveAndFinish() {
  if (isNew_ && !dirty_) {
    finish();
    return;
  }
  if (ReadingPlan::dayCount(slot_.daysMask) == 0) {
    errorId_ = StrId::STR_INVALID_DAYS;
    requestUpdate();
    return;
  }
  if (slot_.endMin <= slot_.startMin) {
    errorId_ = StrId::STR_INVALID_TIME;
    requestUpdate();
    return;
  }

  char iso[ReadingPlan::ISO_CAP] = {};
  PlanTime::nowIso(iso, sizeof(iso));
  const bool saved = isNew_ ? (PLAN_STORE.addSlot(slot_, iso) >= 0) : PLAN_STORE.updateSlot(slotIndex_, slot_, iso);
  if (!saved) {
    LOG_ERR("PLAN", "slot save failed (index %d, new=%d)", slotIndex_, isNew_ ? 1 : 0);
    errorId_ = StrId::STR_ERROR_GENERAL_FAILURE;
    requestUpdate();
    return;
  }
  finish();
}

void FreeTimeEditActivity::deleteSlot() {
  auto handler = [this](const ActivityResult& result) {
    if (result.isCancelled) {
      requestUpdate();
      return;
    }
    char iso[ReadingPlan::ISO_CAP] = {};
    PlanTime::nowIso(iso, sizeof(iso));
    if (!PLAN_STORE.removeSlot(slotIndex_, iso)) {
      LOG_ERR("PLAN", "slot delete failed (index %d)", slotIndex_);
      errorId_ = StrId::STR_ERROR_GENERAL_FAILURE;
      requestUpdate();
      return;
    }
    finish();
  };
  const std::string body = slot_.name[0] != '\0' ? std::string(slot_.name) : std::string(tr(STR_NOT_SET));
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE), body), handler);
}

void FreeTimeEditActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 isNew_ ? tr(STR_NEW_SLOT) : tr(STR_FREE_TIME));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const int rows = rowCount();

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, rows, selectedRow_,
      [](int index) -> std::string {
        if (index == ROW_NAME) return std::string(tr(STR_SLOT_NAME));
        if (index >= ROW_FIRST_DAY && index < ROW_START) {
          return std::string(I18N.get(WEEKDAY_LABELS[weekdayForRow(index)]));
        }
        if (index == ROW_START) return std::string(tr(STR_SLOT_START));
        if (index == ROW_END) return std::string(tr(STR_SLOT_END));
        if (index == ROW_FLEXIBLE) return std::string(tr(STR_SLOT_FLEXIBLE));
        return std::string(tr(STR_DELETE));
      },
      nullptr, nullptr,
      [this](int index) -> std::string {
        if (index == ROW_NAME) {
          return slot_.name[0] != '\0' ? std::string(slot_.name) : std::string(tr(STR_NOT_SET));
        }
        if (index >= ROW_FIRST_DAY && index < ROW_START) {
          return std::string(I18N.get(ReadingPlan::daySelected(slot_.daysMask, weekdayForRow(index)) ? StrId::STR_ON
                                                                                                     : StrId::STR_OFF));
        }
        if (index == ROW_START || index == ROW_END) {
          char value[8];
          ReadingPlan::formatHhMm(value, sizeof(value), index == ROW_START ? slot_.startMin : slot_.endMin);
          return value;
        }
        if (index == ROW_FLEXIBLE) {
          return std::string(I18N.get(slot_.flexible ? StrId::STR_ON : StrId::STR_OFF));
        }
        return std::string();
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_SAVE), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (errorId_ != StrId::STR_NONE_OPT) {
    GUI.drawPopup(renderer, I18N.get(errorId_));
  }

  renderer.displayBuffer();
}
