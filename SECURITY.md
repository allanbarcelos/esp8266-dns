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
- **Remote access relay**: reaching the device through its relay Worker without the panel login
  or the API token, connecting to someone's Worker as their device, reading the state the Worker
  keeps without the token, or getting a device to deploy a Worker over someone else's.
- **Setup mode**: keeping or forcing a configured device in the setup access point.
- **Memory safety**: crashes or code execution from a crafted HTTP request, ntfy message, relay
  frame, or GitHub/Cloudflare response.

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
- **The relay Worker runs in the user's Cloudflare account.** Whoever controls that account (or
  the API token stored on the device) can replace the Worker, read the relay secret and the ntfy
  token the device sends it, and see the traffic between browsers and the device as it passes
  through Cloudflare. Use a token limited to the permissions listed in
  [docs/REMOTE-ACCESS.md](docs/REMOTE-ACCESS.md) and keep the account protected.
- **ntfy.sh topics are public by name.** Anyone who knows a topic can read it, and anyone who knows
  the command topic can send commands, unless an ntfy access token is configured. Keep the topic
  names secret and long.
- **Physical access.** Whoever can reach the serial pins can reflash the device and read its flash,
  including the stored credentials.
- Volumetric denial of service against the device or the local network.
- Vulnerabilities in third-party components (ESP8266 Arduino core, BearSSL) that are
  not caused by how this firmware uses them — please report those upstream.
- Reports from automated scanners without a demonstrated impact.

## Security design, in short

- **Signed updates.** Every release is signed with RSA-2048/SHA-256 in CI. The device checks the
  signature before accepting the image; the public key is built into the firmware and the private
  key exists only as a GitHub Actions secret. The signature covers the image together with its
  version tag (`firmware.tagged.bin`: SHA-256 of the image's SHA-256 and the tag), and the device
  checks it against the tag it asked for, so an older release, genuinely signed, cannot be passed
  off as a newer one. Each release also carries `firmware.signed.bin`, signed over the image
  alone, for devices from before the tag.
- **Panel**: a sign-in form with a user and a password of at least 8 characters, set on first use
  and stored only as PBKDF2-HMAC-SHA1 with a random salt (4096 rounds; a password saved by an
  older firmware is replaced by the hash on the first boot), plus a mandatory authenticator (RFC 6238, HMAC-SHA1, 30 s, 6 digits, one step either side). The
  secret is shown once, when it is enrolled, and stored in `/config.json`. Until a code is
  confirmed, only the sign-in and the authenticator page are open. There is no HTTP Basic
  authentication.
- **Each code works once.** The last step accepted is kept in `/config.json`; a code from that
  step or an earlier one is refused, also after a reboot and also when the clock is pushed back
  (SNTP has no authentication). A code captured on the network cannot be replayed.
- **Clock.** Codes are checked against the device clock once SNTP has set it. Until then (setup
  access point, no internet) the browser sends its own time with the form; it can only open a
  step after the last one accepted, so it does not bring old codes back. Steps accepted against the
  browser's clock are kept apart: once the device has its own time, one from a browser clock that
  ran ahead is ignored instead of locking the authenticator until that date. Without either time
  the attempt is refused and does not count as a mistake.
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
  answers 429. A correct sign-in clears the count. There are three counts: the API token's, the
  one for the address of the last sign-in, and the one for every other address. Someone guessing
  elsewhere on the network waits alone; the owner and scripts using the API carry on.
- **Requests**: the panel answers only to its own names (an IP address, a name without a dot, its
  hostname with any domain, the DDNS record, the relay's address and the names listed under
  Settings, Other addresses), so a page on another site cannot reach it through DNS rebinding. A request must
  arrive whole within 3 s and with at most 32 header lines, so a slow client cannot hold the loop
  where the actuator's safety timer runs. The Cloudflare token, record names and relay names
  accept only the characters they can have, so nothing typed changes an API path or header.
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
  publishing) are validated against pinned roots (`CloudflareCA.h`) with a clock synchronized by
  SNTP. The OTA download from GitHub does not validate the certificate: the image is installed only
  if its signature, bound to the version asked for, checks out, so a forged or older download is
  rejected. Someone in the middle can still delay updates.
  Every connection offers only TLS 1.2 ECDHE with AES-GCM (forward secrecy, no CBC or 3DES); the
  other ciphers are not even in the firmware (`TlsTrim.h`).
  Commands older than 10 minutes are ignored.
- **Remote access** (optional): the device opens a WebSocket over TLS to its own Worker, with the
  certificate checked against the pinned roots, and authenticates with a 256-bit secret drawn by
  the hardware RNG at every deploy and compared by the Worker in constant time; another protocol
  version is refused (426). Requests that come down the relay go through the same server, routes,
  sign-in, CSRF and attempt counts as on the local network (the Worker passes the client's
  address; an IPv6 one is hashed into four bytes). Between the browser and Cloudflare it is HTTPS,
  so the session cookie and the API token never travel in clear. The Worker limits sign-in and API
  paths to 60 requests per minute per address, refuses bodies over 2 KB and queues at most 16
  requests. For `/api/state` with the device offline it keeps the last state and an HMAC-SHA1 of
  the API token keyed with the relay secret, never the token itself. The device only deploys over
  a Worker of the same name that already binds the relay's Durable Object; any other is left
  alone.
- **Credentials** live only in `/config.json` on the device's LittleFS, written atomically. The
  panel password is a salted hash; the Wi-Fi password, tokens and the authenticator secret must be
  usable by the device and are stored as they are (physical access reads them).
- **Build**: the CI actions are pinned by commit, and Dependabot proposes updates; the job that
  builds the firmware is the one whose output the release job signs.
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
- To reach the device from outside, turn on **Remote access**, optionally with Cloudflare Access
  in front (see [docs/REMOTE-ACCESS.md](docs/REMOTE-ACCESS.md)); never forward a router port to it.
- Erasing the flash draws a new setup-network password; a build with `-DAP_PASSWORD=\"...\"`
  uses a fixed one instead (keep it long and private).
