#pragma once

#include "components/themes/lyra/LyraTheme.h"

namespace BaizeMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 16,
                                 .batteryHeight = 12,
                                 .topPadding = 5,
                                 .batteryBarHeight = 40,
                                 .headerHeight = 84,
                                 .verticalSpacing = 16,
                                 .previewPadding = 12,
                                 .previewHeightPercent = 30,
                                 .contentSidePadding = 20,
                                 .listRowHeight = 60,
                                 .listWithSubtitleRowHeight = 88,
                                 .menuRowHeight = 64,
                                 .menuSpacing = 8,
                                 .tabSpacing = 8,
                                 .tabBarHeight = 40,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 // Home greeting block: two title lines, a greeting,
                                 // a two-line tagline and the mascot all fit here.
                                 .homeTopPadding = 176,
                                 .homeCoverHeight = 226,
                                 .homeCoverTileHeight = 242,
                                 .homeRecentBooksCount = 1,
                                 .homeContinueReadingInMenu = false,
                                 .homeMenuTopOffset = 16,
                                 .buttonHintsHeight = 40,
                                 .sideButtonHintsWidth = 30,
                                 .progressBarHeight = 16,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 19,
                                 .keyboardKeyWidth = 31,
                                 .keyboardKeyHeight = 40,
                                 .keyboardKeySpacing = 0,
                                 .keyboardBottomKeyHeight = 35,
                                 .keyboardBottomKeySpacing = 5,
                                 .keyboardBottomAligned = true,
                                 .keyboardCenteredText = false,
                                 .keyboardVerticalOffset = -7,
                                 .keyboardTextFieldWidthPercent = 85,
                                 .keyboardWidthPercent = 90,
                                 .keyboardKeyCornerRadius = 10,
                                 .keyboardFillUnselected = false,
                                 .keyboardOutlineAllUnselected = true,
                                 .keyboardDrawSpecialOutlineWhenUnselected = true,
                                 .keyboardSecondaryLabelRightPadding = 1,
                                 .keyboardSecondaryLabelTopPadding = 0,
                                 .keyboardMinArrowHeadSize = 0,
                                 .popupTopOffsetRatio = 0.165f,
                                 .popupMarginX = 16,
                                 .popupMarginY = 12,
                                 .popupFrameThickness = 2,
                                 .popupCornerRadius = 16,
                                 .popupTextBold = false,
                                 .popupTextInverted = false,
                                 .popupTextBaselineOffsetY = -2,
                                 .popupProgressBarHeight = 4,
                                 .popupProgressDrawOutline = false,
                                 .popupProgressClampPercent = false,
                                 .popupProgressFillInverted = false,
                                 .popupProgressOutlineInverted = false,
                                 .textFieldHorizontalPadding = 6,
                                 .textFieldNormalThickness = 1,
                                 .textFieldCursorThickness = 3,
                                 .textFieldLineEndOffset = 0};
}

// Paper cards, soft outlines and companion badges shared by every device screen.
// Keep the reader's chosen body font and the existing cover-buffer lifecycle.
class BaizeTheme : public LyraTheme {
 public:
  bool drawHomeGreeting(const GfxRenderer& renderer, Rect rect, bool hasRecentBook) const override;
  int preferredHomeColumns() const override { return 4; }
  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title,
                  const char* subtitle = nullptr) const override;
  void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                     const char* rightLabel = nullptr) const override;
  void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                  bool selected) const override;
  int getListPageItems(int contentHeight, bool hasSubtitle) const override;
  void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                const std::function<std::string(int)>& rowTitle,
                const std::function<std::string(int)>& rowSubtitle = nullptr,
                const std::function<UIIcon(int)>& rowIcon = nullptr,
                const std::function<std::string(int)>& rowValue = nullptr, bool highlightValue = false,
                const std::function<bool(int)>& rowDimmed = nullptr) const override;
  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                       const char* btn4) const override;
  void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const override;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int)>& buttonLabel,
                      const std::function<UIIcon(int)>& rowIcon) const override;
  void drawHomeIconGrid(GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex, int columns,
                        const std::function<std::string(int)>& tileLabel,
                        const std::function<UIIcon(int)>& tileIcon) const override;
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int previewIndex, bool previewSelected, bool& coverRendered, bool& coverBufferStored,
                           bool& bufferRestored, std::function<bool()> storeCoverBuffer) const override;
  void drawChoiceCards(GfxRenderer& renderer, Rect rect, int cardCount, int selectedIndex,
                       const ChoiceCard* cards) const override;
  Rect drawPopup(const GfxRenderer& renderer, const char* message, bool withProgressBar = false,
                 bool display = true) const override;
  void fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, int progress, bool showPercentLabel = false,
                         bool display = true) const override;
  void drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total) const override;
  void drawSurface(const GfxRenderer& renderer, Rect rect, bool selected = false) const override;
  bool drawSystemMessage(const GfxRenderer& renderer, Rect rect, const char* title, const char* body) const override;
  int drawReaderDecoration(const GfxRenderer& renderer, int x, int y) const override;
  void drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label) const override;
  void drawTextField(const GfxRenderer& renderer, Rect rect, int textWidth, bool cursorMode = false,
                     int contentStartX = 0, int contentWidth = 0) const override;
};
