# LAFVIN ESP32-C6 Development Board (1.47" Color LCD, Model LB008)

Technical documentation, original manuals/datasheet references, hardware specifications, pinouts, and code examples for the **LAFVIN ESP32-C6 Development Board with "3.73" (1.47") Color LCD Display (Model LB008)**.

> **Clarification on "3.73 from LAFVIN":**
> In marketplace catalogs, "3.73" refers to the diagonal screen dimension in centimeters: **$1.47\text{ inches} \times 2.54\text{ cm/inch} \approx 3.7338\text{ cm} \approx 3.73\text{ cm}$**. The board is formally known as the **LAFVIN ESP32-C6-1.47LCD** (Manufacturer Part Code: **LB008**).

---

## 1. Original Documents, Datasheets & Guides

* **Official LAFVIN Product Documentation:**
  * [LAFVIN ESP32-C6-1.47LCD Documentation (Read the Docs)](https://lafvin-esp32-c6-147lcd.readthedocs.io/)
  * [LAFVIN Official Tech Portal](https://lafvintech.com/)
* **Espressif ESP32-C6 Technical Resources:**
  * [ESP32-C6 Series Datasheet (PDF)](https://www.espressif.com/sites/default/files/documentation/esp32-c6_datasheet_en.pdf)
  * [ESP32-C6 Technical Reference Manual (PDF)](https://www.espressif.com/sites/default/files/documentation/esp32-c6_technical_reference_manual_en.pdf)
* **Display & Driver Controller:**
  * [Sitronix ST7789V / ST7789 Datasheet (PDF)](https://www.displayfuture.com/Display/datasheet/controller/ST7789.pdf)

---

## 2. Hardware Features & Pinout

### Core Specifications
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

### Pinout Mapping

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

## 3. Use Cases & Practical Examples

### Primary Use Cases
* **Smart Home Matter / Zigbee Gateway & Controller:** Combining Wi-Fi 6, Thread, and Zigbee to bridge local smart devices while showing real-time states on the color LCD.
* **Low-Power Interactive UI (LVGL):** Building handheld gadgets with swipe menus, system status cards, animations, and micro-SD image rendering.
* **Industrial / Environmental Data Logger:** Reading local sensors, logging time-series records to the Micro-SD card, and visualizing graphs on the color screen.
* **Portable Authenticator or Security Badge:** Wi-Fi 6 2FA hardware token or conference badge using the color display and RGB notification LED.

---

### Example 1: Displaying Text & Graphics with Arduino_GFX

#### Prerequisites
Install **GFX Library for Arduino** by Moon On Our Nation via the Arduino Library Manager.

#### Source Code
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

### Example 2: Wi-Fi 6 Connection & Onboard WS2812 RGB LED Indicator

#### Prerequisites
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
