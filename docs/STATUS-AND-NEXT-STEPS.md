# Firmware status and next steps

Status on 2026-10-15, after the remote access round (`f9af9be` to `ca395c5`). What went in, and in which commit, is in [IMPLEMENTATION.md](IMPLEMENTATION.md).

## Where we are

A 1 MB ESP-01 that keeps the Cloudflare DNS pointing at home, reads a sensor and drives an actuator. It can be controlled from the web panel (in four languages, behind a password and an authenticator), a local API, ntfy and MQTT, and Home Assistant finds the device on its own. The panel and the API also open from anywhere, at the user's own HTTPS address, through a Cloudflare Worker the device sets up itself. It updates itself through signed OTA and installs from the browser.

| | |
| --- | --- |
| Firmware | 499,856 of 500,000 bytes (144 to spare) |
| Signed OTA package | ~354 KB of 400 KB |
| Static RAM | 39,392 bytes |
| Native tests | 601, with 100 % of lines, functions and branches |
| Panel IoT script tests | 24 on Node, 100 % of lines, functions and branches, on the source and minified |
| Relay Worker tests | 24 on Node, 100 % of lines, functions and branches, on the source and on the embedded script |
| In CI | no compiler warnings, ASan and UBSan, cppcheck, fuzzing of 9 inputs |

## Remote access round (2026-10-15)

- **Remote access without a port or a computer at home.** The device keeps a WebSocket over TLS to a Cloudflare Worker in the user's account; requests to the user's address go down it and through the same panel, sign-in and API as at home. The device deploys the Worker itself with the API token (own domain or workers.dev), with a new secret per upload, and again after an update that changes the relay protocol. With the device offline, `/api/state` answers from the last state to the token holder. The Worker limits sign-in and API requests per address.
- **ntfy commands through the relay.** Only one TLS session fits, so the Worker polls the command topic for the device (2, 5, 10 or 30 s) and sends the commands down the relay; if the relay stays down a minute, the device's own stream takes over.
- **Cloudflare from the token alone.** The page lists the token's domains and picks the DDNS record by name, created if missing. No IDs to copy.
- **Cost:** +20 KB of firmware, of which 4.2 KB is the Worker itself (byte-pair encoded) and 1.9 KB the new panel text in four languages. About 1.2 KB more static RAM.

- **One arbiter for the network** (`NetArbiter.h`): the services that need the single TLS session queue by priority (OTA, public IP, DNS, Cloudflare setup, ntfy post, relay, ntfy stream), everyone else closes before the turn runs, and a new session waits up to 30 s for the heap it needs (MQTT paused meanwhile) instead of failing. Status shows who has the network and who waits, the slowest loop and how many passed 200 ms.
- **Where a crash happened:** each part of the loop (a service of the network turn, or web, wifi, iot, mqtt) writes its name to RTC memory as it starts. After an exception or a watchdog reset the next boot logs it with the address the chip recorded: `Crashed in: ip near 0x4021...`. The address points into the ELF of that release. (A first version wrote from the core's crash callback, which did not run on a real software watchdog reset.)
- **The relay deploy overflowed the loop's stack** (found with the address above, `epc1` in `WiFiClientSecureCtx::_run_until`, on v154). The loop runs on a 4 KB stack; the deploy's frames (`_deploy` 1,568 bytes, `_upload` 656, `cfapi::call` 480, `https::exchange` 176) plus the TLS write path and the interrupts on top went past it. Below the stack sit the core's own fields: once overwritten, the core can no longer yield, and the software watchdog fires while TLS waits for data. One Answer serves every call of the deploy now, and the request bodies live in helpers that return before the upload: `_deploy` 896 bytes, `_upload` 304. Status shows the lowest free stack since boot.
- **The panel's CSS and JS came through the relay as binary:** the device sends them gzipped, and a Worker gzips again a body whose `Content-Encoding` says gzip unless the Response is built with `encodeBody: 'manual'`. Fixed in the Worker; the relay protocol went to 2, so a device on the new firmware uploads the new Worker by itself.

**Not verified on a device yet:** the whole round. The Cloudflare API calls (zone list, record creation, Worker upload with its Durable Object migration, custom domain, workers.dev) and the relay's heap with MQTT connected need the [on-device checklist](HARDWARE-TEST.md), section 10c, with a real Cloudflare account.

## Size and security round (2026-10-14)

From 499,248 to 475,136 bytes (−24 KB, a measured analysis first, every step with the native tests, the sanitizers and the fuzzers green), plus the fixes from a security review. Commits in [IMPLEMENTATION.md](IMPLEMENTATION.md).

### Space

- **C runtime:** the libgcc unwinder (nothing unwinds with `-fno-exceptions`) and newlib's stdio `FILE` layer (printf now goes through `vsnprintf`) out of the link: −10.3 KB.
- **TLS roots as trust anchors** (name and key only): −3.2 KB of flash, about 4 KB less heap during each TLS session, no certificate parsing before the handshake.
- **One-shot HTTPS without `HTTPClient`** (`TlsClient.h`): −5.8 KB.
- **Own JSON reader and writer** (`Json.h`), with config.json as a table of fields: −6.4 KB. ArduinoJson stays only in the tests, as an independent check.
- **Web panel code:** routes in a table, escaping as the text goes out, status rows without `String` temporaries, shared strings: −1.9 KB, same pages.

### Security

- **OTA signature bound to the version tag.** Over the unvalidated GitHub connection, an older release, genuinely signed, could be served as a newer one; the signature now covers the tag the device asked for (`firmware.tagged.bin`).
- **HTTP request deadline** (3 s) and a header cap: a slow client could hold the loop, and with it the actuator's safety timer.
- **Three sign-in counts** (API, the owner's address, everyone else): one wrong request every 5 min could keep the owner and the API out for good.
- **Panel password as a salted hash** (PBKDF2-HMAC-SHA1), converted on the first boot.
- **Host check** against DNS rebinding, with Settings, Other addresses for a tunnel's name. A Cloudflare tunnel under its own name needs that name once (see [REMOTE-ACCESS.md](REMOTE-ACCESS.md)).
- **Cloudflare IDs and token** checked before they are saved.
- **Authenticator steps from the browser's clock kept apart**, so a browser clock running ahead cannot lock the authenticator.
- **CI actions pinned by commit**, with Dependabot.

**Not verified on a device yet:** everything in this round, like the previous one. HTTP/1.0 to the Cloudflare API and the tagged OTA signature in particular need the [on-device checklist](HARDWARE-TEST.md).

## Before that (2026-10-13 to 2026-10-14)

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
- **ESP-01 photo back** on the panel (`389e60e`): AVIF, 3.2 KB, on its white background (first transparent; on the dark page the board looked loose). An own codec decoded by the browser was tried first (`32c7779`). It beat WebP below 2 KB, but AVIF needs no decoder in the firmware. In the same room it gives a much better picture (SSIM 0.963 against 0.886), so the own codec was removed.

### Name

- The project is **esp8266-iot**. The repository is renamed from `esp8266-dns`. See "Before renaming the repository" below.

## How it was verified

- Native tests, sanitizers, cppcheck and fuzzing on every change, as in CI.
- MQTT ran against a real Mosquitto, and discovery against a real Home Assistant. That test was done by hand, in an environment set up for it; it is not in CI.
- The web installer was tested in Chrome and Edge, with the serial port simulated. The AVIF photo was checked in Edge.

**Not verified yet:** none of the work since 2026-10-13 ran on a real ESP-01. That covers the lean server, the time window, the SHT3x, the battery, the move to English, the four languages, the authenticator and the photo. The first step is the [on-device checklist](HARDWARE-TEST.md).

## Decisions worth remembering

- **500,000-byte limit.** A project rule, checked in CI. Every change fits inside it. 24,864 bytes are left after the size round.
- **The fast P-256 stays.** Dropping it frees about 10 KB, but ECDHE and ECDSA on P-256 (every handshake) would use the slower generic code. Not a trade worth making while other room exists.
- **The ESP-01 photo stays.** If space runs out, it is not the first thing to go. Its size is set by `PHOTO_BYTES` and `PHOTO_WIDTH` in `tools/embed_assets.py` (1,000 bytes at 140 px today, from 3,250 at 220 px): a smaller photo is the quickest room.
- **Images in formats the browser decodes.** A decoder of our own costs flash; AVIF and SVG cost none.
- **Never open a port to the device.** The ESP-01 does not serve HTTPS; access from outside goes through a tunnel with its own login.
- **Three sign-in counts, not one per address.** The API token, the address of the last sign-in and everyone else each have their own. A full table per address does not fit in memory.
- **HTTP/1.0 for the device's own requests.** No chunked encoding to decode, as the OTA and the ntfy stream already did.
- **MQTT without TLS.** The only TLS session that fits is already shared by DNS, ntfy and OTA.

## Before renaming the repository

The firmware asks `api.github.com/repos/allanbarcelos/esp8266-dns/releases/latest` for the latest version and accepts only HTTP 200. After a rename, GitHub answers that address with a **301**. Every device with today's firmware would show "GitHub HTTP 301" and **stop updating**. The download itself already follows redirects. The web installer's address (`allanbarcelos.github.io/esp8266-dns`) does not redirect either.

Safe order:

1. Release a firmware that asks for the version by the repository's number (`/repositories/<id>/releases/latest`). That address survives any rename.
2. Wait for the devices to take that update.
3. Rename the repository, and update the old name where it is left: `OTAService.h`, the MQTT discovery (`mf` and `o.name`), the sketch name `esp8266-dns.ino` (CI copies `build/esp8266-dns.ino.bin` and reads its `.map`), the matching tests, and the installer links in the README, [HARDWARE-TEST.md](HARDWARE-TEST.md) and `install/index.html`.

## Next steps

In order, with an effort estimate. Items that change the firmware need room first: 1.5 KB are left. The PZEM-004T energy meter (about 1 KB with its Home Assistant entities) waits for room.

1. **Run the on-device test** and fix what comes up, remote access included (section 10c, with a Cloudflare account). 3 to 5 hours of testing, plus whatever needs fixing.
2. **Make room.** The long help texts of the Cloudflare, remote access and ntfy pages (four languages each) and the Worker's own size are the first candidates. Half a day.
3. **Version check that survives the rename** (see above), then the rename itself. Half a day; about the same flash.
4. **Act on the loop measurements.** Status now shows the slowest loop and how many passed 200 ms, and the log names the service of each new worst. With field numbers, decide what to shorten (the TLS handshakes are the expected culprits). Half a day.
5. **Stable release channel.** Automatic OTA installs only versions marked as stable; the others stay as beta, for whoever wants them. About 1 day; ~0.2 KB.
6. **End-to-end test in CI.** Bring the Mosquitto and Home Assistant test, manual today, into CI. About 1 day; does not change the firmware.
7. **Field diagnostics.** Keep the last crash (address and stack) and show it on the panel; publish heap, Wi-Fi signal and uptime over MQTT. About 1 day; the size depends on how much goes in.
8. **Battery in the API and MQTT**, and the INA219 ([BATTERY-AND-SOLAR.md](BATTERY-AND-SOLAR.md)). Small.
9. **Validate GitHub's certificate on OTA.** Today the signature protects the image, but someone in the middle of the connection can block updates. Half a day; ~1.5 KB.
10. **Smaller pieces:** a physical button, a backup second Wi-Fi, a remote log through ntfy ([PORTING-FROM-TASMOTA.md](PORTING-FROM-TASMOTA.md)).

Battery and solar panel, with or without a charge reading: [BATTERY-AND-SOLAR.md](BATTERY-AND-SOLAR.md). Powering it does not change the firmware. The charge reading competes with the button for the two free pins.
