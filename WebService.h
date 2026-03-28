#pragma once
#include <ESP8266WebServer.h>
#include "Config.h"
#include "Logger.h"
#include "ConfigStore.h"
#include "WiFiService.h"
#include "CloudflareDNS.h"
#include "OTAService.h"

// HTML em PROGMEM
static const char _HTML_HEAD[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP8266</title>
<link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.2/dist/css/bootstrap.min.css" rel="stylesheet">
</head>
<body class="bg-dark text-light">
<div class="container py-3">
)rawliteral";

static const char _HTML_FOOT_A[] PROGMEM = R"rawliteral(
<hr class="border-secondary">
<div class="text-secondary small">Firmware: )rawliteral";

static const char _HTML_FOOT_B[] PROGMEM = R"rawliteral(</div>
</div></body></html>
)rawliteral";

class WebService {
public:
    WebService(Config& cfg, Logger& log, ConfigStore& store,
               WiFiService& wifi, CloudflareDNS& dns, OTAService& ota)
        : _server(80), _cfg(cfg), _log(log), _store(store),
          _wifi(wifi), _dns(dns), _ota(ota) {
        _instance = this;
    }

    void begin() {
        _server.on("/",                HTTP_GET,  _hRoot);
        _server.on("/save",            HTTP_POST, _hSave);
        _server.on("/log",             HTTP_GET,  _hLog);
        _server.on("/status",          HTTP_GET,  _hStatus);
        _server.on("/cloudflare",      HTTP_GET,  _hCloudflare);
        _server.on("/cloudflare/save", HTTP_POST, _hCloudflareSave);
        _server.on("/password",        HTTP_GET,  _hPassword);
        _server.on("/password/save",   HTTP_POST, _hPasswordSave);
        _server.begin();
        _log.log("Servidor web iniciado na porta 80");
    }

    void tick() { _server.handleClient(); }

private:
    ESP8266WebServer _server;
    Config&          _cfg;
    Logger&          _log;
    ConfigStore&     _store;
    WiFiService&     _wifi;
    CloudflareDNS&   _dns;
    OTAService&      _ota;

    static WebService* _instance;

    // ---- Autenticação ----
    bool _checkAuth() {
        if (_cfg.webpss.length() == 0) return true;
        if (!_server.authenticate(_cfg.webusr.c_str(), _cfg.webpss.c_str())) {
            _server.requestAuthentication();
            return false;
        }
        return true;
    }

    // ---- Helpers HTML ----
    void _pageBegin() {
        _server.setContentLength(CONTENT_LENGTH_UNKNOWN);
        _server.send(200, "text/html", "");
        _server.sendContent_P(_HTML_HEAD);
    }

    void _pageEnd() {
        _server.sendContent_P(_HTML_FOOT_A);
        _server.sendContent(firmware_version);
        _server.sendContent_P(_HTML_FOOT_B);
    }

    void _box(const char* title) {
        _server.sendContent("<div class='card bg-black border-success mb-3'>"
                            "<div class='card-body text-white'>"
                            "<h6 class='card-title text-success'>");
        _server.sendContent(title);
        _server.sendContent("</h6>");
    }

    void _boxEnd() { _server.sendContent("</div></div>"); }

    // ---- Utilitários ----
    static String _uptime() {
        unsigned long s = millis() / 1000;
        char buf[20];
        snprintf(buf, sizeof(buf), "%luh:%02lum:%02lus", s/3600, (s/60)%60, s%60);
        return String(buf);
    }

    static String _elapsed(unsigned long ms) {
        unsigned long s = ms / 1000;
        char buf[24];
        snprintf(buf, sizeof(buf), "%luh:%02lum:%02lus", s/3600, (s/60)%60, s%60);
        return String(buf);
    }

    static float _chipTemp() {
        float v = analogRead(A0) * (3.3f / 1023.0f);
        return v * 540.0f;
    }

    // ---- Handlers (estáticos → encaminham para _instance) ----
    static void _hRoot()           { _instance->_handleRoot(); }
    static void _hSave()           { _instance->_handleSave(); }
    static void _hLog()            { _instance->_handleLog(); }
    static void _hStatus()         { _instance->_handleStatus(); }
    static void _hCloudflare()     { _instance->_handleCloudflare(); }
    static void _hCloudflareSave() { _instance->_handleCloudflareSave(); }
    static void _hPassword()       { _instance->_handlePassword(); }
    static void _hPasswordSave()   { _instance->_handlePasswordSave(); }

    // ---- Implementações ----
    void _handleRoot() {
        _pageBegin();

        if (WiFi.getMode() == WIFI_AP) {
            _server.sendContent("<h4 class='text-warning'>Configurar Wi-Fi</h4>"
                                "<form action='/save' method='POST'>"
                                "<div class='mb-3'><label class='form-label'>SSID</label>"
                                "<input name='ssid' class='form-control' required></div>"
                                "<div class='mb-3'><label class='form-label'>Senha</label>"
                                "<input name='pass' type='password' class='form-control'></div>"
                                "<button class='btn btn-success w-100'>Salvar</button>"
                                "</form>");
        } else {
            _server.sendContent("<h4 class='text-success'>Wi-Fi Conectado</h4>");
            _box("Conexao Wi-Fi");
            _server.sendContent("Rede: " + WiFi.SSID() + "<br>IP: " + WiFi.localIP().toString());
            _boxEnd();
        }

        _pageEnd();
    }

    void _handleSave() {
        _cfg.ssid = _server.arg("ssid");
        _cfg.pass = _server.arg("pass");
        _store.save();
        _pageBegin();
        _server.sendContent("<h2>Configuracao salva!</h2><p>Reiniciando...</p>");
        _pageEnd();
        _wifi.scheduleRestart(1000);
    }

    void _handleLog() {
        _pageBegin();
        _server.sendContent("<h4 class='text-warning'>Logs</h4>"
                            "<pre class='bg-black text-success p-3 rounded'>");

        uint8_t cnt   = _log.count();
        uint8_t start = _log.startIndex();
        for (uint8_t i = 0; i < cnt; i++) {
            _server.sendContent(_log.entry((start + i) % LOG_BUFFER_SIZE));
            _server.sendContent("\n");
        }

        _server.sendContent("</pre>");
        _pageEnd();
    }

    void _handleStatus() {
        _pageBegin();
        _server.sendContent("<h4 class='text-info'>Status do Sistema</h4>");

        _box("Sistema");
        _server.sendContent("Uptime: " + _uptime() + "<br>");
        _server.sendContent("Reset: " + String(ESP.getResetReason()) + "<br>");
        _server.sendContent("Chip ID: " + String(ESP.getChipId()) + "<br>");
        _server.sendContent("CPU Freq: " + String(ESP.getCpuFreqMHz()) + " MHz<br>");
        _server.sendContent("SDK: " + String(ESP.getSdkVersion()) + "<br>");
        _server.sendContent("Temp estimada: " + String(_chipTemp(), 1) + " C<br>");
        _boxEnd();

        _box("Memoria");
        _server.sendContent("Heap livre: " + String(ESP.getFreeHeap()) + " bytes<br>");
        _server.sendContent("Maior bloco: " + String(ESP.getMaxFreeBlockSize()) + " bytes<br>");
        _server.sendContent("Fragmentacao: " + String(ESP.getHeapFragmentation()) + "%<br>");
        _boxEnd();

        _box("Flash");
        _server.sendContent("Total: " + String(ESP.getFlashChipSize()) + " bytes<br>");
        _server.sendContent("Sketch: " + String(ESP.getSketchSize()) + " bytes<br>");
        _server.sendContent("Livre: " + String(ESP.getFreeSketchSpace()) + " bytes<br>");
        _boxEnd();

        _box("LittleFS");
        FSInfo fs;
        LittleFS.info(fs);
        _server.sendContent("Total: " + String(fs.totalBytes) + " bytes<br>");
        _server.sendContent("Usado: " + String(fs.usedBytes) + " bytes<br>");
        Dir dir = LittleFS.openDir("/");
        int fc = 0; while (dir.next()) fc++;
        _server.sendContent("Arquivos: " + String(fc) + "<br>");
        _boxEnd();

        _box("Wi-Fi");
        bool con = WiFi.isConnected();
        _server.sendContent("SSID: "    + (con ? WiFi.SSID()               : String("Desconectado")) + "<br>");
        _server.sendContent("IP: "      + (con ? WiFi.localIP().toString()  : String("-")) + "<br>");
        _server.sendContent("Gateway: " + (con ? WiFi.gatewayIP().toString(): String("-")) + "<br>");
        _server.sendContent("Mascara: " + (con ? WiFi.subnetMask().toString(): String("-")) + "<br>");
        _server.sendContent("DNS: "     + (con ? WiFi.dnsIP().toString()    : String("-")) + "<br>");
        _server.sendContent("MAC: "     + WiFi.macAddress() + "<br>");
        _server.sendContent("RSSI: "    + String(WiFi.RSSI()) + " dBm<br>");
        _boxEnd();

        _box("DNS");
        _server.sendContent("IP Publico: ");
        _server.sendContent(_cfg.publicIP);
        _server.sendContent("<br>");
        if (_cfg.lastDnsUpdate == 0) {
            _server.sendContent("Ultima atualizacao: nunca<br>");
        } else {
            _server.sendContent("Ultima atualizacao: " +
                                _elapsed(millis() - _cfg.lastDnsUpdate) + " atras<br>");
        }
        _boxEnd();

        _box("OTA");
        _server.sendContent("Em progresso: " + String(_ota.inProgress() ? "Sim" : "Nao") + "<br>");
        _server.sendContent("Bytes: " + String(_ota.bytesWritten()) + " / " +
                            String(_ota.contentLength()) + "<br>");
        _boxEnd();

        _pageEnd();
    }

    void _handleCloudflare() {
        if (!_checkAuth()) return;
        _pageBegin();
        _server.sendContent("<h4 class='text-info'>Cloudflare</h4>");

        if (strlen(_cfg.cf_token) > 0)
            _server.sendContent("<div class='alert alert-success'>Token configurado</div>");
        else
            _server.sendContent("<div class='alert alert-warning'>Token nao configurado</div>");

        _server.sendContent("<form action='/cloudflare/save' method='POST'>");
        _server.sendContent("<input class='form-control mb-2' name='token' placeholder='API Token' value='" +
                            String(_cfg.cf_token) + "'>");
        _server.sendContent("<input class='form-control mb-2' name='zone' placeholder='Zone ID' value='" +
                            String(_cfg.cf_zone) + "'>");
        _server.sendContent("<input class='form-control mb-2' name='host' placeholder='Hostname' value='" +
                            String(_cfg.cf_host) + "'>");
        _server.sendContent("<label>Record IDs:</label>");
        for (int i = 0; i < MAX_RECORDS; i++) {
            String val = (i < _cfg.cf_record_count) ? String(_cfg.cf_records[i]) : "";
            _server.sendContent("<input class='form-control mb-2' name='record" + String(i) +
                                "' placeholder='Record ID " + String(i+1) + "' value='" + val + "'>");
        }
        _server.sendContent("<button class='btn btn-success w-100'>Salvar</button></form>");
        _pageEnd();
    }

    void _handleCloudflareSave() {
        strlcpy(_cfg.cf_token, _server.arg("token").c_str(), sizeof(_cfg.cf_token));
        strlcpy(_cfg.cf_zone,  _server.arg("zone").c_str(),  sizeof(_cfg.cf_zone));
        strlcpy(_cfg.cf_host,  _server.arg("host").c_str(),  sizeof(_cfg.cf_host));
        _cfg.cf_record_count = 0;
        for (uint8_t i = 0; i < MAX_RECORDS; i++) {
            String key = "record" + String(i);
            if (_server.hasArg(key) && _server.arg(key).length() > 0) {
                strlcpy(_cfg.cf_records[_cfg.cf_record_count++],
                        _server.arg(key).c_str(), sizeof(_cfg.cf_records[0]));
            }
        }
        _store.save();
        _pageBegin();
        _server.sendContent("<h2>Cloudflare configurado!</h2><a href='/cloudflare'>Voltar</a>");
        _pageEnd();
    }

    void _handlePassword() {
        _pageBegin();
        _server.sendContent("<h4 class='text-info'>Senha do Painel</h4>"
                            "<form action='/password/save' method='POST'>"
                            "<input class='form-control mb-2' name='user' placeholder='Usuario' value='" +
                            _cfg.webusr + "'>"
                            "<input type='password' class='form-control mb-2' name='pass1' placeholder='Nova senha'>"
                            "<input type='password' class='form-control mb-2' name='pass2' placeholder='Confirmar senha'>"
                            "<button class='btn btn-primary w-100'>Salvar senha</button></form>");
        _pageEnd();
    }

    void _handlePasswordSave() {
        if (!_server.hasArg("pass1") || !_server.hasArg("pass2")) {
            _server.send(400, "text/plain", "Erro");
            return;
        }
        if (_server.arg("pass1") != _server.arg("pass2")) {
            _pageBegin();
            _server.sendContent("<div class='alert alert-danger'>As senhas nao coincidem</div>");
            _pageEnd();
            return;
        }
        _cfg.webusr = _server.arg("user");
        _cfg.webpss = _server.arg("pass1");
        _store.save();
        _server.sendHeader("Location", "/");
        _server.send(303);
    }
};

WebService* WebService::_instance = nullptr;
