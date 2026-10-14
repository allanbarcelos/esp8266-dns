# Firmware status and next steps

Status on 2026-10-13, after the round of work from commit `93d7bd9` to `3e95259`. What was implemented afterwards, and in which commit, is in [IMPLEMENTATION.md](IMPLEMENTATION.md).

## Where we are

A 1 MB ESP-01 that keeps the Cloudflare DNS pointing at home, reads a sensor and drives an actuator, and can be controlled from the web panel, a local API, ntfy and MQTT, with Home Assistant finding the device on its own. It updates itself through signed OTA and installs from the browser.

| | |
| --- | --- |
| Firmware | 498,720 of 500,000 bytes (1,280 to spare) |
| Signed OTA package | ~358 KB of 400 KB |
| Static RAM | 37,808 bytes |
| Native tests | 370, with 100 % of lines, functions and branches |
| In CI | no compiler warnings, ASan and UBSan, cppcheck, fuzzing of 5 inputs |

## What changed in this round

### Security

- **ntfy:** sending notifications now validates the certificate. Before, it went unvalidated, and the token and the command topic could be read by anyone on the path (`93d7bd9`).
- **SECURITY.md** fixed: the OTA download does not validate GitHub's certificate. The protection there is the image's RSA signature (`0a1c054`).
- **Setup AP password** drawn per device on first boot, instead of the same `esp8266dns` for everyone. It shows on the serial port, the panel and the web installer (`4d219b1`).
- **Panel and API:** growing wait after 5 wrong logins; ntfy command topic of at least 20 characters without a token; headers against caching and clickjacking; the log says where each command came from (`3e95259`).

### Features

- **Web installer** on GitHub Pages, with ESP Web Tools: flashes the ESP-01 from the browser, without PlatformIO (`b54b4d0`).
- **MQTT on the local network:** state, commands and availability with a last will, in an own client without a library (`14444f7`).
- **Home Assistant discovery:** the device shows up on its own, with the right entities for each sensor and actuator type, and the old ones go away when the module changes (`29047ab`).

### Flash space

MQTT and discovery did not fit. To make them fit without removing anything the user sees:

- a single JSON reader, everything parsed in place as `char*` (−4.7 KB, `4272ca1`);
- TLS roots in DER, generated from `ca/roots.pem` (−1.5 KB, `2c31926`);
- panel photo in WebP quality 40 and discovery without fields HA does not use (−0.7 KB, `4ed23fc`).

### Quality

- `-Wall -Wextra -Werror` in the tests (`dd34b8b`).
- The whole suite again with AddressSanitizer and UndefinedBehaviorSanitizer (`9bdef25`).
- cppcheck in the `check` job (`abfb58f`).
- libFuzzer fuzzing of everything that comes from outside: MQTT packets, commands, the Cloudflare response, the ntfy stream and `config.json` (`458cefb`).

### Documentation

- [On-device test](HARDWARE-TEST.md): checklist to run on an ESP-01 before calling a version stable (`34f2e2a`).
- [Remote access](REMOTE-ACCESS.md): how to use it away from home without opening a router port (`88c2bb4`).

## How it was verified

- Native tests, sanitizers, cppcheck and fuzzing on every change, as in CI.
- MQTT ran against a real Mosquitto: authentication, wrong password, commands, last will on a drop and a retained command on reconnect.
- Discovery ran against a real Home Assistant, with five sensor and actuator combinations, round trips through HA's services and no warning in its log. This test was done by hand, in an environment set up for it; it is not in CI.
- The web installer and the button that reads the AP password were tested in Chrome and Edge, with the serial port simulated.

**Not verified yet:** nothing in this round ran on a real ESP-01. The first step is the [on-device checklist](HARDWARE-TEST.md).

## Decisions worth remembering

- **500,000-byte limit.** A project rule, checked in CI. Every change fits inside it.
- **The ESP-01 photo stays.** If space runs out, it is not the first thing to go.
- **Never open a port to the device.** The ESP-01 does not serve HTTPS; access from outside goes through a tunnel with its own login.
- **Login failures counted per device, not per address.** Someone on the network can delay the owner's login for a few minutes, but not guess the password. A per-address table does not fit in memory.
- **MQTT without TLS.** The only TLS session that fits is already shared by DNS, ntfy and OTA.

## Next steps

In order, with an effort estimate.

1. **Run the on-device test** and fix what comes up. 2 to 4 hours of testing, plus whatever needs fixing.
2. **Stable release channel.** Automatic OTA installs only versions marked as stable; the others stay as beta, for whoever wants them. It matters more now that the device can be controlled from afar. About 1 day; ~0.2 KB of flash.
3. **Measure main-loop stalls.** TLS connections and the MQTT one hold the loop for a few seconds, and meanwhile the panel does not answer and the actuator pulse can run past what was asked. Measure first (a log warning when a cycle goes past, say, 200 ms), then decide. Half a day to measure.
4. **Lean web server.** Replacing the core's server with one made for the panel's few routes should free 5 to 8 KB. It unlocks items 5 and 6, and gives room back for fixes. 3 to 5 days; the panel tests cover the swap.
5. **MQTT with TLS to a cloud broker** (optional, depends on item 4). Control over the internet without a tunnel. It would be one or the other with the ntfy command stream, because only one TLS session fits. About 2 days; ~1.5 KB.
6. **Validate GitHub's certificate on OTA** (depends on item 4). Today the signature protects the image, but someone in the middle of the connection can block updates. Half a day; ~1.5 KB.
7. **Field diagnostics.** Keep the last crash (address and stack) and show it on the panel; publish heap, Wi-Fi signal and uptime over MQTT. About 1 day; the size depends on how much goes in.
8. **End-to-end test in CI.** Bring the Mosquitto and Home Assistant test, manual today, into CI. About 1 day; does not change the firmware.
9. **Name and language.** Choose between `esp8266-dns` and `esp8266-iot` (both show up today) and offer English identifiers in the API and MQTT, keeping the current ones as synonyms. Half a day to 1 day.

What is worth bringing over from Tasmota to this device, and what is not: [PORTING-FROM-TASMOTA.md](PORTING-FROM-TASMOTA.md). Items 1 to 3 there (time zone and window, button, SHT3x) compete with items 5 and 6 of this list for the room item 4 frees.

Battery and solar panel, with or without a charge reading: [BATTERY-AND-SOLAR.md](BATTERY-AND-SOLAR.md). Powering it does not change the firmware. The charge reading competes with the Tasmota document's button for the two free pins.
