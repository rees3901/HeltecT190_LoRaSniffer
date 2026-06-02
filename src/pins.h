#pragma once

// ── Heltec Vision Master T190 pin map ────────────────────────────────────────
// Reference: Heltec vendor sketch (HT_ST7789spi demo)

// LoRa SX1262 on FSPI (SPI2) — separate bus from TFT which uses HSPI (SPI3)
#define LORA_SCK_PIN    9
#define LORA_MISO_PIN   11
#define LORA_MOSI_PIN   10
#define LORA_NSS_PIN    8
#define LORA_DIO1_PIN   14
#define LORA_RST_PIN    12
#define LORA_BUSY_PIN   13

// TFT power and backlight
// PIN_VEXT_7 = VTFT_CTRL: set LOW to enable display power rail (active LOW)
// PIN_TFT_BL = backlight: set HIGH after display init
#define PIN_VEXT_7      7
#define PIN_TFT_BL      17

// User LED
#define PIN_LED         35

// Packet-activity LED. GPIO0 is the boot strapping pin / boot button; it is
// only driven as an output after boot, so normal boot is unaffected.
#define LORA_LED        0

// User button (active LOW — connects GPIO to GND when pressed)
#define PIN_BTN         21
