#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <RadioLib.h>
#include <ArduinoJson.h>
#include "pins.h"

// ── BluePawz protocol — must match transmitter/receiver config.h ──
#define LORA_FREQ       915.0
#define LORA_SF         8
#define LORA_BW         250.0
#define LORA_CR         5
#define LORA_PREAMBLE   16
#define LORA_SYNC_WORD  0x12

// ── TFT pins (from Heltec T190 vendor sketch) ──
#define TFT_MISO  -1
#define TFT_MOSI  48
#define TFT_SCLK  38
#define TFT_CS    39
#define TFT_DC    47
#define TFT_RST   40
#define TFT_BL    17
#define TFT_VCtrl  7   // VTFT_CTRL: LOW = display power on

// ── Display ──
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
int scrollY = 0;
const int LINE_H  = 16;
const int SCREEN_W = 320;
const int SCREEN_H = 170;
const int MAX_LINES = SCREEN_H / LINE_H;  // 10 lines

// ── LoRa radio on HSPI (SPI3) — TFT uses the default SPI (SPI2/FSPI) ──
SPIClass LoRaSPI(HSPI);
SX1262 lora = new Module(LORA_NSS_PIN, LORA_DIO1_PIN, LORA_RST_PIN, LORA_BUSY_PIN, LoRaSPI);

volatile bool gotPacket = false;
void IRAM_ATTR onLoRaRx() { gotPacket = true; }

uint32_t pktCount = 0;

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

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("[T190] LoRa Sniffer starting...");

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  // Enable display power rail (active LOW)
  pinMode(TFT_VCtrl, OUTPUT);
  digitalWrite(TFT_VCtrl, LOW);

  // Configure SPI2 (FSPI) with TFT pins, then init display
  SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.init(170, 320);
  tft.setRotation(1);       // landscape: 320x170
  tft.invertDisplay(true);  // required for T190 panel
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(2);       // 12x16px per char — matches LINE_H=16
  tft.setTextWrap(false);

  // Backlight ON after display init (vendor sequence)
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  printLine("BluePawz LoRa Sniffer", ST77XX_CYAN);
  printLine("Heltec T190", ST77XX_CYAN);
  printLinef(ST77XX_YELLOW, "%.0fMHz SF%d BW%.0fk", LORA_FREQ, LORA_SF, LORA_BW);
  printLine("Waiting for packets...", ST77XX_GREEN);
  Serial.println("[TFT] init done");

  // ── LoRa init on HSPI (SPI3) ──
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

void handlePacket() {
  String incoming;
  int state = lora.readData(incoming);

  lora.startReceive();

  if (state != RADIOLIB_ERR_NONE) {
    if (state == RADIOLIB_ERR_CRC_MISMATCH) {
      printLine("CRC error", ST77XX_RED);
    } else {
      printLinef(ST77XX_RED, "RX err: %d", state);
    }
    return;
  }

  pktCount++;
  float rssi = lora.getRSSI();
  float snr  = lora.getSNR();

  Serial.println();
  Serial.println("======== LoRa Packet ========");
  Serial.printf("[RF] RSSI: %.1f dBm  SNR: %.1f dB  #%lu\n", rssi, snr, pktCount);

  tft.fillScreen(ST77XX_BLACK);
  scrollY = 0;
  printLinef(ST77XX_YELLOW, "#%lu RSSI:%.0f SNR:%.1f", pktCount, rssi, snr);

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, incoming);

  if (!err) {
    serializeJsonPretty(doc, Serial);
    Serial.println();

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
        snprintf(line, sizeof(line), "%s:[...]", kv.key().c_str());
      }
      printLine(line, ST77XX_WHITE);
      if (scrollY >= MAX_LINES * LINE_H) break;
    }
  } else {
    Serial.println("[RAW] " + incoming);
    printLine("(raw)", ST77XX_MAGENTA);
    int pos = 0;
    while (pos < (int)incoming.length() && scrollY < MAX_LINES * LINE_H) {
      String chunk = incoming.substring(pos, pos + 26);
      printLine(chunk.c_str(), ST77XX_WHITE);
      pos += 26;
    }
  }

  Serial.println("=============================");
}

void loop() {
  static uint32_t lastBlink = 0;
  static bool ledState = false;
  if (millis() - lastBlink >= 500) {
    lastBlink = millis();
    ledState = !ledState;
    digitalWrite(PIN_LED, ledState);
  }

  if (gotPacket) {
    gotPacket = false;
    handlePacket();
  }
}
