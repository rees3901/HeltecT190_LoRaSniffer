#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <RadioLib.h>
#include <ArduinoJson.h>
#include "pins.h"

// ── BluePawz protocol ──
#define LORA_FREQ       868.0
#define LORA_SF         9
#define LORA_BW         125.0
#define LORA_CR         5
#define LORA_PREAMBLE   16
#define LORA_SYNC_WORD  0x12

// ── TFT pins (Heltec T190 vendor spec) ──
#define TFT_MISO  -1
#define TFT_MOSI  48
#define TFT_SCLK  38
#define TFT_CS    39
#define TFT_DC    47
#define TFT_RST   40
#define TFT_BL    17
#define TFT_VCtrl  7

// ── Display ──
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
const int LINE_H      = 16;
const int SCREEN_W    = 320;
const int SCREEN_H    = 170;
const int STATUS_H    = 14;                     // bottom status-bar height (px)
const int BODY_BOTTOM = SCREEN_H - STATUS_H;    // payload text must stay above this
const int MAX_LINES   = SCREEN_H / LINE_H;      // 10 lines (used by boot splash)
const int SUMMARY_ROWS_Y = 3 * LINE_H;          // device tally starts here (y=48)

// "Collar quiet" warning threshold for the status bar. The collar's TX cadence
// is mode-dependent (normal 300s, active 60s, lost 30s), coarse hint only.
const uint32_t QUIET_S = 120;

// Per-device freshness thresholds (seconds) for the summary tally colours.
const uint32_t FRESH_GREEN_S  = 300;   // < 5 min  -> green
const uint32_t FRESH_YELLOW_S = 600;   // < 10 min -> yellow, older -> red

// ── View modes ──
enum ViewMode { VIEW_HISTORY, VIEW_SUMMARY };
ViewMode viewMode = VIEW_HISTORY;

// ── Message history ring buffer ──
struct MsgRecord {
  uint32_t num;
  uint32_t ms;        // millis() at reception (for "age")
  float    rssi, snr;
  String   payload;
};

const int MSG_HISTORY = 10;
MsgRecord msgBuf[MSG_HISTORY];
int      msgCount  = 0;   // messages stored so far (caps at MSG_HISTORY)
int      msgHead   = -1;  // ring buffer index of newest message
int      viewOff   = 0;   // 0 = newest (LIVE), >0 = older (FROZEN)
uint32_t pktCount  = 0;   // good packets received
uint32_t errCount  = 0;   // CRC mismatches + RX errors
uint32_t lastPktMs = 0;   // millis() of most recent good packet

// ── Per-device tally (for summary page) ──
const int MAX_DEVICES = 6;             // fits the summary rows (y=48..140)
struct DevStat {
  char     id[16];
  uint32_t count;
  uint32_t lastMs;
};
DevStat devs[MAX_DEVICES];
int devCount = 0;

// ── Buttons: two-way page navigation ──
// User button = next page, boot button (GPIO0) = previous page.
// Double-press on either returns to the summary page.
const uint32_t DEBOUNCE_MS  = 200;
const uint32_t DBL_CLICK_MS = 350;   // window to catch a second press

struct Button {
  uint8_t  pin;
  bool     isNext;        // true = next page, false = previous page
  bool     lastState;
  uint32_t lastEdgeMs;
  bool     clickPending;
  uint32_t firstClickMs;
};
Button btnUser = { PIN_BTN,  true,  HIGH, 0, false, 0 };
Button btnBoot = { BOOT_BTN, false, HIGH, 0, false, 0 };

// ── LoRa on HSPI (SPI3) ── TFT uses default SPI (SPI2/FSPI)
SPIClass LoRaSPI(HSPI);
SX1262 lora = new Module(LORA_NSS_PIN, LORA_DIO1_PIN, LORA_RST_PIN, LORA_BUSY_PIN, LoRaSPI);

volatile bool gotPacket = false;
void IRAM_ATTR onLoRaRx() { gotPacket = true; }

// Flash the TFT backlight 5x to signal a freshly received packet. Blocking
// (~500ms), fine given how infrequently the collar transmits. The backlight is
// active-high and normally on, so we blink it off->on and leave it on.
void LED_flicker()
{
  for (int i = 0; i < 5; i++)
  {
    digitalWrite(TFT_BL, LOW);    // backlight off
    delay(50);
    digitalWrite(TFT_BL, HIGH);   // backlight on
    delay(50);
  }
}

// ── Helpers ──

int scrollY = 0;

void printLine(const char* text, uint16_t color = ST77XX_WHITE) {
  if (scrollY >= MAX_LINES * LINE_H) {
    tft.fillScreen(ST77XX_BLACK);
    scrollY = 0;
  }
  tft.setTextColor(color, ST77XX_BLACK);
  tft.setCursor(0, scrollY);
  tft.print(text);
  scrollY += LINE_H;
}

void printLinef(uint16_t color, const char* fmt, ...) {
  char buf[128];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  printLine(buf, color);
}

// Green <5min, yellow <10min, red older.
uint16_t freshColor(uint32_t lastMs) {
  uint32_t age = (millis() - lastMs) / 1000;
  if (age < FRESH_GREEN_S)  return ST77XX_GREEN;
  if (age < FRESH_YELLOW_S) return ST77XX_YELLOW;
  return ST77XX_RED;
}

// "45s" under a minute, otherwise "12m".
void fmtAge(uint32_t lastMs, char* out, size_t n) {
  uint32_t age = (millis() - lastMs) / 1000;
  if (age < 60) snprintf(out, n, "%lus", (unsigned long)age);
  else          snprintf(out, n, "%lum", (unsigned long)(age / 60));
}

// Pull a device identity from a parsed payload and fold it into the tally.
// Telemetry uses "id", status/pong/ack use "device", with "device_id" (int)
// as a last resort. Packets with no identity still count toward pktCount.
void tallyDevice(JsonDocument& doc) {
  const char* id = nullptr;
  char tmp[16];
  if (doc["id"].is<const char*>())          id = doc["id"];
  else if (doc["device"].is<const char*>()) id = doc["device"];
  else if (doc["device_id"].is<int>()) {
    snprintf(tmp, sizeof(tmp), "#%d", doc["device_id"].as<int>());
    id = tmp;
  }
  if (!id || !id[0]) return;

  for (int i = 0; i < devCount; i++) {
    if (strncmp(devs[i].id, id, sizeof(devs[i].id)) == 0) {
      devs[i].count++;
      devs[i].lastMs = millis();
      return;
    }
  }
  if (devCount < MAX_DEVICES) {
    strncpy(devs[devCount].id, id, sizeof(devs[devCount].id) - 1);
    devs[devCount].id[sizeof(devs[devCount].id) - 1] = '\0';
    devs[devCount].count  = 1;
    devs[devCount].lastMs = millis();
    devCount++;
  }
  // Table full + new device: silently ignored (rare for this fleet).
}

// ── Bottom status bar: view state, packet age, good/error counts ──
void drawStatusBar() {
  tft.fillRect(0, BODY_BOTTOM, SCREEN_W, STATUS_H, ST77XX_BLACK);
  tft.setTextSize(1);

  char buf[64];
  uint16_t col;
  if (viewMode == VIEW_SUMMARY) {
    uint32_t age = (lastPktMs == 0) ? 0 : (millis() - lastPktMs) / 1000;
    col = (age > QUIET_S) ? ST77XX_RED : (age > QUIET_S / 2) ? ST77XX_YELLOW : ST77XX_GREEN;
    snprintf(buf, sizeof(buf), "SUMMARY  age:%lus  ok:%lu err:%lu",
             (unsigned long)age, (unsigned long)pktCount, (unsigned long)errCount);
  } else if (viewOff == 0) {
    uint32_t age = (lastPktMs == 0) ? 0 : (millis() - lastPktMs) / 1000;
    col = (age > QUIET_S) ? ST77XX_RED : (age > QUIET_S / 2) ? ST77XX_YELLOW : ST77XX_GREEN;
    snprintf(buf, sizeof(buf), "LIVE  age:%lus  ok:%lu err:%lu",
             (unsigned long)age, (unsigned long)pktCount, (unsigned long)errCount);
  } else {
    col = ST77XX_CYAN;
    snprintf(buf, sizeof(buf), "FROZEN %d/%d  ok:%lu err:%lu",
             viewOff + 1, msgCount, (unsigned long)pktCount, (unsigned long)errCount);
  }

  tft.setTextColor(col, ST77XX_BLACK);
  tft.setCursor(2, BODY_BOTTOM + 3);
  tft.print(buf);
  tft.setTextSize(2);   // restore body text size
}

// ── Summary page: device tally rows (drawn from SUMMARY_ROWS_Y down) ──
void drawDeviceRows() {
  tft.setTextSize(2);
  scrollY = SUMMARY_ROWS_Y;
  if (devCount == 0) {
    printLine("(no devices yet)", ST77XX_WHITE);
    return;
  }
  for (int i = 0; i < devCount; i++) {
    if (scrollY + LINE_H > BODY_BOTTOM) break;
    char age[8];
    fmtAge(devs[i].lastMs, age, sizeof(age));
    char line[40];
    snprintf(line, sizeof(line), "%-8s x%lu %s",
             devs[i].id, (unsigned long)devs[i].count, age);
    printLine(line, freshColor(devs[i].lastMs));
  }
}

void renderSummary() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(2);
  scrollY = 0;
  printLine("== SUMMARY ==", ST77XX_CYAN);
  printLinef(ST77XX_YELLOW, "%.0fMHz SF%d BW%.0fk", LORA_FREQ, LORA_SF, LORA_BW);
  printLinef(ST77XX_WHITE, "Pkts:%lu Err:%lu", (unsigned long)pktCount, (unsigned long)errCount);
  drawDeviceRows();
  drawStatusBar();
}

// Repaint just the device-tally band so ages/colours stay live without
// flickering the static header.
void refreshSummaryRows() {
  tft.fillRect(0, SUMMARY_ROWS_Y, SCREEN_W, BODY_BOTTOM - SUMMARY_ROWS_Y, ST77XX_BLACK);
  drawDeviceRows();
}

// ── Render one message from history ──
void renderMessage(int offset) {
  if (msgCount == 0) return;

  int idx = ((msgHead - offset) % MSG_HISTORY + MSG_HISTORY) % MSG_HISTORY;
  MsgRecord& m = msgBuf[idx];

  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(2);
  scrollY = 0;

  // Parse once, up front, so we can tag the header by direction.
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, m.payload);
  bool parsed = !err;
  // A "cmd" key means the base station is talking TO the collar (downlink).
  // Anything else (status/ack/pong/telemetry) is the collar talking back (uplink).
  bool isCmd = parsed && !doc["cmd"].isNull();

  const char* tag;
  uint16_t    hcol;
  if (!parsed)    { tag = "? RAW";  hcol = ST77XX_MAGENTA; }
  else if (isCmd) { tag = "DN CMD"; hcol = ST77XX_ORANGE; }  // base -> collar
  else            { tag = "UP TLM"; hcol = ST77XX_GREEN; }   // collar -> base

  printLinef(hcol, "%s #%lu R:%.0f S:%.1f", tag, m.num, m.rssi, m.snr);

  if (parsed) {
    JsonObject obj = doc.as<JsonObject>();
    for (JsonPair kv : obj) {
      if (scrollY + LINE_H > BODY_BOTTOM) break;
      char line[64];
      if (kv.value().is<bool>()) {
        snprintf(line, sizeof(line), "%s:%s", kv.key().c_str(), kv.value().as<bool>() ? "true" : "false");
      } else if (kv.value().is<int>()) {
        snprintf(line, sizeof(line), "%s:%d", kv.key().c_str(), kv.value().as<int>());
      } else if (kv.value().is<float>() || kv.value().is<double>()) {
        snprintf(line, sizeof(line), "%s:%.4f", kv.key().c_str(), kv.value().as<float>());
      } else if (kv.value().is<const char*>()) {
        snprintf(line, sizeof(line), "%s:%s", kv.key().c_str(), kv.value().as<const char*>());
      } else {
        snprintf(line, sizeof(line), "%s:[?]", kv.key().c_str());
      }
      printLine(line, ST77XX_WHITE);
    }
  } else {
    // Raw payload — wrap at 26 chars (textSize 2, 320px wide)
    printLine("(raw)", ST77XX_MAGENTA);
    int pos = 0;
    while (pos < (int)m.payload.length() && scrollY + LINE_H <= BODY_BOTTOM) {
      String chunk = m.payload.substring(pos, pos + 26);
      printLine(chunk.c_str(), ST77XX_WHITE);
      pos += 26;
    }
  }

  drawStatusBar();
}

// ── Button actions ──
void onSummary() {
  viewMode = VIEW_SUMMARY;
  renderSummary();
  Serial.println("[BTN] -> summary");
}

// Step through history. dir +1 = next (older), -1 = previous (newer). From the
// summary page, the first press just returns to history at the current position.
void stepPage(int dir) {
  if (viewMode == VIEW_SUMMARY) {
    viewMode = VIEW_HISTORY;
    renderMessage(viewOff);
    Serial.println("[BTN] -> history");
    return;
  }
  if (msgCount == 0) return;
  viewOff = (viewOff + dir + msgCount) % msgCount;
  renderMessage(viewOff);
  Serial.printf("[BTN] viewing msg %d/%d\n", viewOff + 1, msgCount);
}

// Service one button: single press steps a page (next/prev), double press jumps
// to the summary. The single press is deferred until the double-press window
// closes so the two gestures don't collide.
void serviceButton(Button& b) {
  bool now = digitalRead(b.pin);
  if (b.lastState == HIGH && now == LOW) {            // falling edge = press
    if (millis() - b.lastEdgeMs >= DEBOUNCE_MS) {
      b.lastEdgeMs = millis();
      if (b.clickPending && (millis() - b.firstClickMs) <= DBL_CLICK_MS) {
        b.clickPending = false;
        onSummary();
      } else {
        b.clickPending = true;
        b.firstClickMs = millis();
      }
    }
  }
  b.lastState = now;

  if (b.clickPending && (millis() - b.firstClickMs) > DBL_CLICK_MS) {
    b.clickPending = false;
    stepPage(b.isNext ? +1 : -1);
  }
}

// ── Setup ──
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("[T190] LoRa Sniffer starting...");

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  pinMode(PIN_BTN,  INPUT_PULLUP);   // user button = next page
  pinMode(BOOT_BTN, INPUT_PULLUP);   // boot button = previous page

  // Display power then init
  pinMode(TFT_VCtrl, OUTPUT);
  digitalWrite(TFT_VCtrl, LOW);

  SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.init(170, 320);
  tft.setRotation(1);
  tft.invertDisplay(true);
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setTextWrap(false);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  scrollY = 0;
  printLine("BluePawz LoRa Sniffer", ST77XX_CYAN);
  printLine("Heltec T190", ST77XX_CYAN);
  printLinef(ST77XX_YELLOW, "%.0fMHz SF%d BW%.0fk", LORA_FREQ, LORA_SF, LORA_BW);
  printLine("Btns:next/prev 2x=sum", ST77XX_WHITE);
  printLine("Waiting...", ST77XX_GREEN);

  // LoRa on HSPI
  LoRaSPI.begin(LORA_SCK_PIN, LORA_MISO_PIN, LORA_MOSI_PIN, LORA_NSS_PIN);

  int state = lora.begin(LORA_FREQ);
  if (state != RADIOLIB_ERR_NONE) {
    printLinef(ST77XX_RED, "LoRa FAIL: %d", state);
    Serial.printf("[LoRa] init failed: %d\n", state);
    while (true) delay(1000);
  }

  lora.setSpreadingFactor(LORA_SF);
  lora.setBandwidth(LORA_BW);
  lora.setCodingRate(LORA_CR);
  lora.setPreambleLength(LORA_PREAMBLE);
  lora.setSyncWord(LORA_SYNC_WORD);
  lora.setCRC(true);
  lora.setDio1Action(onLoRaRx);
  lora.startReceive();

  printLine("Radio OK - listening", ST77XX_GREEN);
  drawStatusBar();
  Serial.println("[LoRa] Sniffer ready");
}

// ── Packet handler ──
void handlePacket() {
  String incoming;
  int state = lora.readData(incoming);
  // Read stats before re-arming so they reflect this packet
  float rssi = lora.getRSSI();
  float snr  = lora.getSNR();
  lora.startReceive();

  if (state != RADIOLIB_ERR_NONE) {
    errCount++;
    if (state == RADIOLIB_ERR_CRC_MISMATCH) Serial.println("[LoRa] CRC mismatch");
    else                                    Serial.printf("[LoRa] RX err: %d\n", state);
    drawStatusBar();   // reflect error count without disturbing the view
    return;
  }

  // Store in ring buffer
  pktCount++;
  msgHead = (msgHead + 1) % MSG_HISTORY;
  if (msgCount < MSG_HISTORY) msgCount++;
  msgBuf[msgHead].num     = pktCount;
  msgBuf[msgHead].ms      = millis();
  msgBuf[msgHead].rssi    = rssi;
  msgBuf[msgHead].snr     = snr;
  msgBuf[msgHead].payload = incoming;
  lastPktMs = millis();

  // Update per-device tally
  JsonDocument tdoc;
  if (deserializeJson(tdoc, incoming) == DeserializationError::Ok) tallyDevice(tdoc);

  // Serial log
  Serial.printf("\n==== Pkt #%lu  RSSI:%.1f  SNR:%.1f ====\n",
                pktCount, rssi, snr);
  Serial.println(incoming);

  if (viewMode == VIEW_SUMMARY) {
    renderSummary();                           // counts/devices changed
  } else if (viewOff == 0) {
    renderMessage(0);                          // live view: jump to newest
  } else {
    // User is browsing history — keep their message on screen. The head just
    // advanced, so bump viewOff to keep pointing at the same physical message.
    if (viewOff < msgCount - 1) viewOff++;
    drawStatusBar();                           // refresh FROZEN position only
  }

  LED_flicker();                               // notify user of the new packet
}

// ── Main loop ──
void loop() {
  // Heartbeat LED
  static uint32_t lastBlink = 0;
  static bool ledState = false;
  if (millis() - lastBlink >= 500) {
    lastBlink = millis();
    ledState = !ledState;
    digitalWrite(PIN_LED, ledState);
  }

  // ~1Hz tick: advance "age" counters / colours on whichever page is showing
  static uint32_t lastStatusMs = 0;
  if (millis() - lastStatusMs >= 1000) {
    lastStatusMs = millis();
    if (viewMode == VIEW_SUMMARY) refreshSummaryRows();
    drawStatusBar();
  }

  // Two-button navigation: user = next, boot (GPIO0) = previous, double = summary
  serviceButton(btnUser);
  serviceButton(btnBoot);

  // LoRa packet
  if (gotPacket) {
    gotPacket = false;
    handlePacket();
  }
}
