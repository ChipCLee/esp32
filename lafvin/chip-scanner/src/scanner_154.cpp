#include "scanner_154.h"
#include "device_table.h"
#include "log_buffer.h"
#include <esp_ieee802154.h>
#include <esp_coexist.h>

typedef struct {
    uint8_t channel;
    int8_t rssi;
    uint8_t lqi;
    uint8_t len;
    uint8_t payload[64];
} raw_154_frame_t;

#define RX_154_RING_SIZE 16
static volatile uint8_t s_rx_head = 0;
static volatile uint8_t s_rx_tail = 0;
static raw_154_frame_t s_rx_ring[RX_154_RING_SIZE];
static uint8_t s_beacon_seq = 1;
static bool s_154_initialized = false;

// ISR Callback when an 802.15.4 frame is received
static void IRAM_ATTR ieee802154_rx_done_isr(uint8_t* frame, esp_ieee802154_frame_info_t* frame_info) {
    if (frame && frame_info && frame[0] >= 5) {
        uint8_t next_head = (s_rx_head + 1) % RX_154_RING_SIZE;
        if (next_head != s_rx_tail) {
            raw_154_frame_t& slot = s_rx_ring[s_rx_head];
            slot.channel = frame_info->channel;
            slot.rssi = frame_info->rssi;
            slot.lqi = frame_info->lqi;
            uint8_t frame_len = frame[0];
            slot.len = (frame_len > sizeof(slot.payload)) ? sizeof(slot.payload) : frame_len;
            memcpy(slot.payload, &frame[1], slot.len);
            s_rx_head = next_head;
        }
    }
    if (frame) {
        esp_ieee802154_receive_handle_done(frame);
    }
}

static void IRAM_ATTR ieee802154_tx_done_isr(const uint8_t* frame, const uint8_t* ack, esp_ieee802154_frame_info_t* ack_frame_info) {
    (void)frame;
    (void)ack_frame_info;
    if (ack) {
        esp_ieee802154_receive_handle_done(ack);
    }
}

static void IRAM_ATTR ieee802154_tx_failed_isr(const uint8_t* frame, esp_ieee802154_tx_error_t error) {
    (void)frame;
    (void)error;
}

void scanner_154_init() {
    esp_ieee802154_event_cb_list_t cb_list = {};
    cb_list.rx_done_cb = ieee802154_rx_done_isr;
    cb_list.tx_done_cb = ieee802154_tx_done_isr;
    cb_list.tx_failed_cb = ieee802154_tx_failed_isr;

    esp_ieee802154_event_callback_list_register(cb_list);
    if (esp_ieee802154_enable() == ESP_OK) {
        #if !CONFIG_IEEE802154_TEST && (CONFIG_ESP_COEX_SW_COEXIST_ENABLE || CONFIG_EXTERNAL_COEX_ENABLE)
        esp_coex_wifi_i154_enable();
        #endif
        esp_ieee802154_set_promiscuous(true);
        esp_ieee802154_set_rx_when_idle(false);
        esp_ieee802154_sleep();
        s_154_initialized = true;
    }
}

// Parse captured IEEE 802.15.4 Beacon or Data/Command frames (Section 3.2)
void scanner_154_process_rx_queue() {
    while (s_rx_tail != s_rx_head) {
        raw_154_frame_t pkt = s_rx_ring[s_rx_tail];
        s_rx_tail = (s_rx_tail + 1) % RX_154_RING_SIZE;

        if (pkt.len < 7) continue;

        uint16_t fcf = ((uint16_t)pkt.payload[1] << 8) | pkt.payload[0];
        uint8_t frame_type = fcf & 0x07;          // 0=Beacon, 1=Data, 2=Ack, 3=MAC Cmd
        uint8_t dst_addr_mode = (fcf >> 10) & 0x03;
        uint8_t src_addr_mode = (fcf >> 14) & 0x03;
        bool pan_id_compressed = (fcf >> 6) & 0x01;

        uint16_t pan_id = 0xFFFF;
        uint16_t src_short = 0xFFFF;
        uint8_t id_bytes[8] = {0};
        size_t pos = 3; // Skip 2-byte FCF + 1-byte SeqNum

        // Parse Destination PAN & Address if present
        if (dst_addr_mode != 0 && pos + 2 <= pkt.len) {
            pan_id = ((uint16_t)pkt.payload[pos + 1] << 8) | pkt.payload[pos];
            pos += 2;
            if (dst_addr_mode == 2) pos += 2;
            else if (dst_addr_mode == 3) pos += 8;
        }

        // Parse Source PAN & Address
        if (src_addr_mode != 0) {
            if (!pan_id_compressed && pos + 2 <= pkt.len) {
                pan_id = ((uint16_t)pkt.payload[pos + 1] << 8) | pkt.payload[pos];
                pos += 2;
            }
            if (src_addr_mode == 2 && pos + 2 <= pkt.len) {
                src_short = ((uint16_t)pkt.payload[pos + 1] << 8) | pkt.payload[pos];
                pos += 2;
            } else if (src_addr_mode == 3 && pos + 8 <= pkt.len) {
                for (int i = 0; i < 8; ++i) {
                    id_bytes[i] = pkt.payload[pos + 7 - i];
                }
                pos += 8;
            }
        }

        // Ignore our own broadcast Beacon Request echoes (PAN 0xFFFF, Command 0x07)
        if (frame_type == 3 && pan_id == 0xFFFF && src_addr_mode == 0) {
            continue;
        }

        // If 64-bit extended address wasn't in MAC header, check Zigbee Beacon payload for Extended PAN ID
        bool has_ext_id = (src_addr_mode == 3);
        const char* type_label = "802.15.4 Node";
        if (frame_type == 0) {
            type_label = "Zigbee/Thread Beacon";
            // Zigbee Beacon payload: 2B Superframe + GTS + Pending + Protocol ID (0) + Stack Profile + 8B ExtPAN
            if (!has_ext_id && pos + 13 <= pkt.len) {
                for (int i = 0; i < 8; ++i) {
                    id_bytes[i] = pkt.payload[pos + 5 + (7 - i)];
                }
                has_ext_id = true;
            }
        } else if (frame_type == 1) {
            type_label = "Zigbee/15.4 Data";
        } else if (frame_type == 3) {
            type_label = "Zigbee MAC Cmd";
        }

        if (!has_ext_id) {
            id_bytes[0] = (uint8_t)(pan_id >> 8);
            id_bytes[1] = (uint8_t)(pan_id & 0xFF);
            id_bytes[6] = (uint8_t)(src_short >> 8);
            id_bytes[7] = (uint8_t)(src_short & 0xFF);
        }

        char node_name[33];
        if (src_short != 0xFFFF) {
            snprintf(node_name, sizeof(node_name), "Zigbee PAN 0x%04X [0x%04X]", pan_id, src_short);
        } else {
            snprintf(node_name, sizeof(node_name), "Zigbee Network 0x%04X", pan_id);
        }

        char details[44];
        snprintf(details, sizeof(details), "%s | LQI:%u", type_label, (unsigned)pkt.lqi);

        device_table_upsert(PROTO_ZIGBEE, id_bytes, 8, node_name, pkt.rssi, pkt.channel, details);
    }
}

void scanner_154_scan_step(uint8_t start_channel, uint8_t num_channels) {
    if (!s_154_initialized) return;

    // Standard IEEE 802.15.4 Beacon Request frame:
    // Byte 0: Length (8 bytes including 2-byte FCS appended by hardware)
    // Byte 1..2: FCF = 0x0803 (MAC Command, Dst Short Addr, No Src Addr)
    // Byte 3: Sequence Number
    // Byte 4..5: Destination PAN ID = 0xFFFF
    // Byte 6..7: Destination Short Addr = 0xFFFF
    // Byte 8: Command Frame Identifier = 0x07 (Beacon Request)
    uint8_t beacon_req[9] = {
        8, 0x03, 0x08, s_beacon_seq++, 0xFF, 0xFF, 0xFF, 0xFF, 0x07
    };

    for (uint8_t c = 0; c < num_channels; ++c) {
        uint8_t ch = start_channel + c;
        if (ch < 11 || ch > 26) continue;

        esp_ieee802154_set_channel(ch);
        esp_ieee802154_receive();
        esp_ieee802154_transmit(beacon_req, true);

        // Dwell 35ms on channel to capture responding Zigbee Coordinator/Router Beacons
        vTaskDelay(pdMS_TO_TICKS(35));
        scanner_154_process_rx_queue();
    }

    // Put 802.15.4 radio back to sleep immediately so SoftAP Wi-Fi has full antenna control
    esp_ieee802154_sleep();
}

void scanner_154_full_sweep() {
    for (uint8_t ch = 11; ch <= 26; ch += 2) {
        scanner_154_scan_step(ch, 2);
        // 100ms Home-Channel Recovery Window for SoftAP Wi-Fi DTIM beacons & HTTP traffic
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
