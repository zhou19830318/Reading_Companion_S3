#include "UiSound.h"

#include <HalAudio.h>
#include <HalPowerManager.h>
#include <Logging.h>

#include <optional>

#include "CrossPointSettings.h"

namespace {

// Frequencies chosen to read as "click" rather than "tone": short, high, and
// quiet enough to be unobtrusive in a quiet room.
constexpr uint16_t KEY_HZ = 1250;
constexpr uint16_t KEY_MS = 16;
constexpr uint16_t PAGE_HZ = 880;       // forward page turn (unchanged)
constexpr uint16_t PAGE_BACK_HZ = 660;  // backward page turn
constexpr uint16_t PAGE_MS = 34;
constexpr uint16_t ALERT_HZ = 1800;
constexpr uint16_t ALERT_MS = 120;
constexpr int16_t AMPLITUDE = 5000;
constexpr int16_t ALERT_AMPLITUDE = 6000;

// Master volume for every UI tone, taken from the user's volume setting
// (0-100). 100 = the amplitude these tones were tuned at on the bench unit;
// three hardware listening rounds brought the default down (60 → 40 → 25) as
// each still read as too loud in a quiet room. The MAX98357A has a fixed gain
// (set by its pin strapping), so loudness is the PCM peak here.
int16_t atVolume(int16_t amplitude) { return static_cast<int16_t>(amplitude * SETTINGS.volume / 100); }

// Duplicate press edges from one physical press must not stack tones. The input
// layer already debounces for 5 ms, so 10 ms only collapses edges that belong
// to the same press; a genuine fast double-press (measured 29 ms apart on
// hardware) still gets its second click.
constexpr unsigned long THROTTLE_MS = 10;
// How long the amp stays powered after the last tone so the DMA ring can empty
// before Q2 cuts the ground: cutting it mid-tone is an audible pop.
constexpr unsigned long AMP_TAIL_MS = 120;

bool ampOn = false;
unsigned long lastToneMs = 0;
unsigned long ampOffAtMs = 0;

// Full-speed lock, held from the first tone until the amp tail expires.
//
// Measured on the V3.1 bench unit: with the CPU parked at LOW_POWER_FREQ (10 MHz)
// i2s_channel_enable() never returns — the channel handler prints "enabling
// channel" and the device then goes silent for good (the same call at 240 MHz
// returns immediately). The I2S clock is derived from PLL_160M, which the 10 MHz
// CPU setting does not keep alive; taking the power lock before touching the I2S
// driver, and holding it until the DMA ring has drained, is the same contract the
// microphone and speaker self-tests already follow.
std::optional<HalPowerManager::Lock> powerLock;

void acquireFullSpeed() {
  if (!powerLock) {
    powerLock.emplace();
  }
}

void releaseFullSpeed() {
  if (powerLock) {
    powerLock.reset();
  }
}

// `force` skips the throttle: the page tone is queued in the same frame as the
// press it belongs to (and the generic click for that press was suppressed), so
// the throttle would swallow exactly the sound the user asked for.
void play(uint16_t hz, uint16_t ms, int16_t amplitude, bool force = false) {
  const unsigned long now = millis();
  if (!force && lastToneMs != 0 && now - lastToneMs < THROTTLE_MS) {
    return;
  }
  acquireFullSpeed();
  if (!AUDIO.begin()) {
    releaseFullSpeed();
    return;
  }
  lastToneMs = now;
  if (!ampOn) {
    AUDIO.setEnabled(true);
    ampOn = true;
    delay(2);  // MAX98357A ground-switch / bias settle
  }
  const bool queued = AUDIO.tone(hz, ms, amplitude);
  LOG_DBG("SND", "%u Hz %u ms amp=1 queued=%d", hz, ms, queued ? 1 : 0);
  ampOffAtMs = millis() + ms + AMP_TAIL_MS;
}

}  // namespace

namespace UiSound {

void key() {
  if (!SETTINGS.soundFeedback) {
    return;
  }
  play(KEY_HZ, KEY_MS, atVolume(AMPLITUDE));
}

void onButtonPress(bool pageTurnKey, bool readerActive) {
  if (pageTurnKey && readerActive) {
    return;
  }
  key();
}

void pageTurn(bool forward) {
  if (!SETTINGS.soundFeedback) {
    return;
  }
  play(forward ? PAGE_HZ : PAGE_BACK_HZ, PAGE_MS, atVolume(AMPLITUDE), /*force=*/true);
}

void alert() {
  if (!SETTINGS.soundFeedback) {
    return;
  }
  for (int i = 0; i < 3; i++) {
    play(ALERT_HZ, ALERT_MS, atVolume(ALERT_AMPLITUDE));
    delay(ALERT_MS + 20);
  }
  // The last beep still has to leave the DMA ring before the screen work that
  // follows; poll() would do it eventually, but an alert is the one place a
  // sound must be heard completely.
  AUDIO.drain(ALERT_MS);
}

void poll() {
  if (!ampOn) {
    return;
  }
  if (static_cast<long>(millis() - ampOffAtMs) < 0) {
    return;
  }
  // A stream started while our tail was still running (TTS right after a key
  // press): leave the amp up, the stream owns it now.
  if (AUDIO.isStreamActive()) {
    ampOn = false;
    releaseFullSpeed();
    return;
  }
  AUDIO.setEnabled(false);
  ampOn = false;
  releaseFullSpeed();
  LOG_DBG("SND", "amp off");
}

void shutdown() {
  if (ampOn) {
    AUDIO.setEnabled(false);
    ampOn = false;
  }
  releaseFullSpeed();
  // Release the I2S channel itself: deep sleep is a chip reset, so there is no
  // benefit in keeping DMA descriptors allocated across it.
  if (AUDIO.isReady()) {
    AUDIO.end();
  }
}

}  // namespace UiSound
