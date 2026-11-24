#pragma once

#include <cstdint>

class GfxRenderer;

// The reading companion ("书灵"): one cartoon mascot shared by the AI screens,
// in a pose per feature and one for waiting/failure. Artwork is generated as
// 1-bit bitmaps by scripts/generate_mascot_icons.py; headers are pre-rotated
// for drawIcon()'s panel mapping.
//
// E-ink constraints: static flash-resident bitmaps (128x128 = 2 KB each, no
// heap), black/white only, no animation. The helpers never draw outside the
// box they are given, so a landscape panel with less room simply gets no
// mascot instead of overlapping text.
namespace CompanionMascot {

enum class Pose : uint8_t {
  Read,    // AI hub, Ask AI: holding an open book
  Chat,    // chat entry: speech bubble
  Listen,  // voice bookmark / speech input: headphones
  Lookup,  // dictionary / word card: magnifier
  Paint,   // image generation: beret and brush
  Think,   // waiting for the relay
  Sad,     // failed or empty state
};

static constexpr int SIZE = 128;

// Paints the mascot with its top-left at (x, y). Returns false when it does
// not fit on the screen. `y` is snapped to a byte boundary because
// drawImageTransparent() addresses the panel in 8-pixel columns.
bool draw(const GfxRenderer& renderer, int x, int y, Pose pose);

// Centres the mascot horizontally in [left, left + width).
bool drawCentered(const GfxRenderer& renderer, int left, int width, int y, Pose pose);

// Mascot plus one centred caption line, centred as a group inside the box.
// Captions longer than the box are cut at a whole-character boundary.
// Returns the height used, or 0 when the box is too small (nothing drawn).
int drawBlock(const GfxRenderer& renderer, int left, int top, int width, int height, Pose pose, const char* caption);

}  // namespace CompanionMascot
