// Flock Safety (surveillance) multi-identity fake decoy generator: for creating detections capabilities
// Runs on ESP32-WROVER-E (2.4 GHz only) or ESP32-C5 (dual band).

#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <soc/soc_caps.h>  
#include <BLEDevice.h>

static const uint32_t WIFI_ROTATE_MS = 2500;
static const uint32_t BLE_ROTATE_MS  = 1000;
static const uint32_t WIFI_PROBE_BURST_MS = 800;  // probe-req burst frequency
static const uint32_t WIFI_SCAN_DWELL_MS     = 30;
static const uint32_t WIFI_SCAN_MIN_DWELL_MS = 20;

#if SOC_WIFI_SUPPORT_5G
static const wifi_band_mode_t WIFI_BAND = WIFI_BAND_MODE_2G_ONLY;
#endif

struct FlockOui {
    uint8_t     oui[3];
    const char* vendor;  
};

static const FlockOui flock_ouis[] = {
    {{0xb4, 0x1e, 0x52}, "Flock Safety"},
    {{0x58, 0x8e, 0x81}, "Flock"},
    {{0xcc, 0xcc, 0xcc}, "Flock"},
    {{0xec, 0x1b, 0xbd}, "Flock"},
    {{0x90, 0x35, 0xea}, "Flock"},
    {{0x04, 0x0d, 0x84}, "Flock"},
    {{0xf0, 0x82, 0xc0}, "Flock"},
    {{0x1c, 0x34, 0xf1}, "Flock"},
    {{0x38, 0x5b, 0x44}, "Flock"},
    {{0x94, 0x34, 0x69}, "Flock"},
    {{0xb4, 0xe3, 0xf9}, "Flock"},
    {{0x70, 0xc9, 0x4e}, "Flock"},
    {{0x3c, 0x91, 0x80}, "Flock"},
    {{0xd8, 0xf3, 0xbc}, "Flock"},
    {{0x80, 0x30, 0x49}, "Flock"},
    {{0x14, 0x5a, 0xfc}, "Flock"},
    {{0x74, 0x4c, 0xa1}, "Flock"},
    {{0x08, 0x3a, 0x88}, "Flock"},
    {{0x9c, 0x2f, 0x9d}, "Flock"},
    {{0x94, 0x08, 0x53}, "Flock"},
    {{0xe4, 0xaa, 0xea}, "Liteon (Flock)"},
    {{0xf4, 0x6a, 0xdd}, "Liteon"},
    {{0xf8, 0xa2, 0xd6}, "Liteon"},
    {{0xe0, 0x0a, 0xf6}, "Liteon"},
    {{0x00, 0xf4, 0x8d}, "Liteon"},
    {{0xd0, 0x39, 0x57}, "USI"},
    {{0xe8, 0xd0, 0xfc}, "USI"},
    {{0xd4, 0x11, 0xd6}, "SoundThinking"},
    {{0xb8, 0x35, 0x32}, "Flock"},
    {{0xc0, 0x35, 0x32}, "Flock"},
    {{0x24, 0xb2, 0xb9}, "Flock"},
    {{0xe0, 0x4f, 0x43}, "Flock"},
    {{0xb8, 0x1e, 0xa4}, "Flock"},
    {{0x70, 0x08, 0x94}, "Flock"},
    {{0x3c, 0x71, 0xbf}, "Flock"},
    {{0x58, 0x00, 0xe3}, "Flock"},
    {{0x5c, 0x93, 0xa2}, "Flock"},
    {{0x64, 0x6e, 0x69}, "Flock"},
    {{0x48, 0x27, 0xea}, "Flock"},
    {{0xa4, 0xcf, 0x12}, "Flock"},
    {{0x82, 0x6b, 0xf2}, "Flock Raven?"},
};
static const size_t NUM_WIFI_IDS = sizeof(flock_ouis) / sizeof(flock_ouis[0]);
static const char* ssid_cycle[] = {"homenet", NULL, "test_flck", NULL};
static const size_t NUM_SSIDS = sizeof(ssid_cycle) / sizeof(ssid_cycle[0]);

static uint8_t     cur_mac[6];
static const char* cur_ssid   = NULL;
static const char* cur_vendor = "";

static int         ble_oui_idx = -1;
static const char* ble_vendor  = "";
static uint8_t     ble_addr_req[6];  

static bool ble_capable_oui(const FlockOui& o) {
    return o.oui[0] >= 0xc0;
}

static int next_ble_oui(int from) {
    for (size_t i = 1; i <= NUM_WIFI_IDS; i++) {
        size_t j = ((size_t)(from + (int)i)) % NUM_WIFI_IDS;
        if (ble_capable_oui(flock_ouis[j])) {
            return (int)j;
        }
    }
    return -1;  // no eligible prefix in the table
}

// Fake BLE identities 
struct BleIdentity {
    const char* name;       // device name, 10 digits triggers name-match
    const char* serial;     // TN+digits, parsed by isFlockCamera() helper
};

static const BleIdentity ble_ids[] = {
    {"1234567890", "TN1234567890"},
    {"9876543210", "TN9876543210"},
    {"5555666677", "TN5555666677"},
};
static const size_t NUM_BLE_IDS = sizeof(ble_ids) / sizeof(ble_ids[0]);

static const uint8_t  XUNTONG_MFG[]   = {0xC8, 0x09};

static int            wifi_idx = -1;
static int            ble_idx  = -1;
static unsigned long  last_wifi_rotate = 0;
static unsigned long  last_ble_rotate  = 0;
static unsigned long  last_probe_burst = 0;

static void apply_ble_address(BLEAdvertising* pAdv) {
    int j = next_ble_oui(ble_oui_idx);
    if (j < 0) {
        return;  
    }
    ble_oui_idx = j;
    const FlockOui& o = flock_ouis[j];
    ble_vendor = o.vendor;

    uint32_t r = esp_random();
    const uint8_t lo0 = (uint8_t)(r & 0xff);
    const uint8_t lo1 = (uint8_t)((r >> 8) & 0xff);
    const uint8_t lo2 = (uint8_t)((r >> 16) & 0xff);

    ble_addr_req[0] = o.oui[0];
    ble_addr_req[1] = o.oui[1];
    ble_addr_req[2] = o.oui[2];
    ble_addr_req[3] = lo0;
    ble_addr_req[4] = lo1;
    ble_addr_req[5] = lo2;

#if defined(CONFIG_NIMBLE_ENABLED)
    uint8_t addr[6] = {lo0, lo1, lo2, o.oui[2], o.oui[1], o.oui[0]};
    int rc = ble_hs_id_set_rnd(addr);
    if (rc != 0) {
        Serial.printf("[BLE] ble_hs_id_set_rnd rc=%d\n", rc);
        return;
    }
    if (!BLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM)) {
        Serial.println("[BLE] setOwnAddrType(RANDOM) failed");
    }
#else
    // Bluedroid takes the address in natural order.
    esp_bd_addr_t addr = {o.oui[0], o.oui[1], o.oui[2], lo0, lo1, lo2};
    if (!pAdv->setDeviceAddress(addr, BLE_ADDR_TYPE_RANDOM)) {
        Serial.println("[BLE] setDeviceAddress failed");
    }
#endif
}

static bool read_ble_address(uint8_t out[6]) {
#if defined(CONFIG_NIMBLE_ENABLED)
    uint8_t le[6];
    if (ble_hs_id_copy_addr(BLE_ADDR_RANDOM, le, NULL) != 0) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        out[i] = le[5 - i];  // host byte order -> printable order
    }
    return true;
#else
    memcpy(out, ble_addr_req, 6);
    return true;
#endif
}

static void apply_ble_identity(int idx) {
    const BleIdentity& b = ble_ids[idx];

    BLEAdvertisementData advData;
    advData.setName(b.name);

    char mfg[2 + 16];
    mfg[0] = (char)XUNTONG_MFG[0];
    mfg[1] = (char)XUNTONG_MFG[1];
    strncpy(mfg + 2, b.serial, sizeof(mfg) - 3);
    mfg[sizeof(mfg) - 1] = '\0';
    advData.setManufacturerData(String(mfg));

    advData.setFlags(0x06);  // GEN_DISC | BREDR_UNSUP

    BLEAdvertising* pAdv = BLEDevice::getAdvertising();
    pAdv->stop();
    apply_ble_address(pAdv);  // must happen while advertising is stopped
    pAdv->setAdvertisementData(advData);

#if defined(CONFIG_NIMBLE_ENABLED)
    pAdv->setAdvertisementType(BLE_GAP_CONN_MODE_NON);
#else
    pAdv->setAdvertisementType(ADV_TYPE_SCAN_IND);
#endif
    pAdv->start();

    uint8_t addr[6];
    if (read_ble_address(addr)) {
        Serial.printf("[BLE] -> %-14s addr=%02x:%02x:%02x:%02x:%02x:%02x  name=%s  serial=%s\n",
                      ble_vendor,
                      addr[0], addr[1], addr[2], addr[3], addr[4], addr[5],
                      b.name, b.serial);
    } else {
        Serial.printf("[BLE] -> identity %d  name=%s  serial=%s  (addr readback failed)\n",
                      idx, b.name, b.serial);
    }
}

static void apply_wifi_identity(int idx) {
    const FlockOui& o = flock_ouis[idx % NUM_WIFI_IDS];
    cur_ssid   = ssid_cycle[idx % NUM_SSIDS];
    cur_vendor = o.vendor;

    cur_mac[0] = o.oui[0];
    cur_mac[1] = o.oui[1];
    cur_mac[2] = o.oui[2];
    uint32_t r = esp_random();
    cur_mac[3] = (uint8_t)(r & 0xff);
    cur_mac[4] = (uint8_t)((r >> 8) & 0xff);
    cur_mac[5] = (uint8_t)((r >> 16) & 0xff);

    esp_wifi_stop();
    delay(30);

    esp_err_t err = esp_wifi_set_mac(WIFI_IF_STA, cur_mac);
    if (err != ESP_OK) {
        Serial.printf("[WIFI] esp_wifi_set_mac err=0x%x\n", err);
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        Serial.printf("[WIFI] esp_wifi_start err=0x%x\n", err);
    }

#if SOC_WIFI_SUPPORT_5G
    err = esp_wifi_set_band_mode(WIFI_BAND);
    if (err != ESP_OK) {
        Serial.printf("[WIFI] esp_wifi_set_band_mode err=0x%x\n", err);
    }
#endif
    delay(50);

    uint8_t actual[6];
    esp_wifi_get_mac(WIFI_IF_STA, actual);
    Serial.printf("[WIFI] -> %-14s mac=%02x:%02x:%02x:%02x:%02x:%02x  ssid=%s\n",
                  cur_vendor,
                  actual[0], actual[1], actual[2], actual[3], actual[4], actual[5],
                  cur_ssid ? cur_ssid : "<hidden>");
}

static void trigger_probe_burst() {
    WiFi.scanNetworks(true /*async*/, true /*hidden*/, false /*passive*/,
                      WIFI_SCAN_DWELL_MS,
                      0 /*channel: all*/, cur_ssid /* may be NULL = wildcard */);
}

static void setup_ble_initial() {
    BLEDevice::init("");
    BLEAdvertising* pAdv = BLEDevice::getAdvertising();
    pAdv->setMinInterval(160);  // 100ms — fast enough that each identity gets
    pAdv->setMaxInterval(160);  //         several adverts before next rotation
    apply_ble_identity(0);
    ble_idx = 0;
}

void setup() {
    Serial.begin(115200);
    delay(800);
    Serial.println();
    Serial.println("=======================================================================================");
    Serial.println("  FlockDecoy: multi-identity fake Flock Surveillance for Detections Development ");
    Serial.println("=======================================================================================");
    Serial.printf("  WiFi identities: %u (rotates every %u ms)\n",
                  (unsigned)NUM_WIFI_IDS, (unsigned)WIFI_ROTATE_MS);
    Serial.printf("  BLE  identities: %u (rotates every %u ms)\n",
                  (unsigned)NUM_BLE_IDS, (unsigned)BLE_ROTATE_MS);

    WiFi.mode(WIFI_STA);
    WiFi.setScanActiveMinTime(WIFI_SCAN_MIN_DWELL_MS);
    delay(100);

    apply_wifi_identity(0);
    wifi_idx = 0;
    last_wifi_rotate = millis();

    setup_ble_initial();
    last_ble_rotate = millis();
}

void loop() {
    unsigned long now = millis();

    if (now - last_wifi_rotate >= WIFI_ROTATE_MS) {
        last_wifi_rotate = now;
        wifi_idx = (wifi_idx + 1) % NUM_WIFI_IDS;
        apply_wifi_identity(wifi_idx);
        last_probe_burst = 0;  // emit probe-req immediately with new MAC
    }

    if (now - last_ble_rotate >= BLE_ROTATE_MS) {
        last_ble_rotate = now;
        ble_idx = (ble_idx + 1) % NUM_BLE_IDS;
        apply_ble_identity(ble_idx);
    }

    if (now - last_probe_burst >= WIFI_PROBE_BURST_MS) {
        last_probe_burst = now;
        trigger_probe_burst();
    }

    delay(50);
}