#include "scanner_ble.h"
#include "device_table.h"
#include "log_buffer.h"
#include <NimBLEDevice.h>

#define GATT_IDLE_TIMEOUT_MS 30000UL

static NimBLEScan* s_ble_scan = nullptr;
static NimBLEClient* s_gatt_client = nullptr;
static SemaphoreHandle_t s_ble_gatt_mutex = nullptr;

// Static .bss buffers for GATT discovery and characteristic read (ble.md BLE-NFR-2 & BLE-NFR-3)
static ble_gatt_discovery_result_t s_gatt_disc_result;
static ble_gatt_read_result_t      s_gatt_read_result;

static uint8_t  s_connected_mac[6] = {0};
static uint8_t  s_connected_addr_type = 0;
static char     s_connected_name[33] = {0};
static uint32_t s_last_gatt_activity_ms = 0;

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
        uint8_t addr_type = dev->getAddressType();
        bool connectable = dev->isConnectable();
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

                device_table_upsert(PROTO_MATTER, mac, 6, mat_name, rssi, 37, details, addr_type, connectable);
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
                device_table_upsert(PROTO_BLE, mac, 6, sname, rssi, 37, details, addr_type, connectable);
                return;
            }
        }

        // Also check advertised Service UUIDs for Matter (0xFFF6) or Tile (0xFEED / 0xFEEC)
        if (dev->isAdvertisingService(NimBLEUUID((uint16_t)0xFFF6))) {
            const char* mname = !adv_name.empty() ? adv_name.c_str() : "Matter Accessory";
            device_table_upsert(PROTO_MATTER, mac, 6, mname, rssi, 37, "Uncommissioned Matter", addr_type, connectable);
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

        device_table_upsert(PROTO_BLE, mac, 6, final_name, rssi, 37, details, addr_type, connectable);
    }
};

static MultiProtoBLEScanCallbacks s_scan_callbacks;

void scanner_ble_init() {
    NimBLEDevice::init("");
    NimBLEDevice::setMTU(247);
    s_ble_gatt_mutex = xSemaphoreCreateMutex();

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

void scanner_ble_stop_scan() {
    if (s_ble_scan && s_ble_scan->isScanning()) {
        s_ble_scan->stop();
    }
}

static NimBLEAddress make_nimble_address(const uint8_t mac[6], uint8_t addr_type) {
    ble_addr_t raw;
    raw.type = addr_type;
    for (int i = 0; i < 6; ++i) {
        raw.val[i] = mac[5 - i];
    }
    return NimBLEAddress(raw);
}

static uint8_t build_char_props(const NimBLERemoteCharacteristic* chr) {
    if (!chr) return 0;
    uint8_t p = 0;
    if (chr->canBroadcast())       p |= BLE_PROP_BROADCAST;
    if (chr->canRead())            p |= BLE_PROP_READ;
    if (chr->canWriteNoResponse()) p |= BLE_PROP_WRITE_NR;
    if (chr->canWrite())           p |= BLE_PROP_WRITE;
    if (chr->canNotify())          p |= BLE_PROP_NOTIFY;
    if (chr->canIndicate())        p |= BLE_PROP_INDICATE;
    if (chr->canWriteSigned())     p |= BLE_PROP_AUTH_SIGN;
    if (chr->hasExtendedProps())   p |= BLE_PROP_EXTENDED;
    return p;
}

static uint16_t parse_uuid16(const char* uuid_str) {
    if (!uuid_str) return 0;
    const char* p = uuid_str;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    if (strlen(p) == 4) {
        return (uint16_t)strtoul(p, nullptr, 16);
    }
    // Check 128-bit Bluetooth Base UUID: 0000XXXX-0000-1000-8000-00805f9b34fb
    if (strlen(p) == 36 && strncasecmp(p, "0000", 4) == 0 &&
        strncasecmp(p + 8, "-0000-1000-8000-00805f9b34fb", 28) == 0) {
        char hex4[5] = {p[4], p[5], p[6], p[7], '\0'};
        return (uint16_t)strtoul(hex4, nullptr, 16);
    }
    return 0;
}

static void decode_characteristic_value(const char* char_uuid,
                                        const uint8_t* data,
                                        size_t len,
                                        const char* utf8_str,
                                        char* out_decoded,
                                        size_t out_len) {
    if (!out_decoded || out_len == 0) return;
    if (!data || len == 0) {
        snprintf(out_decoded, out_len, "<Empty (0 bytes)>");
        return;
    }

    uint16_t u16_uuid = parse_uuid16(char_uuid);

    // Standard Bluetooth SIG Characteristic Decoders (ble.md Section 3.4.4)
    if (u16_uuid == 0x2A00 || (u16_uuid >= 0x2A24 && u16_uuid <= 0x2A29)) {
        snprintf(out_decoded, out_len, "\"%s\"", utf8_str);
        return;
    }
    if (u16_uuid == 0x2A19 && len >= 1) {
        snprintf(out_decoded, out_len, "%u %% (Battery Level)", (unsigned)data[0]);
        return;
    }
    if (u16_uuid == 0x2A6E && len >= 2) {
        int16_t raw = (int16_t)(((uint16_t)data[1] << 8) | data[0]);
        snprintf(out_decoded, out_len, "%.2f C (Temperature)", raw / 100.0f);
        return;
    }
    if (u16_uuid == 0x2A6F && len >= 2) {
        uint16_t raw = ((uint16_t)data[1] << 8) | data[0];
        snprintf(out_decoded, out_len, "%.2f %%RH (Humidity)", raw / 100.0f);
        return;
    }
    if (u16_uuid == 0x2A6D && len >= 4) {
        uint32_t raw = ((uint32_t)data[3] << 24) | ((uint32_t)data[2] << 16) |
                       ((uint32_t)data[1] << 8) | data[0];
        snprintf(out_decoded, out_len, "%.1f hPa (Pressure)", raw / 1000.0f);
        return;
    }
    if (u16_uuid == 0x2A01 && len >= 2) {
        uint16_t app = ((uint16_t)data[1] << 8) | data[0];
        snprintf(out_decoded, out_len, "Appearance: 0x%04X (%u)", app, (unsigned)app);
        return;
    }
    if (u16_uuid == 0x2A04 && len >= 8) {
        uint16_t min_itvl = ((uint16_t)data[1] << 8) | data[0];
        uint16_t max_itvl = ((uint16_t)data[3] << 8) | data[2];
        uint16_t latency  = ((uint16_t)data[5] << 8) | data[4];
        uint16_t timeout  = ((uint16_t)data[7] << 8) | data[6];
        snprintf(out_decoded, out_len, "Conn: %.1f-%.1fms Lat:%u Tmo:%ums",
                 min_itvl * 1.25f, max_itvl * 1.25f, (unsigned)latency, (unsigned)(timeout * 10));
        return;
    }

    // Check if all bytes are printable ASCII
    bool all_printable = true;
    for (size_t i = 0; i < len; ++i) {
        if (data[i] == 0 && i == len - 1) continue;
        if (data[i] < 0x20 || data[i] > 0x7E) {
            all_printable = false;
            break;
        }
    }
    if (all_printable && len >= 2) {
        snprintf(out_decoded, out_len, "Text: \"%s\"", utf8_str);
        return;
    }

    if (len == 1) {
        snprintf(out_decoded, out_len, "uint8: %u | int8: %d | 0x%02X",
                 (unsigned)data[0], (int)(int8_t)data[0], data[0]);
    } else if (len == 2) {
        uint16_t u16 = ((uint16_t)data[1] << 8) | data[0];
        int16_t  s16 = (int16_t)u16;
        snprintf(out_decoded, out_len, "uint16_le: %u | int16_le: %d", (unsigned)u16, (int)s16);
    } else if (len == 4) {
        uint32_t u32 = ((uint32_t)data[3] << 24) | ((uint32_t)data[2] << 16) |
                       ((uint32_t)data[1] << 8) | data[0];
        int32_t  s32 = (int32_t)u32;
        snprintf(out_decoded, out_len, "uint32_le: %lu | int32_le: %ld",
                 (unsigned long)u32, (long)s32);
    } else {
        snprintf(out_decoded, out_len, "%u bytes (Binary / ASCII: \"%s\")", (unsigned)len, utf8_str);
    }
}

static bool ensure_gatt_connected_locked(const uint8_t mac[6],
                                         int addr_type_override,
                                         char* out_err,
                                         size_t err_len) {
    scanner_ble_stop_scan();

    uint8_t resolved_type = 0;
    bool connectable = true;
    char cached_name[33] = {0};
    bool in_table = device_table_lookup_ble(mac, &resolved_type, &connectable, cached_name, sizeof(cached_name));

    if (addr_type_override >= 0 && addr_type_override <= 3) {
        resolved_type = (uint8_t)addr_type_override;
    }

    if (!s_gatt_client) {
        s_gatt_client = NimBLEDevice::createClient();
        if (!s_gatt_client) {
            snprintf(out_err, err_len, "Failed to allocate NimBLEClient");
            return false;
        }
    }

    // Reuse existing connection if already connected to the same target MAC
    if (s_gatt_client->isConnected()) {
        if (memcmp(s_connected_mac, mac, 6) == 0) {
            s_last_gatt_activity_ms = millis();
            return true;
        }
        s_gatt_client->disconnect();
        s_gatt_client->deleteServices();
        vTaskDelay(pdMS_TO_TICKS(40));
    } else {
        s_gatt_client->deleteServices();
    }

    // Coexistence-friendly connection parameters (30ms-50ms interval, 0 latency, 4.0s supervision timeout)
    s_gatt_client->setConnectionParams(24, 40, 0, 400);
    s_gatt_client->setConnectRetries(0);
    s_gatt_client->setConnectTimeout(2500);

    NimBLEAddress peer_addr = make_nimble_address(mac, resolved_type);
    bool ok = s_gatt_client->connect(peer_addr, true /* deleteAttributes */, false /* sync */, true /* exchangeMTU */);

    // If address type was auto-selected and initial attempt failed, try alternate PUBLIC/RANDOM address type
    if (!ok && addr_type_override < 0) {
        uint8_t alt_type = (resolved_type == 0) ? 1 : 0;
        s_gatt_client->setConnectTimeout(1500);
        NimBLEAddress alt_addr = make_nimble_address(mac, alt_type);
        ok = s_gatt_client->connect(alt_addr, true, false, true);
        if (ok) {
            resolved_type = alt_type;
        }
    }

    if (!ok || !s_gatt_client->isConnected()) {
        if (in_table && !connectable) {
            snprintf(out_err, err_len, "Target BLE device is a non-connectable beacon");
        } else {
            snprintf(out_err, err_len, "Connection timed out or BLE peripheral unreachable");
        }
        return false;
    }

    memcpy(s_connected_mac, mac, 6);
    s_connected_addr_type = resolved_type;
    if (cached_name[0] != '\0') {
        strncpy(s_connected_name, cached_name, sizeof(s_connected_name) - 1);
        s_connected_name[sizeof(s_connected_name) - 1] = '\0';
    } else {
        snprintf(s_connected_name, sizeof(s_connected_name), "BLE %02X:%02X", mac[4], mac[5]);
    }
    s_last_gatt_activity_ms = millis();
    return true;
}

const ble_gatt_discovery_result_t* scanner_ble_discover_gatt(const uint8_t mac[6], int addr_type_override) {
    if (s_ble_gatt_mutex) {
        xSemaphoreTake(s_ble_gatt_mutex, portMAX_DELAY);
    }

    memset(&s_gatt_disc_result, 0, sizeof(s_gatt_disc_result));
    memcpy(s_gatt_disc_result.mac, mac, 6);

    if (!ensure_gatt_connected_locked(mac, addr_type_override,
                                      s_gatt_disc_result.error_msg,
                                      sizeof(s_gatt_disc_result.error_msg))) {
        s_gatt_disc_result.success = false;
        s_gatt_disc_result.connected = false;
        log_buffer_push(PROTO_BLE, "GATT conn fail %02X:%02X", mac[4], mac[5]);
        if (s_ble_gatt_mutex) xSemaphoreGive(s_ble_gatt_mutex);
        return &s_gatt_disc_result;
    }

    s_gatt_disc_result.connected = true;
    s_gatt_disc_result.addr_type = s_connected_addr_type;
    strncpy(s_gatt_disc_result.device_name, s_connected_name, sizeof(s_gatt_disc_result.device_name) - 1);
    s_gatt_disc_result.rssi = (int8_t)s_gatt_client->getRssi();
    s_gatt_disc_result.mtu = s_gatt_client->getMTU();

    const auto& services = s_gatt_client->getServices(true);
    uint8_t svc_count = 0;
    uint8_t total_chars = 0;

    for (size_t s = 0; s < services.size() && svc_count < MAX_GATT_SERVICES; ++s) {
        NimBLERemoteService* pSvc = services[s];
        if (!pSvc) continue;

        ble_gatt_service_info_t& svc_info = s_gatt_disc_result.services[svc_count];
        std::string suuid = pSvc->getUUID().toString();
        strncpy(svc_info.uuid, suuid.c_str(), sizeof(svc_info.uuid) - 1);
        svc_info.start_handle = pSvc->getStartHandle();
        svc_info.end_handle = pSvc->getEndHandle();
        svc_info.char_start_idx = total_chars;
        svc_info.char_count = 0;

        if (total_chars < MAX_GATT_TOTAL_CHARS) {
            const auto& chars = pSvc->getCharacteristics(true);
            for (size_t c = 0; c < chars.size() && total_chars < MAX_GATT_TOTAL_CHARS; ++c) {
                NimBLERemoteCharacteristic* pChr = chars[c];
                if (!pChr) continue;

                ble_gatt_char_info_t& chr_info = s_gatt_disc_result.chars[total_chars];
                std::string cuuid = pChr->getUUID().toString();
                strncpy(chr_info.uuid, cuuid.c_str(), sizeof(chr_info.uuid) - 1);
                chr_info.handle = pChr->getHandle();
                chr_info.svc_index = svc_count;
                chr_info.properties = build_char_props(pChr);

                // Opportunistically read GAP Device Name (0x2A00) if device was unnamed
                if (pChr->getUUID().equals(NimBLEUUID((uint16_t)0x2A00)) && pChr->canRead()) {
                    NimBLEAttValue v = pChr->readValue();
                    if (v.size() > 0) {
                        size_t nlen = (v.size() < sizeof(s_connected_name) - 1) ? v.size() : (sizeof(s_connected_name) - 1);
                        memcpy(s_connected_name, v.data(), nlen);
                        s_connected_name[nlen] = '\0';
                        strncpy(s_gatt_disc_result.device_name, s_connected_name, sizeof(s_gatt_disc_result.device_name) - 1);
                    }
                }

                svc_info.char_count++;
                total_chars++;
            }
        }
        svc_count++;
    }

    s_gatt_disc_result.service_count = svc_count;
    s_gatt_disc_result.total_char_count = total_chars;
    s_gatt_disc_result.success = true;
    s_last_gatt_activity_ms = millis();

    log_buffer_push(PROTO_BLE, "GATT %02X:%02X %u svc/%u chr",
                    mac[4], mac[5], (unsigned)svc_count, (unsigned)total_chars);

    if (s_ble_gatt_mutex) xSemaphoreGive(s_ble_gatt_mutex);
    return &s_gatt_disc_result;
}

const ble_gatt_read_result_t* scanner_ble_read_characteristic(const uint8_t mac[6],
                                                              int addr_type_override,
                                                              const char* service_uuid,
                                                              const char* char_uuid,
                                                              uint16_t handle) {
    if (s_ble_gatt_mutex) {
        xSemaphoreTake(s_ble_gatt_mutex, portMAX_DELAY);
    }

    memset(&s_gatt_read_result, 0, sizeof(s_gatt_read_result));
    memcpy(s_gatt_read_result.mac, mac, 6);
    if (service_uuid) strncpy(s_gatt_read_result.service_uuid, service_uuid, sizeof(s_gatt_read_result.service_uuid) - 1);
    if (char_uuid)    strncpy(s_gatt_read_result.char_uuid, char_uuid, sizeof(s_gatt_read_result.char_uuid) - 1);
    s_gatt_read_result.handle = handle;

    if (!ensure_gatt_connected_locked(mac, addr_type_override,
                                      s_gatt_read_result.error_msg,
                                      sizeof(s_gatt_read_result.error_msg))) {
        s_gatt_read_result.success = false;
        if (s_ble_gatt_mutex) xSemaphoreGive(s_ble_gatt_mutex);
        return &s_gatt_read_result;
    }

    // Ensure services are loaded if we reconnected
    const auto& services = s_gatt_client->getServices(false);
    if (services.empty()) {
        s_gatt_client->getServices(true);
    }

    NimBLERemoteCharacteristic* target_chr = nullptr;

    if (handle != 0) {
        target_chr = s_gatt_client->getCharacteristic(handle);
        if (!target_chr) {
            // Search across services if characteristics weren't populated yet
            for (auto* pSvc : s_gatt_client->getServices(false)) {
                if (!pSvc) continue;
                for (auto* pChr : pSvc->getCharacteristics(true)) {
                    if (pChr && pChr->getHandle() == handle) {
                        target_chr = pChr;
                        break;
                    }
                }
                if (target_chr) break;
            }
        }
    }

    if (!target_chr && service_uuid && service_uuid[0] != '\0' && char_uuid && char_uuid[0] != '\0') {
        NimBLERemoteService* pSvc = s_gatt_client->getService(NimBLEUUID(service_uuid));
        if (pSvc) {
            target_chr = pSvc->getCharacteristic(NimBLEUUID(char_uuid));
        }
    }

    if (!target_chr) {
        snprintf(s_gatt_read_result.error_msg, sizeof(s_gatt_read_result.error_msg),
                 "Characteristic not found on connected BLE device");
        s_gatt_read_result.success = false;
        if (s_ble_gatt_mutex) xSemaphoreGive(s_ble_gatt_mutex);
        return &s_gatt_read_result;
    }

    std::string cuuid = target_chr->getUUID().toString();
    strncpy(s_gatt_read_result.char_uuid, cuuid.c_str(), sizeof(s_gatt_read_result.char_uuid) - 1);
    if (target_chr->getRemoteService()) {
        std::string suuid = target_chr->getRemoteService()->getUUID().toString();
        strncpy(s_gatt_read_result.service_uuid, suuid.c_str(), sizeof(s_gatt_read_result.service_uuid) - 1);
    }
    s_gatt_read_result.handle = target_chr->getHandle();
    s_gatt_read_result.properties = build_char_props(target_chr);
    s_gatt_read_result.rssi = (int8_t)s_gatt_client->getRssi();

    NimBLEAttValue val = target_chr->readValue();
    size_t copy_len = (val.size() > MAX_GATT_READ_LEN) ? MAX_GATT_READ_LEN : val.size();
    s_gatt_read_result.value_len = (uint16_t)copy_len;

    if (copy_len > 0) {
        memcpy(s_gatt_read_result.raw_bytes, val.data(), copy_len);
    }

    // Format uppercase space-separated Hex string and printable UTF-8/ASCII preview
    size_t hex_pos = 0;
    for (size_t i = 0; i < copy_len; ++i) {
        uint8_t b = s_gatt_read_result.raw_bytes[i];
        int written = snprintf(s_gatt_read_result.hex_str + hex_pos,
                               sizeof(s_gatt_read_result.hex_str) - hex_pos,
                               (i + 1 < copy_len) ? "%02X " : "%02X", b);
        if (written > 0) hex_pos += (size_t)written;
        s_gatt_read_result.utf8_str[i] = (b >= 0x20 && b <= 0x7E) ? (char)b : '.';
    }
    s_gatt_read_result.hex_str[hex_pos] = '\0';
    s_gatt_read_result.utf8_str[copy_len] = '\0';

    decode_characteristic_value(s_gatt_read_result.char_uuid,
                                s_gatt_read_result.raw_bytes,
                                copy_len,
                                s_gatt_read_result.utf8_str,
                                s_gatt_read_result.decoded_str,
                                sizeof(s_gatt_read_result.decoded_str));

    s_gatt_read_result.success = true;
    s_last_gatt_activity_ms = millis();

    log_buffer_push(PROTO_BLE, "Read %s: %uB", s_gatt_read_result.char_uuid, (unsigned)copy_len);

    if (s_ble_gatt_mutex) xSemaphoreGive(s_ble_gatt_mutex);
    return &s_gatt_read_result;
}

void scanner_ble_disconnect_gatt() {
    if (s_ble_gatt_mutex && xSemaphoreTake(s_ble_gatt_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (s_gatt_client) {
            if (s_gatt_client->isConnected()) {
                s_gatt_client->disconnect();
                log_buffer_push(PROTO_BLE, "GATT disconnected %02X:%02X",
                                s_connected_mac[4], s_connected_mac[5]);
            }
            s_gatt_client->deleteServices();
        }
        memset(s_connected_mac, 0, sizeof(s_connected_mac));
        memset(s_connected_name, 0, sizeof(s_connected_name));
        xSemaphoreGive(s_ble_gatt_mutex);
    }
}

void scanner_ble_check_gatt_idle() {
    if (!s_gatt_client || !s_gatt_client->isConnected()) return;
    uint32_t now = millis();
    if (now - s_last_gatt_activity_ms >= GATT_IDLE_TIMEOUT_MS) {
        scanner_ble_disconnect_gatt();
    }
}

bool scanner_ble_is_gatt_connected() {
    return (s_gatt_client && s_gatt_client->isConnected());
}

void scanner_ble_get_gatt_status(bool* out_connected,
                                 uint8_t out_mac[6],
                                 char* out_name,
                                 size_t name_len,
                                 int8_t* out_rssi,
                                 uint16_t* out_mtu,
                                 uint32_t* out_idle_s) {
    bool conn = (s_gatt_client && s_gatt_client->isConnected());
    if (out_connected) *out_connected = conn;
    if (conn) {
        if (out_mac) memcpy(out_mac, s_connected_mac, 6);
        if (out_name && name_len > 0) {
            strncpy(out_name, s_connected_name, name_len - 1);
            out_name[name_len - 1] = '\0';
        }
        if (out_rssi) *out_rssi = (int8_t)s_gatt_client->getRssi();
        if (out_mtu)  *out_mtu  = s_gatt_client->getMTU();
        if (out_idle_s) {
            uint32_t elapsed = millis() - s_last_gatt_activity_ms;
            *out_idle_s = elapsed / 1000;
        }
    } else {
        if (out_mac) memset(out_mac, 0, 6);
        if (out_name && name_len > 0) out_name[0] = '\0';
        if (out_rssi) *out_rssi = 0;
        if (out_mtu)  *out_mtu  = 0;
        if (out_idle_s) *out_idle_s = 0;
    }
}
