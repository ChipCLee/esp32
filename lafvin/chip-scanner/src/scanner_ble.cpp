#include "scanner_ble.h"
#include "device_table.h"
#include "log_buffer.h"
#include <NimBLEDevice.h>

static NimBLEScan* s_ble_scan = nullptr;

// Decode BLE advertisements to fingerprint Matter (0xFFF6), AirTags/FindMy, SmartTags, and BLE Thermometers
class MultiProtoBLEScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        if (!dev) return;

        const uint8_t* raw_addr = dev->getAddress().getVal();
        uint8_t mac[6];
        // NimBLE stores address in little-endian order; reverse to standard big-endian MAC
        for (int i = 0; i < 6; ++i) {
            mac[i] = raw_addr[5 - i];
        }

        int8_t rssi = dev->getRSSI();
        std::string adv_name = dev->getName();

        // 1. Check for Matter Commissioning BLE Service Data (Service UUID 0xFFF6 - FR-5)
        uint8_t svc_count = dev->getServiceDataCount();
        for (uint8_t i = 0; i < svc_count; ++i) {
            NimBLEUUID suuid = dev->getServiceDataUUID(i);
            std::string sdata = dev->getServiceData(i);

            if (suuid.equals(NimBLEUUID((uint16_t)0xFFF6)) && sdata.size() >= 7) {
                const uint8_t* p = (const uint8_t*)sdata.data();
                uint16_t disc = ((uint16_t)(p[2] & 0x0F) << 8) | p[1];
                uint16_t vid  = ((uint16_t)p[4] << 8) | p[3];
                uint16_t pid  = ((uint16_t)p[6] << 8) | p[5];

                char mat_name[33];
                if (!adv_name.empty()) {
                    snprintf(mat_name, sizeof(mat_name), "%s", adv_name.c_str());
                } else {
                    snprintf(mat_name, sizeof(mat_name), "Matter Dev %04X:%04X", vid, pid);
                }

                char details[44];
                snprintf(details, sizeof(details), "VID:0x%04X PID:0x%04X Disc:%u", vid, pid, disc);

                device_table_upsert(PROTO_MATTER, mac, 6, mat_name, rssi, 37, details);
                return;
            }

            // Check for ATC / PVVX Xiaomi Mijia BLE Thermometer (Service UUID 0x181A - FR-3)
            if (suuid.equals(NimBLEUUID((uint16_t)0x181A)) && sdata.size() >= 13) {
                const uint8_t* p = (const uint8_t*)sdata.data();
                int16_t temp_raw = ((int16_t)p[6] << 8) | p[7];
                uint8_t hum = p[8];
                uint8_t batt = p[9];
                char details[44];
                snprintf(details, sizeof(details), "Sensor %.1fC %u%%RH Bat:%u%%",
                         temp_raw / 10.0f, (unsigned)hum, (unsigned)batt);
                const char* sname = !adv_name.empty() ? adv_name.c_str() : "BLE Env Sensor";
                device_table_upsert(PROTO_BLE, mac, 6, sname, rssi, 37, details);
                return;
            }
        }

        // Also check advertised Service UUIDs for Matter (0xFFF6) or Tile (0xFEED / 0xFEEC)
        if (dev->isAdvertisingService(NimBLEUUID((uint16_t)0xFFF6))) {
            const char* mname = !adv_name.empty() ? adv_name.c_str() : "Matter Accessory";
            device_table_upsert(PROTO_MATTER, mac, 6, mname, rssi, 37, "Uncommissioned Matter");
            return;
        }

        char details[44] = "BLE 5.3 Peripheral";
        char fallback_name[33] = {0};

        if (dev->isAdvertisingService(NimBLEUUID((uint16_t)0xFEED)) ||
            dev->isAdvertisingService(NimBLEUUID((uint16_t)0xFEEC))) {
            strncpy(details, "Tile Tracker", sizeof(details) - 1);
            strncpy(fallback_name, "Tile Smart Tracker", sizeof(fallback_name) - 1);
        } else if (dev->isAdvertisingService(NimBLEUUID((uint16_t)0xFD5A))) {
            strncpy(details, "Samsung SmartTag", sizeof(details) - 1);
            strncpy(fallback_name, "Galaxy SmartTag", sizeof(fallback_name) - 1);
        }

        // 2. Check Manufacturer Specific Data (Apple 0x004C AirTag/FindMy/Continuity, Govee, etc.)
        if (dev->haveManufacturerData()) {
            std::string mfg = dev->getManufacturerData(0);
            if (mfg.size() >= 2) {
                const uint8_t* mp = (const uint8_t*)mfg.data();
                uint16_t company_id = ((uint16_t)mp[1] << 8) | mp[0];

                if (company_id == 0x004C && mfg.size() >= 3) {
                    uint8_t sub_type = mp[2];
                    if (sub_type == 0x12 || sub_type == 0x07) {
                        strncpy(details, "Apple FindMy / AirTag", sizeof(details) - 1);
                        if (adv_name.empty()) strncpy(fallback_name, "Apple AirTag / FindMy", sizeof(fallback_name) - 1);
                    } else if (sub_type == 0x02) {
                        strncpy(details, "Apple iBeacon", sizeof(details) - 1);
                        if (adv_name.empty()) strncpy(fallback_name, "Apple iBeacon", sizeof(fallback_name) - 1);
                    } else {
                        snprintf(details, sizeof(details), "Apple Continuity (0x%02X)", sub_type);
                        if (adv_name.empty()) strncpy(fallback_name, "Apple Device", sizeof(fallback_name) - 1);
                    }
                } else if (company_id == 0x0075) {
                    strncpy(details, "Samsung BLE Device", sizeof(details) - 1);
                    if (adv_name.empty()) strncpy(fallback_name, "Samsung Device", sizeof(fallback_name) - 1);
                } else if (company_id == 0x0006) {
                    strncpy(details, "Microsoft SwiftPair", sizeof(details) - 1);
                    if (adv_name.empty()) strncpy(fallback_name, "Windows / Surface", sizeof(fallback_name) - 1);
                } else if (company_id == 0xEC88 && mfg.size() >= 6) {
                    strncpy(details, "Govee Thermo-Hygrometer", sizeof(details) - 1);
                    if (adv_name.empty()) strncpy(fallback_name, "Govee Sensor", sizeof(fallback_name) - 1);
                } else {
                    snprintf(details, sizeof(details), "Mfg ID: 0x%04X", company_id);
                }
            }
        }

        const char* final_name = !adv_name.empty()
                                     ? adv_name.c_str()
                                     : (fallback_name[0] != '\0' ? fallback_name : "<BLE Node>");

        device_table_upsert(PROTO_BLE, mac, 6, final_name, rssi, 37, details);
    }
};

static MultiProtoBLEScanCallbacks s_scan_callbacks;

void scanner_ble_init() {
    NimBLEDevice::init("");
    s_ble_scan = NimBLEDevice::getScan();
    s_ble_scan->setScanCallbacks(&s_scan_callbacks, false);
    s_ble_scan->setActiveScan(true);
    // Coexistence-friendly scan duty cycle (45ms window every 90ms interval)
    s_ble_scan->setInterval(90);
    s_ble_scan->setWindow(45);
    s_ble_scan->setMaxResults(0); // Stream directly into our callback without duplicating RAM
}

void scanner_ble_scan_window(uint32_t duration_ms) {
    if (!s_ble_scan) return;
    // Run in short coexistence-safe bursts (<= 600ms per burst with 50% radio duty cycle)
    s_ble_scan->getResults(duration_ms, false);
    s_ble_scan->clearResults();
}

