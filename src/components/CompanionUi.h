#pragma once

#include "components/themes/BaseTheme.h"

class GfxRenderer;

// Small, theme-agnostic drawing primitives shared by the reading-companion
// screens (Ask AI, voice bookmark, dictionary, review, chapter discussion).
// E-ink constraints: black / white / one light-grey fill, no animation, no
// colour, and every helper sizes itself from the active ThemeMetrics.
namespace CompanionUi {

// Section eyebrow: bold label followed by a hairline rule to the right edge.
// Advances the caller by one line height; returns the consumed height.
int drawSectionLabel(const GfxRenderer& renderer, int x, int y, int width, const char* label);

// A single selectable row: [01] title ............ trailing.
// Selected rows get a light-grey rounded fill and a black leading bar; the
// index square inverts. Unselected rows sit on a hairline baseline.
void drawSelectRow(const GfxRenderer& renderer, Rect rect, int index, const char* title, const char* trailing,
                   bool selected);

// Outlined (or filled) status pill, right-aligned at `right`. Returns width.
int drawChip(const GfxRenderer& renderer, int right, int y, const char* text, bool filled = false);

// Discrete step indicator, current step filled. Used for the 4 chapter
// discussion questions instead of a "1/4" text counter.
void drawStepDots(const GfxRenderer& renderer, int right, int y, int count, int current);

}  // namespace CompanionUi
