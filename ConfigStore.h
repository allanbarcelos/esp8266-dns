// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <cstddef>
#include <memory>
#include <LittleFS.h>
#include "Json.h"
#include "Config.h"
#include "Logger.h"
#include "IoTCatalog.h"
#include "IoTState.h"
#include "Totp.h"
#include "Password.h"

// /config.json with a version ("v"). On load, an old version goes
// through the migrations in order up to the current one, the original file
// is kept in /config.v1.json and the new one is written. A missing field
// gets its default; an unknown field is ignored, and so is a value of the
// wrong type or out of the field's range. A file from a newer version (an
// OTA that went back) is read as far as it is understood and is not
// rewritten on its own: an unknown type identifier stays as is.
//
// Every persisted field is a row of FIELDS (name, place in Config, type,
// default): load and save are two loops over that table.
class ConfigStore {
public:
    ConfigStore(Config& cfg, Logger& log) : _cfg(cfg), _log(log) {}

    static constexpr int    VERSION  = 2;
    // Nothing save() writes comes close: anything bigger is a foreign file
    static constexpr size_t MAX_FILE = 4096;

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

        // Read whole; the reader walks the text without copying it
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

        // A broken file changes nothing: checked whole before the defaults
        if (!json::object(text.get(), nullptr, nullptr)) {
            _log.log(PSTR("JSON error"));
            return false;
        }
        for (size_t i = 0; i < FIELD_COUNT; i++) _default(_field(FIELDS, i));
        Load ctx;
        ctx.self = this;
        json::object(text.get(), _member, &ctx);

        // A password from before the hash: replaced now, and saved below
        bool rehash = _cfg.webpss.length() && !Password::hashed(_cfg.webpss);
        if (rehash) {
            _cfg.webpss = Password::hash(_cfg.webpss);
            _log.log(PSTR("Panel password: now stored as a hash"));
        }

        if (ctx.version == 1) {
            _migrateV1(ctx);
            _log.log(PSTR("Configuration migrated to v%d"), VERSION);
            _backup();
            save();
            return true;
        }
        if (ctx.version > VERSION) _log.log(PSTR("Configuration v%d, newer than the firmware"), (int)ctx.version);
        else if (rehash) save();
        _log.log(PSTR("Configuration loaded"));
        return true;
    }

    // Atomic write: writes a temporary file and renames it over the old one.
    // A power cut in the middle of the write keeps the previous config.
    bool save() {
        json::Writer measure;
        if (_write(measure) >= MAX_FILE) {   // load() would refuse it
            _log.log(PSTR("Config exceeds the file limit"));
            return false;
        }

        File f = LittleFS.open(TMP_PATH, "w");
        if (!f) {
            _log.log(PSTR("Could not save config"));
            return false;
        }
        json::Writer w(&f);
        _write(w);
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

    enum : uint8_t { T_STRING, T_CHARS, T_HEX20, T_BOOL, T_I8, T_U8, T_I16, T_U16, T_U32 };

    struct Field {
        PGM_P    key;
        uint16_t off;    // bytes from the start of the target struct
        uint8_t  type;
        uint8_t  size;
        int16_t  def;    // numbers and flags
    };

private:
    Config& _cfg;
    Logger& _log;

    // What a load gathers besides Config: the version, whether cf_record was
    // text, and the v1 fields, applied only if the file turns out to be v1
    struct Load {
        ConfigStore* self = nullptr;
        long      version = 1;
        bool      record  = false;
        IoTConfig v1;
        int8_t    sensor = -1, module = -1;
        uint8_t   sensorType = 0, moduleType = 0, manual = 0;
    };

#define CS_KEY(name, text) static constexpr char name[] PROGMEM = text;
    CS_KEY(K_V, "v") CS_KEY(K_RECORDS, "cf_records")
    CS_KEY(K_WEBUSR, "webusr") CS_KEY(K_WEBPSS, "webpss") CS_KEY(K_TOTP, "totp")
    CS_KEY(K_TOTP_STEP, "totp_step") CS_KEY(K_TOTP_BSTEP, "totp_bstep") CS_KEY(K_SSID, "ssid") CS_KEY(K_PASS, "pass")
    CS_KEY(K_NTFY_TOPIC, "ntfy_topic") CS_KEY(K_NTFY_CMD, "ntfy_cmd") CS_KEY(K_NTFY_TOKEN, "ntfy_token") CS_KEY(K_NTFY_POLL, "ntfy_poll") CS_KEY(K_NTFY_HOST, "ntfy_host")
    CS_KEY(K_MQTT_HOST, "mqtt_host") CS_KEY(K_MQTT_USER, "mqtt_user") CS_KEY(K_MQTT_PASS, "mqtt_pass")
    CS_KEY(K_NAME, "name") CS_KEY(K_HOSTNAME, "hostname") CS_KEY(K_HOSTS, "hosts") CS_KEY(K_TZ, "tz") CS_KEY(K_BAT_SDA, "bat_sda")
    CS_KEY(K_LANG, "lang") CS_KEY(K_OTA_AUTO, "ota_auto") CS_KEY(K_API_TOKEN, "api_token")
    CS_KEY(K_CF_TOKEN, "cf_token") CS_KEY(K_CF_ZONE, "cf_zone") CS_KEY(K_CF_HOST, "cf_host")
    CS_KEY(K_CF_RECORD, "cf_record") CS_KEY(K_CF_ACCOUNT, "cf_account")
    CS_KEY(K_RELAY_HOST, "relay_host")
    CS_KEY(K_RELAY_KEY, "relay_key") CS_KEY(K_RELAY_VER, "relay_ver")
    CS_KEY(K_RELAY_NAME, "relay_name") CS_KEY(K_RELAY_ZONE, "relay_zone")
    CS_KEY(K_SENSOR, "sensor") CS_KEY(K_SENSOR_PIN, "sensor_pin")
    CS_KEY(K_SENSOR_HIGH, "sensor_high") CS_KEY(K_SENSOR_ON, "sensor_on") CS_KEY(K_SENSOR_OFF, "sensor_off")
    CS_KEY(K_METRIC, "metric") CS_KEY(K_NOTIFY, "notify") CS_KEY(K_ACTUATOR, "actuator")
    CS_KEY(K_UNIT, "unit") CS_KEY(K_TANK_EMPTY, "tank_empty") CS_KEY(K_TANK_FULL, "tank_full")
    CS_KEY(K_TANK_L, "tank_l") CS_KEY(K_PPL, "ppl") CS_KEY(K_PULSE, "pulse")
    CS_KEY(K_ACTUATOR_PIN, "actuator_pin") CS_KEY(K_ACTUATOR_HIGH, "actuator_high")
    CS_KEY(K_ANGLE_ON, "angle_on") CS_KEY(K_ANGLE_OFF, "angle_off") CS_KEY(K_DUTY_ON, "duty_on")
    CS_KEY(K_DUTY_OFF, "duty_off") CS_KEY(K_LEDS, "leds") CS_KEY(K_MAX_ON, "max_on")
    CS_KEY(K_RESTORE, "restore") CS_KEY(K_RULE, "rule") CS_KEY(K_ABOVE, "above")
    CS_KEY(K_THRESHOLD, "threshold") CS_KEY(K_MARGIN, "margin") CS_KEY(K_TOGGLE, "toggle")
    CS_KEY(K_LINEAR, "linear") CS_KEY(K_THRESHOLD_HI, "threshold_hi") CS_KEY(K_WINDOW, "window")
    CS_KEY(K_WIN_START, "win_start") CS_KEY(K_WIN_END, "win_end")
    // v1 (before the type identifiers): the IoT fields had other names
    CS_KEY(K1_SENSOR, "iot_sensor") CS_KEY(K1_MODULE, "iot_module")
    CS_KEY(K1_SENSOR_TYPE, "iot_sensor_type") CS_KEY(K1_MODULE_TYPE, "iot_module_type")
    CS_KEY(K1_MANUAL, "iot_manual") CS_KEY(K1_SENSOR_HIGH, "iot_sensor_high")
    CS_KEY(K1_MODULE_HIGH, "iot_module_high") CS_KEY(K1_NOTIFY, "iot_notify")
    CS_KEY(K1_METRIC, "iot_metric") CS_KEY(K1_ABOVE, "iot_above") CS_KEY(K1_THRESHOLD, "iot_threshold")
    CS_KEY(K1_MARGIN, "iot_margin") CS_KEY(K1_ANGLE_ON, "iot_angle_on") CS_KEY(K1_ANGLE_OFF, "iot_angle_off")
    CS_KEY(K1_DUTY_ON, "iot_duty_on") CS_KEY(K1_DUTY_OFF, "iot_duty_off") CS_KEY(K1_TOGGLE, "iot_toggle")
    CS_KEY(K1_LINEAR, "iot_linear") CS_KEY(K1_THRESHOLD_HI, "iot_threshold_hi")
#undef CS_KEY

// Config holds Strings, so it is not standard-layout: offsetof works on
// GCC for these plain members, and it is the whole point of the table
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#define CS_CFG(key, m, type, def) {key, (uint16_t)offsetof(Config, m), type, (uint8_t)sizeof(Config::m), def}
#define CS_IOT(key, m, type, def) \
    {key, (uint16_t)(offsetof(Config, iot) + offsetof(IoTConfig, m)), type, (uint8_t)sizeof(IoTConfig::m), def}
#define CS_V1(key, m, type) {key, (uint16_t)offsetof(Load, m), type, (uint8_t)sizeof(Load::m), 0}
#define CS_V1IOT(key, m, type) \
    {key, (uint16_t)(offsetof(Load, v1) + offsetof(IoTConfig, m)), type, (uint8_t)sizeof(IoTConfig::m), 0}

    // In the order they are written
    static constexpr Field FIELDS[] PROGMEM = {
        CS_CFG(K_WEBUSR, webusr, T_STRING, 0), CS_CFG(K_WEBPSS, webpss, T_STRING, 0),
        CS_CFG(K_TOTP, totp, T_HEX20, 0), CS_CFG(K_TOTP_STEP, totpStep, T_U32, 0),
        CS_CFG(K_TOTP_BSTEP, totpBrowserStep, T_U32, 0),
        CS_CFG(K_SSID, ssid, T_STRING, 0), CS_CFG(K_PASS, pass, T_STRING, 0),
        CS_CFG(K_NTFY_TOPIC, ntfy_topic, T_CHARS, 0), CS_CFG(K_NTFY_CMD, ntfy_cmd, T_CHARS, 0),
        CS_CFG(K_NTFY_TOKEN, ntfy_token, T_CHARS, 0), CS_CFG(K_NTFY_POLL, ntfy_poll, T_U8, 5),
        CS_CFG(K_NTFY_HOST, ntfy_host, T_CHARS, 0),
        CS_CFG(K_MQTT_HOST, mqtt_host, T_CHARS, 0),
        CS_CFG(K_MQTT_USER, mqtt_user, T_CHARS, 0), CS_CFG(K_MQTT_PASS, mqtt_pass, T_CHARS, 0),
        CS_CFG(K_NAME, dev_name, T_CHARS, 0), CS_CFG(K_HOSTNAME, hostname, T_CHARS, 0),
        CS_CFG(K_HOSTS, web_hosts, T_CHARS, 0),
        CS_CFG(K_TZ, tz, T_I16, 0), CS_CFG(K_BAT_SDA, bat_sda, T_I8, -1),
        CS_CFG(K_LANG, lang, T_CHARS, 0), CS_CFG(K_OTA_AUTO, ota_auto, T_BOOL, 1),
        CS_CFG(K_API_TOKEN, api_token, T_CHARS, 0), CS_CFG(K_CF_TOKEN, cf_token, T_CHARS, 0),
        CS_CFG(K_CF_ZONE, cf_zone, T_CHARS, 0), CS_CFG(K_CF_HOST, cf_host, T_CHARS, 0),
        CS_CFG(K_CF_RECORD, cf_record, T_CHARS, 0), CS_CFG(K_CF_ACCOUNT, cf_account, T_CHARS, 0),
        CS_CFG(K_RELAY_HOST, relay_host, T_CHARS, 0), CS_CFG(K_RELAY_KEY, relay_secret, T_CHARS, 0),
        CS_CFG(K_RELAY_VER, relay_ver, T_U8, 0), CS_CFG(K_RELAY_NAME, relay_name, T_CHARS, 0),
        CS_CFG(K_RELAY_ZONE, relay_zone, T_CHARS, 0),
        CS_IOT(K_SENSOR, sensor, T_CHARS, 0), CS_IOT(K_SENSOR_PIN, sensorPin, T_I8, -1),
        CS_IOT(K_SENSOR_HIGH, sensorHigh, T_BOOL, 1), CS_IOT(K_SENSOR_ON, onText, T_CHARS, 0),
        CS_IOT(K_SENSOR_OFF, offText, T_CHARS, 0), CS_IOT(K_METRIC, metric, T_U8, 0),
        CS_IOT(K_UNIT, unit, T_U8, 0), CS_IOT(K_TANK_EMPTY, tankEmpty, T_U16, 100),
        CS_IOT(K_TANK_FULL, tankFull, T_U16, 20), CS_IOT(K_TANK_L, tankL, T_U16, 1000),
        CS_IOT(K_PPL, ppl, T_U16, 450), CS_IOT(K_PULSE, pulse, T_U16, 0),
        CS_IOT(K_NOTIFY, notify, T_BOOL, 0), CS_IOT(K_ACTUATOR, actuator, T_CHARS, 0),
        CS_IOT(K_ACTUATOR_PIN, actuatorPin, T_I8, -1), CS_IOT(K_ACTUATOR_HIGH, actuatorHigh, T_BOOL, 0),
        CS_IOT(K_ANGLE_ON, angleOn, T_U8, 90), CS_IOT(K_ANGLE_OFF, angleOff, T_U8, 0),
        CS_IOT(K_DUTY_ON, dutyOn, T_U8, 100), CS_IOT(K_DUTY_OFF, dutyOff, T_U8, 0),
        CS_IOT(K_LEDS, leds, T_U8, 8), CS_IOT(K_MAX_ON, maxOn, T_U16, 0),
        CS_IOT(K_RESTORE, restore, T_BOOL, 0), CS_IOT(K_RULE, rule, T_BOOL, 1),
        CS_IOT(K_ABOVE, above, T_BOOL, 0), CS_IOT(K_THRESHOLD, threshold, T_I16, 0),
        CS_IOT(K_MARGIN, margin, T_U8, 0), CS_IOT(K_TOGGLE, toggle, T_BOOL, 0),
        CS_IOT(K_LINEAR, linear, T_BOOL, 0), CS_IOT(K_THRESHOLD_HI, thresholdHi, T_I16, 0),
        CS_IOT(K_WINDOW, window, T_BOOL, 0), CS_IOT(K_WIN_START, winStart, T_U16, 0),
        CS_IOT(K_WIN_END, winEnd, T_U16, 0),
    };
    static constexpr size_t FIELD_COUNT = sizeof(FIELDS) / sizeof(FIELDS[0]);

    // v1 fields, into Load (defaults: those of Load and IoTConfig)
    static constexpr Field V1_FIELDS[] PROGMEM = {
        CS_V1(K1_SENSOR, sensor, T_I8), CS_V1(K1_MODULE, module, T_I8),
        CS_V1(K1_SENSOR_TYPE, sensorType, T_U8), CS_V1(K1_MODULE_TYPE, moduleType, T_U8),
        CS_V1(K1_MANUAL, manual, T_U8),
        CS_V1IOT(K1_SENSOR_HIGH, sensorHigh, T_BOOL), CS_V1IOT(K1_MODULE_HIGH, actuatorHigh, T_BOOL),
        CS_V1IOT(K1_NOTIFY, notify, T_BOOL), CS_V1IOT(K1_METRIC, metric, T_U8),
        CS_V1IOT(K1_ABOVE, above, T_BOOL), CS_V1IOT(K1_THRESHOLD, threshold, T_I16),
        CS_V1IOT(K1_MARGIN, margin, T_U8), CS_V1IOT(K1_ANGLE_ON, angleOn, T_U8),
        CS_V1IOT(K1_ANGLE_OFF, angleOff, T_U8), CS_V1IOT(K1_DUTY_ON, dutyOn, T_U8),
        CS_V1IOT(K1_DUTY_OFF, dutyOff, T_U8), CS_V1IOT(K1_TOGGLE, toggle, T_BOOL),
        CS_V1IOT(K1_LINEAR, linear, T_BOOL), CS_V1IOT(K1_THRESHOLD_HI, thresholdHi, T_I16),
    };
    static constexpr size_t V1_COUNT = sizeof(V1_FIELDS) / sizeof(V1_FIELDS[0]);
    static constexpr uint16_t RECORD_OFF = (uint16_t)offsetof(Config, cf_record);
#undef CS_CFG
#undef CS_IOT
#undef CS_V1
#undef CS_V1IOT
#pragma GCC diagnostic pop

    static Field _field(const Field* table, size_t i) {
        Field f;
        memcpy_P(&f, &table[i], sizeof(f));
        return f;
    }

    uint8_t* _at(const Field& f) { return reinterpret_cast<uint8_t*>(&_cfg) + f.off; }

    void _default(const Field& f) {
        uint8_t* p = _at(f);
        if (f.type == T_STRING) {
            *reinterpret_cast<String*>(p) = f.key == K_WEBUSR ? "admin" : "";
            return;
        }
        uint32_t n = (uint32_t)(int32_t)f.def;   // zero for text and the key
        for (uint8_t i = 0; i < f.size; i++) p[i] = i < 4 ? (uint8_t)(n >> (8 * i)) : 0;
    }

    // A value of the field's type, and in its range, goes into p; anything else is ignored
    static void _set(uint8_t* p, const Field& f, const json::Value& v) {
        bool text = v.kind == json::STRING;
        if (f.type == T_STRING) {
            if (text) *reinterpret_cast<String*>(p) = json::str(v);
        } else if (f.type == T_CHARS) {
            if (text) json::copy(v, reinterpret_cast<char*>(p), f.size);
        } else if (f.type == T_HEX20) {
            char hex[Totp::KEY_LEN * 2 + 2];
            bool fits = text && json::copy(v, hex, sizeof(hex)) < sizeof(hex);
            Totp::fromHex(fits ? hex : "", p);
        } else if (f.type == T_BOOL) {
            if (v.kind == json::BOOL) *p = (uint8_t)v.mag;
        } else {
            static const long     LO[] = {-128, 0, -32768, 0, 0};
            static const uint32_t HI[] = {127, 255, 32767, 65535, 0xFFFFFFFFu};
            uint32_t n;
            if (v.fits(LO[f.type - T_I8], HI[f.type - T_I8], n))
                for (uint8_t i = 0; i < f.size; i++) p[i] = (uint8_t)(n >> (8 * i));
        }
    }

    static bool _find(const Field* table, size_t count, const char* key, size_t len, Field& f) {
        for (size_t i = 0; i < count; i++) {
            f = _field(table, i);
            if (json::is(key, len, f.key)) return true;
        }
        return false;
    }

    static void _member(void* c, const char* key, size_t len, const json::Value& v) {
        Load& ctx = *static_cast<Load*>(c);
        Field f;
        if (json::is(key, len, K_V)) {
            uint32_t n;
            if (v.fits(-2147483647L, 0x7FFFFFFFu, n)) ctx.version = (long)(int32_t)n;
        } else if (json::is(key, len, K_RECORDS)) {
            // There used to be up to 5 IDs: the first one, unless cf_record is text
            json::Value first = v.kind == json::ARRAY ? json::first(v) : json::Value();
            if (!ctx.record && first.kind == json::STRING)
                json::copy(first, ctx.self->_cfg.cf_record, sizeof(Config::cf_record));
        } else if (_find(FIELDS, FIELD_COUNT, key, len, f)) {
            _set(ctx.self->_at(f), f, v);
            if (f.off == RECORD_OFF && v.kind == json::STRING) ctx.record = true;
        } else if (_find(V1_FIELDS, V1_COUNT, key, len, f)) {
            _set(reinterpret_cast<uint8_t*>(&ctx) + f.off, f, v);
        }
    }

    size_t _write(json::Writer& w) {
        w.num(K_V, VERSION);
        for (size_t i = 0; i < FIELD_COUNT; i++) {
            Field f = _field(FIELDS, i);
            const uint8_t* p = _at(f);
            switch (f.type) {
                case T_STRING: w.str(f.key, reinterpret_cast<const String*>(p)->c_str()); break;
                case T_CHARS:  w.str(f.key, reinterpret_cast<const char*>(p)); break;
                case T_HEX20: {
                    char hex[Totp::KEY_LEN * 2 + 1] = "";
                    if (!Totp::blank(p)) Totp::toHex(p, hex);
                    w.str(f.key, hex);
                    break;
                }
                case T_BOOL:   w.boolean(f.key, *p != 0); break;
                case T_I8:     w.num(f.key, *reinterpret_cast<const int8_t*>(p)); break;
                case T_U8:     w.num(f.key, *p); break;
                case T_I16:    w.num(f.key, *reinterpret_cast<const int16_t*>(p)); break;
                case T_U16:    w.num(f.key, *reinterpret_cast<const uint16_t*>(p)); break;
                default:       w.unum(f.key, *reinterpret_cast<const uint32_t*>(p)); break;
            }
        }
        return w.end();
    }

    // v1 stored the type by its position in the old table and only drove the
    // module with a sensor. Without a sensor, the module stays off here too,
    // otherwise the pin would start being driven after the update.
    void _migrateV1(const Load& ctx) {
        IoTConfig& t = _cfg.iot;
        t = ctx.v1;
        if (!iot::allowedPin(ctx.sensor)) return;

        t.sensorPin = ctx.sensor;
        strlcpy(t.sensor, iot::v1Sensor(ctx.sensorType), sizeof(t.sensor));
        if (!iot::allowedPin(ctx.module) || ctx.module == ctx.sensor) return;

        t.actuatorPin = ctx.module;
        strlcpy(t.actuator, iot::v1Actuator(ctx.moduleType), sizeof(t.actuator));
        // A fixed "command" (1 on, 2 off) becomes the saved manual state
        if (ctx.manual == 1 || ctx.manual == 2) {
            t.restore = true;
            IoTState st;
            st.manual = true;
            st.on = ctx.manual == 1;
            st.level = t.dutyOn;
            st.angle = t.angleOn;
            st.save();
        }
    }

    // Copy of the original file before the first write in the new format
    static void _backup() {
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
