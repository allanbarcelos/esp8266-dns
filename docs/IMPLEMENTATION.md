# What could be implemented, and what was

Status on 2026-10-13. The list came from the documents of the same day: [Status and next steps](STATUS-AND-NEXT-STEPS.md), [What to port from Tasmota](PORTING-FROM-TASMOTA.md) and [Battery and solar panel](BATTERY-AND-SOLAR.md), plus the README's remote log. The round implemented items 5, 6, 7, 8, 10 and 11, one commit per item. Item 7 came first because it freed the room for the others.

After the round and the adjustments at the end of this page (English, four languages, the authenticator, the photo), the firmware was **499,680 bytes**, under the 500,000 cap (320 to spare). The size and security round that closes this page took it to **475,136 bytes** (24,864 to spare), with 491 native tests at 100 % of lines, functions and branches. What is left, in order: [STATUS-AND-NEXT-STEPS.md](STATUS-AND-NEXT-STEPS.md).

## Do not change the firmware

| # | Item | Effort | Implemented | Commit |
|---|---|---|---|---|
| 1 | On-device test with the [HARDWARE-TEST.md](HARDWARE-TEST.md) checklist: nothing from today's round ran on a real device | 2–4 h + fixes | No | — |
| 2 | End-to-end test in CI with Mosquitto and Home Assistant (manual today) | ~1 day | No | — |
| 3 | Measure main-loop stalls: a log warning when a cycle goes past ~200 ms | ½ day | No | — |

## Fit before the lean server, with minimal panel text

| # | Item | Flash / effort | Implemented | Commit |
|---|---|---|---|---|
| 4 | Stable release channel: automatic OTA installs only versions marked as stable | ~0.2 KB | No | — |
| 5 | MQTT TelePeriod: the state goes out again every 5 min, even without a change | +16 B | Yes | `dd7640d` |
| 6 | Device name (panel and Home Assistant) and DHCP hostname | +1.8 KB | Yes | `be023bb`, moved in `17b1e99` |

## Depended on the lean web server

| # | Item | Flash / effort | Implemented | Commit |
|---|---|---|---|---|
| 7 | Lean web server replacing `ESP8266WebServer`, without losing a route, login, CSRF, the API or an embedded file | **−12.3 KB** | Yes | `f173944` |
| 8 | Time zone and time window on the automatic rule; local time under Status | +2.6 KB | Yes | `18cfe91`, adjusted in `17b1e99` and `bd5b75b` |
| 9 | Physical button on a free pin (hold turns off, a tap switches to manual) | small | No | — |
| 10 | SHT30/SHT31 sensor over I2C (GPIO0 with GPIO2, or RX with TX) | +1.1 KB | Yes | `28e917f` |
| 11 | MAX17048 battery gauge and, in the top-right corner of the panel: on supply, on battery (%) or charging from solar | +2.4 KB | Yes, the MAX17048 only; the INA219 was left out | `98588a9`, moved in `17b1e99` |
| 12 | Backup second Wi-Fi | the form | No | — |
| 13 | Field diagnostics: last crash and heap, RSSI and uptime over MQTT | ~1 day | No | — |
| 14 | Validate GitHub's certificate on OTA | ~1.5 KB | No | — |
| 15 | MQTT with TLS to a cloud broker (one or the other with the ntfy commands) | ~1.5 KB, ~2 days | No | — |
| 16 | Remote log through ntfy (`[log]` on the data topic) | small | No | — |

## Decision without code

| # | Item | Effort | Implemented | Commit |
|---|---|---|---|---|
| 17 | Project name (`esp8266-dns` or `esp8266-iot`) and English identifiers in the API and MQTT, with the current ones as synonyms | ½–1 day | Yes: everything moved to English, with no Portuguese synonyms; the name is **esp8266-iot**. The repository rename waits for a version check that survives it ([STATUS](STATUS-AND-NEXT-STEPS.md#before-renaming-the-repository)) | `e2319b4`, `39d0393` |

## Notes on what went in

- **7, lean server.** `HttpServer.h` serves one client at a time and closes the connection at the end of each response. The raw body is still in `plain`, so `curl -d on` works the same. The panel tests now speak real HTTP to the server, over a simulated socket.
- **8, time window.** Outside the window, and with the clock still in 1970, automatic mode keeps the actuator off. A manual command still holds until `auto`. The API and MQTT carry `"hours"` on the actuator when the window is on.
- **10, SHT3x.** The chosen pin is SDA; SCL is the pair's other pin. I2C is done in software (`I2cBus.h`), without the Wire library. The panel does not let the actuator sit on SCL.
- **11, battery.** The state comes from the MAX17048 charge rate. Rising, the battery is charging. Falling, or below 98 % and not charging, the device is on battery. Full and idle, or with no gauge configured, it is on supply. From the rate alone, the gauge cannot tell the solar panel from a power supply: any charge shows as "Charging". The charge does not go to the API or MQTT yet.
- **17, English.** Commands, sensor and actuator identifiers, panel, logs, ntfy prefixes (`[actuator]`, `[error]`), code, tests, tools, CI, the web installer and the docs are in English. The Portuguese commands (`ligar`, `pulso`…) are no longer accepted, and a configuration saved with the Portuguese identifiers (`rele`, `ventilador`, `contato`…) is not migrated: after the update that part stays off, with a warning on the IoT page, until the module is chosen again. The v1 migration (types by table position) still works.
- **Not verified on a device yet:** items 7, 8, 10, 11 and 17 depend on real hardware, network and browser. Go through item 1 before publishing.

## Adjustments after the round

| Commit | What changed | Firmware |
|---|---|---|
| `17b1e99` | **Settings** menu (`/config`) with name, hostname, time zone and battery gauge, moved out of the home page's "Device" card | 495,072 |
| `bd5b75b` | Time zone picked from a list of the 38 official zones (UTC−12 to UTC+14), each with an example place; no automatic daylight saving | 495,456 |
| `e2319b4` | Source, tests and public interface (commands, identifiers, panel, logs) in English | 494,416 |
| `39d0393` | Tooling, CI and the web installer in English; the installer still reads the AP password from firmware that prints `senha:` | 494,416 |
| `3d65b57` | Pin drawing on the IoT page in English | 494,384 |
| `bd2cc11` | Panel in English, Portuguese, Spanish and French (Settings, or the browser's language), from one compressed table decoded by the browser; `sscanf` replaced by a hand parser (−3,152 bytes) to make room | 497,936 |
| `1d89fda` | Panel sign-in with a mandatory authenticator (RFC 6238) instead of HTTP Basic: session cookie, each code once, the browser's time while SNTP is not set, password and code again to change either, CSRF token per session. The ESP-01 photo left the sketch to make room | 499,952 |
| `32c7779` | ESP-01 photo back through an own codec decoded by the browser (wavelet, 1.3 KB + 1.8 KB of script); LittleFS without its debug asserts (`LFS_NO_ASSERT`, −3.4 KB) | 499,968 |
| `389e60e` | Photo as AVIF instead (3.2 KB, transparent background, no decoder in the firmware): SSIM 0.963 against the codec's 0.886 in the same room; the codec removed | 499,936 |
| Pin drawing translated | The pin drawing in the panel language too: i18n.js translates its labels from the same table; its URL now carries a content hash, so the browser fetches a new drawing after an update instead of keeping the old one for 7 days. To fit after the merge with the authenticator and the photo, `TlsTrim.h` also leaves MD5 and the TLS 1.0 PRF out (−1.8 KB; neither is reachable with AES-GCM suites); the fast P-256 stays | 499,024 |
| Branch coverage | Two tests (the first password, a blank key inside a session) and one more invalid browser time; the CSRF check runs both sides, so its guard against an empty token is no longer a branch no test can reach. Back to 100 % of branches on CI | 499,056 |
| Authenticator QR code | /mfa also shows the key as a QR code (otpauth://, named after the device), drawn by i18n.js: a QR encoder of its own for the one size needed (version 5-L, byte mode, mask 0), since the smallest libraries take several KB. Nothing changes in the C++ | 499,952 |
| Menu and footer | The menu only where its pages open (signed in with the authenticator, or the first setup on the access point): not on the sign-in page; on the authenticator page only Sign out. Footer held at the bottom: version on the left, "Created by Barcelos.Dev", the GitHub mark linking to the repository on the right. The links, the credit and the mark come from the gzipped i18n.js and panel.css. Room from `Totp.h` taking HMAC-SHA1 from BearSSL, already in the firmware (−736 bytes) | 499,712 |
| ESP-01 photo on white | The photo keeps its white background: transparent, it looked loose on the dark page. Without the alpha plane the same budget gives AVIF quality 48 instead of 44 (rms error 6.6 against 7.4) | 499,680 |

## Size and security round (2026-10-14)

A measured analysis of the firmware (map, symbols, prototypes built from the same commit) and a security review, then one commit per item. The native tests, ASan and UBSan, cppcheck and the fuzzers ran green on Linux at every step. The starting point, built as CI does, was 499,248 bytes.

| Commit | What changed | Firmware |
|---|---|---|
| `3346611` | HTTP request deadline (3 s) and at most 32 header lines: a slow client could hold the loop and the actuator's safety timer | 499,312 |
| `bb709c6` | libgcc unwinder and newlib's stdio `FILE` layer out of the link (`TlsTrim.h`) | 489,056 |
| `195dda0` | TLS roots as precomputed trust anchors: no X.509 decoder, ~4 KB less heap per session | 485,856 |
| `56b3098` | One-shot HTTPS requests (`TlsClient.h`) instead of `HTTPClient`; a fuzz target for the answers | 480,048 |
| `78028e0` | Own JSON reader and writer (`Json.h`); config.json as a table of fields; ArduinoJson only in the tests | 473,600 |
| `568c261` | OTA signature bound to the version tag (`firmware.tagged.bin`): no downgrade through a man in the middle | 473,872 |
| `c08bc96` | Three sign-in counts (API, the owner's address, everyone else) | 474,064 |
| `d29393f` | Panel password as PBKDF2-HMAC-SHA1 with a salt, converted on the first boot | 475,200 |
| `950d44b` | Cloudflare IDs (32 hex digits) and token (letters, digits, _ and -) checked before they are saved | 475,600 |
| `a13c414` | Host check against DNS rebinding; Settings, Other addresses for a tunnel or proxy | 476,816 |
| `c89ddc3` | Authenticator steps from the browser's clock kept apart (`totp_bstep`) | 477,040 |
| `9887b1e` | CI actions pinned by commit, Dependabot for updates | 477,040 |
| `d4331c2` | Web panel code: route table, streaming escape, rows without `String` temporaries, shared strings | 475,136 |

## Remote access round (2026-10-15)

The panel and the API from anywhere at the user's own HTTPS address, with no open port and no computer at home: the device keeps a WebSocket to a Cloudflare Worker it deploys itself with the API token ([REMOTE-ACCESS.md](REMOTE-ACCESS.md)). The Cloudflare page went to the token alone along the way. Native tests, ASan and UBSan, cppcheck and the fuzzers green on Linux at every step; the Worker's tests at 100 % on Node.

| Commit | What changed | Firmware |
|---|---|---|
| `f9af9be` | `HttpServer` takes a request from any link (`HttpLink`), not only the socket | 475,312 |
| `d00b6e3` | WebSocket client (`WsClient.h`), with a fuzz target; not linked yet | 475,312 |
| `630d609` | Relay connection on the device (`RelayService.h`): requests through the panel's server, hello with a keyed hash of the API token, state on change; a fuzz target for the frames | 479,024 |
| `8a7d91e` | The Worker (`relay/worker.mjs`): Durable Object with hibernation, queue, rate limit, last state for the API, ntfy polling; tests on Node in CI | 479,024 |
| `5a706c8` | Cloudflare from the token alone: domain list, DDNS record by name (created if missing), jobs from the loop; streaming JSON reader (`JsonStream.h`) and one API call shared with the DDNS (`CloudflareApi.h`) | 483,680 |
| `30d78ac` | The device deploys its Worker: script byte-pair encoded in the firmware (`RelayScript.h`, 7.8 KB in 4.2 KB), own domain or workers.dev, a new secret per upload, redeploy after a protocol change | 491,152 |
| `7b87752` | Remote access page | 493,664 |
| `ca395c5` | ntfy commands through the relay, with the device's own stream as the fallback; poll interval on the ntfy page | 495,104 |
| Token length | Cloudflare tokens of the new format (`cfut_`, 53 characters) fit: the field took 47 | 495,200 |
| `9fb5361` | More heap for the OTA check: the relay and the ntfy stream allocate their TLS client only while connected; MQTT pauses during the check too | 495,168 |
| `c1df65a` | One arbiter for the network turn (`NetArbiter.h`): priority, waiting for the heap instead of failing, the turn on Status | 496,016 |
| `8d8ca03` | The slowest loop and the count over 200 ms on Status; each new worst in the log, with the service that held the turn | 496,320 |
| `24b3b40` | Crash place, from the core's crash callback (it did not run on a real watchdog reset) | 496,704 |
| `3fba386` | Each part of the loop records itself in RTC memory as it starts; after an exception or a watchdog reset the next boot logs it with the chip's recorded address | 496,656 |
| `ec24db1` | The relay deploy overflowed the loop's 4 KB stack (software watchdog during "turn on"): one Answer for every call and the request bodies in helpers, ~1 KB less at the deepest call; the lowest free stack on Status | 496,976 |
| Gzip through the relay | The Worker passes the device's gzipped CSS/JS as they are (`encodeBody: 'manual'`) instead of gzipping them again; relay protocol 2, so devices redeploy the Worker after the update | 496,976 |
