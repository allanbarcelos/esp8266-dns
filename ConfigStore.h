#pragma once
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "Config.h"
#include "Logger.h"

class ConfigStore {
public:
    ConfigStore(Config& cfg, Logger& log) : _cfg(cfg), _log(log) {}

    bool begin() {
        if (!LittleFS.begin()) {
            _log.log("Falha ao montar LittleFS");
            return false;
        }
        _log.log("LittleFS inicializado");
        return true;
    }

    bool load() {
        if (!LittleFS.exists("/config.json")) {
            _log.log("config.json nao encontrado");
            return false;
        }

        File f = LittleFS.open("/config.json", "r");
        if (!f) {
            _log.log("Erro ao abrir config.json");
            return false;
        }

        StaticJsonDocument<640> doc;
        DeserializationError err = deserializeJson(doc, f);
        f.close();

        if (err) {
            _log.log("Erro JSON: %s", err.c_str());
            return false;
        }

        _cfg.webusr = doc["webusr"] | "admin";
        _cfg.webpss = doc["webpss"] | "";
        _cfg.ssid   = doc["ssid"].as<String>();
        _cfg.pass   = doc["pass"].as<String>();

        strlcpy(_cfg.ntfy_topic, doc["ntfy_topic"] | "", sizeof(_cfg.ntfy_topic));
        strlcpy(_cfg.cf_token, doc["cf_token"] | "", sizeof(_cfg.cf_token));
        strlcpy(_cfg.cf_zone,  doc["cf_zone"]  | "", sizeof(_cfg.cf_zone));
        strlcpy(_cfg.cf_host,  doc["cf_host"]  | "", sizeof(_cfg.cf_host));

        _cfg.cf_record_count = doc["cf_records"].size();
        for (uint8_t i = 0; i < _cfg.cf_record_count && i < MAX_RECORDS; i++) {
            strlcpy(_cfg.cf_records[i], doc["cf_records"][i] | "", sizeof(_cfg.cf_records[0]));
        }

        _log.log("Configuracao carregada");
        return true;
    }

    bool save() {
        StaticJsonDocument<640> doc;

        doc["webusr"]   = _cfg.webusr;
        doc["webpss"]   = _cfg.webpss;
        doc["ssid"]     = _cfg.ssid;
        doc["pass"]     = _cfg.pass;
        doc["ntfy_topic"] = _cfg.ntfy_topic;
        doc["cf_token"] = _cfg.cf_token;
        doc["cf_zone"]  = _cfg.cf_zone;
        doc["cf_host"]  = _cfg.cf_host;

        JsonArray arr = doc.createNestedArray("cf_records");
        for (uint8_t i = 0; i < _cfg.cf_record_count; i++) {
            arr.add(_cfg.cf_records[i]);
        }

        File f = LittleFS.open("/config.json", "w");
        if (!f) {
            _log.log("Erro ao salvar config");
            return false;
        }

        serializeJson(doc, f);
        f.close();
        _log.log("Configuracao salva");
        return true;
    }

private:
    Config& _cfg;
    Logger& _log;
};
