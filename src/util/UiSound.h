#pragma once

#include <cstdint>

// Short feedback tones for button presses, page turns and the focus timer.
//
// Product behaviour, not a HAL: it reads SETTINGS and decides when a sound is
// wanted, while HalAudio only knows how to put samples on the wire. Kept out of
// lib/hal so the HAL stays free of settings dependencies.
//
// The amplifier switch (Q2/AUDIO_EN) is the expensive part: toggling it per
// tone both clicks audibly and wastes the settle delay. So a tone powers the amp
// up, queues itself, and leaves the amp on until poll() sees the tail expire —
// one switch per burst instead of one per sound.
namespace UiSound {

// Every button press (called from the main loop's input hook).
void key();

// Main-loop input hook: plays the generic click for a press, except for the
// reader's page-turn keys while a reader is the current activity. Those presses
// get their own sound from pageTurn() at the moment the page actually moves, so
// one press never rings twice (and a press that changes nothing stays silent).
void onButtonPress(bool pageTurnKey, bool readerActive);

// A page push. Played immediately before the panel refresh so the 1-2 s update
// overlaps the sound instead of delaying it. Forward keeps the original tone;
// backward uses a lower one so the direction is audible without looking.
void pageTurn(bool forward = true);

// Focus timer finished: three short beeps. Blocking for roughly 0.8 s, which is
// fine because the alert screen is already blinking.
void alert();

// Release the amp once no tone is playing. Cheap; call once per main-loop pass.
void poll();

// Power the amp down immediately (sleep, shutdown).
void shutdown();

}  // namespace UiSound
