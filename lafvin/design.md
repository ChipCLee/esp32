# System Design Document: ESP32-C6 Multi-Protocol Spectrum Discovery & Web Hub

## 1. Executive Summary & Objectives

The goal of this project is to develop an autonomous, standalone RF device discovery station and web dashboard deployed on the **LAFVIN ESP32-C6 Development Board with 1.47" Color LCD (Model LB008)**.

The system leverages the ESP32-C6's integrated **2.4 GHz multi-protocol transceiver** to concurrently detect, fingerprint, and monitor nearby wireless devices across **Wi-Fi 6 (802.11ax/b/g/n)**, **Bluetooth Low Energy (BLE 5.3)**, **IEEE 802.15.4 (Zigbee 3.0 & Thread)**, and **Matter** ecosystems.

The device operates entirely without external cloud infrastructure:
1. It hosts a standalone **Wi-Fi SoftAP** and embedded HTTP web server.
2. The web dashboard and REST APIs are secured with **password authentication** (default password: `pw4ggyy!`).
3. Any client device (smartphone, tablet, laptop) connecting to the access point can navigate to `http://192.168.4.1`, authenticate, and inspect/filter nearby RF emitters.
4. The onboard **1.47" IPS color screen** and **WS2812 RGB LED** provide real-time hardware telemetry, connected user count, protocol breakdown stats, and a live rolling log terminal.
5. Optional offline captures are written to an onboard **Micro-SD (TF) card**.

---

## 2. Requirements Traceability Matrix

### 2.1 Functional Requirements (FR)

| ID | Requirement Category | Specification Details |
| :--- | :--- | :--- |
| **FR-1** | **Wi-Fi SoftAP & Web Server** | Broadcast a standalone 802.11ax/n Access Point (`SSID: chip-scanner`, default IP: `192.168.4.1`) with a lightweight embedded HTTP server serving a single-page responsive dashboard. |
| **FR-2** | **Wi-Fi Device Discovery** | Perform periodic active/passive channel scans across 2.4 GHz channels 1–13. Capture SSID, BSSID (MAC), RSSI, channel, encryption type (Open, WPA2, WPA3), and 802.11 generation (Wi-Fi 4/5/6). |
| **FR-3** | **BLE 5.3 Discovery** | Execute active BLE scanning cycles to discover BLE peripherals, beacons, smart trackers (Apple AirTags, Tile, SmartTags), and environmental sensors (e.g., Govee/Mijia broadcasting temperature/humidity in manufacturer data payloads). |
| **FR-4** | **IEEE 802.15.4 / Zigbee Discovery** | Scan channels 11–26 for 802.15.4 energy levels, beacon requests, and active Zigbee/Thread networks. Extract PAN IDs, Extended PAN IDs, channels, and LQI (Link Quality Indication). |
| **FR-5** | **Matter Uncommissioned Discovery** | Detect BLE Matter Commissioning advertisements (Service UUID `0xFFF6`) and decode Vendor ID (VID), Product ID (PID), and 12-bit device discriminators. |
| **FR-6** | **Time-Sliced RF Coexistence** | Implement a deterministic FreeRTOS scheduler that shares the single 2.4 GHz RF frontend among Wi-Fi AP transmission, BLE scanning, and 802.15.4 listening without dropping web client sockets. |
| **FR-7** | **RESTful API & Real-time Updates** | Expose JSON endpoints (`/api/devices`, `/api/stats`, `/api/scan`, `/api/logs`) with optional Server-Sent Events (SSE) or polling for real-time frontend refreshes. |
| **FR-8** | **Hardware Screen UX (Onboard LCD)** | Render a comprehensive real-time status dashboard on the 1.47" ST7789 IPS LCD (172×320):<br>1. **Connection & System Status:** SoftAP SSID, IP address, and **live count of connected Wi-Fi users** (`WiFi.softAPgetStationNum()`).<br>2. **Protocol Breakdown Counters:** Distinct visual counters for detected **Wi-Fi, BLE, Zigbee/802.15.4, and Matter** devices, plus total unique tally.<br>3. **Recent Activity Logs:** A live rolling log window displaying the latest 5–6 system and discovery events with timestamps and protocol badges.<br>4. **Display Mode Toggle:** Support switching between Split Dashboard view and Full-Screen Event Log view via the User Button (`GPIO 9`). |
| **FR-9** | **RGB Visual Diagnostics (WS2812)** | Use the onboard addressable LED (`GPIO 8`) to display scan states: Blue (BLE scan), Purple (802.15.4 scan), Green (Wi-Fi AP serving), and Red (error/buffer overflow). |
| **FR-10** | **Local SD Logging** | Record discovery events and timestamps in CSV format onto the onboard Micro-SD card slot for offline RF site-survey analysis. |
| **FR-11** | **Dashboard Password Protection** | Require authentication to access the web dashboard (`/`) and all management REST endpoints (`/api/*`).<br>• **Default Username:** `admin`<br>• **Default Password:** `pw4ggyy!`<br>Unauthenticated requests must be rejected with HTTP 401 Unauthorized (via standard HTTP Basic Auth or session login challenge). |

### 2.2 Non-Functional Requirements (NFR)

* **NFR-1 (Stability & Memory):** Sustained free HP SRAM $> 90\text{ KB}$ under heavy web client loads; zero memory leaks over 48 hours of continuous round-robin scanning.
* **NFR-2 (Web Latency):** Web dashboard first-load time $< 1.2\text{ seconds}$; REST API response latency $< 120\text{ ms}$.
* **NFR-3 (Coexistence Reliability):** SoftAP beacon loss $< 2\%$ during active BLE and 802.15.4 scan windows.
* **NFR-4 (Self-Contained & Cloudless):** 100% functionality with zero internet connectivity. All CSS/JS assets bundled in Flash (PROGMEM or LittleFS).
* **NFR-5 (LCD Refresh Performance):** Screen redraw latency $< 40\text{ ms}$ with partial bounding-box updates to prevent visible display flicker and minimize SPI contention with the SD card.
* **NFR-6 (Security Boundary):** Password verification performed in constant time to mitigate timing attacks; unauthenticated brute-force attempts rate-limited (5-second lockout after 5 consecutive failures).
* **NFR-7 (Graceful Degradation):** Automatic detection of missing Micro-SD card or LCD failure without blocking wireless scanning.

---

## 3. System Architecture & Detailed Design

### 3.1 Hardware Topology & Pin Allocation

```mermaid
flowchart TD
    subgraph ESP32-C6 SoC
        CPU[32-bit RISC-V Core @ 160 MHz]
        RAM[512 KB HP SRAM]
        RADIO[2.4 GHz Multi-Protocol Transceiver]
    end

    RADIO -->|Wi-Fi 6 AP| WEB[Web Browser Clients - Auth: pw4ggyy!]
    RADIO -->|BLE 5.3 Scan| BLE_DEV[Nearby BLE / Trackers / Sensors]
    RADIO -->|802.15.4 Scan| ZB_DEV[Zigbee / Thread Networks]

    CPU -->|SPI: GPIO 6, 7, 14, 15, 21, 22| LCD[1.47-in IPS LCD ST7789]
    CPU -->|SPI: GPIO 6, 7, 5, 4| SD[Micro-SD Card Slot]
    CPU -->|RMT / Single-Wire: GPIO 8| LED[WS2812 Addressable RGB LED]
    CPU -->|GPIO 9| BTN[User / Boot Button]
```

#### Pin Assignment Table
| Subsystem | Signal Name | ESP32-C6 GPIO | Hardware Notes |
| :--- | :--- | :--- | :--- |
| **LCD Display (ST7789)** | SPI MOSI | `GPIO 6` | Shared SPI Bus |
| | SPI SCK | `GPIO 7` | Shared SPI Bus |
| | LCD CS | `GPIO 14` | Active Low Chip Select |
| | LCD DC | `GPIO 15` | Data / Command Control |
| | LCD RST | `GPIO 21` | Hardware Reset |
| | LCD BL | `GPIO 22` | Backlight PWM control |
| **Micro-SD (TF) Card** | SD MOSI | `GPIO 6` | Shared SPI Bus |
| | SD SCK | `GPIO 7` | Shared SPI Bus |
| | SD MISO | `GPIO 5` | SPI Master-In Slave-Out |
| | SD CS | `GPIO 4` | Independent SD Chip Select |
| **Addressable RGB** | WS2812 DIN | `GPIO 8` | Driven by hardware RMT peripheral |
| **User Input** | User/Boot Button| `GPIO 9` | Active-low button to trigger manual scan / toggle display view |

---

### 3.2 RF Coexistence & Time-Slicing Scheduler

Because the ESP32-C6 features a **single physical RF frontend**, concurrent operation requires a round-robin time-division multiplexing (TDM) scheme managed by FreeRTOS:

```mermaid
stateDiagram-v2
    [*] --> SoftAP_Idle : System Init Complete
    
    state "Wi-Fi AP Window (Serve Auth Web Clients)" as SoftAP_Idle
    state "BLE Scan Window (Active BLE 5.3)" as BLE_Scan
    state "Wi-Fi Spectrum Scan (AP + Background Scan)" as WiFi_Scan
    state "802.15.4 Scan Window (Zigbee / Thread)" as IEEE_Scan
    state "Data Merge & SD Flush" as Data_Sync

    SoftAP_Idle --> BLE_Scan : Timer (Every 10s)
    BLE_Scan --> SoftAP_Idle : Window Timeout (1.5s)
    SoftAP_Idle --> WiFi_Scan : Timer (Next Slot)
    WiFi_Scan --> SoftAP_Idle : Async Scan Complete (1.0s)
    SoftAP_Idle --> IEEE_Scan : Timer (Next Slot)
    IEEE_Scan --> SoftAP_Idle : Window Timeout (1.5s)
    SoftAP_Idle --> Data_Sync : New Entities Detected
    Data_Sync --> SoftAP_Idle : Buffer Cleared
```

* **Slot 1 (Wi-Fi AP Master, continuous base):** Maintains 802.11 DTIM beacons and handles authenticated HTTP requests from connected phones/PCs.
* **Slot 2 (BLE Scan Window, $\approx 1.5\text{s}$):** Hardware switches to BLE RX. Discovered advertisement payloads are pushed to an in-memory thread-safe queue.
* **Slot 3 (Wi-Fi Channel Scan, $\approx 1.0\text{s}$):** The Wi-Fi driver issues non-blocking passive/active probe requests on channels 1–13 while preserving the AP connection.
* **Slot 4 (802.15.4 Sniff Window, $\approx 1.5\text{s}$):** Radio switches to 802.15.4 packet reception to capture Zigbee beacon frames, channel energy (ED), and PAN IDs.
* **Slot 5 (Aggregator & Deduplicator):** Merges duplicates, updates device RSSI and last-seen timestamps, pushes telemetry and new log entries to the LCD, and optionally appends records to `discovery.csv` on the SD card.

---

### 3.3 Firmware Software Architecture

```
                    ┌────────────────────────────────────────┐
                    │               FreeRTOS                 │
                    └──────────────────┬─────────────────────┘
                                       │
    ┌────────────────────────┬─────────┴──────────────┬────────────────────────┐
    ▼                        ▼                        ▼                        ▼
[Task 1: Core Web & Auth] [Task 2: RF Scanner]    [Task 3: Display UX]    [Task 4: Logger]
- Priority: 3            - Priority: 2           - Priority: 1           - Priority: 1
- Stack: 4 KB            - Stack: 6 KB           - Stack: 3 KB           - Stack: 3 KB
- HTTP Auth Filter       - WiFi / BLE / 15.4     - ST7789 UI Loop        - Micro-SD SPI
- Static Assets          - Deduplication Table   - WS2812 RMT Control    - Circular buffer
- REST API Handlers      - Circular Ring Buffer  - Log Stream Renderer   - Flush to CSV
```

---

### 3.4 On-board LCD UI/UX Design & Layout (1.47" ST7789)

The ST7789 color display is configured at **$172 \times 320$ pixels** in portrait mode with a **34-pixel column offset**.

```
┌──────────────────────────────────────────────┐ 0 px
│  ESP32-C6 SPECTRUM HUB         [STATE: IDLE] │
│  SSID: chip-scanner            IP: 192.168.4.1│
│  Connected Users: 2 👥         Auth: ACTIVE 🔒│
├──────────────────────────────────────────────┤ 50 px
│ DETECTED DEVICES BREAKDOWN                   │
│ ┌──────────────────────┬───────────────────┐ │
│ │ 📶 Wi-Fi:   14       │ 🔵 BLE:     32    │ │
│ ├──────────────────────┼───────────────────┤ │
│ │ ⚡ Zigbee:   6       │ 🌐 Matter:   2    │ │
│ └──────────────────────┴───────────────────┘ │
│ Total Unique Emitters: 54                    │
├──────────────────────────────────────────────┤ 145 px
│ RECENT ACTIVITY LOGS                         │
│ [14:21:05] [BLE]  AirTag nearby (-64 dBm)    │
│ [14:21:08] [AP]   User auth OK: 192.168.4.2  │
│ [14:21:12] [15.4] Zigbee PanID 0x1A2B ch15   │
│ [14:21:15] [WIFI] New AP: "Lab_Net" (-48dBm) │
│ [14:21:19] [MAT]  Matter uncommissioned dev  │
│ [14:21:22] [BLE]  Mijia Temp: 22.8C 45%RH    │
└──────────────────────────────────────────────┘ 320 px
```

#### Screen Layout Specifications
1. **Header Zone ($Y: 0 \rightarrow 48\text{ px}$):**
   * Displays SoftAP name, server IP address (`192.168.4.1`), **live Connected Users count** (`WiFi.softAPgetStationNum()`), and Lock status (`🔒`).
2. **Protocol Breakdown Matrix ($Y: 49 \rightarrow 144\text{ px}$):**
   * 2×2 grid of colored metric cards: **Wi-Fi** (Blue), **BLE** (Cyan), **Zigbee** (Purple), and **Matter** (Amber).
3. **Rolling Log Terminal ($Y: 145 \rightarrow 320\text{ px}$):**
   * Renders the last 6 events from `lcd_log_ring_buffer` including successful/failed authentication events (`[AP] User auth OK / Failed`).
4. **Display Mode Toggle (Button `GPIO 9`):**
   * **Short Press:** Toggles between **Standard Dashboard** and **Full Terminal Mode** (14 lines of continuous historical logs).
   * **Long Press ($>2\text{s}$):** Toggles display backlight on/off (`GPIO 22`).

---

### 3.5 Web Frontend & Authentication Architecture

#### Access Control & Security Specification
* **Mechanism:** HTTP Basic Authentication (`Authorization: Basic <base64>`) enforced across all HTTP routes and REST API endpoints.
* **Credentials:**
  * **Username:** `admin`
  * **Default Password:** `pw4ggyy!`
* **Implementation Pattern in `ESPAsyncWebServer`:**
  ```cpp
  const char* http_username = "admin";
  const char* http_password = "pw4ggyy!"; // Default password

  bool check_auth(AsyncWebServerRequest *request) {
      if (!request->authenticate(http_username, http_password)) {
          request->requestAuthentication("ESP32-C6 Scanner Web Hub");
          return false;
      }
      return true;
  }
  ```
* **Security Behavior:**
  * Unauthenticated requests receive `HTTP 401 Unauthorized` with header `WWW-Authenticate: Basic realm="ESP32-C6 Scanner Web Hub"`.
  * The browser automatically prompts the user with a native, secure login modal.
  * Once authenticated, credentials are cached by the browser session for all subsequent REST calls (`/api/devices`, `/api/stats`, `/api/logs`).
  * Unauthorized REST queries receive `{ "error": "Unauthorized", "code": 401 }`.

#### REST API Endpoints (All Protected)

| Endpoint | Method | Response Format | Purpose |
| :--- | :--- | :--- | :--- |
| `/` | `GET` | `text/html` (Gzipped) | Serves the single-page application dashboard (requires auth). |
| `/api/devices` | `GET` | `application/json` | Returns array of all discovered devices in memory with timestamps and RSSI. |
| `/api/stats` | `GET` | `application/json` | System telemetry: Free heap, uptime, Wi-Fi channel, **connected users count**, and counts per protocol. |
| `/api/logs` | `GET` | `application/json` | Returns the recent event logs array matching the on-screen LCD log terminal. |
| `/api/scan` | `POST` | `application/json` | Manually triggers an out-of-order scan for a specified protocol (`wifi`, `ble`, `15.4`). |
| `/api/clear` | `POST` | `application/json` | Flushes the in-memory discovery cache and log buffer. |
| `/api/export.csv`| `GET` | `text/csv` | Streams the session's discovery log directly from the SD card. |

---

## 4. Deliverables & Project Breakdown

### 4.1 Project Directory Structure

```
esp32-c6-multiscanner/
├── platformio.ini               # Build configuration, board defs & library dependencies
├── src/
│   ├── main.cpp                 # System initialization & FreeRTOS task orchestration
│   ├── rf_scheduler.h/cpp       # Time-sliced RF coexistence engine
│   ├── scanner_wifi.h/cpp       # Wi-Fi AP + Background scanning modules
│   ├── scanner_ble.h/cpp        # BLE 5.3 Scanner + Beacon/Sensor decoder
│   ├── scanner_154.h/cpp        # IEEE 802.15.4 & Zigbee energy/beacon sniffer
│   ├── device_table.h/cpp       # Thread-safe in-memory cache, aging, and deduplication
│   ├── log_buffer.h/cpp         # Circular log buffer for LCD and REST API
│   ├── auth_config.h            # Default credentials ("admin", "pw4ggyy!") & NVS handler
│   ├── web_server.h/cpp         # Async WebServer with HTTP Basic Auth filter
│   ├── display_manager.h/cpp    # ST7789 display renderer (status, counters & rolling log)
│   ├── led_status.h/cpp         # WS2812 RMT driver and color state machine
│   └── sd_logger.h/cpp          # Micro-SD SPI file writer and CSV exporter
├── data/
│   └── index.html               # Minified Single-Page Web Dashboard (served from SPIFFS/PROGMEM)
└── docs/
    └── design.md                # This system architecture and specification document
```

### 4.2 Key Software Libraries

1. **`Arduino_GFX_Library` (Moon On Our Nation):** Optimized ST7789 display driver with custom offset handling.
2. **`ESPAsyncWebServer` + `AsyncTCP`:** High-performance asynchronous HTTP engine that handles client connections and Basic Auth without stalling background RF tasks.
3. **`ArduinoJson` (v7):** Zero-allocation JSON serialization for REST API responses.
4. **`Adafruit_NeoPixel`:** Simple, reliable control for the onboard WS2812 RGB LED (`GPIO 8`).
5. **`esp_zb_` & `esp_ieee802154_` (ESP-IDF / Arduino ESP32 core):** Low-level driver access for 802.15.4 frame reception.

---

## 5. Implementation Roadmap (Phases & Milestones)

```mermaid
flowchart LR
    M1["M1: Web Core<br>SoftAP + Auth + Web UI"] --> M2["M2: Dual Scanner<br>Wi-Fi + BLE Engine"]
    M2 --> M3["M3: 802.15.4<br>Zigbee/Thread Sniffer"]
    M3 --> M4["M4: Hardware UX<br>ST7789 LCD + WS2812"]
    M4 --> M5["M5: Integration<br>SD Logging + Security Soak"]
```

* **Milestone 1 (M1) — Foundation & Security:** Implement Wi-Fi SoftAP (`192.168.4.1`), HTTP Basic Auth filter (`admin:pw4ggyy!`), and minimal authenticated HTML dashboard.
* **Milestone 2 (M2) — Dual Scanner & API:** Add asynchronous Wi-Fi scanning and BLE 5.3 scanner. Implement `/api/devices` and `/api/stats` JSON responses and frontend auto-refresh table.
* **Milestone 3 (M3) — 802.15.4 / Zigbee Engine:** Integrate IEEE 802.15.4 channel energy scanning and beacon parsing. Add Zigbee tab to web UI.
* **Milestone 4 (M4) — Hardware HUD & Feedback:** Program 1.47" ST7789 screen with live **connected users count, protocol counts matrix, and rolling activity log window**; connect WS2812 LED to reflect real-time scan modes.
* **Milestone 5 (M5) — Storage & Production Hardening:** Add Micro-SD CSV logging, button display toggle (`GPIO 9`), memory leak verification, and 48-hour soak testing.

---

## 6. Verification & Validation Plan

### 6.1 Test Matrix & Acceptance Criteria

| Test ID | Test Category | Procedure / Input | Acceptance Criteria |
| :--- | :--- | :--- | :--- |
| **TC-01** | **SoftAP Stability** | Connect 3 mobile devices to `chip-scanner` and browse dashboard concurrently. | All 3 devices load dashboard in $< 2\text{s}$; zero disconnects during scans. |
| **TC-02** | **Wi-Fi Detection** | Place device near 2.4 GHz routers (WPA2, WPA3, Open). | Detects $\ge 90\%$ of local 2.4 GHz APs within 2 scan cycles. |
| **TC-03** | **BLE Tracking** | Bring an Apple AirTag and a BLE temperature sensor within 5 meters. | Displays MAC/Payload, identifies tag type, and reads temperature in $< 5\text{s}$. |
| **TC-04** | **Zigbee Detection** | Trigger pairing mode on an IKEA/Tuya Zigbee bulb or sensor. | Captures 802.15.4 beacon request, displays PAN ID and channel on web UI and LCD. |
| **TC-05** | **Onboard LCD UX** | 1. Connect and disconnect client phones to the SoftAP.<br>2. Trigger discovery of new BLE/Wi-Fi devices.<br>3. Press User Button (`GPIO 9`). | 1. **Connected Users** count increments/decrements live on LCD.<br>2. **Protocol counters** match `/api/devices` count.<br>3. **Recent logs window** renders new event entries with timestamp and color badge with $< 40\text{ms}$ refresh and zero flicker.<br>4. Button press cleanly toggles between Dashboard HUD and Full Terminal Log views. |
| **TC-06** | **Memory Soak Test** | Run continuous round-robin scanning for 24 hours under continuous HTTP polling. | Free heap does not degrade below $80\text{ KB}$; zero panic/watchdog resets. |
| **TC-07** | **SD Card Hot-Plug** | Power on without SD card, insert card mid-run, check `/api/export.csv`. | System does not freeze; logs resume gracefully upon insertion. |
| **TC-08** | **Dashboard Authentication** | 1. Connect to SoftAP and open `http://192.168.4.1` without entering password.<br>2. Enter incorrect password.<br>3. Enter default credentials (`admin` / `pw4ggyy!`).<br>4. Attempt curl to `/api/devices` with/without Basic Auth header. | 1. Browser prompts with HTTP 401 Basic Auth dialog.<br>2. Re-prompts on incorrect credentials; REST calls return 401.<br>3. Correct password `pw4ggyy!` grants full dashboard access.<br>4. Failed attempts trigger log entry on LCD: `[AP] Auth failed: <IP>`. |

### 6.2 Sign-off Criteria
The project will be deemed complete when **TC-01 through TC-08** pass unconditionally on the physical LAFVIN ESP32-C6 (LB008) hardware module.
