#pragma once

#include <Arduino.h>
#include <InputManager.h>

#if defined(ONEPAGE_S3)
// OnePage V3.1 pin map (ESP32-S3R8) — docs/SCH_Sch_OnePage_V3_1_2026-11-11.pdf
// Display SPI (FSPI)
#define EPD_SCLK 39  // SPI Clock
#define EPD_MOSI 38  // SPI MOSI
#define EPD_CS 40    // Chip Select (EPD)
#define EPD_DC 41    // Data/Command
#define EPD_RST 42   // Reset (panel reset, not a power rail on this board)
#define EPD_BUSY 45  // Busy (strapping pin, panel drives it)
#define SPI_MISO -1  // EPD is write-only; SD uses the separate HSPI bus

#define BAT_GPIO0 2   // Battery voltage (GPIO2 = ADC1_CH1, 5.1M/5.1M divider -> x2)
#define BAT_CHG_EN 5  // MSTP4056H CE; low = pause charging while sampling BAT_ADC

// SD card, SPI mode on HSPI (the board is wired 4-bit SDMMC, but SdFat has no
// native ESP32 SDIO driver). In SPI mode: CS=D3, MOSI=CMD, MISO=D0, SCLK.
#define SD_CS 8
#define SD_SCLK 12
#define SD_MOSI 9
#define SD_MISO 11
#define SD_EN 14  // MT9700 load switch: high = +SD3V3 on

#define UART0_RXD 13  // USB_DET (LM66200 ST, open-drain: low = USB present)

#define POWER_HOLD 18  // Soft power latch: drive high ASAP and keep high

#define I2C_SDA 16  // SHT30 (0x44) + PCF8563 RTC (0x51)
#define I2C_SCL 17
#define S3_I2C_SDA I2C_SDA
#define S3_I2C_SCL I2C_SCL
#define S3_I2C_FREQ 400000

#define PDM_CLK 3  // Microphone PDM clock (MCU output)
#define PDM_DIN 4  // Microphone PDM data (MCU input)

#define AUDIO_EN 43     // Q2 amp-ground switch: high = MAX98357A powered
#define AUDIO_BCLK 44   // I2S bit clock
#define AUDIO_LRCLK 46  // I2S word select (strapping pin: keep low/floating)
#define AUDIO_DIN 47    // I2S data to MAX98357A

#define RTC_IRQ 48  // PCF8563 /INT (open-drain, active low)

#else
// Display SPI pins — OnePage ESP32-C61 board (see moink-esp32c61 docs/hardware_io.md)
#define EPD_SCLK 22  // SPI Clock
#define EPD_MOSI 23  // SPI MOSI (Master Out Slave In)
#define EPD_CS 25    // Chip Select (EPD)
#define EPD_DC 8     // Data/Command
#define EPD_RST 27   // Reset (also SD/MIC power-enable, high = active)
#define EPD_BUSY 29  // Busy

#define SPI_MISO 24  // SPI MISO, shared between SD card and display (Master In Slave Out)

#define BAT_GPIO0 5  // Battery voltage (GPIO5 = ADC1_CH3)

#define UART0_RXD 11  // USB connection detection (LM66200 ST, open-drain: low = USB present)

#ifdef ONEPAGE_C61
// OnePage-only pins for deep-sleep power gating and charging control
// (see moink-esp32c61 docs/hardware_io.md §2/§6/§10). GPIO27 (EPD_RST) is the
// shared SD/MIC/EPD power-enable; before cutting it at sleep the shared SPI and
// PDM lines must be silenced to avoid back-powering through ESD diodes.
#define SD_CS 26       // SD chip select (shares the EPD SPI bus)
#define PDM_CLK 7      // Microphone PDM clock (MCU output)
#define PDM_DIN 3      // Microphone PDM data (MCU input)
#define BAT_CHG_EN 10  // Charging enable; drive LOW to pause charging while reading BAT_ADC
#endif
#endif  // ONEPAGE_S3

// Xteink X3 Hardware
#define X3_I2C_SDA 20
#define X3_I2C_SCL 0
#define X3_I2C_FREQ 400000

// TI BQ27220 Fuel gauge I2C
#define I2C_ADDR_BQ27220 0x55  // Fuel gauge I2C address
#define BQ27220_SOC_REG 0x2C   // StateOfCharge() command code (%)
#define BQ27220_CUR_REG 0x0C   // Current() command code (signed mA)
#define BQ27220_VOLT_REG 0x08  // Voltage() command code (mV)

// Analog DS3231 RTC I2C
#define I2C_ADDR_DS3231 0x68  // RTC I2C address
#define DS3231_SEC_REG 0x00   // Seconds command code (BCD)

// PCF8563 RTC I2C (OnePage V3.1)
#define I2C_ADDR_PCF8563 0x51  // RTC I2C address
// SHT30 temperature/humidity sensor I2C (OnePage V3.1)
#define I2C_ADDR_SHT30 0x44

// QST QMI8658 IMU I2C
#define I2C_ADDR_QMI8658 0x6B        // IMU I2C address
#define I2C_ADDR_QMI8658_ALT 0x6A    // IMU I2C fallback address
#define QMI8658_WHO_AM_I_REG 0x00    // WHO_AM_I command code
#define QMI8658_WHO_AM_I_VALUE 0x05  // WHO_AM_I expected value

// MEMSensing MSA311 accelerometer I2C (OnePage V3.1 U19, silkscreen "MSA321")
#define I2C_ADDR_MSA311 0x62      // MSA311 I2C address
#define I2C_ADDR_MSA311_ALT 0x63  // Alternate SDO-pulled-high address
#define MSA311_PARTID_REG 0x01
#define MSA311_PARTID_VALUE 0x13

class HalGPIO {
#if CROSSPOINT_EMULATED == 0
  InputManager inputMgr;
#endif

  bool lastUsbConnected = false;
  bool usbStateChanged = false;

 public:
  enum class DeviceType : uint8_t { X4, X3 };

 private:
  DeviceType _deviceType = DeviceType::X4;

 public:
  HalGPIO() = default;

  // Inline device type helpers for cleaner downstream checks
  inline bool deviceIsX3() const { return _deviceType == DeviceType::X3; }
  inline bool deviceIsX4() const { return _deviceType == DeviceType::X4; }

  // Start button GPIO and setup SPI for screen and SD card
  void begin();

  // Button input methods
  void update();
  bool isPressed(uint8_t buttonIndex) const;
  bool wasPressed(uint8_t buttonIndex) const;
  // Inject a synthetic "just pressed" event for one button this frame (used by
  // the BLE page-turner). Passthrough to InputManager::injectPressedEvents;
  // must be called from the main-loop task right after update().
  void injectPress(uint8_t buttonIndex);
  // Synthetic press held for durationMs (debug: long-press/repeat testing via
  // CMD:KEYHOLD). Passthrough to InputManager::injectHoldEvents; same context
  // rules as injectPress — main-loop task only.
  void injectHold(uint8_t buttonIndex, uint32_t durationMs);
  // Sample hardware and latch physical press edges while the main loop is
  // blocked (serial SCREENSHOT dump). update() delivers them on the next
  // frame. Main-loop task only.
  void pollWhileBlocked();
  bool wasAnyPressed() const;
  bool wasReleased(uint8_t buttonIndex) const;
  bool wasAnyReleased() const;
  unsigned long getHeldTime() const;
  unsigned long getPowerButtonHeldTime() const;

  // Setup wake up GPIO and enter deep sleep
  void startDeepSleep();

  // OnePage: silence the shared SPI/PDM lines and cut the SD/MIC/EPD power rail
  // (GPIO27) before deep sleep, so those peripherals don't keep drawing current
  // and can't back-power the cut rail through their ESD diodes. No-op elsewhere.
  // Call immediately before esp_deep_sleep_start(), after the EPD is parked.
  void powerDownPeripheralsForDeepSleep();

  // Verify power button was held long enough after wakeup.
  // If verification fails, enters deep sleep and does not return.
  // Should only be called when wakeup reason is PowerButton.
  void verifyPowerButtonWakeup(uint16_t requiredDurationMs, bool shortPressAllowed);

  // Check if USB is connected
  bool isUsbConnected() const;

  // Returns true once per edge (plug or unplug) since the last update()
  bool wasUsbStateChanged() const;

  enum class WakeupReason { PowerButton, AfterFlash, AfterUSBPower, Other };

  WakeupReason getWakeupReason() const;

  // Button indices
  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;
};

extern HalGPIO gpio;
