#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "WiFi.h"
#include "esp_chip_info.h"

// LAFVIN / Waveshare ESP32-C6-LCD-1.47 Pin Configuration
#define LCD_MOSI  6
#define LCD_SCLK  7
#define LCD_CS   14
#define LCD_DC   15
#define LCD_RST  21
#define LCD_BL   22

#define LCD_WIDTH   172
#define LCD_HEIGHT  320

// Onboard RGB LED (GPIO 8)
#ifndef RGB_BUILTIN
  #define RGB_BUILTIN 8
#endif

// Hardware SPI for ST7789
Adafruit_ST7789 tft = Adafruit_ST7789(&SPI, LCD_CS, LCD_DC, LCD_RST);

void setLedColor(uint8_t r, uint8_t g, uint8_t b) {
  rgbLedWrite(RGB_BUILTIN, r, g, b);
}

void setup() {
  Serial.begin(115200);

  // Initialize Backlight (Active HIGH)
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);

  // Initialize Hardware SPI on ESP32-C6 pins
  SPI.begin(LCD_SCLK, -1, LCD_MOSI, LCD_CS);

  // Initialize 1.47" ST7789 Display (172x320)
  tft.init(LCD_WIDTH, LCD_HEIGHT);
  tft.setRotation(1); // Landscape mode: 320 x 172
  tft.fillScreen(ST77XX_BLACK);

  // Splash Screen
  tft.fillRect(0, 0, 320, 28, 0x0A9F); // Dark blue header
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(15, 6);
  tft.println("ESP32-C6 Wi-Fi 6");

  tft.setTextSize(1);
  tft.setTextColor(ST77XX_GREEN);
  tft.setCursor(15, 45);
  tft.println("Chip: ESP32-C6 (RISC-V)");
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(15, 65);
  tft.printf("MAC: %s", WiFi.macAddress().c_str());
  tft.setCursor(15, 85);
  tft.println("Display: 1.47\" ST7789 IPS LCD (320x172)");
  tft.setCursor(15, 115);
  tft.setTextColor(ST77XX_YELLOW);
  tft.println("Initializing Wi-Fi radio...");

  // Set WiFi to Station mode
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(1200);
}

void drawSignalBar(int x, int y, int rssi) {
  // Draw mini 4-bar signal indicator
  int bars = 1;
  uint16_t color = ST77XX_RED;
  if (rssi >= -60) {
    bars = 4;
    color = ST77XX_GREEN;
  } else if (rssi >= -70) {
    bars = 3;
    color = ST77XX_GREEN;
  } else if (rssi >= -80) {
    bars = 2;
    color = ST77XX_YELLOW;
  }

  for (int b = 0; b < 4; b++) {
    int h = (b + 1) * 3;
    uint16_t c = (b < bars) ? color : 0x4208; // Gray if inactive
    tft.fillRect(x + (b * 3), y + (12 - h), 2, h, c);
  }
}

void loop() {
  // Update header on LCD: "Scanning..."
  tft.fillRect(0, 0, 320, 24, 0x0011); // Dark navy header
  tft.setTextColor(0x07FF); // Cyan
  tft.setTextSize(1);
  tft.setCursor(8, 8);
  tft.print("ESP32-C6 Wi-Fi 6 Scanner");

  tft.setCursor(215, 8);
  tft.setTextColor(ST77XX_YELLOW);
  tft.print("[Scanning...]");

  // Blue LED pulse during scan
  setLedColor(0, 0, 40);

  int n = WiFi.scanNetworks();
  setLedColor(0, 30, 0); // Green when scan done

  // Clear display body
  tft.fillRect(0, 24, 320, 148, ST77XX_BLACK);

  // Status Bar below header
  tft.fillRect(0, 24, 320, 16, 0x18E3); // Slate gray
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(8, 28);
  tft.printf("Networks: %d found", n);

  if (n == 0) {
    tft.setCursor(40, 80);
    tft.setTextColor(ST77XX_RED);
    tft.setTextSize(2);
    tft.println("No Networks Found");
    setLedColor(50, 0, 0);
  } else {
    // Show top 6 networks on LCD
    int maxDisplay = (n > 6) ? 6 : n;
    int yStart = 45;

    for (int i = 0; i < maxDisplay; i++) {
      int y = yStart + (i * 18);
      int rssi = WiFi.RSSI(i);
      String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) ssid = "(Hidden SSID)";

      // Draw signal bars
      drawSignalBar(8, y, rssi);

      // SSID Name (truncated to 18 chars to fit nicely)
      tft.setTextColor(ST77XX_WHITE);
      tft.setTextSize(1);
      tft.setCursor(25, y + 2);
      if (ssid.length() > 18) ssid = ssid.substring(0, 17) + "~";
      tft.print(ssid);

      // RSSI in dBm
      tft.setCursor(160, y + 2);
      if (rssi >= -65) tft.setTextColor(ST77XX_GREEN);
      else if (rssi >= -78) tft.setTextColor(ST77XX_YELLOW);
      else tft.setTextColor(ST77XX_RED);
      tft.printf("%3d dBm", rssi);

      // Channel
      tft.setTextColor(0x7BEF); // light gray
      tft.setCursor(220, y + 2);
      tft.printf("CH%d", (int)WiFi.channel(i));

      // Security Tag
      tft.setCursor(260, y + 2);
      switch (WiFi.encryptionType(i)) {
        case WIFI_AUTH_OPEN:
          tft.setTextColor(ST77XX_RED);
          tft.print("OPEN");
          break;
        case WIFI_AUTH_WPA3_PSK:
        case WIFI_AUTH_WPA2_WPA3_PSK:
          tft.setTextColor(0x07E0); // Bright green
          tft.print("WPA3");
          break;
        default:
          tft.setTextColor(0x07FF); // Cyan
          tft.print("WPA2");
          break;
      }
    }

    // Footer
    tft.drawFastHLine(0, 156, 320, 0x4208);
    tft.setCursor(8, 160);
    tft.setTextColor(0x7BEF);
    tft.printf("Next scan in 8s... (Total: %d APs)", n);
  }

  // Print also to Serial
  Serial.printf("\n[Scan Result] Found %d networks.\n", n);

  WiFi.scanDelete();

  // Wait 8 seconds before next scan
  delay(8000);
}
