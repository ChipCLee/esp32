# esp32

Technical documentation, projects, and reference guides for ESP32 development boards.

## Boards & Project Documentation

### 1. Ideaspark ESP32 (0.96" OLED & CH340)
* [Ideaspark ESP32 Reference Guide](ideaspark/ideaspark-esp32-oled.md) — Hardware specs, SSD1306 display pinouts, and Arduino Wi-Fi scanner examples.

### 2. LAFVIN ESP32-C6 (1.47" Color LCD, Model LB008)
* [LAFVIN ESP32-C6 Reference Guide](lafvin/lafvin-esp32-c6-lcd.md) — Next-gen Wi-Fi 6, BLE 5.3, Zigbee/Thread 802.15.4 specs, ST7789 display setup, and NeoPixel code.
* [Chip-Scanner Firmware (`lafvin/chip-scanner`)](lafvin/chip-scanner/README.md) — Complete multi-protocol RF spectrum discovery firmware (Wi-Fi 6, BLE 5.3, Matter, IEEE 802.15.4 / Zigbee), onboard 1.47" ST7789 HUD, and password-protected web dashboard (`SSID: chip-scanner`).
* [System Design: Multi-Protocol Spectrum Discovery & Web Hub](lafvin/chip-scanner/design.md) — Architectural requirements, time-sliced RF coexistence scheduler, embedded web server, deliverables, and validation plan.

---

## Comparison Matrix

| Specification | Ideaspark ESP32 OLED | LAFVIN ESP32-C6 LCD (LB008) |
| :--- | :--- | :--- |
| **Core Architecture** | Dual-Core Xtensa LX6 (240 MHz) | Single-Core RISC-V HP (160 MHz) + LP (20 MHz) |
| **Display Form Factor** | 0.96" Monochrome OLED (~2.44 cm) | 1.47" Color IPS LCD (~3.73 cm) |
| **Display Resolution** | 128 × 64 pixels | 172 × 320 pixels |
| **Display Interface** | I2C (`GPIO 21`, `GPIO 22`) | SPI (`GPIO 6, 7, 14, 15, 21, 22`) |
| **Wireless Protocol** | Wi-Fi 4 (802.11 b/g/n) + BLE 4.2 | Wi-Fi 6 (802.11ax) + BLE 5.3 + Zigbee/Thread |
| **USB Interface** | Micro-USB (via WCH CH340 Bridge) | Type-C (Native USB Serial/JTAG) |
| **External Storage** | None onboard | Integrated Micro-SD (TF) Card Slot |
| **Onboard RGB** | None (standard power/user LEDs) | 1× Addressable WS2812 RGB LED (`GPIO 8`) |
| **Best Suited For** | Basic telemetry, compact IoT monitors, crypto tickers | Modern smart home Matter bridges, LVGL graphical dashboards |
