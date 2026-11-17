#include "oled_ui.h"
#include "mp3_player.h"
#include "bt_manager.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

static Adafruit_SSD1306 s_display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
static bool s_oled_ready = false;

static uint8_t s_peak_hold_l = 0;
static uint8_t s_peak_hold_r = 0;
static uint16_t s_scroll_offset = 0;
static uint32_t s_last_scroll_ms = 0;

void oled_ui_init() {
    pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
    Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL, 400000);

    if (s_display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR)) {
        s_oled_ready = true;
        s_display.clearDisplay();
        s_display.setTextSize(1);
        s_display.setTextColor(SSD1306_WHITE);
        s_display.setTextWrap(false);
        s_display.setCursor(0, 0);
        s_display.println(F("Ideaspark MP3 Hub"));
        s_display.println(F("AP: ideaspark-mp3"));
        s_display.println(F("IP: 192.168.4.1"));
        s_display.display();
    } else {
        Serial.println("[OLED] SSD1306 init failed at 0x3C");
    }
}

static void poll_boot_button() {
    static bool btn_down = false;
    static uint32_t down_ms = 0;
    static bool long_handled = false;

    bool pressed = (digitalRead(PIN_BOOT_BTN) == LOW);
    uint32_t now = millis();

    if (pressed && !btn_down) {
        btn_down = true;
        down_ms = now;
        long_handled = false;
    } else if (pressed && btn_down && !long_handled) {
        if (now - down_ms >= 1500) {
            long_handled = true;
            mp3_player_next();
        }
    } else if (!pressed && btn_down) {
        if (!long_handled && (now - down_ms >= 50)) {
            mp3_player_toggle_pause();
        }
        btn_down = false;
    }
}

static void draw_vu_bar(int y, const char* label, uint8_t val, uint8_t& hold_val) {
    if (val > hold_val) hold_val = val;
    else if (hold_val > 1) hold_val -= 2;
    else hold_val = 0;

    s_display.setCursor(0, y);
    s_display.print(label);

    const int bar_x = 14;
    const int bar_max_w = 88;
    s_display.drawRect(bar_x, y, bar_max_w + 2, 8, SSD1306_WHITE);

    int fill_w = (val * bar_max_w) / 100;
    if (fill_w > 0) {
        s_display.fillRect(bar_x + 1, y + 1, fill_w, 6, SSD1306_WHITE);
    }

    int hold_x = bar_x + 1 + ((hold_val * (bar_max_w - 1)) / 100);
    if (hold_val > 2) {
        s_display.drawFastVLine(hold_x, y + 1, 6, SSD1306_WHITE);
    }

    s_display.setCursor(106, y);
    s_display.printf("%3u", (unsigned)val);
}

void oled_ui_task(void* param) {
    (void)param;
    char title[36], artist[36], bt_name[36], bt_mac[18];

    for (;;) {
        poll_boot_button();

        if (s_oled_ready) {
            player_state_t pstate = mp3_player_get_state();
            mp3_player_get_now_playing(nullptr, 0, title, sizeof(title), artist, sizeof(artist));

            uint8_t vu_l = 0, vu_r = 0;
            mp3_player_get_vu(&vu_l, &vu_r);

            bool bt_conn = false, bt_scan = false;
            bt_manager_get_status(&bt_conn, &bt_scan, bt_name, sizeof(bt_name), bt_mac, sizeof(bt_mac));

            s_display.clearDisplay();

            // Zone 1: Yellow Top Status Band (y = 0..13)
            s_display.setCursor(0, 2);
            if (pstate == PLAYER_PLAYING)      s_display.print(F("> PLAY "));
            else if (pstate == PLAYER_PAUSED)  s_display.print(F("|| PAUSE"));
            else                               s_display.print(F("[] STOP "));

            s_display.setCursor(54, 2);
            if (bt_conn) {
                char short_bt[13];
                strncpy(short_bt, bt_name, 12);
                short_bt[12] = '\0';
                s_display.printf("BT:%s", short_bt);
            } else if (bt_scan) {
                s_display.print(F("BT:SCAN..."));
            } else {
                s_display.print(F("192.168.4.1"));
            }
            s_display.drawFastHLine(0, 14, OLED_WIDTH, SSD1306_WHITE);

            // Zone 2: Track Title (auto-scroll if > 21 chars)
            size_t tlen = strlen(title);
            s_display.setCursor(0, 17);
            if (tlen <= 21) {
                s_display.print(title);
                s_scroll_offset = 0;
            } else {
                uint32_t now = millis();
                if (now - s_last_scroll_ms >= 280) {
                    s_last_scroll_ms = now;
                    s_scroll_offset = (s_scroll_offset + 1) % (tlen + 4);
                }
                char win[22];
                for (int i = 0; i < 21; ++i) {
                    size_t idx = (s_scroll_offset + i) % (tlen + 4);
                    win[i] = (idx < tlen) ? title[idx] : ' ';
                }
                win[21] = '\0';
                s_display.print(win);
            }

            // Zone 3: Artist Name
            s_display.setCursor(0, 28);
            char short_art[22];
            strncpy(short_art, artist, 21);
            short_art[21] = '\0';
            s_display.print(short_art);

            // Zone 4: Live Left & Right Audio VU Bars
            draw_vu_bar(42, "L:", vu_l, s_peak_hold_l);
            draw_vu_bar(54, "R:", vu_r, s_peak_hold_r);

            s_display.display();
        }

        vTaskDelay(pdMS_TO_TICKS(66)); // ~15 FPS
    }
}
