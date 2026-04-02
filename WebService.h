#pragma once
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include "Config.h"
#include "Logger.h"
#include "ConfigStore.h"
#include "WiFiService.h"
#include "CloudflareDNS.h"
#include "OTAService.h"
#include "NtfyNotifier.h"

// ── PROGMEM ───────────────────────────────────────────────────────────────────
// Cabeçalho dividido para permitir injeção de <meta refresh> antes de </head>

static const char _HTML_HEAD_A[] PROGMEM = R"(<!DOCTYPE html>
<html lang="pt"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP8266 DNS</title>
<link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.2/dist/css/bootstrap.min.css" rel="stylesheet">
)";

static const char _HTML_HEAD_B[] PROGMEM = R"(</head>
<body class="bg-dark text-light">
<div class="container py-3">
)";

static const char _HTML_NAV[] PROGMEM = R"(
<nav class="navbar navbar-dark bg-black mb-3 rounded px-3 py-2">
  <span class="navbar-brand text-success fw-bold mb-0">&#9679; ESP8266 DNS</span>
  <div class="d-flex gap-2 flex-wrap">
    <a href="/"          class="btn btn-sm btn-outline-light">Home</a>
    <a href="/status"    class="btn btn-sm btn-outline-info">Status</a>
    <a href="/log"       class="btn btn-sm btn-outline-warning">Logs</a>
    <a href="/cloudflare"class="btn btn-sm btn-outline-primary">Cloudflare</a>
    <a href="/ntfy"      class="btn btn-sm btn-outline-success">Ntfy</a>
    <a href="/password"  class="btn btn-sm btn-outline-secondary">Senha</a>
  </div>
</nav>
)";

static const char _HTML_FOOT_A[] PROGMEM = R"(<hr class="border-secondary mt-4">
<p class="text-secondary small mb-0">Firmware: )";

static const char _HTML_FOOT_B[] PROGMEM = R"(</p></div></body></html>)";

// ─────────────────────────────────────────────────────────────────────────────

class WebService {
public:
    WebService(Config& cfg, Logger& log, ConfigStore& store,
               WiFiService& wifi, CloudflareDNS& dns, OTAService& ota,
               NtfyNotifier& ntfy)
        : _server(80), _cfg(cfg), _log(log), _store(store),
          _wifi(wifi), _dns(dns), _ota(ota), _ntfy(ntfy)
    {
        _instance = this;
        _generateCSRF();
    }

    void begin() {
        _server.on("/",                HTTP_GET,  _hRoot);
        _server.on("/save",            HTTP_POST, _hSave);
        _server.on("/log",             HTTP_GET,  _hLog);
        _server.on("/status",          HTTP_GET,  _hStatus);
        _server.on("/cloudflare",      HTTP_GET,  _hCloudflare);
        _server.on("/cloudflare/save", HTTP_POST, _hCloudflareSave);
        _server.on("/ntfy",            HTTP_GET,  _hNtfy);
        _server.on("/ntfy/save",       HTTP_POST, _hNtfySave);
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
    NtfyNotifier&    _ntfy;

    char _csrfToken[9]; // 8 hex + null

    static WebService* _instance;

    // ── Segurança ─────────────────────────────────────────────────────────────

    void _generateCSRF() {
        uint32_t seed = ESP.getChipId() ^ (uint32_t)(millis() ^ 0xDEADBEEFUL);
        snprintf(_csrfToken, sizeof(_csrfToken), "%08lx", (unsigned long)seed);
    }

    bool _checkAuth() {
        if (_cfg.webpss.length() == 0) return true;
        if (!_server.authenticate(_cfg.webusr.c_str(), _cfg.webpss.c_str())) {
            _server.requestAuthentication();
            return false;
        }
        return true;
    }

    bool _checkCSRF() {
        if (_server.arg("_csrf") != String(_csrfToken)) {
            _server.send(403, "text/plain", "Token CSRF invalido");
            return false;
        }
        return true;
    }

    // ── Helpers HTML ──────────────────────────────────────────────────────────

    void _pageBegin(bool autoRefresh = false) {
        _server.setContentLength(CONTENT_LENGTH_UNKNOWN);
        _server.send(200, "text/html", "");
        _server.sendContent_P(_HTML_HEAD_A);
        if (autoRefresh)
            _server.sendContent("<meta http-equiv='refresh' content='30'>");
        _server.sendContent_P(_HTML_HEAD_B);
        _server.sendContent_P(_HTML_NAV);
    }

    void _pageEnd() {
        _server.sendContent_P(_HTML_FOOT_A);
        _server.sendContent(firmware_version);
        _server.sendContent_P(_HTML_FOOT_B);
    }

    void _box(const char* title) {
        _server.sendContent(
            "<div class='card bg-black border-secondary mb-3'>"
            "<div class='card-header text-success fw-semibold small py-1'>");
        _server.sendContent(title);
        _server.sendContent("</div><div class='card-body p-0'>");
    }

    void _boxEnd() { _server.sendContent("</div></div>"); }

    // Label + valor numa linha com separador
    void _row(const char* label, const String& value) {
        _server.sendContent(
            "<div class='d-flex justify-content-between px-3 py-1 border-bottom border-secondary'>"
            "<span class='text-secondary small'>");
        _server.sendContent(label);
        _server.sendContent("</span><span class='small text-end ms-2'>");
        _server.sendContent(value);
        _server.sendContent("</span></div>");
    }

    void _csrfField() {
        _server.sendContent(
            String("<input type='hidden' name='_csrf' value='") + _csrfToken + "'>");
    }

    void _alert(const char* cls, const char* msg) {
        _server.sendContent(String("<div class='alert alert-") + cls + " py-2'>");
        _server.sendContent(msg);
        _server.sendContent("</div>");
    }

    // Página de confirmação com redirect automático
    void _savedPage(const char* msg, const char* backHref) {
        _pageBegin();
        _alert("success", msg);
        _server.sendContent(
            String("<meta http-equiv='refresh' content='2;url=") + backHref + "'>"
            "<a href='" + backHref + "' class='btn btn-outline-secondary btn-sm'>Voltar</a>"
        );
        _pageEnd();
    }

    // Máscara para tokens sensíveis: mostra apenas os últimos 4 chars
    static String _maskSecret(const char* s) {
        size_t len = strlen(s);
        if (len == 0) return String();
        return String("●●●●") + String(s + (len > 4 ? len - 4 : 0));
    }

    // ── Utilitários ───────────────────────────────────────────────────────────

    static String _uptime() {
        unsigned long s = millis() / 1000;
        char buf[20];
        snprintf(buf, sizeof(buf), "%luh %02lum %02lus", s/3600, (s/60)%60, s%60);
        return String(buf);
    }

    static String _elapsed(unsigned long ms) {
        unsigned long s = ms / 1000;
        char buf[24];
        snprintf(buf, sizeof(buf), "%luh %02lum %02lus", s/3600, (s/60)%60, s%60);
        return String(buf);
    }

    static float _chipTemp() {
        float v = analogRead(A0) * (3.3f / 1023.0f);
        return v * 540.0f;
    }

    void _otaProgressBar() {
        if (!_ota.inProgress()) return;
        int pct = (_ota.contentLength() > 0)
            ? (int)(_ota.bytesWritten() * 100UL / (size_t)_ota.contentLength())
            : 0;
        _server.sendContent(
            "<div class='alert alert-info py-2 mb-3'>"
            "<div class='d-flex justify-content-between small mb-1'>"
            "<span>OTA em progresso</span><span>" + String(pct) + "%</span></div>"
            "<div class='progress bg-secondary' style='height:8px'>"
            "<div class='progress-bar progress-bar-striped progress-bar-animated bg-success' "
            "style='width:" + String(pct) + "%'></div></div>"
            "<div class='text-secondary small mt-1'>" +
            String(_ota.bytesWritten()) + " / " + String(_ota.contentLength()) + " bytes"
            "</div></div>"
        );
    }

    // ── Dispatch estático ─────────────────────────────────────────────────────

    static void _hRoot()           { _instance->_handleRoot(); }
    static void _hSave()           { _instance->_handleSave(); }
    static void _hLog()            { _instance->_handleLog(); }
    static void _hStatus()         { _instance->_handleStatus(); }
    static void _hCloudflare()     { _instance->_handleCloudflare(); }
    static void _hCloudflareSave() { _instance->_handleCloudflareSave(); }
    static void _hNtfy()           { _instance->_handleNtfy(); }
    static void _hNtfySave()       { _instance->_handleNtfySave(); }
    static void _hPassword()       { _instance->_handlePassword(); }
    static void _hPasswordSave()   { _instance->_handlePasswordSave(); }

    // ── Handlers ─────────────────────────────────────────────────────────────

    void _handleRoot() {
        if (!_checkAuth()) return;
        _pageBegin();

        if (WiFi.getMode() == WIFI_AP) {
            _alert("warning", "Modo de configuração — conecte ao Wi-Fi");
            _server.sendContent("<form action='/save' method='POST'>");
            _csrfField();
            _server.sendContent(
                "<div class='mb-3'><label class='form-label'>SSID</label>"
                "<input name='ssid' class='form-control bg-dark text-light border-secondary' required></div>"
                "<div class='mb-3'><label class='form-label'>Senha</label>"
                "<input name='pass' type='password' autocomplete='new-password' "
                "class='form-control bg-dark text-light border-secondary'></div>"
                "<button class='btn btn-success w-100'>Conectar</button></form>"
            );
        } else {
            _otaProgressBar();

            _box("Rede");
            _row("SSID",       WiFi.SSID());
            _row("IP Local",   WiFi.localIP().toString());
            _row("IP Público", _cfg.publicIP[0] ? String(_cfg.publicIP) : String("—"));
            _row("RSSI",       String(WiFi.RSSI()) + " dBm");
            _boxEnd();

            _box("DNS Cloudflare");
            _row("Host", _cfg.cf_host[0] ? String(_cfg.cf_host) : String("—"));
            if (_cfg.lastDnsUpdate == 0) {
                _row("Última atualização", "nunca");
            } else {
                _row("Última atualização", _elapsed(millis() - _cfg.lastDnsUpdate) + " atrás");
            }
            _boxEnd();
        }

        _pageEnd();
    }

    void _handleSave() {
        if (!_checkCSRF()) return;
        _cfg.ssid = _server.arg("ssid");
        _cfg.pass = _server.arg("pass");
        _store.save();
        _savedPage("Wi-Fi configurado! Reiniciando...", "/");
        _wifi.scheduleRestart(2000);
    }

    void _handleLog() {
        if (!_checkAuth()) return;
        _pageBegin();
        _server.sendContent("<h5 class='text-warning mb-3'>Log do sistema</h5>"
                            "<pre class='bg-black text-success p-3 rounded small'>");
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
        if (!_checkAuth()) return;
        _pageBegin(true); // auto-refresh a cada 30s

        _otaProgressBar();

        _box("Sistema");
        _row("Uptime",     _uptime());
        _row("Reset",      ESP.getResetReason());
        _row("Chip ID",    String(ESP.getChipId(), HEX));
        _row("CPU",        String(ESP.getCpuFreqMHz()) + " MHz");
        _row("SDK",        String(ESP.getSdkVersion()));
        _row("Temp (ADC)", String(_chipTemp(), 1) + " °C");
        _boxEnd();

        _box("Memória");
        _row("Heap livre",   String(ESP.getFreeHeap()) + " B");
        _row("Maior bloco",  String(ESP.getMaxFreeBlockSize()) + " B");
        _row("Fragmentação", String(ESP.getHeapFragmentation()) + "%");
        _boxEnd();

        _box("Flash");
        _row("Total",  String(ESP.getFlashChipSize()) + " B");
        _row("Sketch", String(ESP.getSketchSize()) + " B");
        _row("Livre",  String(ESP.getFreeSketchSpace()) + " B");
        _boxEnd();

        _box("LittleFS");
        FSInfo fs; LittleFS.info(fs);
        _row("Total", String(fs.totalBytes) + " B");
        _row("Usado", String(fs.usedBytes) + " B");
        Dir dir = LittleFS.openDir("/");
        int fc = 0; while (dir.next()) fc++;
        _row("Arquivos", String(fc));
        _boxEnd();

        _box("Wi-Fi");
        bool con = WiFi.isConnected();
        _row("SSID",    con ? WiFi.SSID()                : String("—"));
        _row("IP",      con ? WiFi.localIP().toString()  : String("—"));
        _row("Gateway", con ? WiFi.gatewayIP().toString(): String("—"));
        _row("Máscara", con ? WiFi.subnetMask().toString(): String("—"));
        _row("DNS",     con ? WiFi.dnsIP().toString()    : String("—"));
        _row("MAC",     WiFi.macAddress());
        _row("RSSI",    String(WiFi.RSSI()) + " dBm");
        _boxEnd();

        _pageEnd();
    }

    void _handleCloudflare() {
        if (!_checkAuth()) return;
        _pageBegin();
        _server.sendContent("<h5 class='text-info mb-3'>Cloudflare DDNS</h5>");

        bool hasToken = strlen(_cfg.cf_token) > 0;
        if (hasToken)
            _alert("success", ("Token configurado: " + _maskSecret(_cfg.cf_token)).c_str());
        else
            _alert("warning", "Token não configurado");

        _server.sendContent("<form action='/cloudflare/save' method='POST'>");
        _csrfField();

        // Token: campo vazio = mantém o atual; preenchido = substitui
        _server.sendContent(
            "<div class='mb-2'><label class='form-label small'>API Token"
            " <span class='text-secondary'>(deixe em branco para manter)</span></label>"
            "<input class='form-control bg-dark text-light border-secondary' "
            "name='token' type='password' autocomplete='new-password' "
            "placeholder='Novo token'></div>"

            "<div class='mb-2'><label class='form-label small'>Zone ID</label>"
            "<input class='form-control bg-dark text-light border-secondary' "
            "name='zone' value='"
        );
        _server.sendContent(String(_cfg.cf_zone) + "'></div>"

            "<div class='mb-2'><label class='form-label small'>Hostname</label>"
            "<input class='form-control bg-dark text-light border-secondary' "
            "name='host' value='" + String(_cfg.cf_host) + "'></div>"
            "<label class='form-label small'>Record IDs</label>"
        );

        for (int i = 0; i < MAX_RECORDS; i++) {
            String val = (i < _cfg.cf_record_count) ? String(_cfg.cf_records[i]) : "";
            _server.sendContent(
                "<input class='form-control bg-dark text-light border-secondary mb-1' "
                "name='record" + String(i) + "' placeholder='Record ID " +
                String(i+1) + "' value='" + val + "'>"
            );
        }

        _server.sendContent(
            "<button class='btn btn-success w-100 mt-2'>Salvar</button></form>"
        );
        _pageEnd();
    }

    void _handleCloudflareSave() {
        if (!_checkAuth()) return;
        if (!_checkCSRF()) return;

        String newToken = _server.arg("token");
        if (newToken.length() > 0)
            strlcpy(_cfg.cf_token, newToken.c_str(), sizeof(_cfg.cf_token));

        strlcpy(_cfg.cf_zone, _server.arg("zone").c_str(), sizeof(_cfg.cf_zone));
        strlcpy(_cfg.cf_host, _server.arg("host").c_str(), sizeof(_cfg.cf_host));

        _cfg.cf_record_count = 0;
        for (uint8_t i = 0; i < MAX_RECORDS; i++) {
            String key = "record" + String(i);
            if (_server.hasArg(key) && _server.arg(key).length() > 0)
                strlcpy(_cfg.cf_records[_cfg.cf_record_count++],
                        _server.arg(key).c_str(), sizeof(_cfg.cf_records[0]));
        }
        _store.save();
        _savedPage("Cloudflare configurado!", "/cloudflare");
    }

    void _handleNtfy() {
        if (!_checkAuth()) return;
        _pageBegin();
        _server.sendContent("<h5 class='text-success mb-3'>Notificações ntfy</h5>");

        bool hasTopic = strlen(_cfg.ntfy_topic) > 0;
        if (hasTopic)
            _alert("success", ("Tópico: " + String(_cfg.ntfy_topic)).c_str());
        else
            _alert("warning", "Tópico não configurado — notificações desativadas");

        _server.sendContent("<form action='/ntfy/save' method='POST'>");
        _csrfField();
        _server.sendContent(
            "<div class='mb-2'><label class='form-label small'>Tópico ntfy.sh</label>"
            "<input class='form-control bg-dark text-light border-secondary' "
            "name='topic' value='" + String(_cfg.ntfy_topic) + "' "
            "placeholder='meu-topico-secreto'></div>"
            "<div class='text-secondary small mb-3'>"
            "Deixe em branco para desativar as notificações.</div>"
            "<button class='btn btn-success w-100'>Salvar</button></form>"
        );
        _pageEnd();
    }

    void _handleNtfySave() {
        if (!_checkAuth()) return;
        if (!_checkCSRF()) return;
        strlcpy(_cfg.ntfy_topic, _server.arg("topic").c_str(), sizeof(_cfg.ntfy_topic));
        _store.save();
        _savedPage("Configuração ntfy salva!", "/ntfy");
    }

    void _handlePassword() {
        if (!_checkAuth()) return;
        _pageBegin();
        _server.sendContent(
            "<h5 class='text-info mb-3'>Senha do painel</h5>"
            "<form action='/password/save' method='POST'>"
        );
        _csrfField();
        _server.sendContent(
            "<div class='mb-2'><label class='form-label small'>Usuário</label>"
            "<input class='form-control bg-dark text-light border-secondary' "
            "name='user' value='" + _cfg.webusr + "' required></div>"
            "<div class='mb-2'><label class='form-label small'>Nova senha</label>"
            "<input type='password' class='form-control bg-dark text-light border-secondary' "
            "name='pass1' autocomplete='new-password' required></div>"
            "<div class='mb-2'><label class='form-label small'>Confirmar senha</label>"
            "<input type='password' class='form-control bg-dark text-light border-secondary' "
            "name='pass2' autocomplete='new-password' required></div>"
            "<button class='btn btn-primary w-100'>Salvar</button></form>"
        );
        _pageEnd();
    }

    void _handlePasswordSave() {
        if (!_checkAuth()) return;
        if (!_checkCSRF()) return;
        if (!_server.hasArg("pass1") || !_server.hasArg("pass2")) {
            _server.send(400, "text/plain", "Parametros ausentes");
            return;
        }
        if (_server.arg("pass1") != _server.arg("pass2")) {
            _pageBegin();
            _alert("danger", "As senhas não coincidem");
            _server.sendContent(
                "<a href='/password' class='btn btn-outline-secondary btn-sm'>Voltar</a>"
            );
            _pageEnd();
            return;
        }
        _cfg.webusr = _server.arg("user");
        _cfg.webpss = _server.arg("pass1");
        _store.save();
        _generateCSRF(); // rotaciona CSRF após mudança de credenciais
        _savedPage("Senha atualizada!", "/");
    }
};

WebService* WebService::_instance = nullptr;
