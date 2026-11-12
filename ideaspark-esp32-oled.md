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
| **Boot Button** | `GPIO 0` | Download / Boot Mode switch |
| **EN / RST** | Reset pin | Hardware reset button |
| **Breakout GPIOs** | Full 30/36-pin header | Standard ESP32 GPIOs (ADC, DAC, PWM, UART, SPI) |

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

