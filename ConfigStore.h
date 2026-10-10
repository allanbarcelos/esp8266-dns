#pragma once
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "Config.h"
#include "Logger.h"

class ConfigStore {
public:
    ConfigStore(Config& cfg, Logger& log) : _cfg(cfg), _log(log) {}

    // Capacidade folgada para o pior caso (todos os campos no tamanho máximo)
    static constexpr size_t JSON_CAPACITY = 1536;

    bool begin() {
        if (!LittleFS.begin()) {
            _log.log("Falha ao montar LittleFS");
            return false;
        }
        _log.log("LittleFS inicializado");
        return true;
    }

    bool load() {
        if (!LittleFS.exists(PATH)) {
            _log.log("config.json nao encontrado");
            return false;
        }

        File f = LittleFS.open(PATH, "r");
        if (!f) {
            _log.log("Erro ao abrir config.json");
            return false;
        }

        DynamicJsonDocument doc(JSON_CAPACITY);
        DeserializationError err = deserializeJson(doc, f);
        f.close();

        if (err) {
            _log.log("Erro JSON: %s", err.c_str());
            return false;
        }

        _cfg.webusr = doc["webusr"] | "admin";
        _cfg.webpss = doc["webpss"] | "";
        _cfg.ssid   = doc["ssid"]   | "";
        _cfg.pass   = doc["pass"]   | "";

        strlcpy(_cfg.ntfy_topic, doc["ntfy_topic"] | "", sizeof(_cfg.ntfy_topic));
        _cfg.ota_auto = doc["ota_auto"] | true;
        strlcpy(_cfg.cf_token,   doc["cf_token"]   | "", sizeof(_cfg.cf_token));
        strlcpy(_cfg.cf_zone,    doc["cf_zone"]    | "", sizeof(_cfg.cf_zone));
        strlcpy(_cfg.cf_host,    doc["cf_host"]    | "", sizeof(_cfg.cf_host));

        uint8_t count = 0;
        for (JsonVariantConst item : doc["cf_records"].as<JsonArrayConst>()) {
            if (count == MAX_RECORDS) break;
            const char* rec = item.as<const char*>(); // nullptr se não for texto
            strlcpy(_cfg.cf_records[count++], rec ? rec : "", sizeof(_cfg.cf_records[0]));
        }
        _cfg.cf_record_count = count;

        _log.log("Configuracao carregada");
        return true;
    }

    // Escrita atômica: grava em arquivo temporário e renomeia por cima.
    // Uma queda de energia no meio da escrita preserva o config anterior.
    bool save() {
        DynamicJsonDocument doc(JSON_CAPACITY);

        doc["webusr"]     = _cfg.webusr;
        doc["webpss"]     = _cfg.webpss;
        doc["ssid"]       = _cfg.ssid;
        doc["pass"]       = _cfg.pass;
        doc["ntfy_topic"] = _cfg.ntfy_topic;
        doc["ota_auto"]   = _cfg.ota_auto;
        doc["cf_token"]   = _cfg.cf_token;
        doc["cf_zone"]    = _cfg.cf_zone;
        doc["cf_host"]    = _cfg.cf_host;

        JsonArray arr = doc.createNestedArray("cf_records");
        for (uint8_t i = 0; i < _cfg.cf_record_count; i++) {
            arr.add(_cfg.cf_records[i]);
        }

        if (doc.overflowed()) {
            _log.log("Config excede capacidade JSON");
            return false;
        }

        File f = LittleFS.open(TMP_PATH, "w");
        if (!f) {
            _log.log("Erro ao salvar config");
            return false;
        }
        serializeJson(doc, f);
        f.close();

        if (!LittleFS.rename(TMP_PATH, PATH)) {
            _log.log("Erro ao substituir config.json");
            LittleFS.remove(TMP_PATH);
            return false;
        }

        _log.log("Configuracao salva");
        return true;
    }

private:
    Config& _cfg;
    Logger& _log;

    static constexpr const char* PATH     = "/config.json";
    static constexpr const char* TMP_PATH = "/config.tmp";
};
