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
const int LINE_H   = 16;
const int SCREEN_W = 320;
const int SCREEN_H = 170;
const int MAX_LINES = SCREEN_H / LINE_H;  // 10 lines

// ── Message history ring buffer ──
struct MsgRecord {
  uint32_t num;
  float    rssi, snr;
  String   payload;
};

const int MSG_HISTORY = 10;
MsgRecord msgBuf[MSG_HISTORY];
int      msgCount  = 0;   // messages stored so far (caps at MSG_HISTORY)
int      msgHead   = -1;  // ring buffer index of newest message
int      viewOff   = 0;   // 0 = newest, 1 = one older, etc.
uint32_t pktCount  = 0;

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

// ── Render one message from history ──
void renderMessage(int offset) {
  if (msgCount == 0) return;

  int idx = ((msgHead - offset) % MSG_HISTORY + MSG_HISTORY) % MSG_HISTORY;
  MsgRecord& m = msgBuf[idx];

  tft.fillScreen(ST77XX_BLACK);
  scrollY = 0;

  // Header: packet info + position in history
  // e.g.  "#42 R:-87 S:3.1  1/7"
  printLinef(ST77XX_YELLOW, "#%lu R:%.0f S:%.1f  %d/%d",
             m.num, m.rssi, m.snr, offset + 1, msgCount);

  // Parse payload
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, m.payload);

  if (!err) {
    JsonObject obj = doc.as<JsonObject>();
    for (JsonPair kv : obj) {
      char line[64];
      if (kv.value().is<float>() || kv.value().is<double>()) {
        snprintf(line, sizeof(line), "%s:%.4f", kv.key().c_str(), kv.value().as<float>());
      } else if (kv.value().is<int>()) {
        snprintf(line, sizeof(line), "%s:%d", kv.key().c_str(), kv.value().as<int>());
      } else if (kv.value().is<bool>()) {
        snprintf(line, sizeof(line), "%s:%s", kv.key().c_str(), kv.value().as<bool>() ? "true" : "false");
      } else if (kv.value().is<const char*>()) {
        snprintf(line, sizeof(line), "%s:%s", kv.key().c_str(), kv.value().as<const char*>());
      } else {
        snprintf(line, sizeof(line), "%s:[?]", kv.key().c_str());
      }
      printLine(line, ST77XX_WHITE);
      if (scrollY >= MAX_LINES * LINE_H) break;
    }
  } else {
    // Raw payload — wrap at 26 chars (textSize 2, 320px wide)
    printLine("(raw)", ST77XX_MAGENTA);
    int pos = 0;
    while (pos < (int)m.payload.length() && scrollY < MAX_LINES * LINE_H) {
      String chunk = m.payload.substring(pos, pos + 26);
      printLine(chunk.c_str(), ST77XX_WHITE);
      pos += 26;
    }
  }
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
    if (state == RADIOLIB_ERR_CRC_MISMATCH) {
      printLine("CRC error", ST77XX_RED);
    } else {
      printLinef(ST77XX_RED, "RX err: %d", state);
    }
    return;
  }

  // Store in ring buffer
  pktCount++;
  msgHead = (msgHead + 1) % MSG_HISTORY;
  if (msgCount < MSG_HISTORY) msgCount++;
  msgBuf[msgHead].num     = pktCount;
  msgBuf[msgHead].rssi    = rssi;
  msgBuf[msgHead].snr     = snr;
  msgBuf[msgHead].payload = incoming;

  // Serial log
  Serial.printf("\n==== Pkt #%lu  RSSI:%.1f  SNR:%.1f ====\n",
                pktCount, rssi, snr);
  Serial.println(incoming);

  // Snap view to newest and render
  viewOff = 0;
  renderMessage(0);
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
