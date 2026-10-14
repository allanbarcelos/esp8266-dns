# Security policy

This firmware runs on a small device that sits inside a home or office network, holds Wi-Fi and
Cloudflare credentials, updates itself from the internet, and can switch real equipment (relays,
pumps, locks). A flaw that lets someone take over the device, run a firmware they built, read its
credentials, or operate what it controls without permission is treated as the most serious kind of
bug this project can have.

## Supported versions

Only the latest published release (`vN` tag) receives security fixes. There are no backports.
Devices with automatic updates enabled install a new release within about an hour.

| Version | Supported |
| --- | --- |
| Latest `vN` release (GitHub Releases, installed by OTA) | ✅ |
| Older releases | ❌ — update from the panel (**Update → Check now**) |
| Local builds (`dev`) | ❌ — rebuild from `main` |

## Reporting a vulnerability

**Please do not open a public issue, pull request, or discussion for a security problem.**

Report it privately through GitHub: **Security → Report a vulnerability** on this repository
(private vulnerability reporting). Reports in English or Portuguese are welcome.

Helpful to include:

- what an attacker can do, and with which starting access (same Wi-Fi network, the setup access
  point, the internet, knowledge of a ntfy topic, physical access);
- steps to reproduce, or a proof of concept against your own device;
- the firmware version (panel footer) or commit you tested;
- any suggested fix.

What to expect:

- an acknowledgement, and a first assessment of severity and scope;
- updates while a fix is prepared, and a request for your review of it when that helps;
- a published release and a GitHub Security Advisory, crediting you unless you prefer otherwise.

Please give a reasonable time for a fix before disclosing publicly, and test only against a device
you own.

## Scope

**In scope** — anything that breaks the promises the firmware makes:

- **Firmware updates**: getting a device to install an image that was not signed with the
  project's private key, downgrading it, or bricking it through the OTA path.
- **Panel authentication**: reaching any page or action without the panel session (user, password
  and authenticator),
  cross-site request forgery, stored or reflected XSS (SSID, log lines, sensor names, any value
  shown back), or reading a stored secret (Wi-Fi password, Cloudflare token, API token) from a page.
- **Device control**: operating a sensor or actuator, or reading its state, without the panel
  login or the API token, or through a command that the configured module type should refuse.
- **Setup mode**: keeping or forcing a configured device in the setup access point.
- **Memory safety**: crashes or code execution from a crafted HTTP request, ntfy message, or
  GitHub/Cloudflare response.

**Out of scope** — known and documented limits:

- **Plain HTTP on the local network.** The panel and the local API do not use TLS (the ESP-01 has
  no memory for a TLS server). Anyone who can capture traffic on the same network can reuse the
  panel session cookie or read the API token. The cookie is HttpOnly and SameSite=Strict. It is
  not marked Secure, because the device speaks plain HTTP. The authenticator still stops a stolen
  password from being enough on its own, and a stolen cookie cannot change the password or the
  authenticator. The authenticator key itself crosses the network once, when it is enrolled:
  enroll it on a network you trust. Use the device on a trusted network.
- **The first authenticator is set by whoever has the password.** A device with no authenticator
  yet (a new one, or one updated from a firmware without it) asks only for the password and then
  shows a new key. Set it up right after setting the password or updating.
- **The setup access point** is protected by a password drawn per device on first boot (60 bits
  from the hardware RNG, kept in `/ap.key`). It is printed on the serial port and shown on the
  panel, never sent over the network. While no panel password is set, whoever joins the access
  point can configure the device, so knowing the AP password means physical access to the serial
  port or to the panel. A password derived from the chip ID would not do: the chip ID is part of
  the MAC address the access point broadcasts.
- **ntfy.sh topics are public by name.** Anyone who knows a topic can read it, and anyone who knows
  the command topic can send commands, unless an ntfy access token is configured. Keep the topic
  names secret and long.
- **Physical access.** Whoever can reach the serial pins can reflash the device and read its flash,
  including the stored credentials.
- Volumetric denial of service against the device or the local network.
- Vulnerabilities in third-party components (ESP8266 Arduino core, BearSSL, ArduinoJson) that are
  not caused by how this firmware uses them — please report those upstream.
- Reports from automated scanners without a demonstrated impact.

## Security design, in short

- **Signed updates.** Every release is signed with RSA-2048/SHA-256 in CI. The device checks the
  signature before accepting the image; the public key is built into the firmware and the private
  key exists only as a GitHub Actions secret.
- **Panel**: a sign-in form with a user and a password of at least 8 characters, set on first use,
  plus a mandatory authenticator (RFC 6238, HMAC-SHA1, 30 s, 6 digits, one step either side). The
  secret is shown once, when it is enrolled, and stored in `/config.json`. Until a code is
  confirmed, only the sign-in and the authenticator page are open. There is no HTTP Basic
  authentication.
- **Each code works once.** The last step accepted is kept in `/config.json`; a code from that
  step or an earlier one is refused, also after a reboot and also when the clock is pushed back
  (SNTP has no authentication). A code captured on the network cannot be replayed.
- **Clock.** Codes are checked against the device clock once SNTP has set it. Until then (setup
  access point, no internet) the browser sends its own time with the form; it can only open a
  step after the last one accepted, so it does not bring old codes back. Without either time the
  attempt is refused and does not count as a mistake.
- **Sensitive changes** ask again for the current password and a current code: a new panel password
  and a new authenticator. Replacing the authenticator keeps the current key until a code from the
  new one is confirmed; giving up halfway (signing out, the session ending, a reboot) changes
  nothing.
- **Session**: one cookie (12 h, RAM only, cleared on reboot and when the password changes); a new
  sign-in replaces it, and each change of level (setup → signed in) draws a new one.
- **Forms**: the signed-in forms carry a CSRF token drawn by the hardware RNG at each sign-in, so it
  is never the one the public sign-in page shows (that one guards only the sign-in and the first
  setup, is drawn at each boot and rotated when the password changes). Signing out needs the
  session, so another device on the network cannot sign the owner out. Every value shown back is
  HTML-escaped, and stored secrets are never sent back to the browser (tokens are masked; the
  authenticator secret is not shown again). Pages and API answers are never cached and cannot be
  framed by another site.
- **Wrong passwords**: after 5 wrong panel passwords, authenticator codes or API tokens in a row,
  each new mistake doubles a wait (2 s up to 5 min) during which nothing is checked and the device
  answers 429. A correct sign-in clears the count. The count is per device, not per address (there
  is no memory for a table), so someone on the network can delay the owner's login by a few minutes,
  but not guess the password.
- **Local API**: `/api/state` and `/api/cmd` exist only while a sensor or actuator is configured,
  and require a Bearer token separate from the panel password (32 hex characters from the hardware
  RNG, compared in constant time, regenerated from the IoT page).
- **MQTT** (optional) runs without TLS on the local network, with the broker's user and
  password. Whoever can publish to the device's command topic on the broker controls the
  actuator. Retained commands are ignored, so a stale message never repeats on reconnect.
- **Commands**: the panel, the local API, ntfy and MQTT share one parser that accepts only the commands
  the configured actuator supports. A pulse is never stored as the state to restore, and an
  optional safety timer turns an actuator off after a set time.
- **TLS to the internet**: Cloudflare, the public-IP services and ntfy (command stream and
  publishing) are validated against pinned root certificates (`CloudflareCA.h`) with a clock
  synchronized by SNTP. The OTA download from GitHub does not validate the certificate: the
  image is installed only if its RSA signature checks out, so a forged download is rejected.
  Every connection offers only TLS 1.2 ECDHE with AES-GCM (forward secrecy, no CBC or 3DES); the
  other ciphers are not even in the firmware (`TlsTrim.h`).
  Commands older than 10 minutes are ignored.
- **Credentials** live only in `/config.json` on the device's LittleFS, written atomically.
- **No restart loops**: the device does not reboot because the internet is down; a rejected update
  is remembered and not downloaded again.

## Keeping a device safe

- Keep automatic updates on (**Update → Automatic update**).
- Use a strong panel password, different from the Wi-Fi password, and keep the authenticator app.
  If the authenticator is lost, the device is recovered from the serial port: physical access can
  read the flash. **Password → Replace authenticator** issues a new key after the current password
  and the current code.
- Keep the device on a trusted network, and reserve its IP on the router instead of exposing the
  panel to the internet. Do not forward port 80 to it.
- Use long, random ntfy topic names, or an ntfy access token. Without a token, the panel refuses
  a command topic shorter than 20 characters.
- To reach the device from outside, use a tunnel with its own login (see
  [docs/REMOTE-ACCESS.md](docs/REMOTE-ACCESS.md)); never forward a router port to it.
- Erasing the flash draws a new setup-network password; a build with `-DAP_PASSWORD=\"...\"`
  uses a fixed one instead (keep it long and private).
