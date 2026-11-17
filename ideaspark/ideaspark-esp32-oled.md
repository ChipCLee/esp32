# Ideaspark ESP32 Development Board (0.96" OLED & CH340)

Technical documentation, original manuals/datasheet references, hardware specifications, pinouts, and code examples for the **Ideaspark ESP32 Development Board with Integrated "2.44" (0.96") OLED Display & CH340**.

> **Clarification on "2.44 OLED":**
> E-commerce listings (AliExpress, Amazon, Temu) frequently list this item with a metric translation: **$0.96\text{ inches} \times 2.54\text{ cm/inch} \approx 2.4384\text{ cm} \approx 2.44\text{ cm}$**. The onboard display is an industry-standard **0.96-inch monochrome I2C OLED** (SSD1306 driver, 128×64 resolution).

---

## 1. Original Documents, Datasheets & Drivers

* **Board Overview & Guides:**
  * [Manuals+ Ideaspark ESP32 0.96-inch OLED User Manual](https://manuals.plus/m/7a82b86c125df93a6bfcfb71e1b12b504ba488d8b99c7198ba16892558f62f3f)
  * [Device.report Ideaspark ESP32 OLED Product Reference (ASIN: B0CN4F354N)](https://device.report/asin/B0CN4F354N)
  * [Device.report Ideaspark Hardware Database](https://device.report/ideaspark)
* **SoC Datasheet & Technical Manuals (Espressif):**
  * [ESP32-WROOM-32 Series Datasheet (PDF)](https://www.espressif.com/sites/default/files/documentation/esp32-wroom-32_datasheet_en.pdf)
  * [ESP32 Technical Reference Manual (PDF)](https://www.espressif.com/sites/default/files/documentation/esp32_technical_reference_manual_en.pdf)
* **USB-to-UART Bridge Driver (WCH CH340):**
  * [Official WCH CH340/CH341 USB to Serial Drivers (Windows, macOS, Linux)](https://www.wch-ic.com/downloads/CH341SER_ZIP.html)
* **OLED Controller Datasheet:**
  * [Solomon Systech SSD1306 Datasheet (PDF)](https://cdn-shop.adafruit.com/datasheets/SSD1306.pdf)

---

## 2. Hardware Features & Pinout

### Core Specifications
* **Microcontroller:** Espressif ESP32-WROOM-32
  * **CPU:** Tensilica Xtensa Dual-Core 32-bit LX6 up to 240 MHz
  * **Memory:** 520 KB internal SRAM, 4 MB SPI Flash
  * **Wireless:** 802.11 b/g/n Wi-Fi (up to 150 Mbps) + Bluetooth v4.2 BR/EDR and BLE
* **Display Subsystem:**
  * **Type:** 0.96" Monochrome OLED (Yellow/Blue or White on Black)
  * **Resolution:** 128 × 64 pixels
  * **Driver IC:** SSD1306
  * **Interface:** Hardware I2C (hardwired directly on the PCB)
* **Connectivity & Power:**
  * **USB Interface:** Micro-USB port for programming and 5V power
  * **USB-to-Serial:** WCH CH340 / CH340C chip
  * **Voltage Regulation:** Onboard 3.3V Low-Dropout (LDO) regulator
  * **Battery Support:** Many variants include a 2-pin JST connector for 3.7V Lithium batteries with charging IC

### Pinout Mapping

| Function | ESP32 GPIO | Description |
| :--- | :--- | :--- |
| **OLED SDA** | `GPIO 21` | I2C Data line (Internal trace to SSD1306) |
| **OLED SCL** | `GPIO 22` | I2C Clock line (Internal trace to SSD1306) |
| **I2C Address** | `0x3C` (default) or `0x3D` | Hexadecimal device address |
| **Boot Button** | `GPIO 0` | Download / Boot Mode switch (Right side next to USB; usable as user input after boot) |
| **EN / RST** | Reset pin | Hardware reset button (Left side next to USB) |
| **Onboard LED** | `GPIO 2` | Active-HIGH blue status LED |
| **UART0 (USB)** | `TX = GPIO 1`, `RX = GPIO 3` | Connected to onboard WCH CH340 USB-to-Serial bridge |
| **UART2** | `RX2 = GPIO 16`, `TX2 = GPIO 17` | Hardware Serial 2 for external GPS/sensors |
| **SPI (`VSPI` / `HSPI`)** | `VSPI`: `23/19/18/5` / `HSPI`: `13/12/14/15` | Hardware SPI buses (`MOSI/MISO/SCK/CS`) |
| **True Analog DAC** | `DAC1 = GPIO 25`, `DAC2 = GPIO 26` | Dual 8-bit true analog voltage/audio outputs |
| **Capacitive Touch** | `T0–T9` (`GPIO 4,0,2,15,13,12,14,27,33,32`) | 10 hardware capacitive touch channels |
| **Breakout GPIOs** | Full 30/36-pin header | Standard 3.3V logic GPIOs (not 5V-tolerant) |

---

### Inside the `ESP32-WROOM-32` Module & Hardware Pin Rules

The metal RF-shielded **`ESP32-WROOM-32`** module soldered onto the Ideaspark board integrates:
1. **`ESP32-D0WDQ6` / `ESP32-D0WD-V3` SoC**: Two 32-bit Tensilica Xtensa LX6 CPU cores (`PRO_CPU` Core 0 for Wi-Fi/BT protocol stacks and `APP_CPU` Core 1 for Arduino `setup()`/`loop()`) + an Ultra-Low-Power (ULP) co-processor (`~10 µA` in Deep Sleep).
2. **4 MB Quad-SPI Flash Chip**: Wired internally to `GPIO 6`–`GPIO 11`.
3. **40 MHz Crystal Oscillator & 2.4 GHz MIFA PCB Antenna**.

#### Important Hardware Pin Rules ("Gotchas")
* **Input-Only Pins (`GPIO 34`, `35`, `36/VP`, `39/VN`)**: Cannot be configured as `OUTPUT` and have **no internal pull-up (`INPUT_PULLUP`) or pull-down resistors**. Ideal for `ADC1` analog inputs or externally pulled-up signals.
* **Reserved Internal Flash Pins (`GPIO 6` – `GPIO 11`)**: Connected directly to the internal SPI Flash chip—never wire external circuitry to these pins.
* **Boot Strapping Pins (`GPIO 0`, `2`, `5`, `12`, `15`)**: Sampled at reset to select boot mode and flash regulator voltage (`GPIO 12` must not be pulled `HIGH` at power-up).
* **`ADC2` vs. Wi-Fi Conflict**: The `ADC2` peripheral is used internally by the Wi-Fi driver. Whenever Wi-Fi is active, always use **`ADC1` pins (`GPIO 32`, `33`, `34`, `35`, `36`, `39`)** for `analogRead()`.

---

### Physical Button Layout & Functions (`RST` vs. `BOOT`)

When holding the board upright so the **0.96" OLED text is right-side up** (with the **USB port at the bottom**):

```
      ┌───────────────────────────┐
      │   [  ESP32-WROOM-32  ]    │  ◄── Metal RF Shield / Antenna (Top)
      │                           │
      │  ┌─────────────────────┐  │
      │  │   0.96" OLED Screen │  │
      │  │      (128 x 64)     │  │
      │  └─────────────────────┘  │
      │                           │
  EN  │ [RST/EN]         [BOOT]   │  D0 / D2
  pin │  Button          Button   │  side
      └─────────[  USB  ]─────────┘  ◄── USB Port (Bottom)
       LEFT SIDE        RIGHT SIDE
```

1. **Left Button — `RST` / `EN` (Hardware Reset)**:
   * Located on the same edge as the **`EN` / `VP` / `VN` / `3V3`** header pins.
   * Pulling `CHIP_PU` (`EN`) `LOW` immediately hard-resets the processor and restarts your sketch from `setup()`. Cannot be read as a software input.
2. **Right Button — `BOOT` / `FLASH` (`GPIO 0`)**:
   * Located on the same edge as the **`D0` / `D2` / `D21` (`SDA`) / `D22` (`SCL`)** header pins.
   * **At Reset**: Holding `BOOT` while tapping `RST` forces the chip into **UART Firmware Download Mode**.
   * **After Boot (In Your Code)**: Acts as a **free active-LOW user push-button** (`pinMode(0, INPUT_PULLUP);` — reads `LOW` when pressed, `HIGH` when released).

---

### Bluetooth Specifications & Capabilities (Dual-Mode v4.2)

Unlike newer Espressif chips (`ESP32-C3`, `ESP32-S3`, `ESP32-C6`) which only support Bluetooth Low Energy (BLE), the `ESP32-WROOM-32` on the Ideaspark board features a **Dual-Mode Bluetooth v4.2 radio** supporting both **Bluetooth Classic (`BR/EDR`)** and **Bluetooth Low Energy (`BLE 4.2`)**:

| Parameter | Specification |
| :--- | :--- |
| **Bluetooth Standard** | Bluetooth v4.2 BR/EDR (Classic) and BLE (Dual-Mode) |
| **RF Frequency Band** | 2.4 GHz ISM Band (`2402 MHz – 2480 MHz`) |
| **Transmit Power** | Class-1, Class-2, and Class-3 transmitter: **up to `+9 dBm`** (programmable `-12 dBm` to `+9 dBm` in `3 dB` steps) |
| **Receiver Sensitivity** | **NZIF receiver with `-97 dBm` BLE sensitivity** (`-94 dBm` @ Basic Rate `1 Mbps`, `-90 dBm` @ EDR `2 Mbps`, `-84 dBm` @ EDR `3 Mbps`) |
| **Modulation** | `GFSK` (1 Mbps Basic Rate & BLE), `π/4-DQPSK` (2 Mbps EDR), `8-DPSK` (3 Mbps EDR) |
| **Frequency Hopping** | Adaptive Frequency Hopping (`AFH`) across 79 channels (1 MHz spacing, Classic) or 40 channels (2 MHz spacing, BLE) |
| **Audio Codecs & Interfaces** | CVSD and SBC audio codecs in hardware; internal 8-bit DACs (`GPIO 25` / `GPIO 26`) or external `I2S` DAC |
| **Host Stacks (Arduino)** | **Bluedroid** (Dual-mode Classic + BLE, `<BluetoothSerial.h>` / `<BLEDevice.h>`, `~130 KB` RAM) or **NimBLE-Arduino** (BLE-only lightweight stack, `<NimBLEDevice.h>`, `< 35 KB` RAM) |

#### Supported Bluetooth Profiles & Roles
1. **Bluetooth Classic (`BR/EDR`) Profiles**:
   * **SPP (Serial Port Profile)**: Wireless UART serial link (`<BluetoothSerial.h>`) up to `~1–2 Mbps` with Android, Windows, macOS, and Linux.
   * **A2DP (Advanced Audio Distribution Profile)**: Wireless stereo audio **Sink** (Bluetooth speaker) and **Source** (Bluetooth audio transmitter).
   * **AVRCP (Audio/Video Remote Control Profile)**: Play/Pause/Track/Volume metadata and media controls.
   * **Classic HID Host/Device**: Connects directly to Bluetooth Classic gamepads (PS3, PS4, Wii, Switch Pro) via libraries like `Bluepad32`.
2. **Bluetooth Low Energy (`BLE 4.2`) Roles**:
   * **GATT Server (Peripheral)**: Exposes custom Services & Characteristics or acts as a **BLE HID Keyboard/Mouse/Gamepad** (`ESP32-BLE-Keyboard`).
   * **GATT Client (Central)**: Connects to BLE peripherals/sensors to discover and read/write GATT characteristics.
   * **Observer (Scanner) & Broadcaster (Beacon)**: Scans for nearby BLE advertisements (`iBeacon`, `Eddystone`, AirTags, thermometers) or broadcasts beacon payloads.

#### Deep-Dive: What Bluetooth Audio (`A2DP` & `AVRCP`) Can Do
* **`A2DP` (Advanced Audio Distribution Profile)** streams stereo 44.1 kHz 16-bit audio (`SBC` codec) in two directions:
  1. **`A2DP Sink` (ESP32 as Bluetooth Speaker / Receiver)**: Pair your phone or PC to the Ideaspark board and:
     * **Direct Analog Output**: Route stereo audio straight out of the built-in 8-bit DACs on **`GPIO 25` (Left)** and **`GPIO 26` (Right)** into headphones or an amp with zero external DAC ICs.
     * **Digital `I2S` Output**: Stream 16-bit digital audio to an external `I2S` amplifier/DAC (`MAX98357A` or `PCM5102A`).
     * **Standalone OLED Music Visualizer**: Process the incoming 44.1 kHz audio samples in software (`FFT` or peak detection) to draw a live dancing spectrum analyzer or stereo VU meter on the 0.96" OLED with zero external parts.
  2. **`A2DP Source` (ESP32 as Bluetooth Audio Transmitter)**: Pair the ESP32 directly with **Bluetooth headphones, AirPods, or a Bluetooth speaker** to transmit synthesized tones, flash-stored `.wav` alerts, or live `I2S` microphone audio.
* **`AVRCP` (Audio/Video Remote Control Profile)** runs alongside `A2DP` to provide two-way metadata and media control:
  1. **Live "Now Playing" Metadata on the 0.96" OLED**: Receives the current **Track Title**, **Artist**, **Album**, and **Play/Pause/Volume state** from Spotify, Apple Music, or YouTube Music on your phone.
  2. **Media Remote Control**: Sends `play()`, `pause()`, `next()`, `previous()`, and `volume_up()`/`volume_down()` commands back to your phone (e.g., using the onboard **`BOOT` button on `GPIO 0`**).

---

### Wi-Fi Power-Save Modes (`listen_interval` & Deep Sleep vs. Wi-Fi 6 TWT)

While **Target Wake Time (TWT)** is exclusive to Wi-Fi 6 (`802.11ax`) chips (`ESP32-C6`) paired with a Wi-Fi 6 router, the Ideaspark `ESP32-WROOM-32` supports two power-saving modes that work on **any** Wi-Fi 4/5/6 router:

| Power-Save Method | Router Required | Keeps Wi-Fi & IP Alive? | Typical Sleep Window | Supported On |
| :--- | :--- | :--- | :--- | :--- |
| **Target Wake Time (`iTWT`)** | **Wi-Fi 6 (`802.11ax`) only** | ✅ Yes (Router buffers packets until TWT SP) | `1s` up to `hours` | **ESP32-C6** |
| **`WIFI_PS_MAX_MODEM` (`listen_interval`)** | **Any router** (Wi-Fi 4/5/6) | ✅ Yes (Router buffers packets across N beacons) | `100 ms` – `3 seconds` | **All ESP32s** (`WROOM-32`, `C3`, `C6`) |
| **Deep Sleep (`esp_deep_sleep_start`)** | **Any router** (Wi-Fi 4/5/6) | ❌ No (Reconnects on wake, `~10 µA`) | `Seconds` up to `days` | **All ESP32s** (`WROOM-32`, `C3`, `C6`) |

```cpp
// Enable Extended Modem Sleep on ESP32-WROOM-32 (Router buffers incoming packets between wake beacons)
#include <WiFi.h>
#include <esp_wifi.h>

void enableExtendedModemSleep() {
  wifi_config_t cfg;
  esp_wifi_get_config(WIFI_IF_STA, &cfg);
  cfg.sta.listen_interval = 10; // Wake up every 10 router beacons (~1.0 second)
  esp_wifi_set_config(WIFI_IF_STA, &cfg);
  esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
}
```

---

### Comparison: Ideaspark (`ESP32-WROOM-32`) vs. LAFVIN (`ESP32-C6`)

| Feature / Subsystem | Ideaspark ESP32 (`ESP32-WROOM-32`) | LAFVIN (`ESP32-C6` 1.47" LCD) |
| :--- | :--- | :--- |
| **CPU Architecture** | **Dual-Core Tensilica Xtensa 32-bit LX6** @ 240 MHz | **Single-Core 32-bit RISC-V** @ 160 MHz (+ 20 MHz LP RISC-V core) |
| **Memory** | 520 KB SRAM / 4 MB Flash | 512 KB HP SRAM (+ 16 KB LP SRAM) / 4 MB Flash |
| **Wi-Fi Standard** | **Wi-Fi 4** (802.11 b/g/n) | **Wi-Fi 6** (802.11 ax/b/g/n, supports **Target Wake Time / TWT**) |
| **Bluetooth Version** | **Bluetooth 4.2 Dual-Mode (Classic `BR/EDR` + `BLE`)** | **Bluetooth 5.3 (`BLE` Only — No Bluetooth Classic)** |
| **Bluetooth Classic (`SPP` Serial)** | ✅ **Yes** (`<BluetoothSerial.h>`) | ❌ No |
| **Bluetooth Audio (`A2DP` / `AVRCP`)** | ✅ **Yes** (Stereo Sink & Source) | ❌ No |
| **Classic Gamepad Host (`PS3`/`PS4`)** | ✅ **Yes** (`Bluepad32` / `PS4Controller`) | ❌ No (BLE HID controllers only) |
| **BLE GATT Server / Client / HID** | ✅ **Yes** (BLE 4.2) | ✅ **Yes** (BLE 5.3) |
| **BLE 5.0+ Long Range & High Speed** | ❌ No (`1M PHY` only, 31-byte legacy adv) | ✅ **Yes** (`1M`, `2M`, `Coded PHY` Long Range, 251B Extended Adv) |
| **Zigbee 3.0 / Thread (802.15.4)** | ❌ No | ✅ **Yes** (`esp_ieee802154` hardware radio) |
| **Matter Support** | Matter over Wi-Fi + BLE Commissioning | Matter over **Thread** or **Wi-Fi 6** + BLE Commissioning |
| **Onboard Display** | **0.96" Monochrome I2C OLED** (`SSD1306`, 128×64, `SDA=21, SCL=22`) | **1.47" IPS Color SPI LCD** (`ST7789`, 172×320 16-bit RGB, `CS=14`) |
| **Onboard Storage & RGB LED** | None (Single blue LED on `GPIO 2`) | **Micro-SD (TF) Slot** (`CS=4`) + **WS2812 RGB LED** (`GPIO 8`) |
| **True Analog Output (`DAC`)** | ✅ **Yes** (2 × 8-bit DACs on `GPIO 25` & `GPIO 26`) | ❌ No |
| **Capacitive Touch Inputs** | ✅ **Yes** (10 channels, `T0`–`T9`) | ❌ No |
| **Arduino Board / FQBN** | `esp32:esp32:esp32` (**ESP32 Dev Module**) | `esp32:esp32:esp32c6` (**ESP32C6 Dev Module**) |

---

## 3. Use Cases & Practical Examples

### Primary Use Cases
* **Wi-Fi Environment Scanner & Packet Monitor:** Displaying network status, RSSI signal strength, and connected clients on-screen without tethering to a computer.
* **Mini IoT Desktop Clock / Weather Station:** Fetching NTP time and OpenWeather API data over Wi-Fi and presenting local metrics.
* **Cryptocurrency / Stock Ticker:** Compact desktop display fetching real-time price feeds via REST APIs.
* **Hardware System Monitor:** Displaying CPU load, GPU temperatures, and memory utilization sent via serial from a PC.

---

### Example 1: Arduino IDE Setup & OLED Display Test

#### Prerequisites (Libraries)
In Arduino IDE, open **Sketch > Include Library > Manage Libraries...** and install:
1. `Adafruit GFX Library`
2. `Adafruit SSD1306`

#### Source Code
```cpp
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C // Change to 0x3D if 0x3C does not initialize

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

void setup() {
  Serial.begin(115200);

  // Initialize I2C with Ideaspark onboard pins
  Wire.begin(21, 22);

  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("SSD1306 allocation failed. Check I2C address!"));
    for(;;);
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("Ideaspark ESP32"));
  display.println(F("---------------------"));
  display.printf("CPU Freq: %d MHz\n", ESP.getCpuFreqMHz());
  display.printf("Flash: %d MB\n", ESP.getFlashChipSize() / (1024 * 1024));
  display.println(F("Status: Ready!"));
  display.display();
}

void loop() {
  // Loop execution
}
```

---

### Example 2: Wi-Fi Scanner with Live OLED Readout

```cpp
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
}

void loop() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("Scanning Wi-Fi..."));
  display.display();

  int n = WiFi.scanNetworks();
  display.clearDisplay();
  display.setCursor(0, 0);

  if (n == 0) {
    display.println(F("No networks found"));
  } else {
    display.printf("Found %d networks:\n", n);
    // Display up to 5 networks on screen
    for (int i = 0; i < n && i < 5; ++i) {
      display.printf("%d: %-10.10s (%d)\n", i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i));
    }
  }
  display.display();
  delay(5000);
}
```

---

### Example 3: Bluetooth Classic Serial (`SPP`) to OLED Display

Pair your laptop or phone with **`Ideaspark-ESP32`** and send any text message over Bluetooth Serial to display it live on the 0.96" OLED screen:

```cpp
#include <BluetoothSerial.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

BluetoothSerial SerialBT;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);

  SerialBT.begin("Ideaspark-ESP32");

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("BT Classic SPP Ready"));
  display.println(F("Name: Ideaspark-ESP32"));
  display.println(F("---------------------"));
  display.println(F("Waiting for text..."));
  display.display();
}

void loop() {
  if (SerialBT.available()) {
    String msg = SerialBT.readStringUntil('\n');
    msg.trim();

    Serial.printf("[BT RX]: %s\n", msg.c_str());
    SerialBT.printf("ACK: %s\n", msg.c_str());

    display.clearDisplay();
    display.setCursor(0, 0);
    display.println(F("BT Message Received:"));
    display.println(F("---------------------"));
    display.println(msg);
    display.display();
  }
}
```

---

### Example 4: Bluetooth Audio (`A2DP` + `AVRCP`) "Now Playing" OLED Display & Stereo VU Meter

Using the [`ESP32-A2DP` library](https://github.com/pschatzmann/ESP32-A2DP) (compile with Partition Scheme **`Huge APP (3MB No OTA/1MB SPIFFS)`**), this sketch turns the Ideaspark board—with zero external wiring—into a Bluetooth audio receiver that displays the **Song Title, Artist, and a live Left/Right stereo VU meter** on the 0.96" OLED, and uses the **`BOOT` button (`GPIO 0`)** to Play/Pause your phone:

```cpp
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "BluetoothA2DPSink.h"

Adafruit_SSD1306 display(128, 64, &Wire, -1);
BluetoothA2DPSink a2dp_sink;

String currentTitle  = "Waiting for music...";
String currentArtist = "Pair: Ideaspark-Audio";
volatile int peakLeft = 0;
volatile int peakRight = 0;
bool isPlaying = false;

// 1. AVRCP Callback: Receives Track Title & Artist from Phone
void avrc_metadata_callback(uint8_t id, const uint8_t *text) {
  if (id == ESP_AVRC_MD_ATTR_TITLE) {
    currentTitle = String((const char*)text);
  } else if (id == ESP_AVRC_MD_ATTR_ARTIST) {
    currentArtist = String((const char*)text);
  }
}

// 2. A2DP Audio Stream Callback: Computes Live Left/Right VU Meter Peaks
void read_data_stream(const uint8_t *data, uint32_t length) {
  const int16_t *samples = (const int16_t*)data;
  uint32_t sampleCount = length / 2; // 16-bit stereo interleaved (L, R, L, R...)
  int maxL = 0, maxR = 0;

  for (uint32_t i = 0; i + 1 < sampleCount; i += 2) {
    int l = abs(samples[i]);
    int r = abs(samples[i + 1]);
    if (l > maxL) maxL = l;
    if (r > maxR) maxR = r;
  }
  peakLeft  = map(maxL, 0, 32767, 0, 100);
  peakRight = map(maxR, 0, 32767, 0, 100);
}

void setup() {
  Serial.begin(115200);
  pinMode(0, INPUT_PULLUP); // Onboard BOOT button (Right side) = Play/Pause toggle

  // Initialize Ideaspark 0.96" OLED (SDA=21, SCL=22)
  Wire.begin(21, 22);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Register AVRCP metadata and A2DP raw PCM stream callbacks
  a2dp_sink.set_avrc_metadata_callback(avrc_metadata_callback);
  a2dp_sink.set_stream_reader(read_data_stream, false); // false = no external I2S DAC required
  a2dp_sink.start("Ideaspark-Audio");
}

void loop() {
  // Press BOOT button (GPIO 0) to toggle Play / Pause via AVRCP
  if (digitalRead(0) == LOW) {
    if (isPlaying) a2dp_sink.pause();
    else           a2dp_sink.play();
    isPlaying = !isPlaying;
    delay(300);
  }

  // Refresh 0.96" OLED screen with Now Playing + Live Stereo VU Bars
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println(F("BT A2DP + AVRCP"));
  display.drawFastHLine(0, 10, 128, SSD1306_WHITE);

  display.setCursor(0, 14);
  display.println(currentTitle.substring(0, 21));
  display.setCursor(0, 26);
  display.println(currentArtist.substring(0, 21));

  // Draw Left & Right Channel Audio VU Meter Bars
  display.setCursor(0, 42);
  display.print(F("L:"));
  display.fillRect(14, 42, peakLeft, 7, SSD1306_WHITE);

  display.setCursor(0, 53);
  display.print(F("R:"));
  display.fillRect(14, 53, peakRight, 7, SSD1306_WHITE);

  display.display();
  peakLeft  = peakLeft * 0.7; // Smooth bar decay when silent
  peakRight = peakRight * 0.7;
  delay(50);
}
```
