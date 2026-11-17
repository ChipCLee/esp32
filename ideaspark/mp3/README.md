# Ideaspark ESP32 Wi-Fi MP3 Player & Bluetooth `A2DP Source` Hub

Firmware implementation of [design.md](design.md) for the **Ideaspark ESP32 Development Board (`ESP32-WROOM-32` with 0.96" SSD1306 OLED & CH340)**.

## Features
- **Local Wi-Fi Web Portal (`ideaspark-mp3` / `pw4ggyy!`)**: Hosts a standalone SoftAP at `http://192.168.4.1` with a responsive web dashboard.
- **MP3 Uploader & `LittleFS` Library**: Stream `.mp3` files directly from your browser into the custom `2.062 MB` `LittleFS` Flash partition with automatic **ID3v1 / ID3v2** Title & Artist extraction.
- **Bluetooth Speaker Discovery & Pairing**: Scan for nearby Bluetooth Classic (`BR/EDR`) speakers, pair/connect via `BluetoothA2DPSource`, and remember the paired speaker in NVS.
- **Web & Hardware Playback Controls**: Play, Pause, Skip Next/Previous, and adjust Volume from the web page, or use the onboard **right-side `BOOT` button (`GPIO 0`)** for Play/Pause (short press) and Next Track (long press `> 1.5s`).
- **0.96" OLED Live HUD (`128 × 64`)**: Displays Bluetooth status, Play/Pause state, scrolling Track Title, Artist Name, and real-time Left/Right Stereo Audio VU Meter bars with peak-hold ticks.

---

## Build & Deployment (PlatformIO)

1. **Install PlatformIO CLI** (if not installed):
   ```bash
   brew install platformio
   ```
2. **Compile Firmware**:
   ```bash
   cd ideaspark/mp3
   pio run
   ```
3. **Upload Firmware to Ideaspark ESP32**:
   ```bash
   pio run --target upload
   ```
   *(If flashing stalls at `Connecting........`, hold the right-side **`BOOT` button (`GPIO 0`)** for 1–2 seconds until upload begins.)*
4. **Open Serial Monitor**:
   ```bash
   pio device monitor -b 115200
   ```
