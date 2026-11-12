#include "display_manager.h"
#include "auth_config.h"
#include "device_table.h"
#include "log_buffer.h"
#include "rf_scheduler.h"
#include "scanner_wifi.h"
#include "led_status.h"
#include <Arduino_GFX_Library.h>

// RGB565 Color Palette (Section 3.4)
#define COL_BG_DARK      0x0841 // Deep slate black
#define COL_HEADER_BG    0x0013 // Dark navy header
#define COL_CARD_WIFI    0x0339 // Deep Blue (#0066CC)
#define COL_CARD_BLE     0x04DF // Cyan (#0099FF)
#define COL_CARD_ZIGBEE  0x781F // Purple (#7F00FF)
#define COL_CARD_MATTER  0xFCC0 // Amber (#FF9900)
#define COL_TEXT_WHITE   0xFFFF
#define COL_TEXT_MUTED   0xAD55
#define COL_TEXT_GREEN   0x07E0
#define COL_TEXT_YELLOW  0xFFE0
#define COL_TEXT_RED     0xF800
#define COL_BORDER       0x3186

static Arduino_DataBus* s_bus = nullptr;
static Arduino_GFX* s_gfx = nullptr;
static volatile display_mode_t s_display_mode = DISPLAY_MODE_DASHBOARD;
static bool s_backlight_on = true;
static bool s_force_full_redraw = true;

// Cached state for flicker-free partial bounding-box updates (NFR-6)
static uint8_t s_prev_users = 0xFF;
static scan_state_t s_prev_state = (scan_state_t)0xFF;
static protocol_counts_t s_prev_counts = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
static uint32_t s_prev_log_rev = 0;

static uint16_t proto_badge_color(rf_protocol_t proto) {
    switch (proto) {
        case PROTO_WIFI:   return COL_CARD_WIFI;
        case PROTO_BLE:    return COL_CARD_BLE;
        case PROTO_ZIGBEE: return COL_CARD_ZIGBEE;
        case PROTO_MATTER: return COL_CARD_MATTER;
        default:           return COL_TEXT_GREEN;
    }
}

void display_manager_set_backlight(bool on) {
    s_backlight_on = on;
    g_config.lcd_backlight = on;
    digitalWrite(PIN_LCD_BL, on ? HIGH : LOW);
}

bool display_manager_get_backlight() {
    return s_backlight_on;
}

display_mode_t display_manager_get_mode() {
    return s_display_mode;
}

void display_manager_toggle_mode() {
    s_display_mode = (s_display_mode == DISPLAY_MODE_DASHBOARD)
                         ? DISPLAY_MODE_TERMINAL
                         : DISPLAY_MODE_DASHBOARD;
    s_force_full_redraw = true;
}

void display_manager_init(SPIClass* shared_spi) {
    pinMode(PIN_LCD_BL, OUTPUT);
    display_manager_set_backlight(g_config.lcd_backlight);
    pinMode(PIN_USER_BTN, INPUT_PULLUP);

    if (spi_bus_mutex && xSemaphoreTake(spi_bus_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        s_bus = new Arduino_HWSPI(
            PIN_LCD_DC,
            PIN_LCD_CS,
            PIN_SPI_SCK,
            PIN_SPI_MOSI,
            PIN_SPI_MISO,
            shared_spi,
            false /* is_shared_interface handled by our FreeRTOS spi_bus_mutex */
        );

        // ST7789 172x320 IPS panel with 34-pixel column offset (Section 3.4)
        s_gfx = new Arduino_ST7789(
            s_bus,
            PIN_LCD_RST,
            0 /* Rotation: Portrait */,
            true /* IPS */,
            LCD_WIDTH,
            LCD_HEIGHT,
            LCD_COL_OFFSET,
            0,
            LCD_COL_OFFSET,
            0
        );

        s_gfx->begin(40000000);
        s_gfx->fillScreen(COL_BG_DARK);
        xSemaphoreGive(spi_bus_mutex);
    }
    s_force_full_redraw = true;
}

static void draw_header_zone(bool full_repaint) {
    uint8_t users = scanner_wifi_get_connected_users();
    scan_state_t st = rf_scheduler_get_state();

    if (full_repaint) {
        s_gfx->fillRect(0, 0, LCD_WIDTH, 49, COL_HEADER_BG);
        s_gfx->drawFastHLine(0, 49, LCD_WIDTH, COL_BORDER);

        s_gfx->setTextSize(1);
        s_gfx->setTextColor(COL_TEXT_WHITE, COL_HEADER_BG);
        s_gfx->setCursor(4, 4);
        s_gfx->print("ESP32-C6 HUB");

        s_gfx->setTextColor(COL_TEXT_MUTED, COL_HEADER_BG);
        s_gfx->setCursor(4, 19);
        s_gfx->print("SSID: chip-scanner");

        s_gfx->setCursor(4, 30);
        s_gfx->print("IP:   192.168.4.1");
    }

    if (full_repaint || st != s_prev_state) {
        s_prev_state = st;
        uint16_t st_col = (st == SCAN_STATE_IDLE) ? COL_TEXT_GREEN : COL_TEXT_YELLOW;
        s_gfx->fillRect(108, 2, 62, 13, COL_HEADER_BG);
        s_gfx->setTextSize(1);
        s_gfx->setTextColor(st_col, COL_HEADER_BG);
        s_gfx->setCursor(110, 4);
        s_gfx->printf("[%s]", rf_scheduler_get_state_str());
    }

    if (full_repaint || users != s_prev_users) {
        s_prev_users = users;
        s_gfx->fillRect(4, 39, 164, 9, COL_HEADER_BG);
        s_gfx->setTextSize(1);
        s_gfx->setTextColor(users > 0 ? COL_TEXT_GREEN : COL_TEXT_MUTED, COL_HEADER_BG);
        s_gfx->setCursor(4, 39);
        s_gfx->printf("Users: %-2u", (unsigned)users);

        s_gfx->setTextColor(auth_is_locked_out() ? COL_TEXT_RED : COL_TEXT_YELLOW, COL_HEADER_BG);
        s_gfx->setCursor(86, 39);
        s_gfx->print(auth_is_locked_out() ? "Auth:LOCKED" : "Auth:ACTIVE");
    }
}

static void draw_metric_card(int16_t x, int16_t y, int16_t w, int16_t h,
                             uint16_t bg_col, const char* label, uint16_t count, bool full_box) {
    if (full_box) {
        s_gfx->fillRoundRect(x, y, w, h, 4, bg_col);
        s_gfx->setTextSize(1);
        s_gfx->setTextColor(COL_TEXT_WHITE, bg_col);
        s_gfx->setCursor(x + 5, y + 4);
        s_gfx->print(label);
    }
    s_gfx->fillRect(x + 5, y + 14, w - 10, 16, bg_col);
    s_gfx->setTextSize(2);
    s_gfx->setTextColor(COL_TEXT_WHITE, bg_col);
    s_gfx->setCursor(x + 6, y + 14);
    s_gfx->printf("%-3u", (unsigned)count);
}

static void draw_protocol_matrix(bool full_repaint) {
    protocol_counts_t c = device_table_get_counts();

    if (full_repaint) {
        s_gfx->fillRect(0, 50, LCD_WIDTH, 95, COL_BG_DARK);
        s_gfx->setTextSize(1);
        s_gfx->setTextColor(COL_TEXT_MUTED, COL_BG_DARK);
        s_gfx->setCursor(4, 53);
        s_gfx->print("DETECTED BY PROTOCOL");
        s_gfx->drawFastHLine(0, 144, LCD_WIDTH, COL_BORDER);
    }

    if (full_repaint || c.wifi_count != s_prev_counts.wifi_count) {
        draw_metric_card(4, 64, 80, 32, COL_CARD_WIFI, "Wi-Fi 6", c.wifi_count, full_repaint);
    }
    if (full_repaint || c.ble_count != s_prev_counts.ble_count) {
        draw_metric_card(88, 64, 80, 32, COL_CARD_BLE, "BLE 5.3", c.ble_count, full_repaint);
    }
    if (full_repaint || c.zigbee_count != s_prev_counts.zigbee_count) {
        draw_metric_card(4, 99, 80, 32, COL_CARD_ZIGBEE, "Zigbee", c.zigbee_count, full_repaint);
    }
    if (full_repaint || c.matter_count != s_prev_counts.matter_count) {
        draw_metric_card(88, 99, 80, 32, COL_CARD_MATTER, "Matter", c.matter_count, full_repaint);
    }

    if (full_repaint || c.total_count != s_prev_counts.total_count) {
        s_gfx->fillRect(4, 133, 164, 9, COL_BG_DARK);
        s_gfx->setTextSize(1);
        s_gfx->setTextColor(COL_TEXT_WHITE, COL_BG_DARK);
        s_gfx->setCursor(4, 133);
        s_gfx->printf("Total Unique Emitters: %u", (unsigned)c.total_count);
    }

    s_prev_counts = c;
}

static void draw_logs_zone(int16_t start_y, size_t max_lines, bool full_repaint) {
    uint32_t rev = log_buffer_get_revision();
    if (!full_repaint && rev == s_prev_log_rev) {
        return;
    }
    s_prev_log_rev = rev;

    if (full_repaint) {
        s_gfx->fillRect(0, start_y, LCD_WIDTH, LCD_HEIGHT - start_y, 0x0000);
        s_gfx->setTextSize(1);
        s_gfx->setTextColor(COL_TEXT_YELLOW, 0x0000);
        s_gfx->setCursor(4, start_y + 3);
        s_gfx->print(max_lines > 6 ? "FULL ACTIVITY LOG TERMINAL" : "RECENT ACTIVITY LOGS");
    } else {
        s_gfx->fillRect(0, start_y + 14, LCD_WIDTH, LCD_HEIGHT - (start_y + 14), 0x0000);
    }

    lcd_log_entry_t entries[14];
    size_t count = log_buffer_get_recent(entries, max_lines);

    int16_t y = start_y + 15;
    s_gfx->setTextSize(1);
    for (size_t i = 0; i < count; ++i) {
        char ts[16];
        log_format_timestamp(entries[i], ts, sizeof(ts));

        // Line 1 of entry: [HH:MM:SS] [PROTO]
        s_gfx->setTextColor(COL_TEXT_MUTED, 0x0000);
        s_gfx->setCursor(3, y);
        s_gfx->printf("%s ", ts);

        uint16_t pcol = proto_badge_color(entries[i].protocol);
        s_gfx->setTextColor(pcol, 0x0000);
        s_gfx->printf("[%s]", protocol_to_str(entries[i].protocol));

        // Line 2 of entry: truncated message (up to 27 chars across 172px width)
        y += 10;
        s_gfx->setTextColor(COL_TEXT_WHITE, 0x0000);
        s_gfx->setCursor(6, y);
        char line_msg[28];
        strncpy(line_msg, entries[i].message, 27);
        line_msg[27] = '\0';
        s_gfx->print(line_msg);

        y += 14;
        if (y + 18 > LCD_HEIGHT) break;
    }
}

static void poll_user_button() {
    static bool btn_pressed = false;
    static uint32_t press_start_ms = 0;
    static bool long_handled = false;

    bool is_low = (digitalRead(PIN_USER_BTN) == LOW);
    uint32_t now = millis();

    if (is_low && !btn_pressed) {
        btn_pressed = true;
        press_start_ms = now;
        long_handled = false;
    } else if (is_low && btn_pressed) {
        if (!long_handled && (now - press_start_ms > 2000)) {
            long_handled = true;
            // Long press (>2s): Toggle LCD backlight (Section 3.4)
            display_manager_set_backlight(!s_backlight_on);
        }
    } else if (!is_low && btn_pressed) {
        btn_pressed = false;
        if (!long_handled && (now - press_start_ms > 40)) {
            // Short press: Toggle between Split Dashboard HUD and Full Log Terminal (FR-8)
            display_manager_toggle_mode();
        }
    }
}

void display_manager_task(void* param) {
    (void)param;
    uint32_t last_draw_ms = 0;

    for (;;) {
        poll_user_button();
        led_status_tick();

        uint32_t now = millis();
        if (s_backlight_on && s_gfx && (now - last_draw_ms >= 250 || s_force_full_redraw)) {
            last_draw_ms = now;
            if (spi_bus_mutex && xSemaphoreTake(spi_bus_mutex, pdMS_TO_TICKS(60)) == pdTRUE) {
                bool full = s_force_full_redraw;
                s_force_full_redraw = false;

                if (full) {
                    s_gfx->fillScreen(COL_BG_DARK);
                }

                draw_header_zone(full);
                if (s_display_mode == DISPLAY_MODE_DASHBOARD) {
                    draw_protocol_matrix(full);
                    draw_logs_zone(145, 6, full);
                } else {
                    draw_logs_zone(50, 11, full);
                }

                xSemaphoreGive(spi_bus_mutex);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

