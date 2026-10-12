// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "Config.h"
#include "Logger.h"
#include "IoTCatalog.h"
#include "IoTState.h"

// /config.json com versão ("v"). Ao carregar, uma versão antiga passa
// pelas migrações em sequência até a atual, o arquivo original fica em
// /config.v1.json e o novo é gravado. Campo que falta recebe o padrão;
// campo desconhecido é ignorado. Arquivo de versão mais nova (OTA que
// voltou atrás) é lido no que se conhece e não é regravado sozinho: um
// identificador de tipo desconhecido fica guardado como está.
class ConfigStore {
public:
    ConfigStore(Config& cfg, Logger& log) : _cfg(cfg), _log(log) {}

    static constexpr int    VERSION       = 2;
    // Folga para o pior caso (todos os campos no tamanho máximo)
    static constexpr size_t JSON_CAPACITY = 3072;

    bool begin() {
        if (!LittleFS.begin()) {
            _log.log(PSTR("Falha ao montar LittleFS"));
            return false;
        }
        _log.log(PSTR("LittleFS inicializado"));
        return true;
    }

    bool load() {
        if (!LittleFS.exists(PATH)) {
            _log.log(PSTR("config.json nao encontrado"));
            return false;
        }

        File f = LittleFS.open(PATH, "r");
        if (!f) {
            _log.log(PSTR("Erro ao abrir config.json"));
            return false;
        }

        DynamicJsonDocument doc(JSON_CAPACITY);
        DeserializationError err = deserializeJson(doc, f);
        f.close();

        if (err) {
            _log.log(PSTR("Erro JSON: %s"), err.c_str());
            return false;
        }

        _cfg.webusr = doc["webusr"] | "admin";
        _cfg.webpss = doc["webpss"] | "";
        _cfg.ssid   = doc["ssid"]   | "";
        _cfg.pass   = doc["pass"]   | "";

        strlcpy(_cfg.ntfy_topic, doc["ntfy_topic"] | "", sizeof(_cfg.ntfy_topic));
        strlcpy(_cfg.ntfy_cmd,   doc["ntfy_cmd"]   | "", sizeof(_cfg.ntfy_cmd));
        strlcpy(_cfg.ntfy_token, doc["ntfy_token"] | "", sizeof(_cfg.ntfy_token));
        _cfg.ota_auto = doc["ota_auto"] | true;
        strlcpy(_cfg.api_token,  doc["api_token"]  | "", sizeof(_cfg.api_token));
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

        int version = doc["v"] | 1;
        if (version == 1) {
            _migrateV1(doc);
            _log.log(PSTR("Configuracao migrada para v%d"), VERSION);
            _backup();
            save();
            return true;
        }
        _loadIoT(doc);
        if (version > VERSION) _log.log(PSTR("Configuracao v%d, mais nova que o firmware"), version);
        _log.log(PSTR("Configuracao carregada"));
        return true;
    }

    // Escrita atômica: grava em arquivo temporário e renomeia por cima.
    // Uma queda de energia no meio da escrita preserva o config anterior.
    bool save() {
        DynamicJsonDocument doc(JSON_CAPACITY);

        doc["v"]          = VERSION;
        doc["webusr"]     = _cfg.webusr;
        doc["webpss"]     = _cfg.webpss;
        doc["ssid"]       = _cfg.ssid;
        doc["pass"]       = _cfg.pass;
        doc["ntfy_topic"] = _cfg.ntfy_topic;
        doc["ntfy_cmd"]   = _cfg.ntfy_cmd;
        doc["ntfy_token"] = _cfg.ntfy_token;
        doc["ota_auto"]   = _cfg.ota_auto;
        doc["api_token"]  = _cfg.api_token;
        doc["cf_token"]   = _cfg.cf_token;
        doc["cf_zone"]    = _cfg.cf_zone;
        doc["cf_host"]    = _cfg.cf_host;

        JsonArray arr = doc.createNestedArray("cf_records");
        for (uint8_t i = 0; i < _cfg.cf_record_count; i++) {
            arr.add(_cfg.cf_records[i]);
        }

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

        if (doc.overflowed()) {
            _log.log(PSTR("Config excede capacidade JSON"));
            return false;
        }

        File f = LittleFS.open(TMP_PATH, "w");
        if (!f) {
            _log.log(PSTR("Erro ao salvar config"));
            return false;
        }
        serializeJson(doc, f);
        f.close();

        if (!LittleFS.rename(TMP_PATH, PATH)) {
            _log.log(PSTR("Erro ao substituir config.json"));
            LittleFS.remove(TMP_PATH);
            return false;
        }

        _log.log(PSTR("Configuracao salva"));
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
    }

    // v1 gravava o tipo pela posição na tabela antiga e só acionava o
    // módulo com sensor. Sem sensor, o módulo fica desligado também aqui,
    // senão o pino passaria a ser acionado depois da atualização.
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
        // "Comando" fixo (1 ligado, 2 desligado) vira o estado manual guardado
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

    // Cópia do arquivo original antes da primeira gravação no formato novo
    void _backup() {
        File in = LittleFS.open(PATH, "r");  // acabou de ser lido
        File out = LittleFS.open(BACKUP_PATH, "w");
        if (!out) return;
        char buf[64];
        size_t n;
        while ((n = in.readBytes(buf, sizeof(buf))) > 0) out.write((const uint8_t*)buf, n);
        in.close();
        out.close();
    }
};
