# On-device test

The automated tests run on the computer, with the Arduino core simulated. They catch almost everything, but not what only happens on the chip: TLS handshake timing, real memory, Wi-Fi dropping, a relay clicking. This checklist is for an ESP-01 before marking a version as stable, or after any large change.

It takes about two hours the first time. After that, you can skip what did not change.

## Material

- A 1 MB ESP-01 or ESP-01S and a 3.3 V USB-serial adapter.
- A jumper for GPIO0.
- An LED with a resistor (or a relay module) on GPIO0, and a button or a wire between GPIO2 and GND to act as a digital sensor.
- Chrome or Edge on the computer; a serial monitor (PlatformIO's, Arduino's or `screen /dev/ttyUSB0 115200`).
- For the optional parts: a domain on Cloudflare, the ntfy app on the phone, an MQTT broker on the network (Mosquitto) and a Home Assistant.

Write down the version under test and the result of each item in the table at the end.

## 1. Flashing with the web installer

1. Open the installer (`https://allanbarcelos.github.io/esp8266-dns/`), tie GPIO0 to GND and plug in the adapter.
2. Click **Install on the ESP-01**, pick the port and accept erasing the device.
3. When it finishes, unplug the adapter, release GPIO0 and plug it in again.

**Expected:** flashing reaches 100 % without errors. If the installer does not find the chip, it is almost always GPIO0 not being on GND at power-up.

## 2. AP password and initial setup

1. In the installer, click **Read the device password** and pick the port.
2. Within 30 s a 12-character password shows up (lowercase letters and digits 2 to 9).
3. Check the line `AP ESP_Config, password: ...` on the serial monitor, repeated every 30 s.
4. On the phone or computer, join the `ESP_Config` network with that password. Try `esp8266dns` first: it must be refused.
5. Open `http://192.168.4.1`, enter the home Wi-Fi and set the panel user and password.

**Expected:** the device reboots, joins the home network and the AP goes away. The password does not show in the panel log.

Flash again **without** erasing: the AP password must stay the same. Then flash **erasing**: a new password must come up.

## 3. Panel and login

1. Find the device's IP on the router and reserve it (DHCP reservation).
2. Open the panel. The first time, sign in with the user and password only.
3. The panel shows an authenticator key, as text and as a QR code. Scan the QR code with an authenticator app (it shows the device name), or type the key, and enter the 6-digit code.
4. Under **Status**, check that the time is shown and the **AP password ESP_Config** row is there. Sign out, and sign in again with the user, the password and a current code.
5. Open a private window and get the password or the code wrong 6 times in a row.

**Expected:** from the sixth mistake on, "Too many attempts; please wait" shows for a few seconds, even with the right password and code. The log shows `Login: repeated failures, growing wait`. After the wait, the right password and code get in and the count resets. Signing in again with a code that was already used is refused. With the device on the setup access point (no internet, clock not synchronized), the sign-in still works with a current code: the browser sends its time.

## 4. Wi-Fi and resilience

1. Turn the router off for 2 minutes and on again.
2. Unplug the device and plug it in again.
3. Change the Wi-Fi password on the panel to a wrong one and reboot.

**Expected:** in the first case the device comes back on its own, without rebooting for lack of internet. In the second, it comes back with the same configuration and the actuator in the state set in "On power-up". In the third, the `ESP_Config` AP comes back with the **same** password as in step 2, and the device keeps trying the network in the background.

## 5. Public IP and Cloudflare DNS

1. With neither Cloudflare nor ntfy configured, wait for the clock to sync and open **Status**.
2. Leave the device on for 15 minutes and look at the log.
3. If you use Cloudflare: set the token, zone and record ID on the **Cloudflare** page.
4. Change that record's IP to `1.1.1.1` in the Cloudflare dashboard and reboot the device. It compares the public IP with the last one it sent, not with the record, but every boot sends once.

**Expected:** the **Public IP** card under Status shows the home IP, the provider's reverse name (or `-` if the provider has none) and the last check, which moves every 5 minutes. The log has one `Public IP: ...` line at boot and nothing more while the IP does not change. With Cloudflare, the log shows `Updating DNS to ...` and then `DNS updated!`, and the record goes back to the home IP. With the clock still in 1970 (right after power-up), the device waits instead of failing.

## 6. ntfy (if used)

1. On the **Ntfy** page, enter a data topic and generate a command topic with the button.
2. Try to save a short command topic (`test`) without a token: it must be refused.
3. Subscribe the phone to the data topic.
4. Send `on` to the command topic from the app.

**Expected:** the public IP arrives at boot. The command turns the LED on in about 1 s, and the notification with the action buttons arrives. The buttons work too.

## 7. IoT

1. Set up the digital sensor on GPIO2 and the relay (or LED) on GPIO0, with the rule on.
2. Press the sensor button a few times.
3. From the panel: `on`, `off`, `pulse 3`, `auto`.

**Expected:** the rule follows the sensor; the pulse turns off by itself after 3 s; the log shows `Panel: command "..."` for each one.

## 8. Local API

```bash
curl -H 'Authorization: Bearer <token>' http://<ip>/api/state
curl -H 'Authorization: Bearer <token>' -d on http://<ip>/api/cmd
curl -i -H 'Authorization: Bearer wrong' http://<ip>/api/state
```

**Expected:** the state JSON; the LED turns on and the log shows `API: command "on"`; a wrong token gets 401 (and 429 after 5 mistakes in a row). The responses carry `Cache-Control: no-store` and `X-Frame-Options: DENY`.

## 9. MQTT and Home Assistant (if used)

1. On the **MQTT** page, enter the broker (IP and port), user and password.
2. On a computer in the network: `mosquitto_sub -h <broker> -u <user> -P <password> -v -t 'esp8266iot/#' -t 'homeassistant/#'`.
3. In Home Assistant, open the MQTT integration.

**Expected:**
- On **MQTT**, the state turns "connected".
- `.../avail online`, the discovery at `homeassistant/device/esp8266iot_<id>/config` and the state at `.../state` show up.
- The **ESP8266 IoT** device appears in HA with the sensor and the actuator. Switching on and off from HA works both ways.
- Unplugging the device, the broker publishes `offline` and the entities become unavailable.
- Publishing `on` **retained** to the command topic and powering the device back on must not turn the LED on: the log shows `MQTT: retained command ignored`.
- Change the actuator on the panel (for example, to an LED with level): the old entity disappears from HA and the new one shows up.

## 10. Update (OTA)

1. Under **Update**, click **Check now** with a new version published.
2. Install it.

**Expected:** the progress bar moves, the device reboots into the new version and keeps its configuration. During the download, MQTT drops and comes back on its own afterwards. If the heap cannot fit the download, the device reboots in recovery mode, finishes the OTA and goes back to normal. The log shows `Firmware URL: .../firmware.tagged.bin`. A device on a firmware from before the tagged signature still updates, through `firmware.signed.bin`.

## 10b. After the size and security round

1. Update from a firmware older than the round and sign in: the log shows `Panel password: now stored as a hash`, and `/config.json` no longer holds the password as typed (read it back with the web installer's file dump, or check that sign-in still works after a reboot).
2. With Cloudflare set up, change the public IP (or the record) and check the log: `DNS updated!` (the request now goes as HTTP/1.0).
3. Open the panel as `http://<ip>`, as `http://<hostname>.local` and through the router's name for the device: all open. Point a name of your own at the device's IP (hosts file) and open it: `Unknown address`. Add that name under **Settings → Other addresses** and it opens.
4. With a slow client (`telnet <ip> 80`, type a few characters and wait), the panel answers again in at most 3 s on another browser tab, and a running `pulse 5` ends on time.
5. Without internet (setup access point), sign in with the browser clock set a day ahead; then give the device internet back, wait for the clock, and sign in with a current code: it works.

## 11. Memory after a day

Leave the device on for 24 h with ntfy, MQTT and DNS active and check **Status**:

**Expected:** free heap above 15 KB, fragmentation below 50 % and no reset other than the scheduled daily reboot. The reason for the last reset shows under **Status**.

## Record

| Date | Version | Item | Result | Note |
| --- | --- | --- | --- | --- |
| | | 1. Flashing | | |
| | | 2. AP password | | |
| | | 3. Panel and login | | |
| | | 4. Wi-Fi | | |
| | | 5. DNS | | |
| | | 6. ntfy | | |
| | | 7. IoT | | |
| | | 8. API | | |
| | | 9. MQTT and HA | | |
| | | 10. OTA | | |
| | | 10b. Round of 2026-10-14 | | |
| | | 11. Memory | | |
