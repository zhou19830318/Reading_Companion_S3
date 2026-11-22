#include "HalPowerManager.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#if defined(ONEPAGE_S3)
#include <driver/rtc_io.h>
#endif

#include <cassert>

#include "HalGPIO.h"

HalPowerManager powerManager;  // Singleton instance

volatile bool HalPowerManager::bleActive_ = false;

void HalPowerManager::begin() {
#if defined(ONEPAGE_S3)
  // V3.1: no fuel gauge; battery is a 5.1M/5.1M divider on GPIO2. The same I2C
  // bus carries the PCF8563 RTC and the SHT30, so initialise it here (after
  // gpio.begin(), which has already released any deep-sleep pad holds).
  pinMode(BAT_GPIO0, INPUT);
  pinMode(BAT_CHG_EN, OUTPUT);
  digitalWrite(BAT_CHG_EN, HIGH);  // CE active-low: charging enabled
  Wire.begin(S3_I2C_SDA, S3_I2C_SCL, S3_I2C_FREQ);
  Wire.setTimeOut(4);
  _batteryUseI2C = false;
#else
  if (gpio.deviceIsX3()) {
    // X3 uses an I2C fuel gauge for battery monitoring.
    // I2C init must come AFTER gpio.begin() so early hardware detection/probes are finished.
    Wire.begin(X3_I2C_SDA, X3_I2C_SCL, X3_I2C_FREQ);
    Wire.setTimeOut(4);
    _batteryUseI2C = true;
  } else {
    pinMode(BAT_GPIO0, INPUT);
  }
#endif
  normalFreq = getCpuFrequencyMhz();
  currentSetFreq = normalFreq;
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return;  // invalid state
  }

  auto wifiMode = WiFi.getMode();
  if (wifiMode != WIFI_MODE_NULL) {
    // Wifi is active, force disabling power saving
    enabled = false;
  }

  // The whole decide-and-switch sequence must be atomic against other tasks:
  // the render task's Lock raises the frequency while the main loop's idle tick
  // lowers it. Concurrent setCpuFrequencyMhz() calls (or a stale "go idle"
  // decision landing after a Lock) reconfigure SPI/PSRAM clocks mid-transaction
  // and wedge the render task in an SD/cache wait forever (observed: TWDT abort
  // with both cores IDLE right after a 10 MHz -> 240 MHz wake).
  xSemaphoreTake(modeMutex, portMAX_DELAY);
  const bool locked = lockCount > 0;
  const bool ble = bleActive_;

  // Decide the target CPU frequency by priority:
  //   Lock (perf-critical task) / WiFi  -> full speed (normalFreq)
  //   BLE link up (no WiFi/lock)         -> BLE_ACTIVE_FREQ (80 MHz): low enough
  //                                         to cut CPU power, high enough to keep
  //                                         the BLE radio timing stable (10 MHz
  //                                         trips the interrupt watchdog).
  //   idle power-saving requested        -> LOW_POWER_FREQ (10 MHz)
  //   otherwise                          -> full speed
  int targetFreq;
  if (locked || !enabled) {
    // A lock is held, or the caller does not want power saving right now.
    // BLE still gets its 80 MHz clamp (a link being up is not "full speed"
    // work), but a NormalSpeed lock or WiFi overrides everything.
    targetFreq = (ble && !locked && wifiMode == WIFI_MODE_NULL) ? BLE_ACTIVE_FREQ : normalFreq;
  } else {
    // enabled && no lock: idle. Drop to 10 MHz, unless a BLE link forbids it.
    targetFreq = ble ? BLE_ACTIVE_FREQ : LOW_POWER_FREQ;
  }

  const int currentFreq = currentSetFreq > 0 ? currentSetFreq : normalFreq;
  if (targetFreq == currentFreq) {
    xSemaphoreGive(modeMutex);
    return;  // no change needed
  }

  LOG_DBG("PWR", "CPU frequency -> %d MHz", targetFreq);
  if (!setCpuFrequencyMhz(targetFreq)) {
    LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", targetFreq);
    xSemaphoreGive(modeMutex);
    return;
  }
  currentSetFreq = targetFreq;
  xSemaphoreGive(modeMutex);
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio) const {
  // Ensure that the power button has been released to avoid immediately turning back on if you're holding it
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }

#ifdef ENABLE_SERIAL_LOG
  // Tear down HWCDC so the host sees a clean disconnect and the peripheral
  // doesn't hold power domains that interfere with USB-powered GPIO wake.
  // logSerial is the raw HWCDC reference; Serial is the MySerialImpl proxy
  // (which doesn't expose end()).
  logSerial.end();
#endif

  // OnePage: cut the SD/MIC/EPD power rail (GPIO27) and silence the shared
  // SPI/PDM lines so those peripherals stop drawing current in deep sleep and
  // can't back-power the cut rail. No-op on X4/X3. The EPD was already parked
  // by display.deepSleep() before this call (see main.cpp goToDeepSleep).
  gpio.powerDownPeripheralsForDeepSleep();

  // Pre-sleep routines from the original firmware
  // GPIO13 is connected to battery latch MOSFET, we need to make sure it's low during sleep
  // Note that this means the MCU will be completely powered off during sleep, including RTC
#if !defined(ONEPAGE_C61) && !defined(ONEPAGE_S3)
  // GPIO13 is the X4 battery-latch MOSFET. On OnePage GPIO13 is USB_DET, so
  // driving it low would fight the LM66200 — skip this whole block there.
  constexpr gpio_num_t GPIO_SPIWP = GPIO_NUM_13;
  gpio_set_direction(GPIO_SPIWP, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_SPIWP, 0);
  esp_sleep_config_gpio_isolate();
  gpio_deep_sleep_hold_en();
  gpio_hold_en(GPIO_SPIWP);
#endif
  // Arm the wakeup trigger *after* the button is released
  // Note: this is only useful for waking up on USB power. On battery, the MCU will be completely powered off, so the
  // power button is hard-wired to briefly provide power to the MCU, waking it up regardless of the wakeup source
  // configuration
#if defined(ONEPAGE_S3)
  pinMode(InputManager::POWER_BUTTON_PIN, INPUT_PULLUP);
  pinMode(InputManager::BTN_UP_PIN, INPUT_PULLUP);
  pinMode(InputManager::BTN_DOWN_PIN, INPUT_PULLUP);
  rtc_gpio_pullup_en(static_cast<gpio_num_t>(InputManager::POWER_BUTTON_PIN));
  rtc_gpio_pulldown_dis(static_cast<gpio_num_t>(InputManager::POWER_BUTTON_PIN));
  constexpr uint64_t wakeMask = (1ULL << InputManager::POWER_BUTTON_PIN) | (1ULL << InputManager::BTN_UP_PIN) |
                                (1ULL << InputManager::BTN_DOWN_PIN);
  esp_sleep_enable_ext1_wakeup(wakeMask, ESP_EXT1_WAKEUP_ANY_LOW);
#else
  pinMode(InputManager::POWER_BUTTON_PIN, INPUT_PULLUP);
  esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
#endif
  // Enter Deep Sleep
  esp_deep_sleep_start();
}

uint16_t HalPowerManager::getBatteryPercentage() const {
  const unsigned long now = millis();
  // Both backends block: the I2C branch runs a Wire transaction, the ADC branch
  // pauses charging and delay(5)s for the terminal voltage to settle. A status
  // bar draw calls this twice (BaseTheme.cpp:828 for layout width, then again
  // inside drawBatteryLeft/drawBatteryRight), so share the I2C branch's poll
  // window across both backends rather than sampling once per call site.
  if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
    return _batteryReturnedPercent;
  }

  if (_batteryUseI2C) {
    // Read SOC directly from I2C fuel gauge (16-bit LE register).
    // On I2C error, keep last known value to avoid UI jitter/slowdowns.
    Wire.beginTransmission(I2C_ADDR_BQ27220);
    Wire.write(BQ27220_SOC_REG);
    if (Wire.endTransmission(false) != 0) {
      _batteryLastPollMs = now;
      _batteryReturnedPercent = _batteryCachedPercent;
      return _batteryCachedPercent;
    }
    Wire.requestFrom(I2C_ADDR_BQ27220, (uint8_t)2);
    if (Wire.available() < 2) {
      _batteryLastPollMs = now;
      _batteryReturnedPercent = _batteryCachedPercent;
      return _batteryCachedPercent;
    }
    const uint8_t lo = Wire.read();
    const uint8_t hi = Wire.read();
    const uint16_t soc = (hi << 8) | lo;
    _batteryCachedPercent = soc > 100 ? 100 : soc;
    _batteryLastPollMs = now;
    _batteryReturnedPercent = _batteryCachedPercent;
    return _batteryCachedPercent;
  }
  static const BatteryMonitor battery = BatteryMonitor(BAT_GPIO0);

#if defined(ONEPAGE_C61) || defined(ONEPAGE_S3)
  // Pause charging while sampling so the charger's terminal voltage doesn't
  // inflate the reading (V3.1: BAT_PWR_SW=GPIO5 to MSTP4056H CE, low=paused).
  pinMode(BAT_CHG_EN, OUTPUT);
  digitalWrite(BAT_CHG_EN, LOW);
  delay(5);  // let the battery terminal voltage settle
  const uint16_t mv = battery.readMillivolts();
  digitalWrite(BAT_CHG_EN, HIGH);  // resume charging
  const uint16_t pct = BatteryMonitor::percentageFromMillivolts(mv);
#else
  const uint16_t pct = battery.readPercentage();
#endif

  // smooth the battery %.
  if (_batteryCachedPercent == 0) {
    _batteryCachedPercent = 10 * pct;
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + pct * 10) / 10;
  }
  _batteryLastPollMs = now;
  _batteryReturnedPercent = _batteryCachedPercent / 10;
  return _batteryReturnedPercent;
}

HalPowerManager::Lock::Lock() {
  // A frequency change can wait on a busy SPI/SD transaction elsewhere, so a
  // subscribed task (the render task) must keep feeding the watchdog while it
  // waits for the mode mutex.
  while (xSemaphoreTake(powerManager.modeMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
    if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset();
  }
  const bool wasIdle = powerManager.lockCount == 0;
  if (powerManager.lockCount < 255) powerManager.lockCount++;
  valid = true;
  xSemaphoreGive(powerManager.modeMutex);
  if (wasIdle) {
    // Immediately restore normal CPU frequency if currently in low-power mode.
    powerManager.setPowerSaving(false);
  }
}

HalPowerManager::Lock::~Lock() {
  while (xSemaphoreTake(powerManager.modeMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
    if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset();
  }
  if (valid && powerManager.lockCount > 0) powerManager.lockCount--;
  xSemaphoreGive(powerManager.modeMutex);
}
