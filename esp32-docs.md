# ESP32 Development Boards Reference Guide

This document provides technical documentation, original manuals/datasheet references, hardware specifications, pinouts, and code examples for two integrated ESP32 display development modules:
1. **Ideaspark ESP32 Development Board with Integrated "2.44" (0.96") OLED Display & CH340**
2. **LAFVIN ESP32-C6 Development Board with "3.73" (1.47") Color LCD Display (Model LB008)**

---

## 1. Ideaspark ESP32 with Integrated OLED Display (CH340)

> **Clarification on "2.44 OLED":**
> E-commerce listings (AliExpress, Amazon, Temu) frequently list this item with a metric translation: **$0.96\text{ inches} \times 2.54\text{ cm/inch} \approx 2.4384\text{ cm} \approx 2.44\text{ cm}$**. The onboard display is an industry-standard **0.96-inch monochrome I2C OLED** (SSD1306 driver, 128×64 resolution).

---

### 1.1 Original Documents, Datasheets & Drivers

* **Board Overview & Guides:**
  * [Manuals+ Ideaspark ESP32 0.96-inch OLED User Manual](https://manuals.plus/m/7a82b86c125df93a6bfcfb71e1b12b504ba488d8b99c7198ba16892558f62f3f)
  * [Manuals+ Ideaspark ESP32 OLED Product Reference (ASIN: B0CN4F354N)](https://manuals.plus/asin/B0CN4F354N)
  * [Device.report Ideaspark Hardware Database](https://device.report/ideaspark)
* **SoC Datasheet & Technical Manuals (Espressif):**
  * [ESP32-WROOM-32 Series Datasheet (PDF)](https://www.espressif.com/sites/default/files/documentation/esp32-wroom-32_datasheet_en.pdf)
  * [ESP32 Technical Reference Manual (PDF)](https://www.espressif.com/sites/default/files/documentation/esp32_technical_reference_manual_en.pdf)
* **USB-to-UART Bridge Driver (WCH CH340):**
  * [Official WCH CH340/CH341 USB to Serial Drivers (Windows, macOS, Linux)](https://www.wch-ic.com/downloads/CH341SER_ZIP.html)
* **OLED Controller Datasheet:**
  * [Solomon Systech SSD1306 Datasheet (PDF)](https://cdn-shop.adafruit.com/datasheets/SSD1306.pdf)

---

### 1.2 Hardware Features & Pinout

#### Core Specifications
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

#### Pinout Mapping

| Function | ESP32 GPIO | Description |
| :--- | :--- | :--- |
| **OLED SDA** | `GPIO 21` | I2C Data line (Internal trace to SSD1306) |
| **OLED SCL** | `GPIO 22` | I2C Clock line (Internal trace to SSD1306) |
| **I2C Address** | `0x3C` (default) or `0x3D` | Hexadecimal device address |
| **Boot Button** | `GPIO 0` | Download / Boot Mode switch |
| **EN / RST** | Reset pin | Hardware reset button |
| **Breakout GPIOs** | Full 30/36-pin header | Standard ESP32 GPIOs (ADC, DAC, PWM, UART, SPI) |

---

### 1.3 Use Cases & Practical Examples

#### Primary Use Cases
* **Wi-Fi Environment Scanner & Packet Monitor:** Displaying network status, RSSI signal strength, and connected clients on-screen without tethering to a computer.
* **Mini IoT Desktop Clock / Weather Station:** Fetching NTP time and OpenWeather API data over Wi-Fi and presenting local metrics.
* **Cryptocurrency / Stock Ticker:** Compact desktop display fetching real-time price feeds via REST APIs.
* **Hardware System Monitor:** Displaying CPU load, GPU temperatures, and memory utilization sent via serial from a PC.

---

#### Example 1: Arduino IDE Setup & OLED Display Test

##### Prerequisites (Libraries)
In Arduino IDE, open **Sketch > Include Library > Manage Libraries...** and install:
1. `Adafruit GFX Library`
2. `Adafruit SSD1306`

##### Source Code
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

#### Example 2: Wi-Fi Scanner with Live OLED Readout

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

## 2. LAFVIN ESP32-C6 with Integrated Color LCD (Model LB008)

> **Clarification on "3.73 from LAFVIN":**
> In marketplace catalogs, "3.73" refers to the diagonal screen dimension in centimeters: **$1.47\text{ inches} \times 2.54\text{ cm/inch} \approx 3.7338\text{ cm} \approx 3.73\text{ cm}$**. The board is formally known as the **LAFVIN ESP32-C6-1.47LCD** (Manufacturer Part Code: **LB008**).

---

### 2.1 Original Documents, Datasheets & Guides

* **Official LAFVIN Product Documentation:**
  * [LAFVIN ESP32-C6-1.47LCD Documentation (Read the Docs)](https://lafvin-esp32-c6-147lcd.readthedocs.io/)
  * [LAFVIN Official Tech Portal](https://lafvintech.com/)
* **Espressif ESP32-C6 Technical Resources:**
  * [ESP32-C6 Series Datasheet (PDF)](https://www.espressif.com/sites/default/files/documentation/esp32-c6_datasheet_en.pdf)
  * [ESP32-C6 Technical Reference Manual (PDF)](https://www.espressif.com/sites/default/files/documentation/esp32-c6_technical_reference_manual_en.pdf)
* **Display & Driver Controller:**
  * [Sitronix ST7789V / ST7789 Datasheet (PDF)](https://www.displayfuture.com/Display/datasheet/controller/ST7789.pdf)

---

### 2.2 Hardware Features & Pinout

#### Core Specifications
* **Microcontroller:** Espressif ESP32-C6 (RISC-V Architecture)
  * **HP CPU:** Single-core 32-bit RISC-V clocking up to 160 MHz
  * **LP CPU:** Low-power 32-bit RISC-V clocking up to 20 MHz
  * **Memory:** 512 KB HP SRAM, 16 KB LP SRAM, 320 KB ROM
  * **Flash:** 4 MB SiP (System-in-Package) SPI Flash
* **Next-Gen Multiprotocol Wireless Connectivity:**
  * **Wi-Fi 6:** 2.4 GHz IEEE 802.11ax protocol (supports 20 MHz bandwidth, Target Wake Time [TWT], OFDMA)
  * **Bluetooth:** Bluetooth 5.3 Low Energy (LE) with Long Range (Coded PHY) support
  * **IEEE 802.15.4:** Native support for **Zigbee 3.0** and **Thread 1.3**
  * **Matter:** Protocol-ready for cross-vendor smart home devices
* **Display Subsystem:**
  * **Screen:** 1.47-inch IPS Color LCD Screen
  * **Resolution:** 172 × 320 pixels (wide viewing angle, 262K colors)
  * **Driver IC:** ST7789
  * **Interface:** High-speed 4-wire SPI
* **Peripherals & Expansion:**
  * **Storage:** Onboard Micro-SD / TF card slot (SPI bus)
  * **Lighting:** Onboard addressable WS2812 / RGB LED
  * **USB:** Native USB Type-C interface (USB Serial / JTAG Controller, no external USB-UART IC required)
  * **Buttons:** Reset (RST) and Boot / User buttons

#### Pinout Mapping

| Component | Pin Label | ESP32-C6 GPIO | Notes |
| :--- | :--- | :--- | :--- |
| **LCD Display** | MOSI (SDA) | `GPIO 6` | Hardware SPI Data |
| | SCLK (SCK) | `GPIO 7` | Hardware SPI Clock |
| | LCD_CS | `GPIO 14` | Chip Select (Active Low) |
| | LCD_DC | `GPIO 15` | Data / Command Selection |
| | LCD_RST | `GPIO 21` | Hardware Reset |
| | LCD_BL | `GPIO 22` | Backlight PWM control |
| **Micro-SD Card** | SD_MOSI | `GPIO 6` | Shared SPI Bus |
| | SD_SCLK | `GPIO 7` | Shared SPI Bus |
| | SD_MISO | `GPIO 5` | SPI Master-In Slave-Out |
| | SD_CS | `GPIO 4` | Independent SD Chip Select |
| **RGB LED** | WS2812 DIN | `GPIO 8` | Onboard Addressable RGB |
| **Buttons** | BOOT / User | `GPIO 9` | Boot Mode & Programmable Button |

> [!NOTE]
> Because the 172×320 active pixel array is mounted on an ST7789 controller with native 240×320 internal framebuffers, a **column offset of 34 or 52** (depending on panel orientation/rotation) must be set in graphic drivers (such as `TFT_eSPI`, `Arduino_GFX`, or `LVGL`).

---

### 2.3 Use Cases & Practical Examples

#### Primary Use Cases
* **Smart Home Matter / Zigbee Gateway & Controller:** Combining Wi-Fi 6, Thread, and Zigbee to bridge local smart devices while showing real-time states on the color LCD.
* **Low-Power Interactive UI (LVGL):** Building handheld gadgets with swipe menus, system status cards, animations, and micro-SD image rendering.
* **Industrial / Environmental Data Logger:** Reading local sensors, logging time-series records to the Micro-SD card, and visualizing graphs on the color screen.
* **Portable Authenticator or Security Badge:** Wi-Fi 6 2FA hardware token or conference badge using the color display and RGB notification LED.

---

#### Example 1: Displaying Text & Graphics with Arduino_GFX

##### Prerequisites
Install **GFX Library for Arduino** by Moon On Our Nation via the Arduino Library Manager.

##### Source Code
```cpp
#include <Arduino_GFX_Library.h>

#define GFX_BL 22 // Backlight pin

// Configure hardware SPI pins for LAFVIN ESP32-C6 1.47 LCD
Arduino_DataBus *bus = new Arduino_ESP32SPI(
    15 /* DC */, 
    14 /* CS */, 
    7  /* SCK */, 
    6  /* MOSI */, 
    GFX_NOT_DEFINED /* MISO */
);

// ST7789 display with 172x320 resolution and 34 col offset
Arduino_GFX *gfx = new Arduino_ST7789(
    bus, 
    21 /* RST */, 
    0  /* Rotation */, 
    true /* IPS */, 
    172 /* Width */, 
    320 /* Height */, 
    34  /* Col offset 1 */, 
    0   /* Row offset 1 */, 
    34  /* Col offset 2 */, 
    0   /* Row offset 2 */
);

void setup() {
  Serial.begin(115200);

  // Turn on LCD backlight
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);

  if (!gfx->begin()) {
    Serial.println("gfx->begin() failed!");
  }

  gfx->fillScreen(BLACK);
  
  // Header Box
  gfx->fillRect(0, 0, 172, 45, BLUE);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(10, 15);
  gfx->println("ESP32-C6");

  // Content
  gfx->setTextColor(GREEN);
  gfx->setTextSize(1);
  gfx->setCursor(10, 60);
  gfx->println("LAFVIN LB008 Module");
  gfx->println("1.47-in (3.73cm) IPS");
  
  gfx->setTextColor(YELLOW);
  gfx->setCursor(10, 100);
  gfx->println("Wireless Protocols:");
  gfx->setTextColor(WHITE);
  gfx->println(" - Wi-Fi 6 (802.11ax)");
  gfx->println(" - Bluetooth 5.3 LE");
  gfx->println(" - Zigbee 3.0 / Thread");
}

void loop() {
  // Main loop logic
}
```

---

#### Example 2: Wi-Fi 6 Connection & Onboard WS2812 RGB LED Indicator

##### Prerequisites
Install `Adafruit NeoPixel` from the Library Manager.

```cpp
#include <WiFi.h>
#include <Adafruit_NeoPixel.h>

#define RGB_PIN 8
#define NUMPIXELS 1

Adafruit_NeoPixel pixels(NUMPIXELS, RGB_PIN, NEO_GRB + NEO_KHZ800);

const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

void setup() {
  Serial.begin(115200);
  pixels.begin();
  pixels.setBrightness(40);

  // Set LED to Blue while connecting
  pixels.setPixelColor(0, pixels.Color(0, 0, 255));
  pixels.show();

  WiFi.begin(ssid, password);
  Serial.print("Connecting to Wi-Fi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nConnected to Wi-Fi!");
  Serial.printf("IP Address: %s\n", WiFi.localIP().toString().c_str());

  // Set LED to Green when connected
  pixels.setPixelColor(0, pixels.Color(0, 255, 0));
  pixels.show();
}

void loop() {
  // Steady heart-beat or status monitor
  delay(1000);
}
```

---

## 3. Comparison Matrix

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
