// Flock Tester — multi-identity fake Flock device emitter for SwizFlockHunter.
//
// Runs on ESP32-WROVER-E (2.4 GHz only). Cycles through a set of fake Flock
// identities covering all three confidence tiers Marauder's flockWifiSnifferCallback
// emits (HIGH/MEDIUM/LOW), and three distinct BLE Penguin signatures.
//
// Why multi-identity: a single MAC + single BLE advert under-tests the pipeline
// because the FAP's HIT pane is sticky for 30s — same MAC just refreshes RSSI on
// the existing badge. Rotating identities exercises:
//   - Marauder's per-frame matcher (different OUIs hit different rule paths)
//   - SWIZ HIT record emit at higher rates with varying content
//   - FAP UART worker thread + view-model queue under burst load
//   - flock_oui.c vendor lookup table on the FAP side
//   - Hit-pane refresh + badge re-render with new vendor/MAC/RSSI per cycle
//
// HARDWARE LIMITS:
//   - WROVER-E is 2.4 GHz only. CANNOT test the Flipper FAP's 5 GHz path.
//     Use a real Flock pole or an ESP32-C5 emitter for 5 GHz testing.
//
// USAGE:
//   - Power up next to the Flipper rig.
//   - On Flipper, pick "Dual band" or "2.4 GHz only" → expect HIT badge cycling
//     through Liteon (MEDIUM 2G), Flock (HIGH 2G), ShotSpotter (HIGH 2G).
//   - Pick "BLE Flock (WiFi off)" → expect HIGH BLE badge cycling between three
//     fake Penguin serials (TN1234567890, TN9876543210, TN5555666677).
//   - Pick "5 GHz only" → expect no hits (correct, no 5 GHz radio here).

#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <NimBLEDevice.h>

// Cycle period for each radio. WiFi is heavier (full WiFi restart per identity
// because esp_wifi_set_mac rejects the LA=0 OUIs, so we go via base_mac + reinit
// which takes ~600ms). BLE rotates faster because we just swap advert payload.
static const uint32_t WIFI_ROTATE_MS = 2500;
static const uint32_t BLE_ROTATE_MS  = 1000;
static const uint32_t WIFI_PROBE_BURST_MS = 800;  // probe-req burst frequency

// Fake WiFi identities — each is a Flock-known OUI plus an SSID that hits a
// different Marauder match rule, so rotating through them exercises the full
// confidence-tier matrix (see fy_flock_match_ssid + fy_flock_match_oui in
// FlockWiFiMarauder/esp32_marauder/WiFiScan.cpp).
struct WifiIdentity {
    uint8_t     mac[6];
    const char* ssid;       // SSID for probe-req payload, or NULL for hidden/wildcard
    const char* label;      // human-readable for serial log
};

static const WifiIdentity wifi_ids[] = {
    // Liteon OUI + ordinary SSID (named) → rule=oui_flock → HIGH (Liteon is in
    // Marauder's direct-Flock list as field-confirmed)
    {{0xe4, 0xaa, 0xea, 0x5f, 0xa1, 0xce}, "homenet",          "Liteon-named"},
    // Direct Flock OUI + NO SSID → rule=oui_flock with ssid=hidden → HIGH conf,
    // FAP renders "<hidden>" — mimics a real Flock camera probing for its
    // hidden uplink SSID without exposing the name in the air.
    {{0xb4, 0x1e, 0x52, 0xbe, 0xef, 0x01}, NULL,               "FlockDirect-hidden"},
    // SoundThinking OUI + test_flck SSID → rule=ssid_exact → HIGH conf
    {{0xd4, 0x11, 0xd6, 0xca, 0xfe, 0x02}, "test_flck",        "SoundThinking-named"},
    // Liteon variant + NO SSID → rule=oui_flock ssid=hidden → HIGH hidden
    {{0xe4, 0xaa, 0xea, 0x12, 0x34, 0x56}, NULL,               "Liteon-hidden"},
};
static const size_t NUM_WIFI_IDS = sizeof(wifi_ids) / sizeof(wifi_ids[0]);

// Fake BLE identities — different Penguin signatures. The C5 firmware matches
// XUNTONG manufacturer ID 0x09C8 plus name/serial heuristics in WiFiScan.cpp,
// so we vary the name (10 digits) and serial (TN-prefixed) per identity.
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

static void apply_ble_identity(int idx) {
    const BleIdentity& b = ble_ids[idx];

    NimBLEAdvertisementData advData;
    advData.setName(b.name);

    std::string mfg;
    mfg.append((const char*)XUNTONG_MFG, sizeof(XUNTONG_MFG));
    mfg.append(b.serial, strlen(b.serial));
    advData.setManufacturerData(mfg);

    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    pAdv->stop();
    pAdv->setAdvertisementData(advData);
    pAdv->setConnectableMode(BLE_GAP_CONN_MODE_NON);
    pAdv->setDiscoverableMode(BLE_GAP_DISC_MODE_GEN);
    pAdv->start();

    Serial.printf("[BLE] -> identity %d  name=%s  serial=%s\n",
                  idx, b.name, b.serial);
}

static void apply_wifi_identity(int idx) {
    const WifiIdentity& w = wifi_ids[idx];

    // Direct IDF sequence: stop the driver (without deiniting), change MAC,
    // restart. This keeps the WiFi core init'd so we don't need to redo
    // esp_wifi_init / event loops on each rotation. arduino-esp32's
    // WiFi.mode(WIFI_OFF) deinits and breaks this; do not use it here.
    esp_wifi_stop();
    delay(30);

    esp_err_t err = esp_wifi_set_mac(WIFI_IF_STA, (uint8_t*)w.mac);
    if (err != ESP_OK) {
        // Some IDF versions reject set_mac for OUIs whose LA bit doesn't match
        // the chip's hard-coded OUI. The Flock OUIs we use are all LA=0
        // (registered prefixes). If this fails, the rotation degrades to
        // SSID-only variation while keeping the chip's hardware MAC.
        Serial.printf("[WIFI] esp_wifi_set_mac err=0x%x\n", err);
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        Serial.printf("[WIFI] esp_wifi_start err=0x%x\n", err);
    }
    delay(50);

    uint8_t actual[6];
    esp_wifi_get_mac(WIFI_IF_STA, actual);
    Serial.printf("[WIFI] -> %s  mac=%02x:%02x:%02x:%02x:%02x:%02x  ssid=%s\n",
                  w.label,
                  actual[0], actual[1], actual[2], actual[3], actual[4], actual[5],
                  w.ssid ? w.ssid : "<hidden>");
}

static void trigger_probe_burst() {
    const WifiIdentity& w = wifi_ids[wifi_idx];
    // Two modes:
    //   - w.ssid named  → directed probe-req with that SSID. Marauder matches
    //                     the SSID field via fy_flock_match_ssid → ssid="..." in HIT.
    //   - w.ssid NULL   → wildcard probe-req (no SSID IE). Marauder matches OUI
    //                     only and emits ssid=hidden in HIT, simulating a real
    //                     Flock camera probing for its hidden uplink. The FAP
    //                     renders "<hidden>" on the SSID line of the badge.
    WiFi.scanNetworks(true /*async*/, true /*hidden*/, false /*passive*/, 80,
                      0 /*channel: all*/, w.ssid /* may be NULL = wildcard */);
}

static void setup_ble_initial() {
    NimBLEDevice::init("");
    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    pAdv->setMinInterval(160);  // 100ms — fast enough that each identity gets
    pAdv->setMaxInterval(160);  //         several adverts before next rotation
    apply_ble_identity(0);
    ble_idx = 0;
}

void setup() {
    Serial.begin(115200);
    delay(800);
    Serial.println();
    Serial.println("=========================================================");
    Serial.println("  Flock Tester — multi-identity fake Flock device emitter");
    Serial.println("=========================================================");
    Serial.printf("  WiFi identities: %u (rotates every %u ms)\n",
                  (unsigned)NUM_WIFI_IDS, (unsigned)WIFI_ROTATE_MS);
    Serial.printf("  BLE  identities: %u (rotates every %u ms)\n",
                  (unsigned)NUM_BLE_IDS, (unsigned)BLE_ROTATE_MS);

    // Bring up WiFi STA mode once so the driver is in started state when
    // apply_wifi_identity runs its esp_wifi_stop/set_mac/start sequence.
    // Without this, the first set_mac fails with ESP_ERR_WIFI_NOT_STOPPED
    // and the chip stays on its hardware MAC for the first cycle.
    WiFi.mode(WIFI_STA);
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
