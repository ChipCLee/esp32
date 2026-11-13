# Chip-Scanner — ESP32-C6 Multi-Protocol Spectrum Discovery & Web Hub

Firmware implementation of [design.md](design.md) for the **LAFVIN ESP32-C6 Development Board with 1.47" Color LCD (Model LB008)**.

## Key Features
- **Standalone Wi-Fi 6 SoftAP (`chip-scanner`)**: Hosts a WPA2-PSK secured local Wi-Fi access point (default password: `pw4ggyy!`) and web portal at `http://192.168.4.1` with zero external router dependencies.
- **Password-Protected Web Dashboard**: HTTP Basic Authentication (`admin` / `pw4ggyy!`) with constant-time credential verification and a 5-failure / 5-second brute-force lockout (`NFR-7`).
- **Time-Sliced Multi-Protocol RF Discovery**:
  - **Wi-Fi 4 / Wi-Fi 6 (802.11 b/g/n/ax)**: Channel-interleaved micro-scans (`<= 90ms` off-channel + `100ms` SoftAP home-channel recovery) across channels 1–13.
  - **BLE 5.3 (`NimBLE-Arduino`)**: Active scanning with manufacturer/service payload decoding for **Apple AirTag / FindMy (`0x004C`)**, **Samsung SmartTag (`0xFD5A`/`0x0075`)**, **Tile (`0xFEED`/`0xFEEC`)**, **Xiaomi Mijia (`0x181A`)**, and **Govee (`0xEC88`)**, plus an interactive **BLE GATT Explorer & Characteristic Reader** ([ble.md](ble.md)) to connect, enumerate GATT Services/Characteristics, and read characteristic values (Hex, UTF-8, and decoded Bluetooth SIG values).
  - **Matter BLE (`0xFFF6`)**: Extracts uncommissioned Matter device `Vendor ID (VID)`, `Product ID (PID)`, and 12-bit `Discriminator`.
  - **IEEE 802.15.4 / Zigbee / Thread**: Native `esp_ieee802154` driver with `esp_coex_wifi_i154_enable()` transmitting active Beacon Requests (`0x07`) and promiscuous MAC frame inspection across channels 11–26.
- **1.47" ST7789 Color LCD HUD (`172 × 320`)**:
  - Displays connected web clients (`USERS`), protocol breakdown cards (`Wi-Fi`, `BLE`, `Zigbee`, `Matter`, `Total`), and a live color-coded rolling activity log terminal.
  - Short-pressing `GPIO 9` toggles between the **Split Dashboard View** and **Full-Screen Log Terminal**; holding `GPIO 9` (`> 2s`) toggles the LCD backlight (`GPIO 22`).
- **Shared FSPI Bus Mutex (`spi_bus_mutex`)**: Arbitrates SPI transactions between the ST7789 LCD (`CS=14`) and Micro-SD card logger (`CS=4`).

---

## Directory Structure
```text
lafvin/chip-scanner/
├── chip-scanner.ino       # Arduino CLI / IDE entry wrapper
├── platformio.ini         # PlatformIO build configuration (Arduino-ESP32 v3.x)
├── partitions.csv         # Custom 4MB Flash layout (3.25MB app0 partition)
├── design.md              # Architecture, requirements, and validation specification
├── data/
│   └── index.html         # Single-Page Web Dashboard source
└── src/
    ├── main.cpp           # Hardware init, SPI CS pre-pull, mutexes, FreeRTOS tasks
    ├── types.h            # Pin definitions, data structures, protocol enums
    ├── auth_config.h/.cpp # NVS config persistence & constant-time Basic Auth
    ├── log_buffer.h/.cpp  # Thread-safe 16-entry log buffer & browser RTC sync
    ├── device_table.h/.cpp# 100-device circular cache with protocol counters
    ├── scanner_wifi.h/.cpp# SoftAP host + channel-interleaved Wi-Fi scanner
    ├── scanner_ble.h/.cpp # NimBLE 5.3 + Smart Tracker & Matter 0xFFF6 decoder
    ├── scanner_154.h/.cpp # Native IEEE 802.15.4 Beacon Request & promiscuous sniffer
    ├── rf_scheduler.h/.cpp# FreeRTOS time-sliced RF coexistence state machine
    ├── display_manager.h/.cpp # ST7789 172x320 partial-redraw HUD + GPIO 9 button
    ├── sd_logger.h/.cpp   # Mutex-guarded Micro-SD CSV logger & export fallback
    ├── led_status.h/.cpp  # WS2812 RGB status indicator (GPIO 8)
    ├── web_assets.h       # GZIP-compressed PROGMEM bundle of data/index.html
    └── web_server.h/.cpp  # Password-protected HTTP server & JSON REST API
```

---

## Building & Flashing

### Option A: Arduino CLI
Because the triple-radio stack (Wi-Fi 6 + NimBLE 5.3 + IEEE 802.15.4) compiles to **~1.49 MB (`1,558,314 bytes`)**, use the included custom `partitions.csv` (`3.25 MB` `app0` partition) by passing `upload.maximum_size=3407872` (or `PartitionScheme=huge_app`):
```bash
arduino-cli compile \
  --fqbn esp32:esp32:esp32c6 \
  --build-property upload.maximum_size=3407872 \
  lafvin/chip-scanner

arduino-cli upload \
  -p /dev/cu.usbmodem* \
  --fqbn esp32:esp32:esp32c6 \
  lafvin/chip-scanner
```

### Option B: PlatformIO
```bash
cd lafvin/chip-scanner
pio run
pio run --target upload
```

---

## Verified Resource Footprint (ESP32-C6)
- **Flash Usage**: `1,586,504 bytes` (`46.5%` of `3,407,872 bytes` `app0` partition)
- **Static SRAM Usage**: `91,060 bytes` (`27.8%` of `327,680 bytes`, leaving **`236,620 bytes` free** for heap/stacks)
- **Embedded Web UI (`index.html.gz`)**: `10,162 bytes` (`PROGMEM`, compressed from `37,563 bytes`)

