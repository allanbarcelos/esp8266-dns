# 📘 ESP8266 Firmware — Cloudflare DDNS + OTA

English | [Português](README.pt.md)

Firmware for the **ESP-01 (ESP8266, 1 MB)** that acts as an autonomous dynamic-DNS agent:

1. **Keeps Cloudflare A records** pointing at the residential public IP.
2. **Sends the public IP via [ntfy.sh](https://ntfy.sh)** at boot and whenever it changes, whether or not Cloudflare is configured.
3. **Updates its own firmware (OTA)** from GitHub Releases, with **on-device RSA signature verification**.
4. Provides an **authenticated local web panel** for configuration and diagnostics.
5. Has **watchdogs** for Wi-Fi, memory and a daily reboot.

---

## 🧩 Architecture

Each service is a header with dependencies injected by reference, composed in layers by `App.h`. The `.ino` only calls `app.begin()` and `app.tick()`. Every `tick(now)` is non-blocking.

| File                 | Responsibility                                                              |
| -------------------- | --------------------------------------------------------------------------- |
| `App.h`              | Service composition, `setup()`/`loop()`                                     |
| `Config.h`           | Configuration and runtime state; AP credentials                             |
| `ConfigStore.h`      | Persistence in `/config.json` (LittleFS) with **atomic writes**             |
| `Logger.h`           | Ring buffer of log lines (Serial + panel)                                   |
| `Clock.h`            | SNTP wall clock (required to validate TLS certificates)                     |
| `WiFiService.h`      | Connection, configuration mode (AP), reconnection and watchdogs             |
| `PublicIPResolver.h` | Public IPv4 over HTTPS (CA bundle in `CloudflareCA.h`), cached for 60 s    |
| `CloudflareDNS.h`    | PATCHes records through the Cloudflare API (TLS validated by `CloudflareCA.h`) |
| `NtfyNotifier.h`     | Public IP notification; resolves the IP on its own                          |
| `OTAService.h`       | Version check and signed firmware download                                  |
| `WebService.h`       | HTTP panel (Basic Auth + CSRF + HTML escaping)                              |
| `Version.h`          | `firmware_version` (injected by CI; `"dev"` for local builds)               |

---

## 🚀 First use

1. Flash over serial (see [Local build](#-local-build)).
2. With no Wi-Fi configured, the ESP creates the **`ESP_Config`** network (default password **`esp8266dns`**, override at build time with `-DAP_PASSWORD=\"...\"`).
3. Open `http://192.168.4.1`, enter the Wi-Fi credentials and **set the panel user and password** (at least 8 characters).
4. After the reboot, open the ESP's local IP and configure **Cloudflare** (token, Zone ID, Record IDs) and, optionally, the **ntfy** topic.

> The panel requires a password. Until one is set, every page redirects to `/password`.
> If the configured Wi-Fi fails at boot, the ESP brings up the AP **and** keeps trying the network in the background. Once it connects, the AP is turned off.

### Cloudflare token

Create a least-privilege token: **Zone → DNS → Edit**, restricted to the zone in use.

---

## 🌐 DNS updates

Every 60 s (while Wi-Fi is up and the clock is synchronized):

1. Fetch the public IP over HTTPS. The body must be a single IPv4 and the certificate must chain to the roots in `CloudflareCA.h`.
2. If it differs from the **last synchronized IP** (kept in memory), send a `PATCH` for each record.
3. Mark it synchronized only when **all** records succeed. On any failure, retry on the next cycle.

The lookup tries `api4.ipify.org`, then `icanhazip.com`, then `ifconfig.co`. All three answer with a bare IPv4 and present the WE1 → GTS Root R4 chain, the same chain `api.cloudflare.com` already validates on devices in the field. No new root is added. The first valid answer is kept for 60 s and shared, so DNS and ntfy open one TLS session per cycle.

ntfy reads that result on its own. A topic with no Cloudflare token still resolves and notifies.

The firmware does not rely on local DNS resolution: it is cached by TTL and returns Cloudflare IPs for *proxied* records. Each boot performs one initial `PATCH`, which is idempotent.

The connection to `api.cloudflare.com` validates the certificate against the roots in `CloudflareCA.h` (GTS Root R1/R4, ISRG Root X1/X2).

---

## 🔄 Signed OTA

```text
GitHub Releases ──(vN)──► ESP8266
  firmware.signed.bin = gzip(firmware) + RSA-2048/SHA-256 signature
```

- Hourly (up to ~23 h of uptime) it queries `releases/latest`. It only installs `vN` tags **greater** than the current one, which prevents downgrades.
- A build that embeds `public.key` makes the core `Updater` **verify the signature** in `Update.end()` and store only the gzip. Manual install downloads the same file as the hourly check: `firmware.signed.bin` when the key is in the build, `firmware.bin` when it is not. The bootloader reads the uncompressed size from the last 4 bytes; on the signed file those bytes are the signature length, and storing that trailer corrupts the sketch.
- PlatformIO embeds `public.key` before compilation (`pre:tools/embed_pubkey.py`), the same way arduino-cli does in CI. The link fails if the ELF lacks the verifier and that key. The native test leaves `ARDUINO_SIGNING` unset and therefore requires `firmware.bin`.
- The image is **gzip-compressed** (the bootloader decompresses it). Without compression, the current and new firmware would not fit together in the 1 MB sketch area.
- The download is aborted if Wi-Fi drops, a flash write fails, or no data arrives for 30 s.

### Release assets

| File                  | Purpose                                                                        |
| --------------------- | ------------------------------------------------------------------------------ |
| `firmware.signed.bin` | gzip + signature: builds that embed `public.key`                               |
| `firmware.bin`        | plain gzip: builds without the key, so the bootloader reads the real size     |

### Signing key

- `public.key` is committed to the repository.
- The private key is **never** committed (it is in `.gitignore`). CI reads it from the **`OTA_SIGNING_KEY`** secret (PEM contents).
- **Back up the private key.** Without it, devices on the new firmware reject every OTA update and can only be reflashed over serial.

To generate a new pair:

```bash
openssl genrsa -out private.key 2048
openssl rsa -in private.key -outform PEM -pubout -out public.key
gh secret set OTA_SIGNING_KEY < private.key
```

---

## 🛡️ Resilience

| Mechanism           | Behaviour                                                                  |
| ------------------- | -------------------------------------------------------------------------- |
| Wi-Fi lost          | Checked every 15 s and reconnected; 5 consecutive failures → reboot        |
| Upstream unreachable | The public-IP lookup fails, is logged, and retries in 60 s. Reboot is reserved for lost Wi-Fi association (5 failures) and the daily restart. |
| Critical memory     | Heap < 15 KB or fragmentation > 50 % on 3 consecutive checks (10 s) → reboot |
| Daily reboot        | After 24 h of uptime (postponed during OTA)                                |
| Configuration mode  | No watchdog takes the AP down while it is active                           |

---

## 🧪 Tests

Native tests (Unity + PlatformIO) use Arduino core stubs in `test/stubs`. CI requires **100 % line, function and branch coverage** of the firmware headers.

```bash
python -m venv venv && . venv/bin/activate
pip install -r requirements.txt

pio test -e native        # any host compiler
./test/coverage.sh        # requires gcc (Linux) — same command as CI
```

---

## 🤖 CI/CD (`.github/workflows/ci.yml`)

| Job       | When                               | What it does                                                            |
| --------- | ---------------------------------- | ----------------------------------------------------------------------- |
| `plan`    | always                             | Decides whether to release and computes the next `vN` tag (numeric sort) |
| `test`    | always                             | Tests + 100 % coverage                                                  |
| `build`   | always                             | Compiles with arduino-cli, checks the signature verifier and flash limits |
| `release` | push to `main` touching firmware   | Compresses, signs, **verifies the signature** and publishes the release |

Changes limited to tests, docs or the workflow do not create a release. To force one, use *Run workflow* with `release: true`.

---

## 🔌 IoT

The **IoT** page configures one sensor and one module on the ESP-01 without editing the firmware. The available pins are GPIO0, GPIO2, TX and RX. The drawing is `docs/esp01-pinout.png`, and the panel loads that image from the repository. The home page shows the module photo in `docs/esp-01.jpg` (SparkFun Electronics, CC BY 2.0).

The sensor can be digital, PIR, door, rain, flame, gas, soil, light, sound, float or button (all by level), DHT11, DHT22, DS18B20, pulses counted every 10 s, or an HC-SR04 ultrasonic sensor on one pin (trig and echo tied together, in cm). A number drives the module above or below a limit, with a margin so it does not chatter. The module can be a relay, LED, buzzer, pump or valve (fixed level), a servo (one angle for on and another for off), or a fan and a motor using PWM from 0 to 100. DHT and DS18B20 need a pull-up resistor. The servo uses a separate 5 V supply and a common ground.

With the new keys missing from `/config.json`, the sensor and the module stay digital and the feature stays off until the user picks the pins. A reading is posted to the ntfy topic already configured, prefixed with `[sensor]`. The IP notice on that same topic uses `[ip]`.

---

## 📋 Remote log (future work)

Not in the firmware yet. The local ring (`Logger.h`, 10 lines of 128 characters, Serial and the panel) stays the RAM limit. The copy goes through [ntfy.sh](https://ntfy.sh) on the topic already configured, with the prefix `[log]`. The IP already goes out as `[ip]` and the sensor as `[sensor]`.

- One POST carries several lines, on the same 60 s interval as the IP notice, and only when heap and a contiguous block are free for TLS. The ESP keeps talking to that single host.
- With the new `/config.json` key absent, sending stays off. Devices already in the field keep the local log only.
- The public ntfy.sh server retains each message for 12 hours. For a permanent archive, `ntfy subscribe` on that topic appends the output to a file.
- The `Reset:` line right after boot is part of that send and records why the previous cycle died. A crash in the middle of TLS is not published.

---

## 🔧 Local build

```bash
arduino-cli core install esp8266:esp8266@3.1.2
arduino-cli lib install "ArduinoJson@6.21.4"
arduino-cli compile --fqbn esp8266:esp8266:generic:eesz=1M64 --output-dir build
arduino-cli upload  --fqbn esp8266:esp8266:generic:eesz=1M64 -p /dev/ttyUSB0 --input-dir build
arduino-cli monitor -p /dev/ttyUSB0 --config 115200
```

Local builds report version `dev` and update to the latest release on the first OTA check.
