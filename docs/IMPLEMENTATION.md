# What could be implemented, and what was

Status on 2026-10-13. The list came from the documents of the same day: [Status and next steps](STATUS-AND-NEXT-STEPS.md), [What to port from Tasmota](PORTING-FROM-TASMOTA.md) and [Battery and solar panel](BATTERY-AND-SOLAR.md), plus the README's remote log. The round implemented items 5, 6, 7, 8, 10 and 11, one commit per item. Item 7 came first because it freed the room for the others.

After the round and the adjustments at the end of this page (English, four languages, the authenticator, the photo), the firmware is **499,056 bytes**, under the 500,000 cap (944 to spare). Before the lean server it was 499,312. There are 450 native tests, with 100 % of lines, functions and branches. What is left, in order: [STATUS-AND-NEXT-STEPS.md](STATUS-AND-NEXT-STEPS.md).

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
