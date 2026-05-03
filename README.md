# flock-emitter

Fake-Flock signal emitter for testing detection rigs without driving to a real ALPR pole. Runs on any classic ESP32 (tested on ESP32-WROVER-E). Built to validate [WatchFlock](https://github.com/0xXyc/WatchFlock) firmware and the [SwizFlockHunter](https://github.com/0xXyc/SwizFlockHunter) Flipper companion.

## What it spoofs

Cycles through multiple fake identities on both radios in parallel.

**WiFi side** (rotates every 2.5s):
- Liteon OUI `e4:aa:ea:5f:a1:ce` probing for SSID `homenet` (named, MEDIUM tier)
- Flock direct OUI `b4:1e:52:be:ef:01` with no SSID (hidden, HIGH tier — what real Falcon V2 cameras look like)
- SoundThinking/ShotSpotter OUI `d4:11:d6:ca:fe:02` probing for SSID `test_flck` (CVE-2025-59409 dev SSID)
- Liteon variant `e4:aa:ea:12:34:56` with no SSID (hidden, HIGH)

**BLE side** (rotates every 1s):
- Penguin battery emulation: XUNTONG manufacturer ID `0x09C8` plus 10-digit name pattern, three rotating serials (`TN1234567890`, `TN9876543210`, `TN5555666677`)

**Probe-req bursts** every 800ms via `WiFi.scanNetworks()` with the current spoofed STA MAC. Hits the receiver across all 11 2.4 GHz channels per burst.

## What it CAN'T do

Single-radio limitations on a classic ESP32:

- **Only one MAC at a time on each radio.** To "be" different Flock OUIs we have to physically restart WiFi between MAC changes (~150-300ms downtime per rotation). All BLE adverts come from one chip, so the BLE address stays constant across the three serial rotations.
- **No 5 GHz.** Classic ESP32 is 2.4 only. To validate a receiver's 5 GHz channels you need a real Flock pole or an ESP32-C5 emitter. The receiver should NOT see hits from this rig in 5 GHz only mode.
- **No real Penguin battery firmware.** We craft adverts that match Marauder's detection signature (XUNTONG mfg ID + name shape) but we're not emulating the actual GAP/GATT services a real Penguin runs.

## Hardware

- ESP32-WROVER-E (or any classic ESP32 with WiFi + BLE coex)
- USB-Serial cable (the WROVER's onboard USB or a CP2102 to UART pins)

## Build and flash

Arduino-cli, classic ESP32 target:

```bash
arduino-cli compile -b "esp32:esp32:esp32wrover:PartitionScheme=huge_app" \
  --output-dir ./build flock_tester
arduino-cli upload -b "esp32:esp32:esp32wrover:PartitionScheme=huge_app" \
  -p /dev/cu.usbserial-XXXX --input-dir ./build flock_tester
```

Expected boot output on the WROVER's serial:

```
=========================================================
  Flock Tester multi-identity fake Flock device emitter
=========================================================
  WiFi identities: 4 (rotates every 2500 ms)
  BLE  identities: 3 (rotates every 1000 ms)
[WIFI] -> Liteon-named  mac=e4:aa:ea:5f:a1:ce  ssid=homenet
[BLE] -> identity 0  name=1234567890  serial=TN1234567890
...
```

## Companion debug scripts

Two helper shell scripts for monitoring an ESP32-C5 over USB-CDC without the open-port-resets-the-chip problem (USB-Serial-JTAG asserts reset on DTR pulse during port open):

- `c5-tail.sh` — live tail of the C5's serial output, holds DTR/RTS low to avoid reset
- `c5-cmd.sh "<command>" [seconds]` — send a single CLI command, capture response for N seconds

Both target `/dev/cu.usbmodem5B7B0330131` by default (override via env or arg). Useful for any Flipper + Marauder + ESP32 dev workflow, not just this project.

## Test matrix

Run the WROVER next to a [WatchFlock](https://github.com/0xXyc/WatchFlock) C5 + Flipper rig, then in the [SwizFlockHunter](https://github.com/0xXyc/SwizFlockHunter) FAP:

| Mode | Expected hits |
|------|---------------|
| Dual band (2.4 + 5 GHz) | 2.4 hits sparse (channel hop spends most time on 5 GHz channels) |
| 2.4 GHz only | All 4 WiFi identities cycle through, plus all 3 BLE serials silently |
| 5 GHz only | No hits (correct, WROVER has no 5 GHz radio) |
| BLE Flock (WiFi off) | One unique BLE MAC, three rotating serials |

Inspired by the same Watch_Dogs vibe as the rest of the WatchFlock ecosystem.
