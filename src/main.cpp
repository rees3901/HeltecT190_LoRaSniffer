#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
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

// ── Display ──
TFT_eSPI tft;
int scrollY = 0;
const int LINE_H = 16;
const int SCREEN_W = 320;  // landscape
const int SCREEN_H = 170;
const int MAX_LINES = SCREEN_H / LINE_H;

// ── LoRa radio on its own SPI bus ──
SPIClass LoRaSPI(FSPI);
SX1262 lora = new Module(LORA_NSS_PIN, LORA_DIO1_PIN, LORA_RST_PIN, LORA_BUSY_PIN, LoRaSPI);

volatile bool gotPacket = false;
void IRAM_ATTR onLoRaRx() {
  gotPacket = true;
}

uint32_t pktCount = 0;


void printLine(const char* text, uint16_t color = TFT_WHITE) {
  if (scrollY >= MAX_LINES * LINE_H) {
    tft.fillScreen(TFT_BLACK);
    scrollY = 0;
  }
  tft.setTextColor(color, TFT_BLACK);
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

  // VTFT_CTRL: enable display power rail before SPI init (vendor sequence)
  pinMode(PIN_VEXT_7, OUTPUT);
  digitalWrite(PIN_VEXT_7, LOW);
  delay(20);

  // ── TFT init ──
  tft.init();
  tft.setRotation(1);  // landscape: 320x170
  tft.setSwapBytes(true);
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(1);
  tft.setTextFont(2);

  // Backlight ON after display init (matches vendor sequence)
  pinMode(PIN_TFT_BL, OUTPUT);
  digitalWrite(PIN_TFT_BL, HIGH);

  printLine("BluePawz LoRa Sniffer", TFT_CYAN);
  printLine("Heltec T190 VisionMaster", TFT_CYAN);
  printLinef(TFT_YELLOW, "%.0fMHz SF%d BW%.0fk", LORA_FREQ, LORA_SF, LORA_BW);
  printLine("Waiting for packets...", TFT_GREEN);

  // ── LoRa init on FSPI ──
  LoRaSPI.begin(LORA_SCK_PIN, LORA_MISO_PIN, LORA_MOSI_PIN, LORA_NSS_PIN);

  int state = lora.begin(LORA_FREQ);
  if (state != RADIOLIB_ERR_NONE) {
    printLinef(TFT_RED, "LoRa FAIL: %d", state);
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

  printLine("Radio OK - listening", TFT_GREEN);
  Serial.println("[LoRa] Sniffer ready");
}

void handlePacket() {
  String incoming;
  int state = lora.readData(incoming);

  // Re-arm receiver immediately
  lora.startReceive();

  if (state != RADIOLIB_ERR_NONE) {
    if (state == RADIOLIB_ERR_CRC_MISMATCH) {
      printLine("CRC error", TFT_RED);
    } else {
      printLinef(TFT_RED, "RX err: %d", state);
    }
    return;
  }

  pktCount++;
  float rssi = lora.getRSSI();
  float snr = lora.getSNR();

  // Serial output (full detail)
  Serial.println();
  Serial.println("======== LoRa Packet ========");
  Serial.printf("[RF] RSSI: %.1f dBm  SNR: %.1f dB  #%lu\n", rssi, snr, pktCount);

  // TFT: header line with RF info
  tft.fillScreen(TFT_BLACK);
  scrollY = 0;
  printLinef(TFT_YELLOW, "#%lu RSSI:%.0f SNR:%.1f", pktCount, rssi, snr);

  // Try to parse as JSON
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, incoming);

  if (!err) {
    // Pretty-print JSON to serial
    serializeJsonPretty(doc, Serial);
    Serial.println();

    // Display key-value pairs on TFT
    JsonObject obj = doc.as<JsonObject>();
    for (JsonPair kv : obj) {
      char line[64];
      if (kv.value().is<float>() || kv.value().is<double>()) {
        snprintf(line, sizeof(line), "%s: %.4f", kv.key().c_str(), kv.value().as<float>());
      } else if (kv.value().is<int>()) {
        snprintf(line, sizeof(line), "%s: %d", kv.key().c_str(), kv.value().as<int>());
      } else if (kv.value().is<bool>()) {
        snprintf(line, sizeof(line), "%s: %s", kv.key().c_str(), kv.value().as<bool>() ? "true" : "false");
      } else if (kv.value().is<const char*>()) {
        snprintf(line, sizeof(line), "%s: %s", kv.key().c_str(), kv.value().as<const char*>());
      } else {
        snprintf(line, sizeof(line), "%s: [...]", kv.key().c_str());
      }
      printLine(line, TFT_WHITE);

      if (scrollY >= MAX_LINES * LINE_H) break;
    }
  } else {
    // Not JSON — display raw
    Serial.println("[RAW] " + incoming);
    printLine("(raw packet)", TFT_MAGENTA);

    // Word-wrap raw data across lines
    int pos = 0;
    while (pos < (int)incoming.length() && scrollY < MAX_LINES * LINE_H) {
      String chunk = incoming.substring(pos, pos + 40);
      printLine(chunk.c_str(), TFT_WHITE);
      pos += 40;
    }
  }

  Serial.println("=============================");
}

void loop() {
  // Heartbeat — blinks LED every 500 ms to confirm firmware is running
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
