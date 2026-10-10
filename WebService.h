#pragma once
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include "Config.h"
#include "Logger.h"
#include "Version.h"
#include "ConfigStore.h"
#include "WiFiService.h"
#include "OTAService.h"
#include "IoTService.h"
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

static const char _HTML_HEAD_B[] PROGMEM = R"(<style>
body,.form-label{color:#f1f3f4}
.card,.kv-label{color:#e8eaed !important}
.kv-value{color:#ffffff !important;font-weight:600;font-size:1rem}
.text-secondary{color:#d7dbe0 !important}
.card-body .border-bottom{border-color:#3d4248 !important}
</style>
</head>
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
    <a href="/iot"       class="btn btn-sm btn-outline-light">IoT</a>
    <a href="/update"    class="btn btn-sm btn-outline-warning">Atualizar</a>
    <a href="/password"  class="btn btn-sm btn-outline-secondary">Senha</a>
  </div>
</nav>
)";

static const char _HTML_FOOT_A[] PROGMEM = R"(<hr class="border-secondary mt-4">
<p class="small mb-0" style="color:#d7dbe0">Firmware: )";

static const char _HTML_FOOT_B[] PROGMEM = R"(</p></div></body></html>)";

#define _INPUT_CLS "form-control bg-dark text-light border-secondary"

// ─────────────────────────────────────────────────────────────────────────────

class WebService {
public:
    WebService(Config& cfg, Logger& log, ConfigStore& store,
               WiFiService& wifi, OTAService& ota, IoTService& iot)
        : _server(80), _cfg(cfg), _log(log), _store(store),
          _wifi(wifi), _ota(ota), _iot(iot) {
        _csrfToken[0] = '\0';
    }

    void begin() {
        _generateCSRF();

        _server.on("/",                HTTP_GET,  [this]() { _handleRoot(); });
        _server.on("/save",            HTTP_POST, [this]() { _handleSave(); });
        _server.on("/log",             HTTP_GET,  [this]() { _handleLog(); });
        _server.on("/status",          HTTP_GET,  [this]() { _handleStatus(); });
        _server.on("/cloudflare",      HTTP_GET,  [this]() { _handleCloudflare(); });
        _server.on("/cloudflare/save", HTTP_POST, [this]() { _handleCloudflareSave(); });
        _server.on("/ntfy",            HTTP_GET,  [this]() { _handleNtfy(); });
        _server.on("/ntfy/save",       HTTP_POST, [this]() { _handleNtfySave(); });
        _server.on("/password",        HTTP_GET,  [this]() { _handlePassword(); });
        _server.on("/password/save",   HTTP_POST, [this]() { _handlePasswordSave(); });
        _server.on("/iot",             HTTP_GET,  [this]() { _handleIot(); });
        _server.on("/iot/save",        HTTP_POST, [this]() { _handleIotSave(); });
        _server.on("/update",          HTTP_GET,  [this]() { _handleUpdate(); });
        _server.on("/update/auto",     HTTP_POST, [this]() { _handleUpdateAuto(); });
        _server.on("/update/check",    HTTP_POST, [this]() { _handleUpdateCheck(); });
        _server.on("/update/install",  HTTP_POST, [this]() { _handleUpdateInstall(); });
        _server.begin();
        _log.log("Servidor web iniciado na porta 80");
    }

    void tick() { _server.handleClient(); }

    const char* csrfToken() const { return _csrfToken; }

    // Escapa texto para uso seguro em conteúdo e atributos HTML
    static String escape(const String& s) {
        String out;
        for (size_t i = 0; i < s.length(); i++) {
            char c = s.c_str()[i];
            switch (c) {
                case '&':  out += "&amp;";  break;
                case '<':  out += "&lt;";   break;
                case '>':  out += "&gt;";   break;
                case '"':  out += "&quot;"; break;
                case '\'': out += "&#39;";  break;
                default:   out += c;
            }
        }
        return out;
    }

    // Máscara para tokens sensíveis: mostra apenas os últimos 4 chars
    static String maskSecret(const char* s) {
        size_t len = strlen(s);
        if (len == 0) return String();
        return String("****") + String(s + (len > 4 ? len - 4 : 0));
    }

    static String formatDuration(unsigned long ms) {
        unsigned long s = ms / 1000;
        char buf[24];
        snprintf(buf, sizeof(buf), "%luh %02lum %02lus", s / 3600, (s / 60) % 60, s % 60);
        return String(buf);
    }

    // Escala binária: 1024 B = 1 KB. Uma casa decimal, e o .0 some.
    // 1500 → "1.5 KB"; 1024 → "1 KB"; 1048576 → "1 MB".
    static String formatBytes(uint32_t n) {
        const char* unit = "B";
        uint32_t scale = 1;
        if (n >= 1024UL * 1024UL * 1024UL) {
            unit = "GB";
            scale = 1024UL * 1024UL * 1024UL;
        } else if (n >= 1024UL * 1024UL) {
            unit = "MB";
            scale = 1024UL * 1024UL;
        } else if (n >= 1024UL) {
            unit = "KB";
            scale = 1024UL;
        }
        char buf[24];
        if (scale == 1) {
            snprintf(buf, sizeof(buf), "%lu B", (unsigned long)n);
            return String(buf);
        }
        unsigned long tenths =
            (unsigned long)(((unsigned long long)n * 10ULL + scale / 2) / scale);
        if (tenths % 10 == 0)
            snprintf(buf, sizeof(buf), "%lu %s", tenths / 10, unit);
        else
            snprintf(buf, sizeof(buf), "%lu.%lu %s", tenths / 10, tenths % 10, unit);
        return String(buf);
    }

private:
    ESP8266WebServer _server;
    Config&          _cfg;
    Logger&          _log;
    ConfigStore&     _store;
    WiFiService&     _wifi;
    OTAService&      _ota;
    IoTService&      _iot;

    char _csrfToken[17]; // 16 hex + null

    // ── Segurança ─────────────────────────────────────────────────────────────

    // Token gerado pelo RNG de hardware a cada boot e a cada troca de senha
    void _generateCSRF() {
        snprintf(_csrfToken, sizeof(_csrfToken), "%08lx%08lx",
                 (unsigned long)ESP.random(), (unsigned long)ESP.random());
    }

    // Autenticação + exigência de senha definida.
    // Sem senha configurada, o painel só libera o modo AP (configuração
    // inicial) e a página de definição de senha (allowSetup).
    bool _guard(bool allowSetup = false) {
        if (_cfg.webpss.length() == 0) {
            if (allowSetup || _wifi.inConfigMode()) return true;
            _server.sendHeader("Location", "/password");
            _server.send(302, "text/plain", "");
            return false;
        }
        if (!_server.authenticate(_cfg.webusr.c_str(), _cfg.webpss.c_str())) {
            _server.requestAuthentication();
            return false;
        }
        return true;
    }

    bool _checkCSRF() {
        if (_server.arg("_csrf") != _csrfToken) {
            _server.send(403, "text/plain", "Token CSRF invalido");
            return false;
        }
        return true;
    }

    // ── Helpers HTML ──────────────────────────────────────────────────────────

    void _pageBegin(int refreshSec = 0, int code = 200) {
        _server.setContentLength(CONTENT_LENGTH_UNKNOWN);
        _server.send(code, "text/html", "");
        _server.sendContent_P(_HTML_HEAD_A);
        if (refreshSec > 0) {
            char meta[56];
            snprintf(meta, sizeof(meta),
                     "<meta http-equiv='refresh' content='%d'>", refreshSec);
            _server.sendContent(meta);
        }
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

    // Label + valor numa linha com separador (valor sempre escapado)
    void _row(const char* label, const String& value) {
        _server.sendContent(
            "<div class='d-flex justify-content-between align-items-baseline px-3 py-2 border-bottom'>"
            "<span class='kv-label small'>");
        _server.sendContent(label);
        _server.sendContent("</span><span class='kv-value text-end ms-3'>");
        _server.sendContent(escape(value));
        _server.sendContent("</span></div>");
    }

    void _csrfField() {
        _server.sendContent(
            String("<input type='hidden' name='_csrf' value='") + _csrfToken + "'>");
    }

    void _alert(const char* cls, const String& msg) {
        _server.sendContent(String("<div class='alert alert-") + cls + " py-2'>");
        _server.sendContent(escape(msg));
        _server.sendContent("</div>");
    }

    void _input(const char* label, const char* name, const String& value,
                const char* extra = "") {
        _server.sendContent(
            String("<div class='mb-2'><label class='form-label small'>") + label +
            "</label><input class='" _INPUT_CLS "' name='" + name +
            "' value='" + escape(value) + "' " + extra + "></div>");
    }

    void _passwordInput(const char* label, const char* name, const char* extra = "") {
        _server.sendContent(
            String("<div class='mb-2'><label class='form-label small'>") + label +
            "</label><input type='password' autocomplete='new-password' class='"
            _INPUT_CLS "' name='" + name + "' " + extra + "></div>");
    }

    // Página de confirmação com redirect automático
    void _savedPage(const char* msg, const char* backHref) {
        _pageBegin();
        _alert("success", msg);
        _server.sendContent(
            String("<meta http-equiv='refresh' content='2;url=") + backHref + "'>"
            "<a href='" + backHref + "' class='btn btn-outline-secondary btn-sm'>Voltar</a>");
        _pageEnd();
    }

    void _errorPage(const char* msg, const char* backHref) {
        _pageBegin(0, 400);
        _alert("danger", msg);
        _server.sendContent(
            String("<a href='") + backHref + "' class='btn btn-outline-secondary btn-sm'>Voltar</a>");
        _pageEnd();
    }

    // Copia o argumento para dst se couber; caso contrário responde 400
    bool _argInto(const char* name, char* dst, size_t size, const char* backHref) {
        String v = _server.arg(name);
        v.trim();
        if (v.length() >= size) {
            _errorPage("Valor muito longo", backHref);
            return false;
        }
        strlcpy(dst, v.c_str(), size);
        return true;
    }

    // Valida usuário/senha do painel enviados nos campos user/pass1/pass2
    bool _readPanelCredentials(const char* backHref) {
        String user  = _server.arg("user");
        String pass1 = _server.arg("pass1");
        user.trim();
        if (user.length() == 0 || user.length() > 32) {
            _errorPage("Usuario invalido", backHref);
            return false;
        }
        if (pass1 != _server.arg("pass2")) {
            _errorPage("As senhas nao coincidem", backHref);
            return false;
        }
        if (pass1.length() < MIN_PANEL_PASSWORD_LEN || pass1.length() > 64) {
            _errorPage("A senha deve ter de 8 a 64 caracteres", backHref);
            return false;
        }
        _cfg.webusr = user;
        _cfg.webpss = pass1;
        return true;
    }

    void _otaProgressBar() {
        if (!_ota.inProgress()) return;
        int pct = _ota.progressPercent();
        _server.sendContent(
            "<div class='alert alert-info py-2 mb-3'>"
            "<div class='d-flex justify-content-between small mb-1'>"
            "<span>OTA em progresso</span><span>" + String(pct) + "%</span></div>"
            "<div class='progress bg-secondary' style='height:8px'>"
            "<div class='progress-bar progress-bar-striped progress-bar-animated bg-success' "
            "style='width:" + String(pct) + "%'></div></div>"
            "<div class='kv-value mt-1'>" +
            formatBytes((uint32_t)_ota.bytesWritten()) + " / " +
            formatBytes((uint32_t)_ota.contentLength()) +
            "</div></div>");
    }

    // ── Handlers ─────────────────────────────────────────────────────────────

    void _handleRoot() {
        if (!_guard()) return;
        _pageBegin();

        if (_wifi.inConfigMode()) {
            _alert("warning", "Modo de configuracao - conecte ao Wi-Fi");
            _server.sendContent("<form action='/save' method='POST'>");
            _csrfField();
            _input("SSID", "ssid", _cfg.ssid, "required maxlength='32'");
            _passwordInput("Senha do Wi-Fi", "pass", "maxlength='63'");
            if (_cfg.webpss.length() == 0) {
                _server.sendContent("<h6 class='mt-3'>Acesso ao painel</h6>");
                _input("Usuario", "user", _cfg.webusr.length() ? _cfg.webusr : String("admin"),
                       "required maxlength='32'");
                _passwordInput("Senha do painel (min. 8)", "pass1", "required minlength='8'");
                _passwordInput("Confirmar senha", "pass2", "required minlength='8'");
            }
            _server.sendContent("<button class='btn btn-success w-100'>Conectar</button></form>");
        } else {
            _otaProgressBar();

            _box("Rede");
            _row("SSID",       WiFi.SSID());
            _row("IP Local",   WiFi.localIP().toString());
            _row("IP Publico", _cfg.publicIP[0] ? String(_cfg.publicIP) : String("-"));
            _row("RSSI",       String(WiFi.RSSI()) + " dBm");
            _boxEnd();

            _box("DNS Cloudflare");
            _row("Host", _cfg.cf_host[0] ? String(_cfg.cf_host) : String("-"));
            _row("Ultima atualizacao", _cfg.lastDnsUpdate == 0
                    ? String("nunca")
                    : formatDuration(millis() - _cfg.lastDnsUpdate) + " atras");
            _boxEnd();
        }

        _pageEnd();
    }

    void _handleSave() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;

        String ssid = _server.arg("ssid");
        String pass = _server.arg("pass");
        if (ssid.length() == 0 || ssid.length() > 32) {
            _errorPage("SSID invalido", "/");
            return;
        }
        if (pass.length() > 63 || (pass.length() > 0 && pass.length() < 8)) {
            _errorPage("Senha do Wi-Fi deve ter de 8 a 63 caracteres", "/");
            return;
        }
        if (_cfg.webpss.length() == 0 && !_readPanelCredentials("/")) return;

        _cfg.ssid = ssid;
        _cfg.pass = pass;
        if (!_store.save()) {
            _errorPage("Falha ao salvar configuracao", "/");
            return;
        }
        _savedPage("Wi-Fi configurado! Reiniciando...", "/");
        _wifi.scheduleRestart(2000);
    }

    void _handleLog() {
        if (!_guard()) return;
        _pageBegin();
        _server.sendContent("<h5 class='text-warning mb-3'>Log do sistema</h5>"
                            "<pre class='bg-black text-success p-3 rounded small'>");
        uint8_t cnt   = _log.count();
        uint8_t start = _log.startIndex();
        for (uint8_t i = 0; i < cnt; i++) {
            _server.sendContent(escape(_log.entry(start + i)));
            _server.sendContent("\n");
        }
        _server.sendContent("</pre>");
        _pageEnd();
    }

    void _handleStatus() {
        if (!_guard()) return;
        _pageBegin(30);

        _otaProgressBar();

        _box("Sistema");
        _row("Uptime",  formatDuration(millis()));
        _row("Reset",   ESP.getResetReason());
        _row("Causa",   ESP.getResetInfo());
        _row("Chip ID", String(ESP.getChipId(), HEX));
        _row("CPU",     String(ESP.getCpuFreqMHz()) + " MHz");
        _row("SDK",     String(ESP.getSdkVersion()));
        _boxEnd();

        _box("Memoria");
        _row("Heap livre",   formatBytes(ESP.getFreeHeap()));
        _row("Maior bloco",  formatBytes(ESP.getMaxFreeBlockSize()));
        _row("Fragmentacao", String(ESP.getHeapFragmentation()) + "%");
        _boxEnd();

        _box("Flash");
        _row("Total",  formatBytes(ESP.getFlashChipSize()));
        _row("Sketch", formatBytes(ESP.getSketchSize()));
        _row("Livre",  formatBytes(ESP.getFreeSketchSpace()));
        _boxEnd();

        _box("LittleFS");
        FSInfo fs;
        LittleFS.info(fs);
        _row("Total", formatBytes((uint32_t)fs.totalBytes));
        _row("Usado", formatBytes((uint32_t)fs.usedBytes));
        Dir dir = LittleFS.openDir("/");
        int fc = 0;
        while (dir.next()) fc++;
        _row("Arquivos", String(fc));
        _boxEnd();

        _box("Wi-Fi");
        bool con = WiFi.isConnected();
        _row("SSID",    con ? WiFi.SSID()                 : String("-"));
        _row("IP",      con ? WiFi.localIP().toString()   : String("-"));
        _row("Gateway", con ? WiFi.gatewayIP().toString() : String("-"));
        _row("Mascara", con ? WiFi.subnetMask().toString(): String("-"));
        _row("DNS",     con ? WiFi.dnsIP().toString()     : String("-"));
        _row("MAC",     WiFi.macAddress());
        _row("RSSI",    String(WiFi.RSSI()) + " dBm");
        _boxEnd();

        _pageEnd();
    }

    void _handleCloudflare() {
        if (!_guard()) return;
        _pageBegin();
        _server.sendContent("<h5 class='text-info mb-3'>Cloudflare DDNS</h5>");

        if (_cfg.cf_token[0])
            _alert("success", "Token configurado: " + maskSecret(_cfg.cf_token));
        else
            _alert("warning", "Token nao configurado");

        _server.sendContent("<form action='/cloudflare/save' method='POST'>");
        _csrfField();

        // Token: campo vazio = mantém o atual; preenchido = substitui
        _passwordInput("API Token <span style='color:#d7dbe0'>(deixe em branco para manter)</span>",
                       "token", "placeholder='Novo token' maxlength='47'");
        _input("Zone ID", "zone", _cfg.cf_zone, "maxlength='39'");
        _input("Hostname (exibicao)", "host", _cfg.cf_host, "maxlength='63'");
        _server.sendContent("<label class='form-label small'>Record IDs</label>");

        for (uint8_t i = 0; i < MAX_RECORDS; i++) {
            String val = (i < _cfg.cf_record_count) ? String(_cfg.cf_records[i]) : String();
            _server.sendContent(
                "<input class='" _INPUT_CLS " mb-1' maxlength='39' name='record" + String(i) +
                "' placeholder='Record ID " + String(i + 1) + "' value='" + escape(val) + "'>");
        }

        _server.sendContent("<button class='btn btn-success w-100 mt-2'>Salvar</button></form>");
        _pageEnd();
    }

    void _handleCloudflareSave() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;

        // Valida tudo em buffers temporários antes de alterar a config
        char token[sizeof(_cfg.cf_token)];
        char zone[sizeof(_cfg.cf_zone)];
        char host[sizeof(_cfg.cf_host)];
        char records[MAX_RECORDS][sizeof(_cfg.cf_records[0])];
        uint8_t count = 0;

        if (!_argInto("token", token, sizeof(token), "/cloudflare")) return;
        if (!_argInto("zone",  zone,  sizeof(zone),  "/cloudflare")) return;
        if (!_argInto("host",  host,  sizeof(host),  "/cloudflare")) return;
        for (uint8_t i = 0; i < MAX_RECORDS; i++) {
            String key = "record" + String(i);
            if (!_argInto(key.c_str(), records[count], sizeof(records[0]), "/cloudflare")) return;
            if (records[count][0]) count++;
        }

        if (token[0]) strlcpy(_cfg.cf_token, token, sizeof(_cfg.cf_token));
        strlcpy(_cfg.cf_zone, zone, sizeof(_cfg.cf_zone));
        strlcpy(_cfg.cf_host, host, sizeof(_cfg.cf_host));
        memcpy(_cfg.cf_records, records, sizeof(records));
        _cfg.cf_record_count = count;

        if (!_store.save()) {
            _errorPage("Falha ao salvar configuracao", "/cloudflare");
            return;
        }
        _savedPage("Cloudflare configurado!", "/cloudflare");
    }

    void _handleNtfy() {
        if (!_guard()) return;
        _pageBegin();
        _server.sendContent("<h5 class='text-success mb-3'>Notificacoes ntfy</h5>");

        if (_cfg.ntfy_topic[0])
            _alert("success", String("Topico: ") + _cfg.ntfy_topic);
        else
            _alert("warning", "Topico nao configurado - notificacoes desativadas");

        _server.sendContent("<form action='/ntfy/save' method='POST'>");
        _csrfField();
        _input("Topico ntfy.sh", "topic", _cfg.ntfy_topic,
               "maxlength='63' pattern='[A-Za-z0-9_\\-]*' placeholder='meu-topico-secreto'");
        _server.sendContent(
            "<div class='small mb-3' style='color:#d7dbe0'>"
            "Enviado no boot e quando o IP mudar. Deixe em branco para desativar.</div>"
            "<button class='btn btn-success w-100'>Salvar</button></form>");
        _pageEnd();
    }

    void _handleNtfySave() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;

        String topic = _server.arg("topic");
        topic.trim();
        if (topic.length() > 0 && !NtfyNotifier::isValidTopic(topic)) {
            _errorPage("Topico invalido (use letras, numeros, _ ou -)", "/ntfy");
            return;
        }
        strlcpy(_cfg.ntfy_topic, topic.c_str(), sizeof(_cfg.ntfy_topic));
        if (!_store.save()) {
            _errorPage("Falha ao salvar configuracao", "/ntfy");
            return;
        }
        _savedPage("Configuracao ntfy salva!", "/ntfy");
    }

    void _handlePassword() {
        if (!_guard(true)) return;
        _pageBegin();
        _server.sendContent("<h5 class='text-info mb-3'>Senha do painel</h5>");
        if (_cfg.webpss.length() == 0)
            _alert("warning", "Defina uma senha para liberar o painel");
        _server.sendContent("<form action='/password/save' method='POST'>");
        _csrfField();
        _input("Usuario", "user", _cfg.webusr.length() ? _cfg.webusr : String("admin"),
               "required maxlength='32'");
        _passwordInput("Nova senha (min. 8)", "pass1", "required minlength='8' maxlength='64'");
        _passwordInput("Confirmar senha", "pass2", "required minlength='8' maxlength='64'");
        _server.sendContent("<button class='btn btn-primary w-100'>Salvar</button></form>");
        _pageEnd();
    }

    void _handlePasswordSave() {
        if (!_guard(true)) return;
        if (!_checkCSRF()) return;
        if (!_readPanelCredentials("/password")) return;
        if (!_store.save()) {
            _errorPage("Falha ao salvar configuracao", "/password");
            return;
        }
        _generateCSRF(); // rotaciona CSRF após mudança de credenciais
        _savedPage("Senha atualizada!", "/");
    }

    void _go(const char* href) {
        _server.sendHeader("Location", href);
        _server.send(303, "text/plain", "");
    }

    void _handleUpdate() {
        if (!_guard()) return;
        _pageBegin(_ota.pending() ? 3 : 0);
        _otaProgressBar();

        _server.sendContent("<h5 class='text-warning mb-3'>Atualizacao</h5>");
        _box("Firmware");
        _row("Versao atual", firmware_version);
        _row("Automatica", _cfg.ota_auto ? String("ligada") : String("desligada"));
        if (_ota.availableVersion()[0] != '\0')
            _row("Disponivel", _ota.availableVersion());
        if (_ota.detail()[0] != '\0')
            _row("Situacao", _ota.detail());
        _boxEnd();

        _server.sendContent("<form action='/update/auto' method='POST' class='mb-3'>");
        _csrfField();
        _server.sendContent(
            "<div class='form-check form-switch mb-2'>"
            "<input class='form-check-input' type='checkbox' role='switch' "
            "id='ota_auto' name='ota_auto' value='1'");
        if (_cfg.ota_auto) _server.sendContent(" checked");
        _server.sendContent(
            "><label class='form-check-label' for='ota_auto'>"
            "Atualizacao automatica</label></div>"
            "<button class='btn btn-outline-light w-100'>Salvar</button></form>");

        _server.sendContent("<form action='/update/check' method='POST' class='mb-2'>");
        _csrfField();
        _server.sendContent(
            "<button class='btn btn-info w-100'>Verificar agora</button></form>");

        if (_ota.availableVersion()[0] != '\0' && !_ota.inProgress()) {
            _server.sendContent("<form action='/update/install' method='POST'>");
            _csrfField();
            _server.sendContent(
                "<button class='btn btn-success w-100'>Instalar esta versao</button></form>");
        }
        _pageEnd();
    }

    void _handleUpdateAuto() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;
        _cfg.ota_auto = _server.arg("ota_auto") == "1";
        _ota.setAutomatic(_cfg.ota_auto);
        if (!_store.save()) {
            _errorPage("Falha ao salvar configuracao", "/update");
            return;
        }
        _savedPage(_cfg.ota_auto
                       ? "Atualizacao automatica ligada"
                       : "Atualizacao automatica desligada",
                   "/update");
    }

    void _handleUpdateCheck() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;
        _ota.requestCheck();
        _go("/update");
    }

    void _handleUpdateInstall() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;
        if (_ota.availableVersion()[0] == '\0') {
            _errorPage("Nenhuma versao nova para instalar", "/update");
            return;
        }
        _ota.requestInstall();
        _go("/update");
    }

    static constexpr const char* _PINOUT_URL =
        "https://raw.githubusercontent.com/allanbarcelos/esp8266-dns/main/docs/esp01-pinout.png";

    void _pinOptions(int8_t current) {
        static const int8_t pins[] = {-1, 2, 0, 1, 3};
        static const char* values[] = {"-1", "2", "0", "1", "3"};
        static const char* labels[] = {
            "Nenhum", "GPIO2", "GPIO0", "TX (GPIO1)", "RX (GPIO3)"
        };
        for (int i = 0; i < 5; i++) {
            _server.sendContent("<option value='");
            _server.sendContent(values[i]);
            _server.sendContent(current == pins[i] ? "' selected>" : "'>");
            _server.sendContent(labels[i]);
            _server.sendContent("</option>");
        }
    }

    void _levelOptions(bool high) {
        _server.sendContent("<option value='1'");
        _server.sendContent(high ? " selected>" : ">");
        _server.sendContent("Alto</option><option value='0'");
        _server.sendContent(high ? ">" : " selected>");
        _server.sendContent("Baixo</option>");
    }

    void _choice(const char* value, const char* label, bool on) {
        _server.sendContent("<option value='");
        _server.sendContent(value);
        _server.sendContent(on ? "' selected>" : "'>");
        _server.sendContent(label);
        _server.sendContent("</option>");
    }

    void _selectOpen(const char* id) {
        _server.sendContent("<select class='" _INPUT_CLS "' id='");
        _server.sendContent(id);
        _server.sendContent("' name='");
        _server.sendContent(id);
        _server.sendContent("' onchange='iotSync()'>");
    }

    void _divOpen(const char* id, bool show) {
        _server.sendContent("<div id='");
        _server.sendContent(id);
        _server.sendContent(show ? "'>" : "' style='display:none'>");
    }

    void _number(const char* name, int value, int min, int max) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "<input class='form-control bg-dark text-light border-secondary' "
                 "type='number' name='%s' value='%d' min='%d' max='%d'>",
                 name, value, min, max);
        _server.sendContent(buf);
    }

    void _handleIot() {
        if (!_guard()) return;
        _pageBegin(IoTService::allowedPin(_cfg.iot_sensor_pin) ? 5 : 0);
        _server.sendContent("<h5 class='text-info mb-3'>IoT</h5>");
        _server.sendContent("<img src='");
        _server.sendContent(_PINOUT_URL);
        _server.sendContent(
            "' alt='Pinos do ESP-01' class='img-fluid bg-white rounded mb-3' "
            "style='max-width:100%'>");
        _server.sendContent(
            "<p class='small mb-3' style='color:#d7dbe0'>"
            "GPIO0 e GPIO2 ficam em alto no boot. O sensor e o modulo usam pinos "
            "diferentes. Digital liga por nivel. DHT, DS18B20 e pulsos (a cada 10 s) "
            "comparam um valor com o limite. O servo vai para um angulo. "
            "DHT e DS18B20 pedem pull-up. A leitura segue no topico ntfy ja "
            "cadastrado, com o prefixo [sensor]. O IP sai como [ip].</p>");

        _box("Agora");
        _row("Sensor", _iot.sensorText());
        _row("Modulo", _iot.moduleText());
        _boxEnd();

        if (_cfg.iot_notify && _cfg.ntfy_topic[0] == '\0')
            _alert("warning", "Cadastre o topico ntfy para enviar a leitura");

        bool numeric = _cfg.iot_sensor_type != IoTService::SENSOR_DIGITAL;
        bool dht = _cfg.iot_sensor_type == IoTService::SENSOR_DHT11 ||
                   _cfg.iot_sensor_type == IoTService::SENSOR_DHT22;
        bool servo = _cfg.iot_module_type == IoTService::MODULE_SERVO;

        _server.sendContent("<form action='/iot/save' method='POST'>");
        _csrfField();
        _server.sendContent("<label class='form-label small'>Pino do sensor</label>");
        _selectOpen("sensor");
        _pinOptions(_cfg.iot_sensor_pin);
        _server.sendContent("</select>");
        _server.sendContent("<label class='form-label small mt-2'>Pino do modulo</label>");
        _selectOpen("module");
        _pinOptions(_cfg.iot_module_pin);
        _server.sendContent("</select>");

        _server.sendContent("<label class='form-label small mt-2'>Tipo do sensor</label>");
        _selectOpen("sensor_type");
        _choice("0", "Digital", _cfg.iot_sensor_type == IoTService::SENSOR_DIGITAL);
        _choice("1", "DHT11", _cfg.iot_sensor_type == IoTService::SENSOR_DHT11);
        _choice("2", "DHT22", _cfg.iot_sensor_type == IoTService::SENSOR_DHT22);
        _choice("3", "DS18B20", _cfg.iot_sensor_type == IoTService::SENSOR_DS18B20);
        _choice("4", "Pulsos (10 s)", _cfg.iot_sensor_type == IoTService::SENSOR_PULSE);
        _server.sendContent("</select>");

        _divOpen("iot_digital", !numeric);
        _server.sendContent(
            "<label class='form-label small mt-2'>Ligar o modulo quando o sensor estiver</label>");
        _selectOpen("sensor_high");
        _levelOptions(_cfg.iot_sensor_high);
        _server.sendContent("</select></div>");

        _divOpen("iot_value", numeric);
        _divOpen("iot_metric", dht);
        _server.sendContent("<label class='form-label small mt-2'>Grandeza</label>");
        _selectOpen("metric");
        _choice("0", "Umidade", _cfg.iot_metric == 0);
        _choice("1", "Temperatura", _cfg.iot_metric == 1);
        _server.sendContent("</select></div>");
        _server.sendContent("<label class='form-label small mt-2'>Ligar quando o valor estiver</label>");
        _selectOpen("above");
        _choice("0", "Abaixo de", !_cfg.iot_above);
        _choice("1", "Acima de", _cfg.iot_above);
        _server.sendContent("</select>");
        _server.sendContent("<label class='form-label small mt-2'>Limite</label>");
        _number("threshold", _cfg.iot_threshold, -55, 100000);
        _server.sendContent("<label class='form-label small mt-2'>Margem</label>");
        _number("margin", _cfg.iot_margin, 0, 50);
        _server.sendContent("</div>");

        _server.sendContent("<label class='form-label small mt-2'>Tipo do modulo</label>");
        _selectOpen("module_type");
        _choice("0", "Nivel", _cfg.iot_module_type == IoTService::MODULE_LEVEL);
        _choice("1", "Servo", _cfg.iot_module_type == IoTService::MODULE_SERVO);
        _server.sendContent("</select>");

        _divOpen("iot_level", !servo);
        _server.sendContent(
            "<label class='form-label small mt-2'>Nivel do pino que liga o modulo</label>");
        _selectOpen("module_high");
        _levelOptions(_cfg.iot_module_high);
        _server.sendContent("</select></div>");

        _divOpen("iot_servo", servo);
        _server.sendContent("<label class='form-label small mt-2'>Angulo ao ligar</label>");
        _number("angle_on", _cfg.iot_angle_on, 0, 180);
        _server.sendContent("<label class='form-label small mt-2'>Angulo ao soltar</label>");
        _number("angle_off", _cfg.iot_angle_off, 0, 180);
        _server.sendContent("</div>");

        _server.sendContent(
            "<div class='form-check form-switch my-3'>"
            "<input class='form-check-input' type='checkbox' role='switch' "
            "id='iot_notify' name='iot_notify' value='1'");
        if (_cfg.iot_notify) _server.sendContent(" checked");
        _server.sendContent(
            "><label class='form-check-label' for='iot_notify'>"
            "Enviar leitura ao ntfy</label></div>"
            "<button class='btn btn-info w-100'>Salvar</button></form>"
            "<script>function iotSync(){"
            "var s=document.getElementById('sensor_type').value;"
            "var m=document.getElementById('module_type').value;"
            "var num=s!='0';"
            "document.getElementById('iot_digital').style.display=num?'none':'';"
            "document.getElementById('iot_value').style.display=num?'':'none';"
            "document.getElementById('iot_metric').style.display=(s=='1'||s=='2')?'':'none';"
            "document.getElementById('iot_level').style.display=m=='1'?'none':'';"
            "document.getElementById('iot_servo').style.display=m=='1'?'':'none';"
            "}iotSync();</script>");
        _pageEnd();
    }

    void _handleIotSave() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;

        int8_t sensor = -1;
        int8_t module = -1;
        if (!IoTService::parsePin(_server.arg("sensor"), sensor) ||
            !IoTService::parsePin(_server.arg("module"), module)) {
            _errorPage("Pino invalido", "/iot");
            return;
        }
        if (sensor != -1 && sensor == module) {
            _errorPage("Sensor e modulo precisam de pinos diferentes", "/iot");
            return;
        }

        uint8_t sensorType = IoTService::SENSOR_DIGITAL;
        if (_server.arg("sensor_type").length() > 0 &&
            !IoTService::parseU8(_server.arg("sensor_type"), 0, 4, sensorType)) {
            _errorPage("Valor invalido", "/iot");
            return;
        }
        uint8_t moduleType = IoTService::MODULE_LEVEL;
        if (_server.arg("module_type").length() > 0 &&
            !IoTService::parseU8(_server.arg("module_type"), 0, 1, moduleType)) {
            _errorPage("Valor invalido", "/iot");
            return;
        }
        uint8_t metric = 0;
        if (_server.arg("metric").length() > 0 &&
            !IoTService::parseU8(_server.arg("metric"), 0, 1, metric)) {
            _errorPage("Valor invalido", "/iot");
            return;
        }

        int16_t threshold = 0;
        uint8_t margin = 0;
        if (sensorType != IoTService::SENSOR_DIGITAL) {
            if (!IoTService::parseI16(_server.arg("threshold"), -55, 100000, threshold)) {
                _errorPage("Valor invalido", "/iot");
                return;
            }
            if (_server.arg("margin").length() > 0 &&
                !IoTService::parseU8(_server.arg("margin"), 0, 50, margin)) {
                _errorPage("Valor invalido", "/iot");
                return;
            }
        }

        uint8_t angleOn = 90;
        uint8_t angleOff = 0;
        if (moduleType == IoTService::MODULE_SERVO) {
            if (!IoTService::parseU8(_server.arg("angle_on"), 0, 180, angleOn) ||
                !IoTService::parseU8(_server.arg("angle_off"), 0, 180, angleOff)) {
                _errorPage("Valor invalido", "/iot");
                return;
            }
        }

        _cfg.iot_sensor_pin   = sensor;
        _cfg.iot_module_pin   = module;
        _cfg.iot_sensor_high  = _server.arg("sensor_high") == "1";
        _cfg.iot_module_high  = _server.arg("module_high") == "1";
        _cfg.iot_notify       = _server.arg("iot_notify") == "1";
        _cfg.iot_sensor_type  = sensorType;
        _cfg.iot_module_type  = moduleType;
        _cfg.iot_metric       = metric;
        _cfg.iot_above        = _server.arg("above") == "1";
        _cfg.iot_threshold    = threshold;
        _cfg.iot_margin       = margin;
        _cfg.iot_angle_on     = angleOn;
        _cfg.iot_angle_off    = angleOff;
        if (!_store.save()) {
            _errorPage("Falha ao salvar configuracao", "/iot");
            return;
        }
        _iot.begin();
        _savedPage("IoT configurado", "/iot");
    }
};
