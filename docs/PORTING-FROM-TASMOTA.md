# What to port from Tasmota

Notes from 2026-10-13. Tasmota is the most proven firmware on the ESP8266. This device is a different product: a 1 MB ESP-01, one sensor and one actuator, Cloudflare DNS, ntfy, MQTT and signed OTA. What is worth bringing over is the small piece that solves a real case on this hardware. The rules engine, the driver list and OTA from a URL do not fit, and OTA from a URL would also weaken what this firmware already does better.

The current room is 1,280 bytes under the 500,000 cap. Nothing on the list below goes in before the lean web server, which should free 5 to 8 KB (see [STATUS-AND-NEXT-STEPS.md](STATUS-AND-NEXT-STEPS.md)). Name, hostname and teleperiod are the only ones that might fit before, if the new panel text is minimal.

## What to port

In order.

### 1. Time zone and a window

The clock here is UTC (`configTime` with a zero offset). Without a time zone, "turn on at 7 am" is wrong all year. Tasmota keeps an offset, daylight saving time and up to 16 timers with a day mask.

Here an offset in hours and a start and end window, gating only the automatic rule, are enough. Outside it, `auto` means off. A manual command still holds until `auto`. With the window on and the clock still in 1970, the actuator stays off.

### 2. A button

Tasmota handles button, switch, touch, hold and multiple clicks, on several GPIOs (`SwitchMode`). The sensor and the actuator already use two pins; two are left. What is missing is an optional pin: holding turns off, a tap switches to manual off until `auto`. The rule that follows the sensor already exists. The other modes stay out.

### 3. SHT3x, a single driver

In Tasmota's sensor list, the one that best replaces the DHT is the SHT30/SHT31. The driver there is generic, with an I2C manager, and does not fit. What gets ported is the protocol: two pins, a 6-byte read, math in tenths, in the same format as the DHT. SDA and SCL on GPIO0 and GPIO2, with pull-ups: the idle bus stays high, which is the boot level of those pins. The actuator moves to TX or RX. The DHT stays in the catalog. BME280, BH1750, energy meters and the rest of the list stay out.

### 4. Second Wi-Fi

Tasmota tries one SSID and, if it fails, another. `WiFiService` keeps a single network. The logic fits in it. The real cost is the panel fields, as text in flash. Useful when the router changes or there is a backup access point.

### 5. Device name and DHCP hostname

There is no name in `Config`. MQTT and Home Assistant use the chip ID. Tasmota separates the name shown in Home Assistant from the hostname shown on the router. Two short fields. RSSI is already on the panel.

### 6. TelePeriod

Tasmota republishes the state from time to time, even without a change. Here MQTT sends on connect and when something changes. A fixed interval, for example 5 minutes, avoids an unavailable entity in Home Assistant if a message is lost. The state JSON already exists.

## What not to port

| Tasmota piece | Why it stays out |
| --- | --- |
| Rules language (Rule1–3, IF, variables, backlog) | It is another product. It eats the whole room and duplicates the rule the panel already writes as a sentence. |
| OTA from an open URL | Any file at that address would be installed. Here an image only goes in with an RSA signature. |
| Dozens of drivers, energy, infrared, displays, Zigbee | They need pins, a bus or flash this board does not have. A second relay is still another ESP-01. |
| mDNS | Already measured at about 21 KB. The device is found by the IP reserved on the router. |
| Deep sleep | The ESP-01 does not have the GPIO16 wake-up. The panel, MQTT and OTA need the radio on. |
| Web console with the whole command language | The panel and the `on` and `pulse 5` commands already cover the actuator. |

## Order relative to the current roadmap

Items 5 and 6 of [STATUS-AND-NEXT-STEPS.md](STATUS-AND-NEXT-STEPS.md) (MQTT with TLS and the GitHub certificate, about 1.5 KB each) compete for the same bytes. Cloud TLS and the GitHub certificate do not increase what the device can automate. The signature already stops a fake image. Someone on the path can only delay the update.

If the goal is what Tasmota does and this device does not do yet, the order is: on-device test, lean web server, time zone and window, button, SHT3x. Name, second Wi-Fi and teleperiod go into whatever is left. The GitHub certificate comes back when there is room.
