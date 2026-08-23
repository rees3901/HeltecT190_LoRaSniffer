# LoRa Plain Payload Sniffer T190 Handover Notes

## Purpose

This firmware turns a Heltec Vision Master T190 into a receive-only LoRa packet
monitor for the BluePawz collar/base-station link. It receives packets using the
same LoRa PHY settings, prints each payload to USB serial, and displays JSON or
raw payloads on the built-in 170 x 320 ST7789 TFT.

Use this only to monitor radio traffic you own or are authorised to inspect.

## Hardware

- Heltec Vision Master T190
- ESP32-S3
- SX1262-compatible LoRa radio
- ST7789 170 x 320 TFT
- Correct 868 MHz antenna connected before using the radio
- USB data cable for programming and serial monitoring

The display and LoRa radio use separate SPI buses. Do not reuse the original
prototype TFT pins `9`, `10`, `11`, `12`, and `14`: those belong to the LoRa
radio on this board.

## Pin Configuration

### TFT ST7789

| Function | GPIO | Notes |
|---|---:|---|
| SCLK | 38 | TFT SPI clock |
| MOSI | 48 | TFT SPI data |
| MISO | Not connected | Configured as `-1` |
| CS | 39 | Chip select |
| DC | 47 | Data/command |
| RST | 40 | Display reset |
| Backlight | 17 | Active HIGH; also used for packet-arrival flicker |
| TFT power control | 7 | Active LOW |

### LoRa SX1262

| Function | GPIO |
|---|---:|
| SCLK | 9 |
| MOSI | 10 |
| MISO | 11 |
| NSS/CS | 8 |
| RESET | 12 |
| BUSY | 13 |
| DIO1/IRQ | 14 |

### Controls and Indicator

| Function | GPIO | Notes |
|---|---:|---|
| User button | 21 | Active LOW, next/older page |
| Boot/PRG button | 0 | Active LOW, previous/newer page |
| Heartbeat output | 35 | Toggled every 500 ms |

GPIO0 remains the ESP32-S3 boot-strapping pin. It is safe to read as a normal
button after startup, but holding it while resetting or powering the board can
enter firmware-download mode.

Pin definitions are in `src/pins.h`; TFT definitions are currently in
`src/main.cpp`.

## LoRa PHY Settings

All communicating radios must use matching PHY settings:

| Parameter | Value |
|---|---:|
| Frequency | 868.0 MHz |
| Spreading factor | SF9 |
| Bandwidth | 125.0 kHz |
| Coding rate | 4/5 (`5` in RadioLib) |
| Preamble | 16 symbols |
| Sync word | `0x12` |
| CRC | Enabled |

These constants are at the top of `src/main.cpp`. If the collar or base station
changes, update this sniffer to match. Frequency, bandwidth, spreading factor,
coding rate, preamble, sync word, CRC, and IQ mode must be compatible for valid
packet reception.

## PlatformIO Setup

The repository already contains `platformio.ini`:

```ini
[env:heltec_t190]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino

monitor_speed = 115200

board_build.flash_mode = dio
board_build.flash_size = 8MB
board_build.filesystem = littlefs

build_flags =
  -DARDUINO_USB_MODE=1
  -DARDUINO_USB_CDC_ON_BOOT=1

lib_deps =
  adafruit/Adafruit ST7735 and ST7789 Library@^1.11.0
  adafruit/Adafruit GFX Library@^1.11.9
  jgromes/RadioLib@^6.6.0
  bblanchon/ArduinoJson@^7.2.0
```

Open the repository folder in VS Code with PlatformIO installed, then use the
PlatformIO Build, Upload, and Monitor commands. From a terminal:

```powershell
pio run
pio run --target upload
pio device monitor --baud 115200
```

If `pio` is not on `PATH` on the current Windows machine, use:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run --target upload
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" device monitor --baud 115200
```

If automatic upload fails, hold the GPIO0 Boot/PRG button, tap Reset, release
Boot, and upload again. Do not hold GPIO0 during a normal startup.

## Operation

On startup the TFT reports the radio parameters and then shows
`Radio OK - listening`. Received packets are printed to serial and stored in a
10-entry RAM ring buffer.

### Button Navigation

- Single press User (GPIO21): next/older message.
- Single press Boot (GPIO0): previous/newer message.
- Double press either button: open the summary page.
- From the summary page, single press either button: return to message history.
- Single presses are resolved after a 350 ms double-press window.

History wraps at either end. `LIVE` means the newest packet is displayed;
`FROZEN` means an older packet is being inspected. A newly received packet does
not pull the display away from a frozen message.

### Packet Display

- `UP TLM` (green): parsed JSON without a `cmd` field; treated as collar uplink.
- `DN CMD` (orange): parsed JSON containing `cmd`; treated as base-to-collar
  downlink.
- `? RAW` (magenta): payload could not be parsed as JSON.
- Header includes packet number, RSSI, and SNR.
- Bottom bar shows view state, last-packet age, good-packet count, and error
  count.
- Each good packet flashes the TFT backlight five times (50 ms off/on), then
  leaves the backlight on. This blocks the main loop for about 500 ms.

### Summary Page

The summary shows:

- Frequency, spreading factor, and bandwidth.
- Good-packet and receive-error totals.
- Up to six device identities, per-device packet counts, and last-seen age.
- Green: seen less than 5 minutes ago.
- Yellow: seen 5 to 10 minutes ago.
- Red: last seen more than 10 minutes ago.

Device identity is extracted in this order:

1. JSON `id` (normal telemetry).
2. JSON `device` (status, pong, and acknowledgement packets).
3. Integer JSON `device_id`, displayed as `#NNN`.

Packets without an identity still increment the total count. All history and
summary counters are held in RAM and reset when the board restarts.

## Code Map

- `platformio.ini`: board, USB serial, and library configuration.
- `src/pins.h`: LoRa pins, buttons, TFT power/backlight, and heartbeat output.
- `src/main.cpp`: radio setup, TFT rendering, history, summary, button handling,
  packet parsing, and notification behavior.

Key values that may be adjusted in `src/main.cpp`:

- `LORA_FREQ`, `LORA_SF`, `LORA_BW`, `LORA_CR`, `LORA_PREAMBLE`,
  `LORA_SYNC_WORD`: radio settings.
- `MSG_HISTORY`: number of packets retained; currently 10.
- `MAX_DEVICES`: summary rows retained; currently 6.
- `FRESH_GREEN_S` and `FRESH_YELLOW_S`: summary freshness thresholds.
- `QUIET_S`: coarse status-bar warning threshold; currently 120 seconds.
- `DEBOUNCE_MS` and `DBL_CLICK_MS`: button timing.

## Troubleshooting

### TFT is blank

- Confirm GPIO7 is driven LOW before TFT initialisation.
- Confirm GPIO17 is driven HIGH after TFT initialisation.
- Confirm the TFT uses pins 38/48/39/47/40, not the LoRa pins.
- Check that `tft.init(170, 320)`, rotation `1`, and display inversion are set.

### `LoRa FAIL` appears

- Check the SX1262 pin table above.
- Confirm the antenna is connected.
- Power-cycle the board after flashing.
- Read the numeric RadioLib error on the TFT or serial monitor.

### No packets arrive

- Verify every PHY setting against the transmitter and receiver.
- Confirm this is an 868 MHz T190 radio variant with an 868 MHz antenna.
- Place the sniffer near the transmitter for the first test.
- Confirm the collar is transmitting plain LoRa packets rather than LoRaWAN.
- Encrypted or application-encoded payloads can be received but cannot be shown
  as readable JSON without the matching decoder/key.

### Buttons behave incorrectly

- Both buttons are active LOW and use `INPUT_PULLUP`.
- A single action is intentionally delayed by approximately 350 ms.
- Do not hold GPIO0 while resetting unless entering download mode intentionally.

## Known Limitations

- Receive-only monitor; it does not forward or modify packets.
- Keeps only the latest 10 messages and six device rows.
- Long JSON payloads are truncated when the available TFT body rows are full.
- Counters and history are not persisted across reboot.
- The backlight notification blocks processing for approximately 500 ms. At a
  high packet rate this could delay servicing another packet or button press.
- Direction detection is heuristic: the presence of `cmd` means downlink.
