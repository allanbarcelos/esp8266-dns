# 📘 ESP8266 IoT — Cloudflare DDNS, OTA and local IoT

Firmware for the **ESP-01 (ESP8266, 1 MB)**. One device does three jobs:

1. **Dynamic DNS**: keeps a Cloudflare A record pointing at the residential public IP and sends that IP through [ntfy.sh](https://ntfy.sh).
2. **IoT**: one sensor and one actuator, each optional. Operated from the panel, from a **local API** that works without internet, and from **ntfy commands** delivered in about a second.
3. **Self-maintenance**: signed OTA updates from GitHub Releases, Wi-Fi/memory watchdogs and a daily reboot.
4. **Remote access**: the panel and the API at an HTTPS address of yours from anywhere, through a Cloudflare Worker the device sets up itself. No open port, no extra computer.

The web panel is self-contained (CSS and images are in the firmware), so it works in setup mode and on networks with no internet.

---

## 🧩 Architecture

Each service is a header with dependencies injected by reference, composed in layers by `App.h`. The `.ino` only calls `app.begin()` and `app.tick()`. Every `tick(now)` is non-blocking.

| File                 | Responsibility                                                                   |
| -------------------- | -------------------------------------------------------------------------------- |
| `App.h`              | Service composition, `setup()`/`loop()`                                          |
| `NetArbiter.h`       | One TLS session at a time: the network turn by priority, waiting for the heap; slowest loop; where a crash happened |
| `Config.h`           | Configuration and runtime state; AP credentials; `IoTConfig`                    |
| `ConfigStore.h`      | `/config.json` (LittleFS) with **atomic writes**, a version number and migrations |
| `Json.h`             | Small JSON reader and writer (config, state, ntfy answers)                       |
| `Password.h`         | Panel password as PBKDF2-HMAC-SHA1 with a random salt                            |
| `Logger.h`           | Ring buffer of log lines (Serial + panel)                                        |
| `Clock.h`            | SNTP wall clock (required to validate TLS certificates)                          |
| `Totp.h`             | Authenticator codes (RFC 6238): HMAC-SHA1 from BearSSL, 30 s, 6 digits           |
| `TlsTrim.h`          | Leaves unused BearSSL and C runtime code out of the firmware (sketch only)       |
| `TlsClient.h`        | TLS client (small buffers, ECDHE + AES-GCM) and one-shot HTTPS requests, body in parts |
| `WiFiService.h`      | Connection, configuration mode (AP), reconnection and watchdogs                  |
| `PublicIPResolver.h` | Public IPv4 over HTTPS every 5 min (trust anchors in `CloudflareCA.h`)           |
| `ReverseDNS.h`       | Reverse name (PTR) of the public IP, asked to the network's DNS over UDP        |
| `CloudflareApi.h`    | One Cloudflare API call; the answer read as it streams in (`JsonStream.h`)       |
| `JsonStream.h`       | JSON read one byte at a time, for answers too big to keep (zone lists)           |
| `CloudflareDNS.h`    | PATCHes the DDNS record through the Cloudflare API                               |
| `CloudflareSetup.h`  | From the token alone: domains, the DDNS record by name, and the relay Worker's deploy |
| `RelayService.h`     | Remote access: keeps a WebSocket to the relay Worker and serves its requests     |
| `WsClient.h`         | WebSocket client (RFC 6455), only what the relay uses                            |
| `RelayScript.h`      | The relay Worker (`relay/worker.mjs`), minified and byte-pair encoded by `tools/embed_relay.py` |
| `NtfyNotifier.h`     | ntfy both ways: publishes IP and IoT events; subscribes to the command topic    |
| `MqttService.h`      | MQTT on the local network: state, commands and availability                     |
| `OTAService.h`       | Version check and signed firmware download (signature bound to the tag)          |
| `IoTCatalog.h`       | Sensor and actuator catalog, by stable identifier and capability                |
| `IoTCommand.h`       | Text command parser shared by panel, API and ntfy                               |
| `IoTService.h`       | Sensor reading, actuator output, rule, pulse, safety off, JSON state            |
| `IoTState.h`         | Last manual command (`/iot.state`), for "restore after power loss"              |
| `I2cBus.h`           | Software I2C, master only (SHT3x and the battery gauge)                          |
| `PowerService.h`     | MAX17048 battery gauge: on supply, on battery (%) or charging                    |
| `Ws2812.h`           | Single-colour WS2812 driver, no library                                          |
| `HttpServer.h`       | Own HTTP server with only what the panel uses (replaces `ESP8266WebServer`)      |
| `WebService.h`       | HTTP panel (sign-in, session, authenticator, CSRF, escaping) and the local API (Bearer token) |
| `Assets.h`           | Panel images embedded in the firmware (generated by `tools/embed_assets.py`)    |
| `WebAssets.h`        | Panel CSS and JavaScript, gzip (generated from `web/` by `tools/embed_web.py`)  |
| `I18n.h`             | Panel text markers (`L_...`), generated from `web/i18n.tsv` by `tools/embed_web.py` |
| `CloudflareCA.h`     | TLS roots in DER (generated from `ca/roots.pem` by `tools/embed_ca.py`)          |
| `Version.h`          | `firmware_version` (injected by CI; `"dev"` for local builds)                    |

---

## 🚀 First use

1. Flash from the browser with the **[web installer](https://allanbarcelos.github.io/esp8266-dns/)** (Chrome or Edge on a computer, nothing to install) or over serial (see [Local build](#-local-build)). Either way, the ESP-01 needs **GPIO0 tied to GND** when the adapter is plugged in; release it after flashing and plug it in again.
2. With no Wi-Fi configured, the ESP creates the **`ESP_Config`** network. The password is **drawn per device** on first boot (12 characters) and printed on the serial port every 30 s (`AP ESP_Config, password: ...`); the web installer reads it with the **Read the device password** button. Afterwards it shows on the panel, under **Status**. For a fixed password, build with `-DAP_PASSWORD=\"...\"`.
3. Open `http://192.168.4.1`, enter the Wi-Fi credentials and **set the panel user and password** (at least 8 characters).
4. After the reboot, open the ESP's local IP and **sign in** with the user and password. The panel shows an authenticator key once, as text and as a QR code. Scan it or type it into an authenticator app and enter the 6-digit code. The code is checked against the device clock; until SNTP has set it (for example on the setup access point), the browser sends its own time with the form. Until that code is accepted, the other pages stay closed. The side menu (a button at the top on a phone) leads to every page; **Home** sums up the network, the sensor and actuator in use and, once there is a Cloudflare token, the remote address and the DDNS record. Then configure **Cloudflare**, **ntfy** and **IoT** as needed; each part is optional. The **Settings** page holds what applies to the whole device: the Cloudflare API token (with **Unregister**, which removes the token and every Cloudflare setting from the device), the panel password and the authenticator, and a **name** (shown at the top of the panel and in Home Assistant), a **hostname** (what the router shows in its DHCP list; it applies on the next Wi-Fi connection), the **time zone** (for the rule's time window) and the panel **language**.

> The panel requires a password and an authenticator (6 digits, a new code every 30 s). Until a password is set, every page redirects to `/password`. A device updated from an older firmware has no authenticator yet: sign in with the password and the panel shows a new key. Each code works once. After 5 wrong passwords, codes or API tokens in a row, each new mistake doubles a wait from 2 s up to 5 min. The sign-in lasts 12 hours, is forgotten on reboot, and a new sign-in replaces the previous browser. Changing the password or the authenticator asks for the current password and a code; changing the password signs out. HTTP Basic authentication is gone: scripts use the local API with its Bearer token.
> To use it away from home, turn on **Remote access** ([docs/REMOTE-ACCESS.md](docs/REMOTE-ACCESS.md)): never forward a router port to the device.
> If the configured Wi-Fi fails at boot, the ESP brings up the AP **and** keeps trying the network in the background. Once it connects, the AP is turned off.

### Panel language

The panel speaks **English, Portuguese, Spanish and French**. It opens in the browser's language (English when that is none of the four); **Settings → Language** fixes one for every browser. The text of the four languages sits in the firmware as one compressed table (`/i18n.js`), so it works in setup mode and without internet. The device sends short markers instead of the text and the browser writes the page, and redraws the pin drawing with its labels translated (the drawing itself stays in English, for opening it directly); it caches the table and downloads it again only after an update that changes the text. The panel needs JavaScript. Logs, the API, MQTT and ntfy stay in English.

Reserve the device's IP in your router (DHCP reservation): the panel and the local API are reached by IP. Name discovery over mDNS (`.local`) was measured and does not fit (see [Flash budget](#-flash-budget)).

### Cloudflare token

The **Cloudflare** page asks only for an API token. **Save and check** lists the domains it reaches; the DDNS record is then picked by domain and name (`home` or `home.example.com`) and created if it does not exist. No zone or record IDs to copy. The work runs in the background and the page refreshes until it is done; a failure shows Cloudflare's own message.

Create a custom token with **Zone → Zone → Read** and **Zone → DNS → Edit** for the DDNS, plus **Account → Workers Scripts → Edit**, **Account → Account Settings → Read** and **Zone → Workers Routes → Edit** for [remote access](docs/REMOTE-ACCESS.md). Restrict the zones to the ones in use. A device set up before keeps its IDs and shows its zone as selected.

---

## 🔌 IoT

The ESP-01 exposes four usable pins (GPIO0, GPIO2, TX/GPIO1, RX/GPIO3) and no analog input, so the device drives **one sensor and one actuator**, both digital. The pin drawing is on the IoT page (`docs/esp01-pinout.svg`, embedded).

### What the device does

The IoT page starts with a single choice:

| Role | Behaviour |
| --- | --- |
| **Sensor only** | Reads and publishes to ntfy (`[sensor] …`). |
| **Actuator only** | Obeys commands from the panel, the local API and ntfy. |
| **Sensor and actuator** | Both; with the **rule** on, the sensor drives the actuator in automatic mode. A manual command wins until `auto`. |
| **Nothing** | IoT off; the pins are left alone. |

Each choice shows only the fields it needs. Every sensor and actuator type shows a one-line wiring hint (pull-up, supply, GPIO0/GPIO2 at boot).

### Sensors

| Identifier | Sensor | Reading |
| --- | --- | --- |
| `dht22`, `dht11` | Temperature and humidity | °C and % (the rule uses the one you pick) |
| `ds18b20` | Temperature (1-Wire) | °C |
| `sht3x` | Temperature and humidity (SHT30/SHT31, I2C at address 0x44): SDA on the chosen pin and SCL on its pair (GPIO0 with GPIO2, RX with TX), with 4.7 kΩ pull-ups on both | °C and % (like the DHT) |
| `pir` | Presence | presence / free |
| `contact` | Door or window (reed) | open / closed |
| `button` | Button or switch | pressed / released |
| `digital` | Generic digital output (rain, gas, flame, soil, float, sound) | state names you choose |
| `distance` | HC-SR04 / JSN-SR04T (trig and echo on one pin) | cm |
| `pulses` | Flow or pulses (YF-S201) | pulses per 10 s |

### Actuators, by capability

The capability decides the control shown in the panel and the commands accepted.

| Capability | Identifiers | Control in the panel | Commands |
| --- | --- | --- | --- |
| **On/off** | `relay`, `pump`, `valve`, `lock`, `buzzer` | switch + pulse | `on`, `off`, `toggle`, `pulse 5` |
| **Level (PWM)** | `led`, `fan`, `motor` | switch + 0–100 % slider + pulse | the above + `50%`, `level 50` |
| **Position** | `servo` | open/close + 0–180° slider + pulse | `open`, `close`, `angle 90`, `pulse 3` |
| **Colour** | `strip` (WS2812, up to 60 LEDs) | switch + colour picker + brightness | the above + `color #ff8800`, `brightness 40` |

Every actuator also accepts `auto` (back to the rule) and `status` (no change; returns the state). Commands are case-insensitive. `open`/`close` are on/off: the servo goes to its open or closed angle.

Per actuator:

- **Pulse**: on for N seconds (1–3600), then off. A pulse is never saved as the state to restore, so a lock cannot reopen after a power loss. With the rule, the end of a pulse returns to automatic.
- **Safety off**: "turn off by itself after N minutes" (0 = never), for pumps, valves and heaters. With the rule, the actuator stays in manual afterwards, or the rule would turn it right back on.
- **After a power loss**: off, or the last manual command (`/iot.state`).

### Rule (sensor and actuator)

- **Digital sensor**: follow (on while active) or toggle (each activation flips the actuator).
- **Numeric sensor**: on above or below a limit, with a margin to turn off (hysteresis), so a relay does not chatter.
- **Proportional level**: a PWM actuator follows the reading linearly between the limit and a high limit.
- **Time window** (optional): the rule only turns the actuator on between a start and an end, in local time (22:00 to 06:00 wraps past midnight). Outside it, and while the clock is still in 1970, automatic mode keeps the actuator off. A manual command still holds until `auto`. The time zone is set on the **Settings** page, from a list of the official zones (UTC−12 to UTC+14, each with an example place; no automatic daylight saving); the local time shows under **Status**.

The form writes the rule back as a sentence ("Turns the fan on when the value goes above 28 °C and off when it drops below 26 °C") before you save.

### The "Now" card and the IoT card on Home

On the IoT page and on **Home** (its **IoT** card), each only while a sensor or actuator is set up and in use. Shows the reading and the actuator state, with the real control for its capability: a switch for on/off (relay, pump, valve, lock, buzzer), a 0–100 % slider for a level (LED, fan, motor), open, close and an angle for a servo, brightness and colour for a strip.

The sensor is drawn by what it measures: a gauge per value on the scale of its datasheet (DHT11 0–50 °C and 20–90 %, DHT22 −40–80 °C, DS18B20 −55–125 °C, SHT3x −40–125 °C, humidity 0–100 %, HC-SR04 0–400 cm), with the saved rule's limit marked on the value it watches; a lamp with the state's name for a digital sensor; the count for pulses. The drawing is made by the browser (`web/iot.js`), so it costs only the script in the flash.

Commands go out without reloading the page and the values refresh every 5 s (`/iot/now`), so nothing being edited in the form is lost. Values are announced to screen readers (`aria-live`); state is always written, never colour only.

### Reconfiguring

**Reconfigure**, at the top of the IoT page once a sensor or an actuator is set up, opens a confirmation page of its own before acting. Once confirmed it deletes the sensor, the actuator, the rule, the saved state (`/iot.state`) and the API token: the actuator turns off and the local API stops answering until a new setup, which creates a new token. The form is then empty, for a new setup.

---

## 🖧 Local API

The same reading and the same commands, for scripts and home automation on the local network, **without internet**. The routes exist only while a sensor or actuator is configured (404 otherwise).

```bash
curl -H 'Authorization: Bearer <token>' http://192.168.1.50/api/state
curl -H 'Authorization: Bearer <token>' -d on http://192.168.1.50/api/cmd
curl -H 'Authorization: Bearer <token>' -d 'cmd=pulse 5' http://192.168.1.50/api/cmd
```

```json
{"api":1,
 "sensor":{"type":"dht22","status":"ok","text":"24.5 C, 61.0%","temperature":24.5,"humidity":61.0},
 "actuator":{"type":"fan","capability":"level","on":true,"mode":"auto","text":"on 60%","level":60}}
```

- The token (32 hex characters from the hardware RNG) is separate from the panel password. It is created when IoT is first configured and shown on the IoT page with these examples; **Generate new token** invalidates the old one. It is compared in constant time.
- `sensor` or `actuator` is `null` when not in use. `status` is `ok`, `waiting` or `error`. Extra fields per type: `active`, `temperature`, `humidity`, `distance`, `pulses`, `level`, `angle`, `color`, `pulse` (seconds left).
- A command that does not parse or does not fit the actuator returns **400** with `{"error":"..."}`; nothing changes.
- In Home Assistant, prefer [MQTT](#-mqtt): the device shows up on its own. Without a broker, HA reads it with the `rest` sensor and sends commands with `rest_command`.

> The panel and the API are plain HTTP (the ESP-01 has no memory for a TLS server). Use them on a trusted network and do not forward port 80 to the device. From outside, the same API answers at the [remote access](docs/REMOTE-ACCESS.md) address over HTTPS, with the same token; with the device offline, `/api/state` answers there from the last state, with `"online": false`.

---

## 📣 ntfy

### Data topic (outgoing)

- `[ip] 1.2.3.4` at boot and whenever the public IP changes. It does not depend on Cloudflare.
- `[sensor] …` and `[actuator] …` when **Send readings and changes to ntfy** is on. Changes and command replies go out within seconds; periodic numeric readings at most once a minute (the ntfy.sh rate limit).
- When a command topic is set, each IoT notification carries up to **three action buttons** that publish a command back: On/Off/Pulse, Off/50 %/100 %, or Open/Close/Pulse, depending on the actuator. With the rule on, the third button is **Auto**. You operate the device from the phone notification without typing.

### Command topic (incoming)

Set a **command topic** on the ntfy page (the button generates a random name). The device subscribes to it with a long-lived connection (`GET /<topic>/json`) and every message becomes a command, delivered in about a second. Replies to invalid commands go to the data topic as `[error] …`.

- **One TLS session at a time.** The ESP-01 has memory for a single TLS session. Before publishing, or when Cloudflare or the OTA is about to use the network, the stream closes. On reopening it asks for `since=<last id>`, and ntfy (which keeps messages for 12 hours) delivers whatever arrived in between. Nothing is lost; at worst a command waits a few seconds.
- **With remote access on**, the relay holds that session, so its Worker checks the command topic for the device (every 2, 5, 10 or 30 s, chosen on the ntfy page; 5 s by default) and sends each command down the relay. The page shows `relay` as the command state. If the relay stays down for more than a minute, the device's own stream takes over again, from the last message.
- **The ntfy.sh certificate is validated** (ISRG Root X1, already embedded), on the command stream and when publishing: a command forged on the path would switch real equipment, and a publish carries the token and, in the action buttons, the command topic. Until the clock syncs nothing is published; the message waits for the next attempt.
- Commands older than 10 minutes are ignored. Failed connections retry from 5 s up to 5 min. A stream silent for 2 minutes is reopened (ntfy sends a keepalive every 45 s).
- ntfy.sh topics are public by name: whoever knows the command topic can send commands. Use a long random name (without a token, the panel refuses names shorter than 20 characters), different from the data topic, or an **ntfy access token** (used for both publishing and subscribing). The action buttons carry the command topic (and the token, if any), so keep the data topic private too.

---

## 🔋 Power and battery

With a battery and a solar panel (see [Battery and solar panel](docs/BATTERY-AND-SOLAR.md)), a **MAX17048** gauge (I2C, address 0x36) reports the charge. Pick its SDA pin on the **Settings** page; SCL goes on the pair (GPIO0 with GPIO2, RX with TX). The gauge shares the bus with the SHT3x when both sit on the same pair; with any other sensor or actuator the pins must differ, and the panel refuses a configuration that mixes them.

The top-right corner of the panel shows:

| Indicator | When |
| --- | --- |
| ⚡ Power | no gauge configured, or the battery full and idle (the supply holds it) |
| 🔋 Battery 76% | the charge is falling, or below 98% and not charging; turns yellow below 20% |
| ☀ Charging 76% | the charge is rising (solar panel or supply) |
| 🔋 Battery ? | the gauge did not answer |

The gauge is read every 10 s. The state, charge, voltage and rate (%/h) show under **Status**, and each change of state goes to the log.

## 📡 MQTT

For Home Assistant, Node-RED or scripts on the local network. Set it on the **MQTT** page: the broker (`host` or `host:port`, default 1883), user and password. Blank disables it.

| Topic (`<id>` = chip ID in hex) | Direction | Content |
| --- | --- | --- |
| `esp8266iot/<id>/state` | device → broker, retained | The same JSON as `/api/state`. Sent on connect, when it changes (at most once per second) and every 5 minutes even without a change, so a lost message does not leave Home Assistant without a state. |
| `esp8266iot/<id>/cmd` | broker → device | The same command text as the panel and the API (`on`, `pulse 5`, `50%`…). |
| `esp8266iot/<id>/avail` | device → broker, retained | `online`; `offline` comes from the last will when the connection drops. |

- **No TLS.** The device's only TLS session is already shared by DNS, ntfy and OTA. Use a broker on the local network, with a user and password: whoever can publish to the broker controls the actuator.
- Use the broker's IP: `.local` names (mDNS) do not resolve.
- A command that arrives retained (from before the connection) is ignored: it would repeat on every reconnect.
- Own minimal client (MQTT 3.1.1, QoS 0, no library). The connection only closes during the OTA download. Failures retry from 5 s up to 5 min.

### Home Assistant (discovery)

With the broker set, the device shows up in Home Assistant on its own (MQTT integration), as a device with the device name (**ESP8266 IoT** when unnamed) and the entities of the configured module:

| Module | Entity |
| --- | --- |
| DHT11, DHT22 | temperature and humidity `sensor` |
| DS18B20 | temperature `sensor` |
| Distance, pulses | `sensor` |
| PIR, contact, button, digital | `binary_sensor` (motion for PIR, door for contact) |
| Relay, pump, valve, lock, buzzer | `switch` |
| LED, fan, motor | `light` with brightness |
| LED strip | `light` with color and brightness |
| Servo | `number` (0 to 180°) |

- A single retained message per device, at `homeassistant/device/esp8266iot_<id>/config`. Changing the module on the panel republishes it and removes from HA the entities that no longer exist.
- Commands from HA become the same text as the panel (`on`, `level 50`, `color #ff0000`, `angle 120`). A sensor with no reading yet is *unknown*.
- To remove the device from HA, delete it in the MQTT integration and clear the broker on the MQTT page.

---

## 🔁 Compatibility and adding modules

Installed devices update themselves, so the firmware never breaks an existing configuration:

- **Stable identifiers.** The configuration stores a sensor or actuator by its identifier (`"actuator":"relay"`), never its position in a table. Identifiers are not renamed or reused; display names and order can change freely. The one exception was the move to English: a configuration saved with the earlier Portuguese identifiers (`rele`, `ventilador`, `contato`…) falls under *unknown type* below, and the module has to be chosen again on the IoT page.
- **Versioned configuration.** `/config.json` carries `"v"`. On load, an older version goes through the migrations in order. Version 1 (types stored by table position) becomes version 2 on the first boot, and the original is kept as `/config.v1.json`. A missing field gets its default; an unknown field is ignored. In v1 a module only ran with a sensor; after the migration a module without a sensor stays off too, so no pin starts switching after the update. The v1 "command" (forced on/off) becomes the saved manual state.
- **Unknown type, safe stop.** If an OTA rollback leaves an older firmware reading a newer configuration, an unknown type keeps that part off (actuator released), shows a warning on the IoT page, and **the file is not rewritten**. Updating again brings everything back.
- **API contract.** Fields and commands are only ever added; existing ones keep their name and meaning. `"api":1` marks the version.

A new module that uses an existing capability is **one line** in `IoTCatalog.h`; panel, commands, API, ntfy and the action buttons follow without other code:

```cpp
{"heater", _A_HEATER, C_ONOFF, 60},   // identifier, label, capability, suggested pulse (s)
```

Only a new capability (for example a stepper motor) needs code: an output in `IoTService` and a control in `WebService`. CI tests the migrations with real configuration files from each version.

---

## 🌐 Public IP and DNS updates

The device checks its public IP **every 5 minutes** (while Wi-Fi is up and the clock is synchronized), whether or not Cloudflare or ntfy are configured:

1. Fetch the public IP over HTTPS. The body must be a single IPv4 and the certificate must chain to the roots in `CloudflareCA.h`. The lookup tries `api4.ipify.org`, then `icanhazip.com`, then `ifconfig.co`; all three present the WE1 → GTS Root R4 chain, the same one `api.cloudflare.com` already validates on devices in the field.
2. The log only records changes: the first IP, a new IP (`Public IP changed: old -> new`), or the lookup starting to fail. A failed lookup retries every minute and keeps the last known IP.
3. When the IP changes, the device asks the network's DNS server for the reverse name (PTR) over UDP.

The **Status** page shows the public IP, its reverse name, when it last changed and when it was last checked; the home page shows it next to the local IP.

With the DDNS record picked on the Cloudflare page (its **DDNS** card), a public IP different from the **last synchronized IP** (kept in memory) sends a `PATCH` to that record; a failure retries every minute. The firmware does not rely on local DNS resolution: it is cached by TTL and returns Cloudflare IPs for *proxied* records. Each boot performs one initial `PATCH`, which is idempotent. The connection to `api.cloudflare.com` validates the certificate against the roots in `CloudflareCA.h` (GTS Root R1/R4, ISRG Root X1/X2). Older configurations with several record IDs keep the first one.

---

## 🔄 Signed OTA

```text
GitHub Releases ──(vN)──► ESP8266
  firmware.tagged.bin = gzip(firmware) + RSA-2048/SHA-256 signature
                        over SHA-256(SHA-256(gzip) || tag)
```

- Hourly (up to ~23 h of uptime) it queries `releases/latest`. It only installs `vN` tags **greater** than the current one, and the signature names the tag: the device checks it against the version it asked for, so an older release cannot be served under a newer tag (the GitHub connection itself is not validated).
- A build that embeds `public.key` makes the core `Updater` **verify the signature** in `Update.end()` and store only the gzip. Manual install downloads the same file as the hourly check: `firmware.tagged.bin` when the key is in the build, `firmware.bin` when it is not. Releases still carry `firmware.signed.bin` (signed over the image alone) for devices from before the tagged signature. The bootloader reads the uncompressed size from the last 4 bytes; on the signed file those bytes are the signature length, and storing that trailer corrupts the sketch.
- The gzip is padded (FNAME field) to a multiple of 4 bytes: the core `Updater` hashes in 4-byte reads and silently fails on a tail of another size.
- PlatformIO embeds `public.key` before compilation (`pre:tools/embed_pubkey.py`), the same way arduino-cli does in CI. The link fails if the ELF lacks the verifier and that key.
- The linker script `ld/eagle.flash.1m64.ota.ld` shortens IROM by **400 KB**. That hole sits against the filesystem and the sketch can never occupy it; a firmware that grows past the cap fails at link time. `package_ota.py` also rejects a gzip/signature larger than the reserve.
- The download is aborted if Wi-Fi drops, a flash write fails, or no data arrives for 60 s. GitHub TLS uses a 16 KB buffer. DNS, ntfy, the ntfy command stream, the remote access relay and MQTT pause while OTA checks or downloads. If the heap cannot fit the session, the device writes `/ota.pending` and reboots into recovery (IoT off) to finish OTA. A version rejected by `Update.end()` is remembered in `/ota.rejected` and only a manual install downloads it again.

### Signing key

- `public.key` is committed to the repository.
- The private key is **never** committed (it is in `.gitignore`). CI reads it from the **`OTA_SIGNING_KEY`** secret (PEM contents).
- **Back up the private key.** Without it, devices on the new firmware reject every OTA update and can only be reflashed over serial.

```bash
openssl genrsa -out private.key 2048
openssl rsa -in private.key -outform PEM -pubout -out public.key
gh secret set OTA_SIGNING_KEY < private.key
```

---

## 📏 Flash budget

The 1 MB flash holds the firmware, a **400 KB OTA reserve** (the next firmware, compressed) and a **64 KB LittleFS** for configuration. Every feature has to fit both the program area and, compressed, the OTA reserve. No feature or fix may take the firmware past **500,000 bytes**: `tools/firmware_limit.py` fails the PlatformIO build and CI above that.

| Item | Size |
| --- | --- |
| Firmware | 498,896 of 500,000 bytes (1,104 to spare) |
| Panel text, 4 languages | 337 texts (the pin drawing included), 45 KB as text, 16 KB in flash |
| Relay Worker | 7.8 KB of script, 4.2 KB in flash (byte-pair encoded) |
| Signed OTA package | ~354 KB of 400 KB (about 46 KB free) |
| Embedded images | 1.1 KB pinout (gzipped SVG), 3.2 KB ESP-01 photo (AVIF, white background) |
| Configuration filesystem | 64 KB, unchanged |

Kept small on purpose: page text in flash (`PSTR`) as 3-byte markers, with the text of the four languages in one gzipped table that the browser decodes (one column per language, rows ordered so similar texts sit together, an empty cell for text equal to English and one byte for text equal to the language before it, compressed with zopfli), no `scanf` (one `sscanf` pulled about 3 KB of the C library), TLS roots as trust anchors (name and key only, 1.6 KB, no X.509 decoder), an own JSON reader and writer (`Json.h`, about 6 KB less than ArduinoJson; config.json is a table of fields), one-shot HTTPS requests without `HTTPClient` (about 6 KB), no libgcc unwinder or stdio `FILE` layer (`TlsTrim.h`, about 10 KB; printf output goes through `vsnprintf`), no float `printf`, LittleFS without its debug asserts (`LFS_NO_ASSERT`, 3.4 KB; errors are still return codes), one TLS session at a time, TLS limited to ECDHE + AES-GCM (CBC, CCM, 3DES, ChaCha20, X25519, MD5, the TLS 1.0 PRF and client certificates are left out of the link by `TlsTrim.h`, about 29 KB; the fast P-256 stays, so the handshake is as fast as before; every server the device uses negotiates ECDHE + AES-128-GCM), lwIP without extras (`ip=lm2n`, about 10 KB: no NAT, IP fragmentation or AutoIP; SNTP keeps one server, so `Clock` alternates between `pool.ntp.org` and `time.google.com` until it syncs), an own HTTP server instead of `ESP8266WebServer` (about 12 KB: one client at a time, routes in a table in flash instead of `std::function`, no upload, Digest, keep-alive or chunked encoding; the page ends when the connection closes), a WS2812 driver without a library, the command word table in flash. **mDNS** was measured at about 21 KB and does not fit; use a DHCP reservation instead.

---

## 🛡️ Resilience

| Mechanism           | Behaviour                                                                  |
| ------------------- | -------------------------------------------------------------------------- |
| Wi-Fi lost          | Checked every 15 s and reconnected; 5 consecutive failures → reboot        |
| No internet         | Lookups fail, are logged and retry; panel, API, sensor, actuator and rule keep working. Reboot is reserved for lost Wi-Fi association and the daily restart. |
| Critical memory     | Heap < 15 KB or fragmentation > 50 % on 3 consecutive checks (10 s) → reboot |
| Daily reboot        | After 24 h of uptime (postponed during OTA)                                |
| Configuration mode  | No watchdog takes the AP down while it is active                           |

---

## 🧪 Tests

Native tests (Unity + PlatformIO) use Arduino core stubs in `test/stubs`. CI requires **100 % line, function and branch coverage** of the firmware headers, builds with no warnings (`-Wall -Wextra -Werror`) and runs everything again under **AddressSanitizer and UndefinedBehaviorSanitizer** (buffer overflow, use after free, leaks, undefined behavior). Whatever comes from outside the device (MQTT packets, commands, the Cloudflare answers, the ntfy stream, the relay's frames, `config.json`) is also **fuzzed** (libFuzzer, `test/fuzz/`). The relay Worker (`relay/worker.mjs`) has its own tests on Node, with the Workers runtime pieces imitated, also at 100 % coverage, and they run again on the minified script the firmware carries.

```bash
python -m venv venv && . venv/bin/activate
pip install -r requirements.txt

pio test -e native        # any host compiler
pio test -e sanitize      # ASan + UBSan (gcc or clang)
tools/static_check.sh     # cppcheck, as in CI
test/fuzz/run.sh 30       # fuzzing, 30 s per target (requires clang)
node --test relay/worker.test.mjs   # the relay Worker (Node 22)
./test/coverage.sh        # requires gcc (Linux) — same command as CI
```

Tests run on the computer, with the core simulated. Before calling a version stable, run the [on-device checklist](docs/HARDWARE-TEST.md) on an ESP-01.

---

## 🤖 CI/CD

| Job (`ci.yml`) | When | What it does |
| --- | --- | --- |
| `plan` | always, first | Decides: is there new firmware (since the last `vN` tag)? only test changes? which version? publish? |
| `check` | always | Repository rules: every source file carries the SPDX license header; static analysis (cppcheck) with no findings; the relay Worker's tests at 100 % coverage, on the source and on the embedded script |
| `build` | new firmware | Embedded CSS/JS, TLS roots and relay Worker up to date, compiles with arduino-cli, firmware within **500,000 bytes**, signature verifier, OTA package size |
| `test` | after `build`, or alone when only tests changed | Tests + 100 % coverage, no warnings; again under ASan and UBSan |
| `release` | push to `main` with new firmware | Compresses, signs, **verifies the signature** and publishes the release |
| `pages` | after `release`, when `install/` changes on `main`, or on *Run workflow* | Publishes the [web installer](https://allanbarcelos.github.io/esp8266-dns/) to GitHub Pages with the latest release's firmware |

The pipeline runs `plan → check → build → test → release → pages`; if one job fails, the next ones do not run. Without new firmware (docs, workflow, tools), `build`, `test` and `release` are skipped. To force a release, use *Run workflow* with `release: true`.

---

## 📋 Remote log (future work)

Not in the firmware yet. The local ring (`Logger.h`, 10 lines of 128 characters, Serial and the panel) stays the RAM limit. The copy would go through ntfy on the data topic, prefixed `[log]`, several lines per POST every 60 s.

---

## 🔧 Local build

The ESP-01 has no auto-reset: to flash, tie **GPIO0 to GND** and power-cycle the adapter. After flashing, release GPIO0 and power-cycle again to run the firmware.

**PlatformIO** (`platformio.ini` already embeds `public.key`, the OTA linker script and drops float `printf`):

```bash
pio run -e esp8266                                   # build
pio run -e esp8266 -t erase  --upload-port COM4      # optional: wipe flash (config included)
pio run -e esp8266 -t upload --upload-port COM4      # re-enter flash mode after an erase
pio device monitor -b 115200 -p COM4
```

**arduino-cli** (same flags as CI):

```bash
arduino-cli config init   # skip if you already have an arduino-cli config
arduino-cli config add board_manager.additional_urls https://arduino.esp8266.com/stable/package_esp8266com_index.json
arduino-cli core update-index
arduino-cli core install esp8266:esp8266@3.1.2
python3 tools/ota_flash_ld.py
arduino-cli compile --fqbn esp8266:esp8266:generic:eesz=1M64 \
  --build-property build.flash_ld=eagle.flash.1m64.ota.ld \
  --build-property "build.float=" \
  --build-property "build.extra_flags=-DARDUINO_SIGNING=1" \
  --output-dir build
arduino-cli upload  --fqbn esp8266:esp8266:generic:eesz=1M64 -p /dev/ttyUSB0 --input-dir build
arduino-cli monitor -p /dev/ttyUSB0 --config 115200
```

Local builds report version `dev` and update to the latest release on the first OTA check (unless automatic update is off). After changing `docs/esp01-pinout.svg`, run `python3 tools/embed_assets.py` and commit `Assets.h`. The same tool turns `docs/esp-01.jpg` into the panel photo: 220 px wide (the size it is shown at), the white background made transparent, AVIF at the best quality that fits in `PHOTO_BYTES` (the browser decodes AVIF itself, so the firmware carries no decoder). It needs Pillow 11.3 or newer.

The panel style and the IoT page script live in `web/panel.css` and `web/iot.js`. The device serves them gzipped (`/s.css`, `/iot.js`): the browser decompresses them and keeps them in its cache, and the URL carries a hash of the content, so a new version never uses a stale copy. After editing, run `python3 tools/embed_web.py` and commit `WebAssets.h`; CI runs `python3 tools/embed_web.py --check`.

Panel text lives in `web/i18n.tsv`: one row per text, with a key and the English, Portuguese, Spanish and French columns (empty = same as English). The C++ uses the key (`L_SAVE` in `I18n.h`) and `web/iot.js` the English text (`L('Save')`), which the build swaps for the row number. After editing the table, run `python3 tools/embed_web.py` and commit `WebAssets.h`, `I18n.h` and `test/i18n_en.h`. With `pip install zopfli` the gzip comes out about 3 % smaller; without it the tool uses zlib, and the check compares the decompressed content either way.

The relay Worker lives in `relay/worker.mjs`; the device uploads it to the user's account itself. After editing it, run `python3 tools/embed_relay.py` and commit `RelayScript.h`; CI runs `python3 tools/embed_relay.py --check`. Its `VERSION` must match `RelayService::PROTOCOL`; raising both makes devices upload the new Worker after the update.

The TLS roots are kept readable in `ca/roots.pem` (name, expiry and SHA-256 of each). The firmware carries them in DER, which takes less flash and needs no decoding on each connection. After changing a root, run `python3 tools/embed_ca.py` and commit `CloudflareCA.h`; CI runs `python3 tools/embed_ca.py --check`.

---

## 🗺️ Status and next steps

What changed in the last round, how it was verified and what comes next: [docs/STATUS-AND-NEXT-STEPS.md](docs/STATUS-AND-NEXT-STEPS.md). What is worth porting from Tasmota: [docs/PORTING-FROM-TASMOTA.md](docs/PORTING-FROM-TASMOTA.md). Battery and solar panel: [docs/BATTERY-AND-SOLAR.md](docs/BATTERY-AND-SOLAR.md). Which of those items are implemented, and in which commit: [docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md).

---

## 📄 License

[MIT](LICENSE) © 2026 Allan Barcelos. Every source file carries an SPDX header (`SPDX-License-Identifier: MIT` and the copyright line); `python3 tools/license_headers.py` adds it to new files and the CI `check` job refuses a change without it. Third-party components and the courtesy requests are listed in [NOTICE](NOTICE). To report a vulnerability, see [SECURITY.md](SECURITY.md).
