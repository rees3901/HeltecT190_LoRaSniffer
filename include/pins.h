#pragma once

// Heltec Vision Master T190 — pin assignments from official factory test
// Source: HelTecAutomation/Heltec_ESP32 Vision_Master_T190_FactoryTest.ino

// ── Power control ──
#define PIN_VEXT        5   // Vext rail enable (HIGH = on)
#define PIN_TFT_BL     17  // TFT backlight enable (HIGH = on)
#define PIN_VEXT_46    46  // Additional power rail
#define PIN_VEXT_7      7  // Inverted power control (LOW = on)

// ── TFT ST7789 (HSPI) ──
#define TFT_SCK_PIN    38
#define TFT_MOSI_PIN   48
#define TFT_CS_PIN     39
#define TFT_DC_PIN     47
#define TFT_RST_PIN    40

// ── LoRa SX1262 (separate SPI) ──
#define LORA_SCK_PIN    9
#define LORA_MOSI_PIN  10
#define LORA_MISO_PIN  11
#define LORA_NSS_PIN    8
#define LORA_RST_PIN   12
#define LORA_DIO1_PIN  14
#define LORA_BUSY_PIN  13

// ── I2C (sensor header) ──
#define I2C_SDA_PIN     2
#define I2C_SCL_PIN     1

// ── User button ──
#define USER_BUTTON_PIN 0
