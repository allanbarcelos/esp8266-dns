# Firmware status and next steps

Status on 2026-10-14, at the merge of the translated pin drawing (after `5c5f7be`). What went in, and in which commit, is in [IMPLEMENTATION.md](IMPLEMENTATION.md).

## Where we are

A 1 MB ESP-01 that keeps the Cloudflare DNS pointing at home, reads a sensor and drives an actuator. It can be controlled from the web panel (in four languages, behind a password and an authenticator), a local API, ntfy and MQTT, and Home Assistant finds the device on its own. It updates itself through signed OTA and installs from the browser.

| | |
| --- | --- |
| Firmware | 499,712 of 500,000 bytes (288 to spare) |
| Signed OTA package | ~363 KB of 400 KB |
| Static RAM | 37,728 bytes |
| Native tests | 452, with 100 % of lines, functions and branches |
| In CI | no compiler warnings, ASan and UBSan, cppcheck, fuzzing of 5 inputs |

## Since the previous status (2026-10-13)

### Space

- **Lean web server** instead of `ESP8266WebServer`: −12.3 KB (`f173944`). That room went to the time window, the SHT3x, the battery gauge, the device name and the panel in four languages.
- **No `sscanf`:** a hand parser for the OTA recovery file, −3.2 KB (`bd2cc11`).
- **MD5 and the TLS 1.0 PRF out of the link** (`TlsTrim.h`): −1.8 KB. Every suite the device offers is AES-GCM, which exists only in TLS 1.2, and there BearSSL uses neither. The fast P-256 stays: the generic one would free about 10 KB more, but every handshake would get slower.
- **HMAC-SHA1 from BearSSL** in `Totp.h` instead of its own SHA-1: −736 bytes. The TLS stack keeps both anyway (SHA-1 for certificate chains, HMAC for the TLS 1.2 PRF). The native tests run the old SHA-1 as a stub (`test/stubs/bearssl/bearssl.h`), checked by the RFC vectors.
- **LittleFS without its debug asserts** (`LFS_NO_ASSERT`, in PlatformIO and CI): −3.4 KB (`32c7779`). Errors still come back as return codes; only the checks for misuse of its API go.

### Security

- **Panel sign-in with a mandatory authenticator** (`1d89fda`). HTTP Basic is gone. The panel uses a form, a session cookie (12 h, RAM only) and an RFC 6238 code. Each code works only once, also after a reboot or with the clock pushed back. Without SNTP (setup access point) the browser's time is used. Changing the password or the authenticator asks again for the current password and a code. Signed-in forms carry a CSRF token per session. Details in [SECURITY.md](../SECURITY.md).

### Features

- **Panel in English, Portuguese, Spanish and French**, from one compressed table that the browser decodes (`bd2cc11`).
- **Settings page**, time zone and time window, SHT3x, MAX17048 battery gauge, device name and hostname, MQTT TelePeriod: see [IMPLEMENTATION.md](IMPLEMENTATION.md).
- **ESP-01 photo back** on the panel (`389e60e`): AVIF, 3.2 KB, transparent background (it suits the dark theme). An own codec decoded by the browser was tried first (`32c7779`). It beat WebP below 2 KB, but AVIF needs no decoder in the firmware. In the same room it gives a much better picture (SSIM 0.963 against 0.886), so the own codec was removed.

### Name

- The project is **esp8266-iot**. The repository is renamed from `esp8266-dns`. See "Before renaming the repository" below.

## How it was verified

- Native tests, sanitizers, cppcheck and fuzzing on every change, as in CI.
- MQTT ran against a real Mosquitto, and discovery against a real Home Assistant. That test was done by hand, in an environment set up for it; it is not in CI.
- The web installer was tested in Chrome and Edge, with the serial port simulated. The AVIF photo was checked in Edge.

**Not verified yet:** none of the work since 2026-10-13 ran on a real ESP-01. That covers the lean server, the time window, the SHT3x, the battery, the move to English, the four languages, the authenticator and the photo. The first step is the [on-device checklist](HARDWARE-TEST.md).

## Decisions worth remembering

- **500,000-byte limit.** A project rule, checked in CI. Every change fits inside it. **Only 288 bytes are left:** any new firmware feature first needs room taken from somewhere else.
- **The fast P-256 stays.** Dropping it frees about 10 KB, but ECDHE and ECDSA on P-256 (every handshake) would use the slower generic code. Not a trade worth making while other room exists.
- **The ESP-01 photo stays.** If space runs out, it is not the first thing to go. Its size is set by `PHOTO_BYTES` in `tools/embed_assets.py` (3,250 today): a smaller photo is the quickest room.
- **Images in formats the browser decodes.** A decoder of our own costs flash; AVIF and SVG cost none.
- **Never open a port to the device.** The ESP-01 does not serve HTTPS; access from outside goes through a tunnel with its own login.
- **Login failures counted per device, not per address.** Someone on the network can delay the owner's login for a few minutes, but not guess the password. A per-address table does not fit in memory.
- **MQTT without TLS.** The only TLS session that fits is already shared by DNS, ntfy and OTA.

## Before renaming the repository

The firmware asks `api.github.com/repos/allanbarcelos/esp8266-dns/releases/latest` for the latest version and accepts only HTTP 200. After a rename, GitHub answers that address with a **301**. Every device with today's firmware would show "GitHub HTTP 301" and **stop updating**. The download itself already follows redirects. The web installer's address (`allanbarcelos.github.io/esp8266-dns`) does not redirect either.

Safe order:

1. Release a firmware that asks for the version by the repository's number (`/repositories/<id>/releases/latest`). That address survives any rename.
2. Wait for the devices to take that update.
3. Rename the repository, and update the old name where it is left: `OTAService.h`, the MQTT discovery (`mf` and `o.name`), the sketch name `esp8266-dns.ino` (CI copies `build/esp8266-dns.ino.bin` and reads its `.map`), the matching tests, and the installer links in the README, [HARDWARE-TEST.md](HARDWARE-TEST.md) and `install/index.html`.

## Next steps

In order, with an effort estimate. Items that change the firmware need room first (see above).

1. **Run the on-device test** and fix what comes up. 2 to 4 hours of testing, plus whatever needs fixing.
2. **Version check that survives the rename** (see above), then the rename itself. Half a day; about the same flash.
3. **Measure main-loop stalls.** TLS connections and the MQTT one hold the loop for a few seconds; meanwhile the panel does not answer and the actuator pulse can run past what was asked. Measure first (a log warning when a cycle goes past, say, 200 ms), then decide. Half a day.
4. **Stable release channel.** Automatic OTA installs only versions marked as stable; the others stay as beta, for whoever wants them. About 1 day; ~0.2 KB.
5. **End-to-end test in CI.** Bring the Mosquitto and Home Assistant test, manual today, into CI. About 1 day; does not change the firmware.
6. **Field diagnostics.** Keep the last crash (address and stack) and show it on the panel; publish heap, Wi-Fi signal and uptime over MQTT. About 1 day; the size depends on how much goes in.
7. **Battery in the API and MQTT**, and the INA219 ([BATTERY-AND-SOLAR.md](BATTERY-AND-SOLAR.md)). Small.
8. **Validate GitHub's certificate on OTA.** Today the signature protects the image, but someone in the middle of the connection can block updates. Half a day; ~1.5 KB.
9. **MQTT with TLS to a cloud broker** (optional). Control over the internet without a tunnel. It would be one or the other with the ntfy command stream, because only one TLS session fits. About 2 days; ~1.5 KB.
10. **Smaller pieces:** a physical button, a backup second Wi-Fi, a remote log through ntfy ([PORTING-FROM-TASMOTA.md](PORTING-FROM-TASMOTA.md)).

Battery and solar panel, with or without a charge reading: [BATTERY-AND-SOLAR.md](BATTERY-AND-SOLAR.md). Powering it does not change the firmware. The charge reading competes with the button for the two free pins.
