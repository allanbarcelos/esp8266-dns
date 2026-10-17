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
// Every persisted field is a row of FIELDS (key, place in Config, type,
// default): load and save are two loops over that table. Since version 3 a
// key is three letters, kept inside the row; the full names of versions 1
// and 2 are only a 16-bit hash, so an older file is still read whole while
// none of those names takes room in the firmware. A v2 file is not rewritten
// on load: the next save writes it as v3.

// 16 bits of FNV-1a: how a key from version 2 or 1 is recognized
constexpr uint16_t configKeyHash(const char* s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    return (uint16_t)(h ^ (h >> 16));
}

class ConfigStore {
public:
    ConfigStore(Config& cfg, Logger& log) : _cfg(cfg), _log(log) {}

    static constexpr int    VERSION  = 3;
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

    static constexpr bool distinct(const uint16_t* a, size_t n) {
        for (size_t i = 0; i < n; i++)
            for (size_t j = i + 1; j < n; j++)
                if (a[i] == a[j]) return false;
        return true;
    }

    struct Field {
        char     key[4]; // since version 3: three letters ("" for a v1 field)
        uint16_t old;    // hash of the name in versions 1 and 2
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


// Config holds Strings, so it is not standard-layout: offsetof works on
// GCC for these plain members, and it is the whole point of the table
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
// The old name is only a hash: the text never reaches the firmware
#define CS_CFG(key, old, m, type, def) \
    {key, configKeyHash(old, sizeof(old) - 1), (uint16_t)offsetof(Config, m), type, (uint8_t)sizeof(Config::m), def}
#define CS_IOT(key, old, m, type, def) \
    {key, configKeyHash(old, sizeof(old) - 1), (uint16_t)(offsetof(Config, iot) + offsetof(IoTConfig, m)), type, \
     (uint8_t)sizeof(IoTConfig::m), def}
#define CS_V1(old, m, type) {"", configKeyHash(old, sizeof(old) - 1), (uint16_t)offsetof(Load, m), type, (uint8_t)sizeof(Load::m), 0}
#define CS_V1IOT(old, m, type) \
    {"", configKeyHash(old, sizeof(old) - 1), (uint16_t)(offsetof(Load, v1) + offsetof(IoTConfig, m)), type, \
     (uint8_t)sizeof(IoTConfig::m), 0}

    // In the order they are written
    static constexpr Field FIELDS[] PROGMEM = {
        CS_CFG("usr", "webusr", webusr, T_STRING, 0), CS_CFG("pwd", "webpss", webpss, T_STRING, 0),
        CS_CFG("otp", "totp", totp, T_HEX20, 0), CS_CFG("ots", "totp_step", totpStep, T_U32, 0),
        CS_CFG("otb", "totp_bstep", totpBrowserStep, T_U32, 0),
        CS_CFG("sid", "ssid", ssid, T_STRING, 0), CS_CFG("wpw", "pass", pass, T_STRING, 0),
        CS_CFG("ntt", "ntfy_topic", ntfy_topic, T_CHARS, 0), CS_CFG("ntc", "ntfy_cmd", ntfy_cmd, T_CHARS, 0),
        CS_CFG("ntk", "ntfy_token", ntfy_token, T_CHARS, 0), CS_CFG("ntp", "ntfy_poll", ntfy_poll, T_U8, 5),
        CS_CFG("nth", "ntfy_host", ntfy_host, T_CHARS, 0),
        CS_CFG("mqh", "mqtt_host", mqtt_host, T_CHARS, 0),
        CS_CFG("mqu", "mqtt_user", mqtt_user, T_CHARS, 0), CS_CFG("mqp", "mqtt_pass", mqtt_pass, T_CHARS, 0),
        CS_CFG("nam", "name", dev_name, T_CHARS, 0), CS_CFG("hnm", "hostname", hostname, T_CHARS, 0),
        CS_CFG("hst", "hosts", web_hosts, T_CHARS, 0),
        CS_CFG("tzo", "tz", tz, T_I16, 0), CS_CFG("bat", "bat_sda", bat_sda, T_I8, -1),
        CS_CFG("lng", "lang", lang, T_CHARS, 0), CS_CFG("ota", "ota_auto", ota_auto, T_BOOL, 1),
        CS_CFG("api", "api_token", api_token, T_CHARS, 0), CS_CFG("cft", "cf_token", cf_token, T_CHARS, 0),
        CS_CFG("cfz", "cf_zone", cf_zone, T_CHARS, 0), CS_CFG("cfh", "cf_host", cf_host, T_CHARS, 0),
        CS_CFG("cfr", "cf_record", cf_record, T_CHARS, 0), CS_CFG("cfa", "cf_account", cf_account, T_CHARS, 0),
        CS_CFG("rlh", "relay_host", relay_host, T_CHARS, 0), CS_CFG("rlk", "relay_key", relay_secret, T_CHARS, 0),
        CS_CFG("rlv", "relay_ver", relay_ver, T_U8, 0), CS_CFG("rln", "relay_name", relay_name, T_CHARS, 0),
        CS_CFG("rlz", "relay_zone", relay_zone, T_CHARS, 0),
        CS_IOT("sen", "sensor", sensor, T_CHARS, 0), CS_IOT("spn", "sensor_pin", sensorPin, T_I8, -1),
        CS_IOT("shi", "sensor_high", sensorHigh, T_BOOL, 1), CS_IOT("son", "sensor_on", onText, T_CHARS, 0),
        CS_IOT("sof", "sensor_off", offText, T_CHARS, 0), CS_IOT("met", "metric", metric, T_U8, 0),
        CS_IOT("unt", "unit", unit, T_U8, 0), CS_IOT("tke", "tank_empty", tankEmpty, T_U16, 100),
        CS_IOT("tkf", "tank_full", tankFull, T_U16, 20), CS_IOT("tkl", "tank_l", tankL, T_U16, 1000),
        CS_IOT("fpl", "ppl", ppl, T_U16, 450), CS_IOT("pls", "pulse", pulse, T_U16, 0),
        CS_IOT("ntf", "notify", notify, T_BOOL, 0), CS_IOT("act", "actuator", actuator, T_CHARS, 0),
        CS_IOT("apn", "actuator_pin", actuatorPin, T_I8, -1), CS_IOT("ahi", "actuator_high", actuatorHigh, T_BOOL, 0),
        CS_IOT("ag1", "angle_on", angleOn, T_U8, 90), CS_IOT("ag0", "angle_off", angleOff, T_U8, 0),
        CS_IOT("dt1", "duty_on", dutyOn, T_U8, 100), CS_IOT("dt0", "duty_off", dutyOff, T_U8, 0),
        CS_IOT("led", "leds", leds, T_U8, 8), CS_IOT("mxo", "max_on", maxOn, T_U16, 0),
        CS_IOT("rst", "restore", restore, T_BOOL, 0), CS_IOT("rul", "rule", rule, T_BOOL, 1),
        CS_IOT("abv", "above", above, T_BOOL, 0), CS_IOT("thd", "threshold", threshold, T_I16, 0),
        CS_IOT("mrg", "margin", margin, T_U8, 0), CS_IOT("tgl", "toggle", toggle, T_BOOL, 0),
        CS_IOT("lin", "linear", linear, T_BOOL, 0), CS_IOT("thh", "threshold_hi", thresholdHi, T_I16, 0),
        CS_IOT("win", "window", window, T_BOOL, 0), CS_IOT("wns", "win_start", winStart, T_U16, 0),
        CS_IOT("wne", "win_end", winEnd, T_U16, 0),
    };
    static constexpr size_t FIELD_COUNT = sizeof(FIELDS) / sizeof(FIELDS[0]);

    // v1 fields, into Load (defaults: those of Load and IoTConfig)
    static constexpr Field V1_FIELDS[] PROGMEM = {
        CS_V1("iot_sensor", sensor, T_I8), CS_V1("iot_module", module, T_I8),
        CS_V1("iot_sensor_type", sensorType, T_U8), CS_V1("iot_module_type", moduleType, T_U8),
        CS_V1("iot_manual", manual, T_U8),
        CS_V1IOT("iot_sensor_high", sensorHigh, T_BOOL), CS_V1IOT("iot_module_high", actuatorHigh, T_BOOL),
        CS_V1IOT("iot_notify", notify, T_BOOL), CS_V1IOT("iot_metric", metric, T_U8),
        CS_V1IOT("iot_above", above, T_BOOL), CS_V1IOT("iot_threshold", threshold, T_I16),
        CS_V1IOT("iot_margin", margin, T_U8), CS_V1IOT("iot_angle_on", angleOn, T_U8),
        CS_V1IOT("iot_angle_off", angleOff, T_U8), CS_V1IOT("iot_duty_on", dutyOn, T_U8),
        CS_V1IOT("iot_duty_off", dutyOff, T_U8), CS_V1IOT("iot_toggle", toggle, T_BOOL),
        CS_V1IOT("iot_linear", linear, T_BOOL), CS_V1IOT("iot_threshold_hi", thresholdHi, T_I16),
    };
    static constexpr size_t V1_COUNT = sizeof(V1_FIELDS) / sizeof(V1_FIELDS[0]);
    static constexpr uint16_t RECORD_OFF = (uint16_t)offsetof(Config, cf_record);
    static constexpr uint16_t RECORDS = configKeyHash("cf_records", 10);   // a list, until version 1
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
            *reinterpret_cast<String*>(p) = f.off == offsetof(Config, webusr) ? "admin" : "";
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

    // By its three letters or by the hash of its old name; no two of these
    // are the same (checked when it compiles, in _unique)
    static bool _find(const Field* table, size_t count, const char* key, size_t len, uint16_t h, Field& f) {
        for (size_t i = 0; i < count; i++) {
            f = _field(table, i);
            if ((len == 3 && memcmp(key, f.key, 3) == 0) || h == f.old) return true;
        }
        return false;
    }


    static void _member(void* c, const char* key, size_t len, const json::Value& v) {
        Load& ctx = *static_cast<Load*>(c);
        Field f;
        uint16_t h = configKeyHash(key, len);
        if (len == 1 && key[0] == 'v') {
            uint32_t n;
            if (v.fits(-2147483647L, 0x7FFFFFFFu, n)) ctx.version = (long)(int32_t)n;
        } else if (h == RECORDS) {
            // There used to be up to 5 IDs: the first one, unless cf_record is text
            json::Value first = v.kind == json::ARRAY ? json::first(v) : json::Value();
            if (!ctx.record && first.kind == json::STRING)
                json::copy(first, ctx.self->_cfg.cf_record, sizeof(Config::cf_record));
        } else if (_find(FIELDS, FIELD_COUNT, key, len, h, f)) {
            _set(ctx.self->_at(f), f, v);
            if (f.off == RECORD_OFF && v.kind == json::STRING) ctx.record = true;
        } else if (_find(V1_FIELDS, V1_COUNT, key, len, h, f)) {
            _set(reinterpret_cast<uint8_t*>(&ctx) + f.off, f, v);
        }
    }

    size_t _write(json::Writer& w) {
        w.num(PSTR("v"), VERSION);
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

public:
    // Every key, in both forms, and cf_records: no two the same, or one would
    // be taken for the other (checked when it compiles, below)
    static constexpr bool keysDistinct() {
        uint16_t all[2 * FIELD_COUNT + V1_COUNT + 1] = {RECORDS};
        size_t n = 1;
        for (const Field& f : FIELDS) {
            all[n++] = f.old;
            all[n++] = configKeyHash(f.key, 3);
        }
        for (const Field& f : V1_FIELDS) all[n++] = f.old;
        return distinct(all, n);
    }
};
static_assert(ConfigStore::keysDistinct(), "two config keys are taken for each other");
