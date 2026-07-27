# FlockDecoy

Fake-Flock signal decoy for testing detection rigs without driving to a real ALPR pole. Runs on any classic ESP32 (tested on ESP32-WROVER-E) or on an ESP32-C5 (tested on ESP32-C5-DevKitC-1, rev v1.2). Built to validate [WatchFlock](https://github.com/0xXyc/WatchFlock) firmware and the [SwizFlockHunter](https://github.com/0xXyc/SwizFlockHunter) Flipper companion.

<img width="600" height="600" alt="Screenshot 2026-07-25 at 1 20 50 PM" src="https://github.com/user-attachments/assets/028bb038-dc1f-4e9e-b861-85678688c536" />

## What it spoofs

Cycles through multiple fake identities on both radios in parallel.

**WiFi side** (rotates every 2.5s):
- Walks all 41 Flock / Liteon / USI / SoundThinking OUI prefixes lifted verbatim from [SwizFlockHunter](https://github.com/0xXyc/SwizFlockHunter)'s `flock_oui.c`, one per rotation — about 103s for a full pass. Nothing is invented; a made-up prefix would put a signature in the air that no real Flock device emits.
- The lower 3 bytes are randomised on every rotation, so each pass is a MAC the receiver has never seen. This matters: the FAP's HIT pane is sticky for 30s and keyed on MAC, so a fixed MAC table only refreshes RSSI on existing badges instead of exercising badge creation and the `flock_oui.c` vendor lookup.
- SSID cycles independently of the OUI, through `homenet` → hidden → `test_flck` (CVE-2025-59409 dev SSID) → hidden, so both Marauder rule paths stay exercised: a named SSID hits `fy_flock_match_ssid`, a hidden one falls through to `fy_flock_match_oui` and the FAP renders `<hidden>`.

**BLE side** (rotates every 1s):
- Penguin battery emulation: XUNTONG manufacturer ID `0x09C8` plus 10-digit name pattern, three rotating serials (`TN1234567890`, `TN9876543210`, `TN5555666677`)
- A fresh advertised address on every rotation, under a real Flock prefix, so the receiver sees a new BLE device each second rather than one device changing its payload.
- Advertised as `ADV_SCAN_IND` (non-connectable but scannable) with a 31-byte payload: name TLV, manufacturer TLV, then a trailing flags TLV of `0x06`.

The BLE address draws from **13 of the 41** prefixes, not all of them. There is no BLE equivalent of `esp_wifi_set_mac`: the only address changeable without tearing the controller down is the *random* one, and Bluetooth requires the top two bits of its most significant byte to be `11` for a static random address. That admits only prefixes at `0xc0` and above and excludes `b4:1e:52` — Flock Safety's own prefix. The eligible subset is computed from the OUI table at runtime rather than duplicated, so adding a prefix makes it available here automatically if it qualifies.

Two consequences worth knowing before you trust a BLE result. The address type on the air is **random, not public**, so a receiver that filters for public addresses will not see these; Marauder matches on the address bytes and the XUNTONG payload, so it does. And to put an excluded prefix like `b4:1e:52` on the air over BLE you would have to change the *public* address via `esp_iface_mac_addr_set(mac, ESP_MAC_BT)`, which only takes effect before the controller starts — a full `BLEDevice::deinit()`/`init()` on every rotation.

**Probe-req sweeps** via `WiFi.scanNetworks()` carrying the current spoofed STA MAC. One sweep per identity — *not* the every-800ms burst rate `WIFI_PROBE_BURST_MS` suggests. `scanNetworks()` returns `WIFI_SCAN_RUNNING` and starts nothing while a sweep is already in flight, so the other ~3 burst calls per rotation are silently dropped rather than queued.

Measured on a C5-DevKitC-1, one full 2.4 GHz sweep takes **1973ms** at arduino-esp32's defaults and **1342ms** at the sketch's lowered dwell — the floor is roughly 95ms per channel of switching overhead that no dwell setting touches. The non-obvious half is the minimum: arduino-esp32 defaults `_scanActiveMinTime` to 100ms and passes it as `scan_time.active.min`, which silently overrides any smaller max you request, so the sketch calls `WiFi.setScanActiveMinTime()` too. With BLE advertising concurrently the sweep does not finish inside the 2.5s rotation window at all, so each identity emits one partial sweep before `esp_wifi_stop()` aborts it.

If the receiver is missing identities, the levers are `WIFI_ROTATE_MS` (give each identity long enough to finish a sweep) or pinning the scan to a single channel per burst — not lowering the dwell further.

## What it CAN'T do

- **Only one MAC at a time on each radio.** To "be" different Flock OUIs we physically restart WiFi between MAC changes (~150-300ms downtime per rotation). Both radios now present unique addresses, but they are unrelated to each other — the rig looks like two separate fleets of devices, never like one device with a WiFi and a BLE interface.
- **BLE can only use 13 of the 41 prefixes**, and advertises them as random rather than public addresses. See the BLE section above for why.
- **No 5 GHz on a classic ESP32.** The WROVER is 2.4 only; the receiver should NOT see hits from it in 5 GHz only mode. A C5 build can do 5 GHz via `WIFI_BAND`, but not for free — see below.
- **5 GHz on the C5 needs a slower rotation.** `WIFI_BAND` defaults to `WIFI_BAND_MODE_2G_ONLY`, which reproduces the WROVER's behaviour exactly. Measured sweep times are ~9.0s for `5G_ONLY` and ~11.0s for `AUTO`, both far longer than the 2.5s rotation that aborts them, so the tail of the channel list is never reached. Set `5G_ONLY` **and** raise `WIFI_ROTATE_MS` to ~12000, or the 5 GHz channels simply never get probed — which on the receiver side is indistinguishable from a receiver bug.
- **No real Penguin battery firmware.** We craft adverts that match Marauder's detection signature (XUNTONG mfg ID + name shape) but we're not emulating the actual GAP/GATT services a real Penguin runs.

## Hardware

- ESP32-WROVER-E (or any classic ESP32 with WiFi + BLE coex), **or** an ESP32-C5-DevKitC-1 for the dual-band option
- USB-Serial cable. The C5-DevKitC-1 has two USB-C ports: the UART-bridge port enumerates as `/dev/cu.usbserial-*` and works with plain arduino-cli (its auto-reset circuit means no button-holding), while the native USB port enumerates as `/dev/cu.usbmodem*` and needs `CDCOnBoot=cdc` to show serial output.

## Build and flash

Uses the core's bundled `BLE` library, which is Bluedroid on classic ESP32 and NimBLE on the C5 — a `#if defined(CONFIG_NIMBLE_ENABLED)` guard covers the one API that differs. **NimBLE-Arduino is deliberately not used**: its bundled host fails to link on the C5 (undefined `r_os_mempool_init` and friends), which would make the sketch WROVER-only.

Classic ESP32 target:

```bash
arduino-cli compile -b "esp32:esp32:esp32wrover:PartitionScheme=huge_app" \
  --output-dir ./build FlockDecoy
arduino-cli upload -b "esp32:esp32:esp32wrover:PartitionScheme=huge_app" \
  -p /dev/cu.usbserial-XXXX --input-dir ./build FlockDecoy
```

ESP32-C5 target (8MB flash; add `,EraseFlash=all` on the upload FQBN if the board is coming from firmware with a different partition layout):

```bash
arduino-cli compile -b "esp32:esp32:esp32c5:PartitionScheme=huge_app,FlashSize=8M" \
  --output-dir ./build-c5 FlockDecoy
arduino-cli upload -b "esp32:esp32:esp32c5:PartitionScheme=huge_app,FlashSize=8M" \
  -p /dev/cu.usbserial-XXXX --input-dir ./build-c5 FlockDecoy
```

Expected boot output:

```
=========================================================
  FlockDecoy — multi-identity fake Flock Surveillance
=========================================================
  WiFi identities: 41 (rotates every 2500 ms)
  BLE  identities: 3 (rotates every 1000 ms)
[WIFI] -> Flock Safety   mac=b4:1e:52:cb:35:b7  ssid=homenet
[BLE] -> Flock          addr=cc:cc:cc:ae:e2:9d  name=1234567890  serial=TN1234567890
[BLE] -> Flock          addr=ec:1b:bd:9c:91:cb  name=9876543210  serial=TN9876543210
...
```

The MAC on each `[WIFI]` line is read back with `esp_wifi_get_mac()` after the change is applied, so it is the radio's actual address rather than what was requested. The `[BLE]` address gets the same treatment via `ble_hs_id_copy_addr()` on the C5. On a classic ESP32 it is the requested value instead — Bluedroid exposes no getter for the random address (`esp_bt_dev_get_address()` returns the public one), so a set failure is logged rather than silently printing a stale address. On a C5 the boot log also carries a benign `MSPI Timing: Failed to allocate dummy cacheline for PSRAM memory barrier!` line; the sketch runs fine at `PSRAM=disabled` and uses ~15% of RAM.

## Companion debug scripts

Two helper shell scripts for monitoring an ESP32-C5 over USB-CDC without the open-port-resets-the-chip problem (USB-Serial-JTAG asserts reset on DTR pulse during port open):

- `c5-tail.sh` — live tail of the C5's serial output, holds DTR/RTS low to avoid reset
- `c5-cmd.sh "<command>" [seconds]` — send a single CLI command, capture response for N seconds

Both target `/dev/cu.usbmodem5B7B0330131` by default (override via env or arg). They matter for the C5's *native* USB port; on the UART-bridge port a plain `arduino-cli monitor -p /dev/cu.usbserial-XXXX -c baudrate=115200` is fine. Useful for any Flipper + Marauder + ESP32 dev workflow, not just this project.

## Test matrix

Run the decoy next to a [WatchFlock](https://github.com/0xXyc/WatchFlock) C5 + Flipper rig, then in the [SwizFlockHunter](https://github.com/0xXyc/SwizFlockHunter) FAP:

| Mode | Expected hits |
|------|---------------|
| Dual band (2.4 + 5 GHz) | 2.4 hits sparse (channel hop spends most time on 5 GHz channels) |
| 2.4 GHz only | A new MAC every 2.5s cycling all 41 OUIs, never repeating, plus all 3 BLE serials silently |
| 5 GHz only | No hits at the default `WIFI_BAND=2G_ONLY`. Needs `5G_ONLY` **and** a raised `WIFI_ROTATE_MS` |
| BLE Flock (WiFi off) | A new BLE address every second cycling the 13 eligible prefixes, three rotating serials |

Because every rotation presents an unseen MAC, the HIT pane should fill with distinct badges rather than refreshing a handful — that, not raw hit count, is the signal that the pipeline is working end to end.

Inspired by the same Watch_Dogs vibe as the rest of the WatchFlock ecosystem.
