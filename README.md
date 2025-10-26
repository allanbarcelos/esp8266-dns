# 📘 **Technical Documentation — ESP8266 DDNS + OTA Firmware**

English | [Português](README.pt.md)

## 📋 **Project Overview**

The goal of this firmware is to use an **ESP8266** as a lightweight, autonomous agent for **dynamic IP updates** and **remote firmware management**.
It performs the following tasks:

1. **Automatically updates the public IP** on **Cloudflare DNS**, keeping the domain synchronized with the current residential IP.
2. **Manages Wi-Fi connection** with fault tolerance and backoff mechanisms.
3. **Performs secure OTA (Over-The-Air) updates** from **GitHub Releases**, supporting **firmware decryption**.
4. Runs a **local HTTP server** for diagnostics.
5. Implements **anti-blocking and anti-reboot-loop mechanisms**, ensuring high resilience.

---

## ⚙️ **Main Components**

| Module                | Function                                                                 |
| --------------------- | ------------------------------------------------------------------------ |
| `ESP8266WiFi.h`       | Wi-Fi connection and management                                          |
| `ESP8266HTTPClient.h` | HTTP and HTTPS communication                                             |
| `Updater.h`           | OTA firmware update handling                                             |
| `EEPROM.h`            | Non-volatile storage for failure counters                                |
| `ESP8266WebServer.h`  | Local HTTP web server                                                    |
| `secrets.h`           | Sensitive configuration (SSID, Cloudflare tokens, GitHub API URLs, etc.) |
| `crypto.h`            | Functions for decrypting OTA binary data                                 |

---

## 🧩 **System Architecture**

```
+----------------------------+
|         User               |
|  (Access via DNS domain)   |
+-------------+--------------+
              |
              v
+----------------------------+
|        Cloudflare          |
| (DNS points to public IP)  |
+-------------+--------------+
              ^
              |
     +--------+--------+
     |      ESP8266     |
     |------------------|
     |  Wi-Fi Manager   |
     |  OTA Updater     |
     |  DNS Updater     |
     |  Web Server      |
     +--------+---------+
              |
              v
     Residential Network (Dynamic IP)
```

---

## 🔄 **Main Execution Flow**

1. **Initialization (`setup()`)**

   * Reads the failure counter from EEPROM.
   * Determines whether to enter “wait mode” after too many failures.
   * Initializes Wi-Fi and the HTTP server.

2. **Main Loop (`loop()`)**

   * Handles the **HTTP server** (`server.handleClient()`).
   * Maintains the **Wi-Fi state machine**.
   * Performs **OTA checks** and **DNS updates** asynchronously (non-blocking).
   * Performs a **daily reboot** to free memory.

3. **Wi-Fi Management (`handleWiFi()`)**

   * Automatically reconnects when disconnected.
   * Uses EEPROM counters to avoid infinite reboot loops.
   * “WAIT” mode prevents continuous reboot cycles after multiple connection failures.

4. **OTA Update (`checkForUpdate()`)**

   * Queries the **GitHub API** for the latest firmware release.
   * If a new version is found → downloads the `.bin` → decrypts it via `decryptBuffer()` → installs via `Update.write()`.
   * Reboots after successful update.

5. **DNS Update (`handleDNSUpdate()`)**

   * Retrieves the current public IP using `api.ipify.org`.
   * Resolves the current DNS IP via `WiFi.hostByName()`.
   * If the two differ → updates the DNS record via the **Cloudflare API**.

---

## 🧠 **Wi-Fi State Diagram**

```text
┌───────────────────┐
│     WIFI_OK       │
│ (Connected)       │
└───┬───────────────┘
    │
    │ Wi-Fi lost
    ▼
┌───────────────────┐
│ WIFI_RECONNECTING │
│ (Trying to connect) │
└───┬───────────────┘
    │
    ├─ Successfully reconnected → WIFI_OK
    │
    ├─ Repeated failure → increment counter
    │
    └─ Too many failures → WIFI_WAIT
        │
        ▼
┌───────────────────┐
│    WIFI_WAIT      │
│ (Wait 30 minutes) │
└───┬───────────────┘
    │ Timeout expired
    ▼
┌───────────────────┐
│ WIFI_RECONNECTING │
└───────────────────┘
```

---

## 🌐 **DNS Update Flow**

```text
┌─────────────────────────────┐
│ handleDNSUpdate()           │
├─────────────────────────────┤
│ 1. Get public IP (ipify)    │
│ 2. Resolve current DNS IP   │
│ 3. Compare both             │
│ 4. If different → PATCH via │
│    Cloudflare API           │
│ 5. Log success or failure   │
└─────────────────────────────┘
```

---

## 🔄 **Secure OTA Update Flow (GitHub → ESP8266)**

```text
┌──────────────────────────────┐
│ checkForUpdate()             │
├──────────────────────────────┤
│ 1. Query GitHub API          │
│    → Extract tag and binary  │
│ 2. Compare with local version│
│ 3. Download `.bin` over HTTPS│
│ 4. Decrypt using crypto.h    │
│ 5. Flash via Update.write()  │
│ 6. Restart ESP8266           │
└──────────────────────────────┘
```

---

## 💾 **State Persistence (EEPROM)**

| Address | Data              | Description                                       |
| ------- | ----------------- | ------------------------------------------------- |
| 0       | `rebootFailCount` | Number of consecutive Wi-Fi reconnection failures |

**Purpose:**
Prevents infinite reboot loops by entering a *non-blocking wait mode* after a configurable number of failed connection attempts.

---

## 🔐 **Security Features**

| Mechanism                       | Description                                                 |
| ------------------------------- | ----------------------------------------------------------- |
| **HTTPS via WiFiClientSecure**  | Encrypted communication with GitHub and Cloudflare          |
| **Bearer Token Authentication** | Secure API access on Cloudflare                             |
| **Encrypted OTA Binaries**      | Prevents malicious firmware updates                         |
| **EEPROM Counters**             | Protects against endless reboot loops and DoS-like behavior |

---

## 🌍 **Local HTTP Server**

* Port: **80**
* Endpoint: `/`
* Response: simple HTML page with “ESP8266” title
  (Used only for health check and diagnostics)

---

## ⚡ **Timing and Intervals**

| Function            | Interval   | Description                               |
| ------------------- | ---------- | ----------------------------------------- |
| `checkInterval`     | 1 hour     | OTA version check interval                |
| `dnsUpdateInterval` | 5 minutes  | DNS record update interval                |
| `reconnectDelay`    | 5 seconds  | Delay between Wi-Fi reconnection attempts |
| `waitAfterFails`    | 30 minutes | Wait time after multiple failures         |
| `daily reboot`      | 24 hours   | Preventive reboot to clean memory         |

---

## 🧩 **Key Variables**

| Variable                      | Type          | Purpose                                 |
| ----------------------------- | ------------- | --------------------------------------- |
| `WifiConnState`               | Enum          | Current Wi-Fi connection state          |
| `rebootFailCount`             | int           | Reboot failure counter stored in EEPROM |
| `checkInterval`               | unsigned long | OTA check interval                      |
| `dnsUpdateInterval`           | unsigned long | DNS update interval                     |
| `lastCheck` / `dnsLastUpdate` | unsigned long | Timers for OTA and DNS                  |
| `reconnectAttempts`           | int           | Reconnection attempt counter            |

---

## 🧰 **External Dependencies (secrets.h)**

The file `secrets.h` should contain the following definitions:

```cpp
const char* ssid = "MyWiFiNetwork";
const char* password = "NetworkPassword";

const char* firmware_version = "v1.0.0";
const char* github_api = "https://api.github.com/repos/user/repository/releases/latest";

#define CF_ZONE "ZONE_ID"
#define CF_RECORD "RECORD_ID"
#define CF_TOKEN "API_TOKEN"
#define CF_HOST "exampledomain.com"
```

---

## 📈 **Sequence Diagram — DNS + OTA Update**

```text
ESP8266         Cloudflare         GitHub
   |                 |                 |
   |--- GET ipify -->|                 |  (get public IP)
   |<-- response ----|                 |
   |--- GET DNS ---->|                 |  (resolve DNS IP)
   |<-- response ----|                 |
   |--- PATCH DNS --->|                 |  (update if changed)
   |<-- OK ----------|                 |
   |---------------------------------->|  (query GitHub API)
   |<----------------------------------|  (receive release JSON)
   |---------------------------------->|  (download encrypted .bin)
   |<----------------------------------|  (OTA stream)
   |   Decrypt + Update.write()        |
   |--- Restart ---------------------->|
```

---

## 🧱 **Implemented Best Practices**

✅ Uses `yield()` during OTA to prevent watchdog resets
✅ Efficient non-blocking EEPROM writes
✅ Robust Wi-Fi reconnection logic with backoff
✅ All HTTP operations are **non-blocking**
✅ Fully compatible with **remote, headless IoT environments**

---

## 🧩 **Possible Extensions**

* Add a **local JSON API** endpoint (e.g., `/status`) showing IP, uptime, version, Wi-Fi status.
* Implement real **AES encryption** in `crypto.h` (e.g., AES-CTR).
* Add **NTP time synchronization** for accurate timestamps.
* Integrate **status LED indicators** (blinking = reconnecting, solid = connected).
* Add **Wi-Fi configuration portal** (using WiFiManager).

---

## 🏁 **Final Summary**

| Function                  | Description                                     |
| ------------------------- | ----------------------------------------------- |
| **Automatic DDNS**        | Keeps Cloudflare DNS up to date with current IP |
| **Secure OTA via GitHub** | Enables remote firmware updates                 |
| **Resilient Wi-Fi**       | Automatic reconnection with safe fallback       |
| **Local Web Server**      | Basic health and connectivity check             |
| **Fault Tolerance**       | EEPROM counters prevent boot loops              |
