#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <RadioLib.h>
#include <ArduinoJson.h>
#include "pins.h"

// ── BluePawz protocol ──
#define LORA_FREQ       915.0
#define LORA_SF         8
#define LORA_BW         250.0
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

// "Collar quiet" warning threshold. The collar's TX cadence is mode-dependent
// (normal 300s, active 60s, lost 30s), so this is a coarse heartbeat hint only.
const uint32_t QUIET_S = 120;

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

// ── Button debounce ──
bool     lastBtnState = HIGH;
uint32_t lastBtnMs    = 0;
const uint32_t DEBOUNCE_MS = 200;

// ── LoRa on HSPI (SPI3) ── TFT uses default SPI (SPI2/FSPI)
SPIClass LoRaSPI(HSPI);
SX1262 lora = new Module(LORA_NSS_PIN, LORA_DIO1_PIN, LORA_RST_PIN, LORA_BUSY_PIN, LoRaSPI);

volatile bool gotPacket = false;
void IRAM_ATTR onLoRaRx() { gotPacket = true; }

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

// ── Bottom status bar: LIVE/FROZEN state, packet age, good/error counts ──
void drawStatusBar() {
  tft.fillRect(0, BODY_BOTTOM, SCREEN_W, STATUS_H, ST77XX_BLACK);
  tft.setTextSize(1);

  char buf[64];
  uint16_t col;
  if (viewOff == 0) {
    uint32_t age = (lastPktMs == 0) ? 0 : (millis() - lastPktMs) / 1000;
    col = (age > QUIET_S) ? ST77XX_RED
        : (age > QUIET_S / 2) ? ST77XX_YELLOW
        : ST77XX_GREEN;
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

// ── Render one message from history ──
void renderMessage(int offset) {
  if (msgCount == 0) return;

  int idx = ((msgHead - offset) % MSG_HISTORY + MSG_HISTORY) % MSG_HISTORY;
  MsgRecord& m = msgBuf[idx];

  tft.fillScreen(ST77XX_BLACK);
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

// ── Setup ──
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("[T190] LoRa Sniffer starting...");

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  pinMode(PIN_BTN, INPUT_PULLUP);

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
  printLine("Btn=cycle history", ST77XX_WHITE);
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
    drawStatusBar();   // reflect error count without disturbing the message view
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

  // Serial log
  Serial.printf("\n==== Pkt #%lu  RSSI:%.1f  SNR:%.1f ====\n",
                pktCount, rssi, snr);
  Serial.println(incoming);

  if (viewOff == 0) {
    renderMessage(0);                          // live view: jump to newest
  } else {
    // User is browsing history — keep their message on screen. The head just
    // advanced, so bump viewOff to keep pointing at the same physical message.
    if (viewOff < msgCount - 1) viewOff++;
    drawStatusBar();                           // refresh FROZEN position only
  }
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

  // Tick the status bar ~1Hz so the "age" counter advances live
  static uint32_t lastStatusMs = 0;
  if (millis() - lastStatusMs >= 1000) {
    lastStatusMs = millis();
    drawStatusBar();
  }

  // Button: cycle history on falling edge with debounce
  bool btnNow = digitalRead(PIN_BTN);
  if (lastBtnState == HIGH && btnNow == LOW) {
    if (millis() - lastBtnMs >= DEBOUNCE_MS && msgCount > 0) {
      lastBtnMs = millis();
      viewOff = (viewOff + 1) % msgCount;
      renderMessage(viewOff);
      Serial.printf("[BTN] viewing msg %d/%d\n", viewOff + 1, msgCount);
    }
  }
  lastBtnState = btnNow;

  // LoRa packet
  if (gotPacket) {
    gotPacket = false;
    handlePacket();
  }
}
