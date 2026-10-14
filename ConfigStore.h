// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <memory>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "Config.h"
#include "Logger.h"
#include "IoTCatalog.h"
#include "IoTState.h"
#include "Totp.h"

// /config.json with a version ("v"). On load, an old version goes
// through the migrations in order up to the current one, the original file
// is kept in /config.v1.json and the new one is written. A missing field
// gets its default; an unknown field is ignored. A file from a newer
// version (an OTA that went back) is read as far as it is understood and
// is not rewritten on its own: an unknown type identifier stays as is.
class ConfigStore {
public:
    ConfigStore(Config& cfg, Logger& log) : _cfg(cfg), _log(log) {}

    static constexpr int    VERSION       = 2;
    // Room for the worst case (every field at its maximum size)
    static constexpr size_t JSON_CAPACITY = 3072;
    // Nothing save() writes comes close: anything bigger is a foreign file
    static constexpr size_t MAX_FILE      = 4096;

    bool begin() {
        if (!LittleFS.begin()) {
            _log.log(PSTR("LittleFS mount failed"));
            return false;
        }
        _log.log(PSTR("LittleFS ready"));
        return true;
    }

    bool load() {
        if (!LittleFS.exists(PATH)) {
            _log.log(PSTR("config.json not found"));
            return false;
        }

        File f = LittleFS.open(PATH, "r");
        if (!f) {
            _log.log(PSTR("Could not open config.json"));
            return false;
        }

        // Read whole and parsed in place (char*): the same JSON reader as the
        // rest of the firmware, and the strings stay in the text, with no copy
        // in the document. The text lives until load ends (the doc points to it).
        size_t n = f.size();
        std::unique_ptr<char, void (*)(void*)> text(
            n < MAX_FILE ? (char*)malloc(n + 1) : nullptr, free);
        if (!text) {
            f.close();
            _log.log(PSTR("config.json unreadable (%u bytes)"), (unsigned)n);
            return false;
        }
        text.get()[f.readBytes(text.get(), n)] = '\0';
        f.close();

        DynamicJsonDocument doc(JSON_CAPACITY);
        DeserializationError err = deserializeJson(doc, text.get());

        if (err) {
            _log.log(PSTR("JSON error: %s"), err.c_str());
            return false;
        }

        _cfg.webusr = doc["webusr"] | "admin";
        _cfg.webpss = doc["webpss"] | "";
        Totp::fromHex(doc["totp"] | "", _cfg.totp);
        _cfg.totpStep = doc["totp_step"] | 0UL;
        _cfg.ssid   = doc["ssid"]   | "";
        _cfg.pass   = doc["pass"]   | "";

        strlcpy(_cfg.ntfy_topic, doc["ntfy_topic"] | "", sizeof(_cfg.ntfy_topic));
        strlcpy(_cfg.ntfy_cmd,   doc["ntfy_cmd"]   | "", sizeof(_cfg.ntfy_cmd));
        strlcpy(_cfg.mqtt_host,  doc["mqtt_host"]  | "", sizeof(_cfg.mqtt_host));
        strlcpy(_cfg.mqtt_user,  doc["mqtt_user"]  | "", sizeof(_cfg.mqtt_user));
        strlcpy(_cfg.mqtt_pass,  doc["mqtt_pass"]  | "", sizeof(_cfg.mqtt_pass));
        strlcpy(_cfg.dev_name,   doc["name"]       | "", sizeof(_cfg.dev_name));
        strlcpy(_cfg.hostname,   doc["hostname"]   | "", sizeof(_cfg.hostname));
        _cfg.tz = doc["tz"] | 0;
        _cfg.bat_sda = doc["bat_sda"] | -1;
        strlcpy(_cfg.lang, doc["lang"] | "", sizeof(_cfg.lang));
        strlcpy(_cfg.ntfy_token, doc["ntfy_token"] | "", sizeof(_cfg.ntfy_token));
        _cfg.ota_auto = doc["ota_auto"] | true;
        strlcpy(_cfg.api_token,  doc["api_token"]  | "", sizeof(_cfg.api_token));
        strlcpy(_cfg.cf_token,   doc["cf_token"]   | "", sizeof(_cfg.cf_token));
        strlcpy(_cfg.cf_zone,    doc["cf_zone"]    | "", sizeof(_cfg.cf_zone));
        strlcpy(_cfg.cf_host,    doc["cf_host"]    | "", sizeof(_cfg.cf_host));

        // There used to be up to 5 IDs in cf_records: the first one stays
        const char* rec = doc["cf_record"];
        if (!rec) rec = doc["cf_records"][0] | "";
        strlcpy(_cfg.cf_record, rec, sizeof(_cfg.cf_record));

        int version = doc["v"] | 1;
        if (version == 1) {
            _migrateV1(doc);
            _log.log(PSTR("Configuration migrated to v%d"), VERSION);
            _backup();
            save();
            return true;
        }
        _loadIoT(doc);
        if (version > VERSION) _log.log(PSTR("Configuration v%d, newer than the firmware"), version);
        _log.log(PSTR("Configuration loaded"));
        return true;
    }

    // Atomic write: writes a temporary file and renames it over the old one.
    // A power cut in the middle of the write keeps the previous config.
    bool save() {
        DynamicJsonDocument doc(JSON_CAPACITY);

        doc["v"]          = VERSION;
        doc["webusr"]     = _cfg.webusr;
        doc["webpss"]     = _cfg.webpss;
        char totpHex[Totp::KEY_LEN * 2 + 1];
        if (Totp::blank(_cfg.totp)) {
            doc["totp"] = "";
        } else {
            Totp::toHex(_cfg.totp, totpHex);
            doc["totp"] = totpHex;
        }
        doc["totp_step"]  = (unsigned long)_cfg.totpStep;
        doc["ssid"]       = _cfg.ssid;
        doc["pass"]       = _cfg.pass;
        doc["ntfy_topic"] = _cfg.ntfy_topic;
        doc["ntfy_cmd"]   = _cfg.ntfy_cmd;
        doc["ntfy_token"] = _cfg.ntfy_token;
        doc["mqtt_host"]  = _cfg.mqtt_host;
        doc["mqtt_user"]  = _cfg.mqtt_user;
        doc["mqtt_pass"]  = _cfg.mqtt_pass;
        doc["name"]       = _cfg.dev_name;
        doc["hostname"]   = _cfg.hostname;
        doc["tz"]         = _cfg.tz;
        doc["bat_sda"]    = _cfg.bat_sda;
        doc["lang"]       = _cfg.lang;
        doc["ota_auto"]   = _cfg.ota_auto;
        doc["api_token"]  = _cfg.api_token;
        doc["cf_token"]   = _cfg.cf_token;
        doc["cf_zone"]    = _cfg.cf_zone;
        doc["cf_host"]    = _cfg.cf_host;
        doc["cf_record"]  = _cfg.cf_record;

        const IoTConfig& t = _cfg.iot;
        doc["sensor"]        = t.sensor;
        doc["sensor_pin"]    = t.sensorPin;
        doc["sensor_high"]   = t.sensorHigh;
        doc["sensor_on"]     = t.onText;
        doc["sensor_off"]    = t.offText;
        doc["metric"]        = t.metric;
        doc["notify"]        = t.notify;
        doc["actuator"]      = t.actuator;
        doc["actuator_pin"]  = t.actuatorPin;
        doc["actuator_high"] = t.actuatorHigh;
        doc["angle_on"]      = t.angleOn;
        doc["angle_off"]     = t.angleOff;
        doc["duty_on"]       = t.dutyOn;
        doc["duty_off"]      = t.dutyOff;
        doc["leds"]          = t.leds;
        doc["max_on"]        = t.maxOn;
        doc["restore"]       = t.restore;
        doc["rule"]          = t.rule;
        doc["above"]         = t.above;
        doc["threshold"]     = t.threshold;
        doc["margin"]        = t.margin;
        doc["toggle"]        = t.toggle;
        doc["linear"]        = t.linear;
        doc["threshold_hi"]  = t.thresholdHi;
        doc["window"]        = t.window;
        doc["win_start"]     = t.winStart;
        doc["win_end"]       = t.winEnd;

        if (doc.overflowed()) {
            _log.log(PSTR("Config exceeds the JSON capacity"));
            return false;
        }

        File f = LittleFS.open(TMP_PATH, "w");
        if (!f) {
            _log.log(PSTR("Could not save config"));
            return false;
        }
        serializeJson(doc, f);
        f.close();

        if (!LittleFS.rename(TMP_PATH, PATH)) {
            _log.log(PSTR("Could not replace config.json"));
            LittleFS.remove(TMP_PATH);
            return false;
        }

        _log.log(PSTR("Configuration saved"));
        return true;
    }

    static constexpr const char* PATH        = "/config.json";
    static constexpr const char* TMP_PATH    = "/config.tmp";
    static constexpr const char* BACKUP_PATH = "/config.v1.json";

private:
    Config& _cfg;
    Logger& _log;

    void _loadIoT(const JsonDocument& doc) {
        IoTConfig& t = _cfg.iot;
        strlcpy(t.sensor, doc["sensor"] | "", sizeof(t.sensor));
        t.sensorPin    = doc["sensor_pin"]    | -1;
        t.sensorHigh   = doc["sensor_high"]   | true;
        strlcpy(t.onText,  doc["sensor_on"]  | "", sizeof(t.onText));
        strlcpy(t.offText, doc["sensor_off"] | "", sizeof(t.offText));
        t.metric       = doc["metric"]        | 0;
        t.notify       = doc["notify"]        | false;
        strlcpy(t.actuator, doc["actuator"] | "", sizeof(t.actuator));
        t.actuatorPin  = doc["actuator_pin"]  | -1;
        t.actuatorHigh = doc["actuator_high"] | false;
        t.angleOn      = doc["angle_on"]      | 90;
        t.angleOff     = doc["angle_off"]     | 0;
        t.dutyOn       = doc["duty_on"]       | 100;
        t.dutyOff      = doc["duty_off"]      | 0;
        t.leds         = doc["leds"]          | 8;
        t.maxOn        = doc["max_on"]        | 0;
        t.restore      = doc["restore"]       | false;
        t.rule         = doc["rule"]          | true;
        t.above        = doc["above"]         | false;
        t.threshold    = doc["threshold"]     | 0;
        t.margin       = doc["margin"]        | 0;
        t.toggle       = doc["toggle"]        | false;
        t.linear       = doc["linear"]        | false;
        t.thresholdHi  = doc["threshold_hi"]  | 0;
        t.window       = doc["window"]        | false;
        t.winStart     = doc["win_start"]     | 0;
        t.winEnd       = doc["win_end"]       | 0;
    }

    // v1 stored the type by its position in the old table and only drove the
    // module with a sensor. Without a sensor, the module stays off here too,
    // otherwise the pin would start being driven after the update.
    void _migrateV1(const JsonDocument& doc) {
        IoTConfig& t = _cfg.iot;
        t = IoTConfig{};
        int8_t sensorPin = doc["iot_sensor"] | -1;
        int8_t modulePin = doc["iot_module"] | -1;
        t.sensorHigh   = doc["iot_sensor_high"]  | true;
        t.actuatorHigh = doc["iot_module_high"]  | false;
        t.notify       = doc["iot_notify"]       | false;
        t.metric       = doc["iot_metric"]       | 0;
        t.above        = doc["iot_above"]        | false;
        t.threshold    = doc["iot_threshold"]    | 0;
        t.margin       = doc["iot_margin"]       | 0;
        t.angleOn      = doc["iot_angle_on"]     | 90;
        t.angleOff     = doc["iot_angle_off"]    | 0;
        t.dutyOn       = doc["iot_duty_on"]      | 100;
        t.dutyOff      = doc["iot_duty_off"]     | 0;
        t.toggle       = doc["iot_toggle"]       | false;
        t.linear       = doc["iot_linear"]       | false;
        t.thresholdHi  = doc["iot_threshold_hi"] | 0;
        if (!iot::allowedPin(sensorPin)) return;

        t.sensorPin = sensorPin;
        strlcpy(t.sensor, iot::v1Sensor(doc["iot_sensor_type"] | 0), sizeof(t.sensor));
        if (!iot::allowedPin(modulePin) || modulePin == sensorPin) return;

        t.actuatorPin = modulePin;
        strlcpy(t.actuator, iot::v1Actuator(doc["iot_module_type"] | 0), sizeof(t.actuator));
        // A fixed "command" (1 on, 2 off) becomes the saved manual state
        uint8_t manual = doc["iot_manual"] | 0;
        if (manual == 1 || manual == 2) {
            t.restore = true;
            IoTState st;
            st.manual = true;
            st.on = manual == 1;
            st.level = t.dutyOn;
            st.angle = t.angleOn;
            st.save();
        }
    }

    // Copy of the original file before the first write in the new format
    void _backup() {
        File in = LittleFS.open(PATH, "r");  // just read
        File out = LittleFS.open(BACKUP_PATH, "w");
        if (!out) return;
        char buf[64];
        size_t n;
        while ((n = in.readBytes(buf, sizeof(buf))) > 0) out.write((const uint8_t*)buf, n);
        in.close();
        out.close();
    }
};
