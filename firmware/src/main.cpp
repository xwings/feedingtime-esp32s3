#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <Wire.h>
#include <driver/i2s_std.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <time.h>
#include "config.h"
#include "es8311.h"

// DNESP32S3B (Alientek) hardware
// LCD: ST7789V via parallel 8080 8-bit (LCD_CAM peripheral), 320x240
#define LCD_DC 2
#define LCD_CS 1
#define LCD_WR 42
#define LCD_RD 41
#define LCD_D0 40
#define LCD_D1 39
#define LCD_D2 38
#define LCD_D3 12
#define LCD_D4 11
#define LCD_D5 10
#define LCD_D6 9
#define LCD_D7 46

// I2C bus -> XL9555 IO expander (PCA9555 compatible) + ES8311 audio codec
#define I2C_SDA 48
#define I2C_SCL 45
#define XL9555_ADDR 0x20
#define XL9555_INPUT0 0x00
#define XL9555_OUTPUT0 0x02
#define XL9555_CONFIG0 0x06
#define XL_PA_PIN 5         // P0.5 high enables ES8311 speaker amplifier
#define XL_BACKLIGHT_PIN 7  // P0.7 high enables LCD backlight
#define XL_KEY1_PIN 4       // P0.4 (cycle view / hold = sync)
#define XL_KEY2_PIN 3       // P0.3 (feeding toggle)

// I2S to ES8311 codec (mclk derived from BCLK, no MCLK pin needed)
#define I2S_PIN_BCLK 21
#define I2S_PIN_WS   13
#define I2S_PIN_DOUT 14
#define I2S_SAMPLE_RATE 16000

#define KEY_POLL_MS 60
#define BUTTON_DEBOUNCE_MS 80
#define TIMER_DIGIT_WIDTH 50
#define TIMER_DIGIT_HEIGHT 150
#define TIMER_SEGMENT_THICKNESS 8
#define TIMER_CHAR_GAP 16
#define TIMER_COLON_WIDTH 14
// cubic11_h_cjk is 11px square; at setTextSize(2) each CJK glyph is ~22px wide.
#define CJK_SUBTITLE_GLYPH_PX 22

Arduino_DataBus *bus = new Arduino_ESP32LCD8(LCD_DC, LCD_CS, LCD_WR, LCD_RD,
                                             LCD_D0, LCD_D1, LCD_D2, LCD_D3,
                                             LCD_D4, LCD_D5, LCD_D6, LCD_D7);
Arduino_GFX *panel = new Arduino_ST7789(bus, GFX_NOT_DEFINED, 1, true, 240, 320, 0, 0, 0, 0);
Arduino_Canvas *gfx = new Arduino_Canvas(320, 240, panel, 0, 0, 0);

uint32_t lastCounterDrawMs = 0;
uint32_t lastClockDrawMs = 0;
bool feedingActive = false;
bool xl9555Ready = false;

struct ButtonState {
  bool pressed = false;
  uint32_t lastChangeMs = 0;
  uint32_t pressStartMs = 0;
  bool longFired = false;
};
ButtonState k1Button;
ButtonState k2Button;
uint32_t lastKeyPollMs = 0;
bool k1IsPressed = false;
bool k2IsPressed = false;

enum ViewMode { VIEW_CLOCK = 0, VIEW_HISTORY = 1, VIEW_COUNTER = 2 };
ViewMode currentView = VIEW_CLOCK;
static const uint32_t K1_LONG_PRESS_MS = 1500;

struct FeedSession {
  time_t startEpoch = 0;
  time_t stopEpoch = 0;
};
static const size_t HISTORY_SIZE = 8;
FeedSession feedHistory[HISTORY_SIZE];
size_t feedHistoryCount = 0;
size_t feedHistoryHead = 0;

struct ActiveCounter {
  bool active = false;
  String title;
  String subtitle;
  uint32_t baseElapsedSeconds = 0;
  uint32_t startedAtMs = 0;
  // Identifies the underlying session — start_epoch when feeding, stop_epoch
  // when "Last fed". Used by the alarm to tell "same session, already
  // dismissed" from "new session, re-arm".
  time_t sessionEpoch = 0;
} activeCounter;

// Alarm state. Fires once per "Last fed" session at ALARM_MINUTES, then stays
// silent until either K1 dismiss or a new session begins.
bool alarmFiring = false;
time_t alarmDismissedForEpoch = 0;
uint32_t alarmLastToggleMs = 0;
uint32_t alarmFiringStartMs = 0;
bool alarmToneOn = false;

// Gateway client state
struct PendingEvent {
  char type[8];
  time_t epoch;
};
static const size_t PENDING_QUEUE_SIZE = 16;
PendingEvent pendingQueue[PENDING_QUEUE_SIZE];
size_t pendingCount = 0;
SemaphoreHandle_t stateMutex = nullptr;
volatile bool gatewayStateDirty = false;
volatile bool gatewayOnline = true;

inline bool gatewayMode() { return GATEWAY_URL[0] != '\0'; }

bool xl9555WriteReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(XL9555_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool xl9555EnableBacklight() {
  // P0.7 = LCD backlight enable. P0.5 = audio amp PA enable — only powered
  // when ALARM_VOLUME > 0; otherwise the amp stays off so the speaker can't
  // hum or pop on idle.
  uint8_t mask = (1 << XL_BACKLIGHT_PIN);
#if ALARM_VOLUME != 0
  mask |= (1 << XL_PA_PIN);
#endif
  if (!xl9555WriteReg(XL9555_OUTPUT0, mask)) return false;
  return xl9555WriteReg(XL9555_CONFIG0, ~mask & 0xFF);
}

bool xl9555ReadPort(uint8_t reg, uint8_t &val) {
  Wire.beginTransmission(XL9555_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)XL9555_ADDR, 1) != 1) return false;
  val = Wire.read();
  return true;
}

void recordFeedStart() {
  time_t now;
  time(&now);
  FeedSession s;
  s.startEpoch = now;
  s.stopEpoch = 0;
  feedHistory[feedHistoryHead] = s;
  feedHistoryHead = (feedHistoryHead + 1) % HISTORY_SIZE;
  if (feedHistoryCount < HISTORY_SIZE) feedHistoryCount++;
}

void recordFeedStop() {
  if (feedHistoryCount == 0) return;
  size_t lastIdx = (feedHistoryHead + HISTORY_SIZE - 1) % HISTORY_SIZE;
  time_t now;
  time(&now);
  feedHistory[lastIdx].stopEpoch = now;
}

String currentTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 0)) return String("(time not synced)");
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &timeinfo);
  return String(buf);
}

String formatElapsed(uint32_t seconds) {
  uint32_t hours = seconds / 3600;
  uint32_t minutes = (seconds % 3600) / 60;
  char buffer[8];
  snprintf(buffer, sizeof(buffer), "%02lu:%02lu", (unsigned long)hours, (unsigned long)minutes);
  return String(buffer);
}

bool getClockText(String &out) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 0)) return false;
  if (timeinfo.tm_year < (2024 - 1900)) return false;
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  out = String(buf);
  return true;
}

// Counts characters in a UTF-8 string (each lead byte; continuation bytes
// have 0b10xx_xxxx). Used to width-align CJK strings whose .length() is bytes.
int utf8CharCount(const String &s) {
  int count = 0;
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t b = (uint8_t)s.charAt(i);
    if ((b & 0xC0) != 0x80) count++;
  }
  return count;
}

bool getDateText(String &out) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 0)) return false;
  if (timeinfo.tm_year < (2024 - 1900)) return false;
  char buf[16];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
           timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
  out = String(buf);
  return true;
}

void flushDisplay() { gfx->flush(); }
void drawSegment(int x, int y, int w, int h, uint16_t color) { gfx->fillRect(x, y, w, h, color); }

void drawTimerDigit(int x, int y, int digit, uint16_t color) {
  static const bool segments[10][7] = {
    {true, true, true, true, true, true, false}, {false, true, true, false, false, false, false},
    {true, true, false, true, true, false, true}, {true, true, true, true, false, false, true},
    {false, true, true, false, false, true, true}, {true, false, true, true, false, true, true},
    {true, false, true, true, true, true, true},  {true, true, true, false, false, false, false},
    {true, true, true, true, true, true, true},   {true, true, true, true, false, true, true},
  };
  if (digit < 0 || digit > 9) return;
  const int t = TIMER_SEGMENT_THICKNESS;
  const int w = TIMER_DIGIT_WIDTH;
  const int h = TIMER_DIGIT_HEIGHT;
  const int horizontalW = w - (2 * t);
  const int upperH = (h - (3 * t)) / 2;
  const int middleY = y + t + upperH;
  const int lowerY = middleY + t;
  const int lowerH = h - (3 * t) - upperH;
  if (segments[digit][0]) drawSegment(x + t, y, horizontalW, t, color);
  if (segments[digit][1]) drawSegment(x + w - t, y + t, t, upperH, color);
  if (segments[digit][2]) drawSegment(x + w - t, lowerY, t, lowerH, color);
  if (segments[digit][3]) drawSegment(x + t, y + h - t, horizontalW, t, color);
  if (segments[digit][4]) drawSegment(x, lowerY, t, lowerH, color);
  if (segments[digit][5]) drawSegment(x, y + t, t, upperH, color);
  if (segments[digit][6]) drawSegment(x + t, middleY, horizontalW, t, color);
}

void drawTimerColon(int x, int y, uint16_t color) {
  const int dot = TIMER_SEGMENT_THICKNESS + 2;
  const int dotX = x + ((TIMER_COLON_WIDTH - dot) / 2);
  const int centerY = y + (TIMER_DIGIT_HEIGHT / 2);
  const int offset = TIMER_DIGIT_HEIGHT / 4;
  gfx->fillRect(dotX, centerY - offset, dot, dot, color);
  gfx->fillRect(dotX, centerY + offset - dot, dot, dot, color);
}

void drawBigDigits(const String &text, int y, uint16_t hourColor, uint16_t colonColor,
                   uint16_t minuteColor = RGB565_MAGENTA) {
  int totalWidth = 0;
  for (size_t i = 0; i < text.length(); i++) {
    totalWidth += (text.charAt(i) == ':') ? TIMER_COLON_WIDTH : TIMER_DIGIT_WIDTH;
    if (i + 1 < text.length()) totalWidth += TIMER_CHAR_GAP;
  }
  // Mathematical center looks slightly right-biased on this panel because the
  // bright orange MM weighs more visually than the dark red HH; nudge left.
  int x = ((gfx->width() - totalWidth) / 2) - 16;
  if (x < 0) x = 0;
  // Heartbeat: 500ms on, 500ms off — one full blink per second. Both clock
  // and counter tickers redraw at 500ms to match.
  bool showColon = ((millis() / 500) & 1U) == 0;
  int section = 0; // 0=HH (before first ':'), 1=MM (after first ':')
  for (uint8_t i = 0; i < text.length(); i++) {
    char c = text.charAt(i);
    if (c == ':') {
      if (showColon) drawTimerColon(x, y, colonColor);
      x += TIMER_COLON_WIDTH + TIMER_CHAR_GAP;
      if (section < 1) section++;
    } else {
      uint16_t color = (section == 0) ? hourColor : minuteColor;
      drawTimerDigit(x, y, c - '0', color);
      x += TIMER_DIGIT_WIDTH + TIMER_CHAR_GAP;
    }
  }
}

void drawTimerText(uint32_t elapsedSeconds) {
  drawBigDigits(formatElapsed(elapsedSeconds), 50, RGB565_RED, RGB565_WHITE,
                RGB565_ORANGE);
}

void drawButtonHint() {
  gfx->println(gatewayMode() ? "K2 feed  K1 next (hold=sync)" : "K2 feed  K1 next");
}

void drawStatus(const String &title, const String &body) {
  activeCounter.active = false;
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(RGB565_CYAN);
  gfx->setTextSize(2);
  gfx->setCursor(12, 12);
  gfx->println(title);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(12, 50);
  gfx->println(body);
  gfx->setTextColor(RGB565_DARKGREY);
  gfx->setTextSize(1);
  gfx->setCursor(12, 226);
  drawButtonHint();
  flushDisplay();
}

void drawClockScreen() {
  String clock;
  bool synced = getClockText(clock);
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(RGB565_CYAN);
  gfx->setTextSize(2);
  gfx->setCursor(14, 12);
  gfx->print(synced ? "Time" : "Syncing time...");

  String date;
  if (getDateText(date)) {
    const int charW = 12;
    int x = gfx->width() - (int)date.length() * charW - 14;
    if (x < 0) x = 0;
    gfx->setTextColor(RGB565_YELLOW);
    gfx->setCursor(x, 12);
    gfx->print(date);
  }

  if (synced) {
    drawBigDigits(clock, 50, RGB565_RED, RGB565_WHITE, RGB565_ORANGE);
  }
  gfx->setTextColor(RGB565_DARKGREY);
  gfx->setTextSize(1);
  gfx->setCursor(14, 210);
  gfx->print(WiFi.localIP().toString());
  if (gatewayMode()) {
    gfx->print("  gateway:");
    gfx->setTextColor(gatewayOnline ? RGB565_GREEN : RGB565_RED);
    gfx->print(gatewayOnline ? "online" : "offline");
    gfx->setTextColor(RGB565_DARKGREY);
  }
  gfx->println();
  gfx->setCursor(14, 226);
  drawButtonHint();
  flushDisplay();
}

void drawCounter(uint32_t elapsedSeconds) {
  gfx->fillScreen(RGB565_BLACK);

  // Combined heading: "Last fed 结束喂养" / "Feeding now 开始喂养",
  // centered on a single top line. Default font is 6x8 base; setTextSize(2)
  // makes each ASCII char 12px wide. cubic11 CJK glyphs are ~22px wide at
  // setTextSize(2). Chinese baseline aligned to y=24 sits next to the
  // top-anchored ASCII title (y=8..y=24), bottom-aligned visually.
  const int titleCharW = 12;
  int titleW = activeCounter.title.length() * titleCharW;
  bool hasSub = activeCounter.subtitle.length() > 0;
  int subW = hasSub ? utf8CharCount(activeCounter.subtitle) * CJK_SUBTITLE_GLYPH_PX : 0;
  int gap = hasSub ? titleCharW : 0;
  int totalW = titleW + gap + subW;
  int hx = (gfx->width() - totalW) / 2;
  if (hx < 0) hx = 0;

  gfx->setTextColor(RGB565_YELLOW);
  gfx->setTextSize(2);
  gfx->setCursor(hx, 8);
  gfx->print(activeCounter.title);

  if (hasSub) {
    gfx->setFont(u8g2_font_cubic11_h_cjk);
    gfx->setTextSize(2);
    gfx->setTextColor(RGB565_YELLOW);
    gfx->setCursor(hx + titleW + gap, 24);
    gfx->print(activeCounter.subtitle);
    gfx->setFont();  // back to default 5x7 ASCII font
    gfx->setTextSize(2);
  }

  drawTimerText(elapsedSeconds);

  String stamp = currentTimestamp();
  if (stamp.length() > 0) {
    gfx->setTextSize(2);
    gfx->setTextColor(RGB565_WHITE);
    const int charW = 12;
    int w = stamp.length() * charW;
    int x = gfx->width() - w - 8;
    if (x < 0) x = 0;
    gfx->setCursor(x, 208);
    gfx->print(stamp);
  }

  gfx->setTextColor(RGB565_DARKGREY);
  gfx->setTextSize(1);
  gfx->setCursor(14, 228);
  gfx->println(gatewayMode() ? "K2 toggle  K1 next (hold=sync)" : "K2 toggle  K1 next");
  flushDisplay();
}

void drawHistoryScreen() {
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(RGB565_CYAN);
  gfx->setTextSize(2);
  gfx->setCursor(14, 8);
  gfx->println("Last feedings");

  if (feedHistoryCount == 0) {
    gfx->setTextSize(2);
    gfx->setTextColor(RGB565_DARKGREY);
    gfx->setCursor(14, 50);
    gfx->println("No feedings yet.");
  } else {
    gfx->setTextSize(2);
    int y = 32;
    const int lineH = 18;
    char prevDate[16] = "";
    for (size_t i = 0; i < feedHistoryCount; i++) {
      size_t idx = (feedHistoryHead + HISTORY_SIZE - 1 - i) % HISTORY_SIZE;
      const FeedSession &s = feedHistory[idx];
      struct tm tmStart;
      localtime_r(&s.startEpoch, &tmStart);

      char dateStr[16];
      snprintf(dateStr, sizeof(dateStr), "%04d-%02d-%02d",
               tmStart.tm_year + 1900, tmStart.tm_mon + 1, tmStart.tm_mday);
      if (strcmp(dateStr, prevDate) != 0) {
        if (y + lineH > 218) break;
        gfx->setTextColor(RGB565_YELLOW);
        gfx->setCursor(14, y);
        gfx->println(dateStr);
        y += lineH;
        strcpy(prevDate, dateStr);
      }

      char line[48];
      if (s.stopEpoch == 0) {
        snprintf(line, sizeof(line), "%u. %02d:%02d -  ...",
                 (unsigned)(i + 1),
                 tmStart.tm_hour, tmStart.tm_min);
      } else {
        struct tm tmStop;
        localtime_r(&s.stopEpoch, &tmStop);
        snprintf(line, sizeof(line), "%u. %02d:%02d - %02d:%02d",
                 (unsigned)(i + 1),
                 tmStart.tm_hour, tmStart.tm_min,
                 tmStop.tm_hour, tmStop.tm_min);
      }
      if (y + lineH > 218) break;
      gfx->setTextColor(s.stopEpoch == 0 ? RGB565_YELLOW : RGB565_WHITE);
      gfx->setCursor(24, y);
      gfx->println(line);
      y += lineH;
    }
  }
  gfx->setTextColor(RGB565_DARKGREY);
  gfx->setTextSize(1);
  gfx->setCursor(14, 228);
  gfx->println(gatewayMode() ? "K1 next  (hold=sync)" : "K1 next");
  flushDisplay();
}

void redrawCurrentView() {
  switch (currentView) {
    case VIEW_HISTORY:
      drawHistoryScreen();
      break;
    case VIEW_COUNTER:
      if (activeCounter.active) {
        uint32_t elapsed = activeCounter.baseElapsedSeconds + ((millis() - activeCounter.startedAtMs) / 1000);
        drawCounter(elapsed);
      } else {
        currentView = VIEW_CLOCK;
        drawClockScreen();
      }
      break;
    case VIEW_CLOCK:
    default:
      drawClockScreen();
      break;
  }
}

void alarmStop();

void cycleView() {
  if (alarmFiring) {
    alarmStop();
    return;
  }
  for (int i = 0; i < 3; i++) {
    currentView = (ViewMode)((currentView + 1) % 3);
    if (currentView == VIEW_COUNTER && !activeCounter.active) continue;
    break;
  }
  redrawCurrentView();
}

void setCounter(const String &title, const String &subtitle = "",
                uint32_t baseElapsedSeconds = 0, time_t sessionEpoch = 0) {
  activeCounter.active = true;
  activeCounter.title = title;
  activeCounter.subtitle = subtitle;
  activeCounter.baseElapsedSeconds = baseElapsedSeconds;
  activeCounter.startedAtMs = millis();
  activeCounter.sessionEpoch = sessionEpoch;
  lastCounterDrawMs = 0;
  currentView = VIEW_COUNTER;
  drawCounter(baseElapsedSeconds);
}

// Audio output: I2S0 → ES8311 codec → onboard speaker.
i2s_chan_handle_t i2s_tx = nullptr;
es8311_handle_t es8311 = nullptr;
// Stereo 16-bit samples. 256 frames = 16ms at 16kHz; we re-pump it each tick.
static int16_t toneBuf[512];
static int16_t silenceBuf[512] = {0};

void buildToneBuf() {
  // ALARM_TONE_HZ square wave at I2S_SAMPLE_RATE, written to L+R.
  int samplesPerHalfPeriod = (I2S_SAMPLE_RATE / ALARM_TONE_HZ) / 2;
  if (samplesPerHalfPeriod < 1) samplesPerHalfPeriod = 1;
  const int16_t amp = 16000;
  for (int i = 0; i < 256; i++) {
    int16_t v = ((i / samplesPerHalfPeriod) & 1) ? amp : -amp;
    toneBuf[i * 2]     = v;
    toneBuf[i * 2 + 1] = v;
  }
}

void alarmInit() {
#if ALARM_VOLUME == 0
  // Silent mode: skip the entire audio stack. Trigger logic still fires,
  // serial log still prints, view still switches — just no sound.
  Serial.println("Audio disabled (ALARM_VOLUME=0)");
  return;
#endif
  buildToneBuf();

  // I2S0 TX, master, 16kHz/16-bit/stereo. 32-bit slot width keeps BCLK at
  // 1.024 MHz which matches an entry in the ES8311 coeff_div table when the
  // codec derives MCLK from BCLK.
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  if (i2s_new_channel(&chan_cfg, &i2s_tx, nullptr) != ESP_OK) {
    Serial.println("i2s_new_channel failed");
    return;
  }

  i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
  slot_cfg.ws_width = 32;

  i2s_std_config_t std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
    .slot_cfg = slot_cfg,
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)I2S_PIN_BCLK,
      .ws   = (gpio_num_t)I2S_PIN_WS,
      .dout = (gpio_num_t)I2S_PIN_DOUT,
      .din  = I2S_GPIO_UNUSED,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  if (i2s_channel_init_std_mode(i2s_tx, &std_cfg) != ESP_OK) {
    Serial.println("i2s_channel_init_std_mode failed");
    return;
  }
  // Channel stays disabled at idle; alarmStart enables it. This way the
  // codec never receives stale DMA junk between alarms.
  i2s_channel_disable(i2s_tx);

  // ES8311 codec on the same I2C bus that XL9555 uses (Wire). The lib uses
  // Arduino Wire under the hood; the i2c_port_t arg is ignored.
  es8311 = es8311_create(I2C_NUM_0, ES8311_ADDRESS_0);
  if (!es8311) {
    Serial.println("es8311_create failed");
    return;
  }
  es8311_clock_config_t clk_cfg = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = false,  // derive from BCLK = 1.024 MHz
    .mclk_frequency = 0,
    .sample_frequency = I2S_SAMPLE_RATE,
  };
  if (es8311_init(es8311, &clk_cfg, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) {
    Serial.println("es8311_init failed");
    return;
  }
  // Map ALARM_VOLUME (0..5) -> codec voice volume (0..100). The codec's
  // volume register is logarithmic (~0.5 dB per LSB), but the lib's
  // volume_set does a linear 0..100 → reg mapping, so low input values fall
  // off a cliff (vol=25 ≈ -96 dB, inaudible). These values are picked to
  // give roughly: 1=-36 dB, 2=-24 dB, 3=-16 dB, 4=-8 dB, 5=0 dB.
  static const int volMap[6] = { 0, 72, 81, 87, 94, 100 };
  int v = ALARM_VOLUME;
  if (v < 0) v = 0;
  if (v > 5) v = 5;
  if (v == 0) {
    es8311_voice_mute(es8311, true);
  } else {
    es8311_voice_mute(es8311, false);
    es8311_voice_volume_set(es8311, volMap[v], nullptr);
  }
  es8311_microphone_config(es8311, false);
  Serial.printf("Audio init OK (vol=%d -> %d)\n", v, volMap[v]);
}

void alarmStop() {
  alarmFiring = false;
  alarmToneOn = false;
  alarmDismissedForEpoch = activeCounter.sessionEpoch;
  if (i2s_tx) i2s_channel_disable(i2s_tx);  // stops DMA → codec goes silent
}

void alarmStart() {
  alarmFiring = true;
  alarmFiringStartMs = millis();
  alarmLastToggleMs = millis();
  alarmToneOn = true;
  if (i2s_tx) i2s_channel_enable(i2s_tx);
}

// 200ms tone / 200ms silence — attention-getting but not painful. Pump each
// loop pass so DMA never starves while firing. Also self-dismisses after
// ALARM_DURATION_SEC if K1 hasn't been pressed.
void alarmTick() {
  if (!alarmFiring) return;
  if (millis() - alarmFiringStartMs >= (uint32_t)ALARM_DURATION_SEC * 1000U) {
    Serial.println("Alarm auto-dismissed (timeout)");
    alarmStop();
    return;
  }
  if (!i2s_tx) return;
  if (millis() - alarmLastToggleMs >= 200) {
    alarmLastToggleMs = millis();
    alarmToneOn = !alarmToneOn;
  }
  size_t bw;
  i2s_channel_write(i2s_tx,
                    alarmToneOn ? (const void *)toneBuf : (const void *)silenceBuf,
                    sizeof(toneBuf),
                    &bw,
                    pdMS_TO_TICKS(5));
}

void checkAlarmTrigger(uint32_t elapsedSeconds) {
  if (alarmFiring) return;
  if (activeCounter.title != "Last fed") return;
  if (activeCounter.sessionEpoch == 0) return;
  if (activeCounter.sessionEpoch == alarmDismissedForEpoch) return;
  if (elapsedSeconds < (uint32_t)ALARM_MINUTES * 60) return;
  alarmStart();
}

void drawSyncStatus(const String &title, const String &body, uint16_t titleColor) {
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(titleColor);
  gfx->setTextSize(3);
  gfx->setCursor(20, 80);
  gfx->println(title);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(20, 140);
  gfx->println(body);
  flushDisplay();
}

// ---------------------------------------------------------------------------
// Gateway HTTP client (called from gatewayTask on core 0)
// ---------------------------------------------------------------------------

String gatewayUrl(const char *path) {
  String base = String(GATEWAY_URL);
  if (base.endsWith("/")) base.remove(base.length() - 1);
  return base + path;
}

// Holds both the HTTPClient and (for https URLs) the underlying secure
// transport. Stack-allocate inline so they share scope; the secure client
// must outlive any active HTTP request.
struct HttpSession {
  HTTPClient http;
  NetworkClientSecure secureClient;
};

bool beginHttp(HttpSession &s, const String &url, uint32_t timeoutMs) {
  s.http.setTimeout(timeoutMs);
  if (url.startsWith("https://")) {
#ifdef GATEWAY_CA_CERT
    s.secureClient.setCACert(GATEWAY_CA_CERT);
#else
    s.secureClient.setInsecure();
#endif
    if (!s.http.begin(s.secureClient, url)) return false;
  } else {
    if (!s.http.begin(url)) return false;
  }
  if (strlen(GATEWAY_TOKEN) > 0) {
    s.http.addHeader("Authorization", String("Bearer ") + GATEWAY_TOKEN);
  }
  return true;
}

bool gatewayPostEvent(const PendingEvent &ev) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HttpSession s;
  if (!beginHttp(s, gatewayUrl("/api/events"), 3000)) return false;
  s.http.addHeader("Content-Type", "application/json");
  JsonDocument doc;
  doc["type"] = ev.type;
  doc["device_id"] = DEVICE_ID;
  doc["timestamp_epoch"] = (long)ev.epoch;
  String body;
  serializeJson(doc, body);
  int status = s.http.POST(body);
  s.http.end();
  return status >= 200 && status < 300;
}

void applyGatewayState(JsonDocument &doc) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);

  // Don't overwrite if we still have unsent events; we're ahead of the gateway.
  if (pendingCount > 0) {
    xSemaphoreGive(stateMutex);
    return;
  }

  time_t now;
  time(&now);

  JsonVariant active = doc["active"];
  feedingActive = !active.isNull();

  feedHistoryCount = 0;
  feedHistoryHead = 0;
  JsonArray history = doc["history"].as<JsonArray>();
  size_t n = history.size();
  for (size_t i = 0; i < n; i++) {
    size_t srcIdx = n - 1 - i; // gateway returns newest-first
    JsonObject r = history[srcIdx];
    FeedSession s;
    s.startEpoch = (time_t)(long)r["start_epoch"];
    JsonVariant stop = r["stop_epoch"];
    s.stopEpoch = stop.isNull() ? 0 : (time_t)(long)stop;
    feedHistory[feedHistoryHead] = s;
    feedHistoryHead = (feedHistoryHead + 1) % HISTORY_SIZE;
    if (feedHistoryCount < HISTORY_SIZE) feedHistoryCount++;
  }

  if (feedingActive) {
    JsonObject act = active.as<JsonObject>();
    time_t startEpoch = (time_t)(long)act["start_epoch"];
    activeCounter.active = true;
    activeCounter.title = "Feeding now";
    activeCounter.subtitle = "开始喂养";
    activeCounter.baseElapsedSeconds = (now > startEpoch) ? (uint32_t)(now - startEpoch) : 0;
    activeCounter.startedAtMs = millis();
    activeCounter.sessionEpoch = startEpoch;
  } else if (feedHistoryCount > 0) {
    size_t lastIdx = (feedHistoryHead + HISTORY_SIZE - 1) % HISTORY_SIZE;
    const FeedSession &s = feedHistory[lastIdx];
    if (s.stopEpoch != 0) {
      activeCounter.active = true;
      activeCounter.title = "Last fed";
      activeCounter.subtitle = "结束喂养";
      activeCounter.baseElapsedSeconds = (now > s.stopEpoch) ? (uint32_t)(now - s.stopEpoch) : 0;
      activeCounter.startedAtMs = millis();
      activeCounter.sessionEpoch = s.stopEpoch;
    }
  } else {
    activeCounter.active = false;
  }

  gatewayStateDirty = true;
  xSemaphoreGive(stateMutex);
}

bool gatewayFetchState() {
  if (WiFi.status() != WL_CONNECTED) return false;
  HttpSession s;
  if (!beginHttp(s, gatewayUrl("/api/state"), 3000)) return false;
  int status = s.http.GET();
  if (status < 200 || status >= 300) {
    s.http.end();
    return false;
  }
  String body = s.http.getString();
  s.http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  applyGatewayState(doc);
  return true;
}

bool gatewayTriggerSync() {
  if (WiFi.status() != WL_CONNECTED) return false;
  HttpSession s;
  if (!beginHttp(s, gatewayUrl("/api/sync"), 15000)) return false;
  s.http.addHeader("Content-Type", "application/json");
  int status = s.http.POST("{}");
  String body = s.http.getString();
  s.http.end();
  Serial.printf("Sync -> %d %s\n", status, body.c_str());
  return status >= 200 && status < 300;
}

void drainPendingQueue() {
  while (true) {
    PendingEvent ev;
    bool have = false;
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    if (pendingCount > 0) {
      ev = pendingQueue[0];
      have = true;
    }
    xSemaphoreGive(stateMutex);
    if (!have) return;
    if (!gatewayPostEvent(ev)) return; // try again next round
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    if (pendingCount > 0) {
      for (size_t i = 1; i < pendingCount; i++) pendingQueue[i - 1] = pendingQueue[i];
      pendingCount--;
    }
    xSemaphoreGive(stateMutex);
  }
}

void enqueuePendingEvent(const char *type, time_t epoch) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if (pendingCount >= PENDING_QUEUE_SIZE) {
    for (size_t i = 1; i < pendingCount; i++) pendingQueue[i - 1] = pendingQueue[i];
    pendingCount--;
  }
  strncpy(pendingQueue[pendingCount].type, type, sizeof(pendingQueue[pendingCount].type) - 1);
  pendingQueue[pendingCount].type[sizeof(pendingQueue[pendingCount].type) - 1] = 0;
  pendingQueue[pendingCount].epoch = epoch;
  pendingCount++;
  xSemaphoreGive(stateMutex);
}

void gatewayTask(void *) {
  while (true) {
    if (gatewayMode() && WiFi.status() == WL_CONNECTED) {
      drainPendingQueue();
      bool ok = gatewayFetchState();
      gatewayOnline = ok;
    }
    vTaskDelay(pdMS_TO_TICKS(GATEWAY_POLL_MS));
  }
}

// ---------------------------------------------------------------------------
// Button actions
// ---------------------------------------------------------------------------

void toggleFeeding() {
  feedingActive = !feedingActive;
  time_t now;
  time(&now);

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if (feedingActive) {
    recordFeedStart();
  } else {
    recordFeedStop();
  }
  xSemaphoreGive(stateMutex);

  if (gatewayMode()) {
    enqueuePendingEvent(feedingActive ? "start" : "stop", now);
  }

  setCounter(feedingActive ? "Feeding now" : "Last fed",
             feedingActive ? "开始喂养" : "结束喂养",
             0, now);
}

void k1LongPress() {
  if (alarmFiring) {
    alarmStop();
    return;
  }
  if (!gatewayMode()) return;  // standalone has nothing to sync
  drawSyncStatus("Sync", "Posting...", RGB565_CYAN);
  bool ok = gatewayTriggerSync();
  if (ok) {
    drawSyncStatus("Sync OK", "Posted to OpenClaw", RGB565_GREEN);
  } else {
    drawSyncStatus("Sync failed", "Gateway error", RGB565_RED);
  }
  delay(2000);
  redrawCurrentView();
}

// ---------------------------------------------------------------------------
// View tickers
// ---------------------------------------------------------------------------

void updateCounter() {
  if (currentView != VIEW_COUNTER || !activeCounter.active) return;
  if (millis() - lastCounterDrawMs < 500) return;
  lastCounterDrawMs = millis();
  uint32_t elapsed = activeCounter.baseElapsedSeconds + ((millis() - activeCounter.startedAtMs) / 1000);
  drawCounter(elapsed);
}

// Runs from loop() regardless of which view is on screen — otherwise sitting
// on the clock view past the threshold would never trigger.
void tickAlarmCheck() {
  if (!activeCounter.active) return;
  uint32_t elapsed = activeCounter.baseElapsedSeconds + ((millis() - activeCounter.startedAtMs) / 1000);

  static uint32_t lastDebugMs = 0;
  if (millis() - lastDebugMs > 30000) {
    lastDebugMs = millis();
    Serial.printf("[alarm] active=%d title=[%s] elapsed=%us threshold=%us session=%ld dismissed=%ld firing=%d\n",
                  activeCounter.active ? 1 : 0,
                  activeCounter.title.c_str(),
                  (unsigned)elapsed,
                  (unsigned)((uint32_t)ALARM_MINUTES * 60),
                  (long)activeCounter.sessionEpoch,
                  (long)alarmDismissedForEpoch,
                  alarmFiring ? 1 : 0);
  }

  bool wasFiring = alarmFiring;
  checkAlarmTrigger(elapsed);
  if (alarmFiring && !wasFiring) {
    Serial.printf("Alarm fired at elapsed=%us session=%ld\n", (unsigned)elapsed, (long)activeCounter.sessionEpoch);
    currentView = VIEW_COUNTER;
    lastCounterDrawMs = 0;
    drawCounter(elapsed);
  }
}

void updateClockScreen() {
  if (currentView != VIEW_CLOCK) return;
  if (millis() - lastClockDrawMs < 500) return;
  lastClockDrawMs = millis();
  drawClockScreen();
}

void pollKeys() {
  if (!xl9555Ready) return;
  if (millis() - lastKeyPollMs < KEY_POLL_MS) return;
  lastKeyPollMs = millis();

  uint8_t p0 = 0xFF;
  if (!xl9555ReadPort(XL9555_INPUT0, p0)) return;
  k1IsPressed = ((p0 >> XL_KEY1_PIN) & 0x01) == 0;
  k2IsPressed = ((p0 >> XL_KEY2_PIN) & 0x01) == 0;
}

// ---------------------------------------------------------------------------
// NTP
// ---------------------------------------------------------------------------

static const char *NTP_CN_SERVERS[] = {
  "cn.ntp.org.cn",
  "ntp.ntsc.ac.cn",
  "cn.pool.ntp.org",
};
static const char *NTP_INTL_SERVERS[] = {
  "pool.ntp.org",
  "time.cloudflare.com",
  "time.google.com",
};
static const uint32_t NTP_PER_SERVER_TIMEOUT_MS = 6000;

bool tryNtpServer(const char *host, uint32_t timeoutMs) {
  configTime(NTP_GMT_OFFSET_SEC, NTP_DST_OFFSET_SEC, host);
  struct tm timeinfo;
  return getLocalTime(&timeinfo, timeoutMs);
}

bool tryNtpServerList(const char *const *servers, size_t count, const char *groupLabel) {
  for (size_t i = 0; i < count; i++) {
    String body = String(groupLabel) + ": " + servers[i];
    drawStatus("Syncing time", body);
    Serial.printf("NTP try %s\n", servers[i]);
    if (tryNtpServer(servers[i], NTP_PER_SERVER_TIMEOUT_MS)) {
      Serial.printf("NTP ok via %s\n", servers[i]);
      return true;
    }
    Serial.printf("NTP fail %s\n", servers[i]);
  }
  return false;
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  drawStatus("WiFi", "Connecting...");
  uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 20000) delay(250);
  if (WiFi.status() != WL_CONNECTED) {
    drawStatus("WiFi failed", "Check config.h");
    return;
  }

  // Wait for DHCP lease before doing anything that needs the network.
  drawStatus("WiFi", "Waiting for DHCP...");
  uint32_t dhcpStarted = millis();
  while (WiFi.localIP() == IPAddress(0, 0, 0, 0) && millis() - dhcpStarted < 10000) {
    delay(100);
  }
  if (WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
    drawStatus("DHCP failed", "No IP from router");
    return;
  }
  Serial.printf("DHCP ok, IP=%s\n", WiFi.localIP().toString().c_str());

  if (!tryNtpServerList(NTP_CN_SERVERS,
                        sizeof(NTP_CN_SERVERS) / sizeof(NTP_CN_SERVERS[0]),
                        "CN")) {
    tryNtpServerList(NTP_INTL_SERVERS,
                     sizeof(NTP_INTL_SERVERS) / sizeof(NTP_INTL_SERVERS[0]),
                     "global");
  }
  drawClockScreen();
}

void setup() {
  Serial.begin(115200);

  stateMutex = xSemaphoreCreateMutex();

  // Init LCD FIRST (LCD_CAM peripheral). Doing Wire.begin before this
  // hangs the chip on DNESP32S3B.
  Serial.println("LCD init...");
  if (!gfx->begin()) Serial.println("gfx->begin() failed");
  gfx->setUTF8Print(true);  // multi-byte decode for u8g2 CJK fonts
  gfx->fillScreen(RGB565_BLACK);
  flushDisplay();

  Wire.setPins(I2C_SDA, I2C_SCL);
  Wire.begin();
  Wire.setClock(100000);

  alarmInit();
  if (xl9555EnableBacklight()) {
    xl9555Ready = true;
    Serial.println("Backlight ON");
  } else {
    Serial.println("XL9555 not responding; backlight stays off");
  }

  connectWiFi();

  if (gatewayMode()) {
    Serial.printf("Gateway mode -> %s (device_id=%s)\n", GATEWAY_URL, DEVICE_ID);
    // 16K stack — TLS handshakes can need >8K when GATEWAY_URL is https.
    xTaskCreatePinnedToCore(gatewayTask, "gateway", 16384, nullptr, 1, nullptr, 0);
  } else {
    Serial.println("Standalone mode (no gateway)");
  }
}

void handleButtonRelease(ButtonState &button, bool pressed, void (*onReleased)()) {
  uint32_t nowMs = millis();
  if (pressed == button.pressed || nowMs - button.lastChangeMs <= BUTTON_DEBOUNCE_MS) return;
  button.lastChangeMs = nowMs;
  button.pressed = pressed;
  if (!pressed) onReleased();
}

void handleButtonPressLong(ButtonState &button, bool pressed,
                            void (*onShortRelease)(), void (*onLongPress)(),
                            uint32_t longMs) {
  uint32_t nowMs = millis();
  if (pressed != button.pressed) {
    if (nowMs - button.lastChangeMs <= BUTTON_DEBOUNCE_MS) return;
    button.lastChangeMs = nowMs;
    button.pressed = pressed;
    if (pressed) {
      button.pressStartMs = nowMs;
      button.longFired = false;
    } else if (!button.longFired) {
      onShortRelease();
    }
    return;
  }
  if (pressed && !button.longFired && (nowMs - button.pressStartMs >= longMs)) {
    button.longFired = true;
    onLongPress();
  }
}

void loop() {
  // Apply gateway-side updates picked up by gatewayTask.
  if (gatewayStateDirty) {
    gatewayStateDirty = false;
    redrawCurrentView();
  }

  updateCounter();
  updateClockScreen();
  tickAlarmCheck();
  alarmTick();
  pollKeys();
  handleButtonPressLong(k1Button, k1IsPressed, cycleView, k1LongPress, K1_LONG_PRESS_MS);
  handleButtonRelease(k2Button, k2IsPressed, toggleFeeding);
  delay(5);
}
