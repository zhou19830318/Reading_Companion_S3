#include "HardwareSelfTest.h"

#include <Arduino.h>
#include <HalAccelSensor.h>
#include <HalAudio.h>
#include <HalClock.h>
#include <HalEnvSensor.h>
#include <HalPowerManager.h>
#include <Logging.h>

#include <cmath>

#if defined(ONEPAGE_S3)
#include <HalMicrophone.h>
#include <Recorder.h>
#endif

namespace {
void logAccelSample() {
  float x = 0;
  float y = 0;
  float z = 0;
  if (!ACCEL.read(x, y, z)) {
    LOG_ERR("HW", "ACCEL read failed");
    return;
  }
  LOG_INF("HW", "ACCEL: X=%d mg Y=%d mg Z=%d mg", static_cast<int>(lroundf(x * 1000.0f)),
          static_cast<int>(lroundf(y * 1000.0f)), static_cast<int>(lroundf(z * 1000.0f)));
}
}  // namespace

void runSensorReport() {
  float t = 0;
  float h = 0;
  if (ENV_SENSOR.read(t, h)) {
    LOG_INF("HW", "SHT30: %d.%d C, %d %%RH", static_cast<int>(t), static_cast<int>(fabsf(t) * 10) % 10,
            static_cast<int>(h));
  } else {
    LOG_ERR("HW", "SHT30 read failed");
  }

  uint8_t hour = 0;
  uint8_t minute = 0;
  if (halClock.getTime(hour, minute)) {
    LOG_INF("HW", "RTC: %02u:%02u", hour, minute);
  } else if (halClock.isAvailable()) {
    LOG_INF("HW", "RTC present (clock not set)");
  } else {
    LOG_ERR("HW", "RTC not available");
  }

  if (!ACCEL.isAvailable() && !ACCEL.begin()) {
    LOG_ERR("HW", "ACCEL not available");
    return;
  }
  logAccelSample();
}

void runAccelReport() {
  if (!ACCEL.isAvailable() && !ACCEL.begin()) {
    LOG_ERR("HW", "ACCEL not available");
    return;
  }
  for (int i = 0; i < 10; i++) {
    logAccelSample();
    delay(100);
  }
}

bool runAudioLoopback(uint32_t seconds) {
#if defined(ONEPAGE_S3)
  HalPowerManager::Lock powerLock;
  Voice::Recorder recorder;
  recorder.setVadThreshold(0);
  LOG_INF("HW", "MIC: starting I2S capture");
  if (!recorder.begin(HalMicrophone::SAMPLE_RATE, seconds)) {
    LOG_ERR("HW", "MIC begin failed (%s)", recorder.error() == Voice::Recorder::Error::Alloc ? "alloc" : "unavailable");
    return false;
  }

  LOG_INF("HW", "MIC: recording %lu s, speak now", static_cast<unsigned long>(seconds));
  while (true) {
    const Voice::Recorder::Event event = recorder.poll();
    if (event == Voice::Recorder::Event::Recording) {
      delay(5);
      continue;
    }
    if (event == Voice::Recorder::Event::ReadFailed) {
      LOG_ERR("HW", "MIC read failed");
    } else if (event == Voice::Recorder::Event::NoAudio) {
      LOG_ERR("HW", "MIC produced no samples");
    }
    break;
  }

  if (recorder.samples() == 0) {
    LOG_ERR("HW", "MIC captured no samples");
    return false;
  }
  LOG_INF("HW", "MIC: rms=%ld peak=%ld dc=%ld samples=%u", static_cast<long>(recorder.rms()),
          static_cast<long>(recorder.peak()), static_cast<long>(recorder.dcOffset()),
          static_cast<unsigned>(recorder.samples()));

  if (!AUDIO.begin()) {
    LOG_ERR("HW", "AUDIO begin failed");
    return false;
  }

  AUDIO.setEnabled(true);
  delay(5);

  bool ok = true;
  constexpr size_t CHUNK = 1600;
  for (size_t offset = 0; offset < recorder.samples(); offset += CHUNK) {
    const size_t count = (recorder.samples() - offset < CHUNK) ? recorder.samples() - offset : CHUNK;
    if (!AUDIO.writePcm(recorder.pcm() + offset, count)) {
      LOG_ERR("HW", "AUDIO write failed at sample %u", static_cast<unsigned>(offset));
      ok = false;
      break;
    }
  }

  delay(120);  // let the last DMA buffer reach the amplifier
  AUDIO.setEnabled(false);
  AUDIO.end();
  LOG_INF("HW", "AUDIO loopback %s", ok ? "played" : "failed");
  return ok;
#else
  (void)seconds;
  LOG_ERR("HW", "Audio loopback not supported on this target");
  return false;
#endif
}

bool runMaxVolumeTest(uint32_t seconds) {
#if defined(ONEPAGE_S3)
  HalPowerManager::Lock powerLock;
  if (!AUDIO.begin()) {
    LOG_ERR("HW", "AUDIO begin failed");
    return false;
  }

  // 1 kHz at 16 kHz = exactly 16 samples per period. Full-scale sine: this is
  // the loudest signal the DAC can produce; the acoustic ceiling is still set
  // by the MAX98357A GAIN_SLOT strap and the speaker itself.
  static constexpr int16_t SINE_1K[16] = {0, 12526,  23170,  30273,  32767,  30273,  23170,  12526,
                                          0, -12526, -23170, -30273, -32767, -30273, -23170, -12526};
  static int16_t buffer[128];
  size_t phase = 0;
  uint32_t written = 0;
  const uint32_t total = AUDIO.SAMPLE_RATE * seconds;

  LOG_INF("HW", "SPK: max-volume 1 kHz sine, %lu s", static_cast<unsigned long>(seconds));
  AUDIO.setEnabled(true);
  delay(5);

  bool ok = true;
  while (written < total) {
    const size_t count =
        (total - written < sizeof(buffer) / sizeof(buffer[0])) ? total - written : sizeof(buffer) / sizeof(buffer[0]);
    for (size_t i = 0; i < count; i++) {
      buffer[i] = SINE_1K[phase];
      phase = (phase + 1) % (sizeof(SINE_1K) / sizeof(SINE_1K[0]));
    }
    if (!AUDIO.writePcm(buffer, count)) {
      LOG_ERR("HW", "AUDIO max-volume write failed at sample %u", static_cast<unsigned>(written));
      ok = false;
      break;
    }
    written += count;
  }

  delay(150);
  AUDIO.setEnabled(false);
  AUDIO.end();
  LOG_INF("HW", "SPK: max-volume test %s", ok ? "done" : "failed");
  return ok;
#else
  (void)seconds;
  LOG_ERR("HW", "Max-volume test not supported on this target");
  return false;
#endif
}

void runHardwareSelfTest() {
  LOG_INF("HW", "=== hardware self-test ===");
  runSensorReport();

  LOG_INF("HW", "SPK: 1 kHz beep");
  bool beepOk = false;
  {
    HalPowerManager::Lock powerLock;
    beepOk = AUDIO.beep(1000, 300);
  }
  LOG_INF("HW", "SPK: beep returned %d", beepOk ? 1 : 0);
  delay(100);
  runAudioLoopback(2);

  LOG_INF("HW", "=== hardware self-test done ===");
}
