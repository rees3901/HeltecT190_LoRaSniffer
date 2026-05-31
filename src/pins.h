#pragma once

// ── Heltec Vision Master T190 pin map ────────────────────────────────────────

// LoRa SX1262 on FSPI (SPI2) — separate bus from TFT
#define LORA_SCK_PIN    9
#define LORA_MISO_PIN   11
#define LORA_MOSI_PIN   10
#define LORA_NSS_PIN    8
#define LORA_DIO1_PIN   14
#define LORA_RST_PIN    12
#define LORA_BUSY_PIN   13

// Power / backlight — must match TFT_BL build flag (17)
#define PIN_TFT_BL      17
#define PIN_VEXT        46   // 3.3V peripheral rail enable (active HIGH)
#define PIN_VEXT_46     46
#define PIN_VEXT_7      7    // secondary peripheral enable (active LOW)

// User LED
#define PIN_LED         35
