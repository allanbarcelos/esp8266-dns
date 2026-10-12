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

static const char _PINOUT_URL[] PROGMEM =
    "https://raw.githubusercontent.com/allanbarcelos/esp8266-dns/main/docs/esp01-pinout.png";
static const char _DEVICE_URL[] PROGMEM =
    "https://raw.githubusercontent.com/allanbarcelos/esp8266-dns/main/docs/esp-01.jpg";

// Texto fixo das páginas fica na flash (PSTR/PROGMEM) e sai por
// sendContent_P: literal comum ocupa RAM no ESP8266, e a heap livre é o
// que o TLS do OTA precisa. Parâmetro PGM_P aceita flash ou RAM (as
// leituras _P funcionam nas duas); nunca passe PGM_P para String(),
// strcmp ou printf sem _P — no aparelho isso gera Exception.
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
        _log.log(PSTR("Servidor web iniciado na porta 80"));
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
            snprintf_P(meta, sizeof(meta),
                       PSTR("<meta http-equiv='refresh' content='%d'>"), refreshSec);
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

    void _p(PGM_P s) { _server.sendContent_P(s); }

    void _box(PGM_P title) {
        _p(PSTR("<div class='card bg-black border-secondary mb-3'>"
                "<div class='card-header text-success fw-semibold small py-1'>"));
        _p(title);
        _p(PSTR("</div><div class='card-body p-0'>"));
    }

    void _boxEnd() { _p(PSTR("</div></div>")); }

    // Label + valor numa linha com separador (valor sempre escapado)
    void _row(PGM_P label, const String& value) {
        _p(PSTR("<div class='d-flex justify-content-between align-items-baseline px-3 py-2 border-bottom'>"
                "<span class='kv-label small'>"));
        _p(label);
        _p(PSTR("</span><span class='kv-value text-end ms-3'>"));
        _server.sendContent(escape(value));
        _p(PSTR("</span></div>"));
    }

    void _csrfField() {
        _p(PSTR("<input type='hidden' name='_csrf' value='"));
        _server.sendContent(_csrfToken);
        _p(PSTR("'>"));
    }

    void _alertOpen(PGM_P cls) {
        _p(PSTR("<div class='alert alert-"));
        _p(cls);
        _p(PSTR(" py-2'>"));
    }

    // Texto variável: sempre escapado
    void _alert(PGM_P cls, const String& msg) {
        _alertOpen(cls);
        _server.sendContent(escape(msg));
        _p(PSTR("</div>"));
    }

    // Texto fixo do firmware (PSTR, sem caracteres especiais de HTML)
    void _alertP(PGM_P cls, PGM_P msg) {
        _alertOpen(cls);
        _p(msg);
        _p(PSTR("</div>"));
    }

    void _labelOpen(PGM_P label) {
        _p(PSTR("<div class='mb-2'><label class='form-label small'>"));
        _p(label);
    }

    void _input(PGM_P label, PGM_P name, const String& value, PGM_P extra) {
        _labelOpen(label);
        _p(PSTR("</label><input class='" _INPUT_CLS "' name='"));
        _p(name);
        _p(PSTR("' value='"));
        _server.sendContent(escape(value));
        _p(PSTR("' "));
        _p(extra);
        _p(PSTR("></div>"));
    }

    void _passwordInput(PGM_P label, PGM_P name, PGM_P extra) {
        _labelOpen(label);
        _p(PSTR("</label><input type='password' autocomplete='new-password' class='"
                _INPUT_CLS "' name='"));
        _p(name);
        _p(PSTR("' "));
        _p(extra);
        _p(PSTR("></div>"));
    }

    void _backButton(PGM_P backHref) {
        _p(PSTR("<a href='"));
        _p(backHref);
        _p(PSTR("' class='btn btn-outline-secondary btn-sm'>Voltar</a>"));
    }

    // Página de confirmação com redirect automático
    void _savedPage(PGM_P msg, PGM_P backHref) {
        _pageBegin();
        _alertP(PSTR("success"), msg);
        _p(PSTR("<meta http-equiv='refresh' content='2;url="));
        _p(backHref);
        _p(PSTR("'>"));
        _backButton(backHref);
        _pageEnd();
    }

    void _errorPage(PGM_P msg, PGM_P backHref) {
        _pageBegin(0, 400);
        _alertP(PSTR("danger"), msg);
        _backButton(backHref);
        _pageEnd();
    }

    // Copia o argumento para dst se couber; caso contrário responde 400
    bool _argInto(const char* name, char* dst, size_t size, const char* backHref) {
        String v = _server.arg(name);
        v.trim();
        if (v.length() >= size) {
            _errorPage(PSTR("Valor muito longo"), backHref);
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
            _errorPage(PSTR("Usuario invalido"), backHref);
            return false;
        }
        if (pass1 != _server.arg("pass2")) {
            _errorPage(PSTR("As senhas nao coincidem"), backHref);
            return false;
        }
        if (pass1.length() < MIN_PANEL_PASSWORD_LEN || pass1.length() > 64) {
            _errorPage(PSTR("A senha deve ter de 8 a 64 caracteres"), backHref);
            return false;
        }
        _cfg.webusr = user;
        _cfg.webpss = pass1;
        return true;
    }

    void _otaProgressBar() {
        if (!_ota.inProgress()) return;
        String pct(_ota.progressPercent());
        _p(PSTR("<div class='alert alert-info py-2 mb-3'>"
                "<div class='d-flex justify-content-between small mb-1'>"
                "<span>OTA em progresso</span><span>"));
        _server.sendContent(pct);
        _p(PSTR("%</span></div>"
                "<div class='progress bg-secondary' style='height:8px'>"
                "<div class='progress-bar progress-bar-striped progress-bar-animated bg-success' "
                "style='width:"));
        _server.sendContent(pct);
        _p(PSTR("%'></div></div><div class='kv-value mt-1'>"));
        _server.sendContent(formatBytes((uint32_t)_ota.bytesWritten()));
        _p(PSTR(" / "));
        _server.sendContent(formatBytes((uint32_t)_ota.contentLength()));
        _p(PSTR("</div></div>"));
    }

    // ── Handlers ─────────────────────────────────────────────────────────────

    void _handleRoot() {
        if (!_guard()) return;
        _pageBegin();
        _deviceMark();

        if (_wifi.inConfigMode()) {
            _alertP(PSTR("warning"), PSTR("Modo de configuracao - conecte ao Wi-Fi"));
            _p(PSTR("<form action='/save' method='POST'>"));
            _csrfField();
            _input(PSTR("SSID"), PSTR("ssid"), _cfg.ssid, PSTR("required maxlength='32'"));
            _passwordInput(PSTR("Senha do Wi-Fi"), PSTR("pass"), PSTR("maxlength='63'"));
            if (_cfg.webpss.length() == 0) {
                _p(PSTR("<h6 class='mt-3'>Acesso ao painel</h6>"));
                _input(PSTR("Usuario"), PSTR("user"), _cfg.webusr.length() ? _cfg.webusr : String("admin"),
                       PSTR("required maxlength='32'"));
                _passwordInput(PSTR("Senha do painel (min. 8)"), PSTR("pass1"), PSTR("required minlength='8'"));
                _passwordInput(PSTR("Confirmar senha"), PSTR("pass2"), PSTR("required minlength='8'"));
            }
            _p(PSTR("<button class='btn btn-success w-100'>Conectar</button></form>"));
        } else {
            _otaProgressBar();

            _box(PSTR("Rede"));
            _row(PSTR("SSID"),       WiFi.SSID());
            _row(PSTR("IP Local"),   WiFi.localIP().toString());
            _row(PSTR("IP Publico"), _cfg.publicIP[0] ? String(_cfg.publicIP) : String("-"));
            _row(PSTR("RSSI"),       String(WiFi.RSSI()) + " dBm");
            _boxEnd();

            _box(PSTR("DNS Cloudflare"));
            _row(PSTR("Host"), _cfg.cf_host[0] ? String(_cfg.cf_host) : String("-"));
            _row(PSTR("Ultima atualizacao"), _cfg.lastDnsUpdate == 0
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
            _errorPage(PSTR("SSID invalido"), PSTR("/"));
            return;
        }
        if (pass.length() > 63 || (pass.length() > 0 && pass.length() < 8)) {
            _errorPage(PSTR("Senha do Wi-Fi deve ter de 8 a 63 caracteres"), PSTR("/"));
            return;
        }
        if (_cfg.webpss.length() == 0 && !_readPanelCredentials("/")) return;

        _cfg.ssid = ssid;
        _cfg.pass = pass;
        if (!_store.save()) {
            _errorPage(PSTR("Falha ao salvar configuracao"), PSTR("/"));
            return;
        }
        _savedPage(PSTR("Wi-Fi configurado! Reiniciando..."), PSTR("/"));
        _wifi.scheduleRestart(2000);
    }

    void _handleLog() {
        if (!_guard()) return;
        _pageBegin();
        _p(PSTR("<h5 class='text-warning mb-3'>Log do sistema</h5>"
                            "<pre class='bg-black text-success p-3 rounded small'>"));
        uint8_t cnt   = _log.count();
        uint8_t start = _log.startIndex();
        for (uint8_t i = 0; i < cnt; i++) {
            _server.sendContent(escape(_log.entry(start + i)));
            _p(PSTR("\n"));
        }
        _p(PSTR("</pre>"));
        _pageEnd();
    }

    void _handleStatus() {
        if (!_guard()) return;
        _pageBegin(30);

        _otaProgressBar();

        _box(PSTR("Sistema"));
        _row(PSTR("Uptime"),  formatDuration(millis()));
        _row(PSTR("Reset"),   ESP.getResetReason());
        _row(PSTR("Causa"),   ESP.getResetInfo());
        _row(PSTR("Chip ID"), String(ESP.getChipId(), HEX));
        _row(PSTR("CPU"),     String(ESP.getCpuFreqMHz()) + " MHz");
        _row(PSTR("SDK"),     String(ESP.getSdkVersion()));
        _boxEnd();

        _box(PSTR("Memoria"));
        _row(PSTR("Heap livre"),   formatBytes(ESP.getFreeHeap()));
        _row(PSTR("Maior bloco"),  formatBytes(ESP.getMaxFreeBlockSize()));
        _row(PSTR("Fragmentacao"), String(ESP.getHeapFragmentation()) + "%");
        _boxEnd();

        _box(PSTR("Flash"));
        _row(PSTR("Total"),      formatBytes(ESP.getFlashChipSize()));
        _row(PSTR("Sketch"),     formatBytes(ESP.getSketchSize()));
        _row(PSTR("Reserva OTA"), formatBytes(FlashLayout::OTA_RESERVE));
        _row(PSTR("Livre"),      formatBytes(ESP.getFreeSketchSpace()));
        _boxEnd();

        _box(PSTR("LittleFS"));
        FSInfo fs;
        LittleFS.info(fs);
        _row(PSTR("Total"), formatBytes((uint32_t)fs.totalBytes));
        _row(PSTR("Usado"), formatBytes((uint32_t)fs.usedBytes));
        Dir dir = LittleFS.openDir("/");
        int fc = 0;
        while (dir.next()) fc++;
        _row(PSTR("Arquivos"), String(fc));
        _boxEnd();

        _box(PSTR("Wi-Fi"));
        bool con = WiFi.isConnected();
        _row(PSTR("SSID"),    con ? WiFi.SSID()                 : String("-"));
        _row(PSTR("IP"),      con ? WiFi.localIP().toString()   : String("-"));
        _row(PSTR("Gateway"), con ? WiFi.gatewayIP().toString() : String("-"));
        _row(PSTR("Mascara"), con ? WiFi.subnetMask().toString(): String("-"));
        _row(PSTR("DNS"),     con ? WiFi.dnsIP().toString()     : String("-"));
        _row(PSTR("MAC"),     WiFi.macAddress());
        _row(PSTR("RSSI"),    String(WiFi.RSSI()) + " dBm");
        _boxEnd();

        _pageEnd();
    }

    void _handleCloudflare() {
        if (!_guard()) return;
        _pageBegin();
        _p(PSTR("<h5 class='text-info mb-3'>Cloudflare DDNS</h5>"));

        if (_cfg.cf_token[0])
            _alert(PSTR("success"), "Token configurado: " + maskSecret(_cfg.cf_token));
        else
            _alertP(PSTR("warning"), PSTR("Token nao configurado"));

        _p(PSTR("<form action='/cloudflare/save' method='POST'>"));
        _csrfField();

        // Token: campo vazio = mantém o atual; preenchido = substitui
        _passwordInput(PSTR("API Token <span style='color:#d7dbe0'>(deixe em branco para manter)</span>"),
                       PSTR("token"), PSTR("placeholder='Novo token' maxlength='47'"));
        _input(PSTR("Zone ID"), PSTR("zone"), _cfg.cf_zone, PSTR("maxlength='39'"));
        _input(PSTR("Hostname (exibicao)"), PSTR("host"), _cfg.cf_host, PSTR("maxlength='63'"));
        _p(PSTR("<label class='form-label small'>Record IDs</label>"));

        for (uint8_t i = 0; i < MAX_RECORDS; i++) {
            String val = (i < _cfg.cf_record_count) ? String(_cfg.cf_records[i]) : String();
            _p(PSTR("<input class='" _INPUT_CLS " mb-1' maxlength='39' name='record"));
            _server.sendContent(String(i));
            _p(PSTR("' placeholder='Record ID "));
            _server.sendContent(String(i + 1));
            _p(PSTR("' value='"));
            _server.sendContent(escape(val));
            _p(PSTR("'>"));
        }

        _p(PSTR("<button class='btn btn-success w-100 mt-2'>Salvar</button></form>"));
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
            _errorPage(PSTR("Falha ao salvar configuracao"), PSTR("/cloudflare"));
            return;
        }
        _savedPage(PSTR("Cloudflare configurado!"), PSTR("/cloudflare"));
    }

    void _handleNtfy() {
        if (!_guard()) return;
        _pageBegin();
        _p(PSTR("<h5 class='text-success mb-3'>Notificacoes ntfy</h5>"));

        if (_cfg.ntfy_topic[0])
            _alert(PSTR("success"), String("Topico: ") + _cfg.ntfy_topic);
        else
            _alertP(PSTR("warning"), PSTR("Topico nao configurado - notificacoes desativadas"));

        _p(PSTR("<form action='/ntfy/save' method='POST'>"));
        _csrfField();
        _input(PSTR("Topico ntfy.sh"), PSTR("topic"), _cfg.ntfy_topic,
               PSTR("maxlength='63' pattern='[A-Za-z0-9_\\-]*' placeholder='meu-topico-secreto'"));
        _p(PSTR("<div class='small mb-3' style='color:#d7dbe0'>"
            "Enviado no boot e quando o IP mudar. Deixe em branco para desativar.</div>"
            "<button class='btn btn-success w-100'>Salvar</button></form>"));
        _pageEnd();
    }

    void _handleNtfySave() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;

        String topic = _server.arg("topic");
        topic.trim();
        if (topic.length() > 0 && !NtfyNotifier::isValidTopic(topic)) {
            _errorPage(PSTR("Topico invalido (use letras, numeros, _ ou -)"), PSTR("/ntfy"));
            return;
        }
        strlcpy(_cfg.ntfy_topic, topic.c_str(), sizeof(_cfg.ntfy_topic));
        if (!_store.save()) {
            _errorPage(PSTR("Falha ao salvar configuracao"), PSTR("/ntfy"));
            return;
        }
        _savedPage(PSTR("Configuracao ntfy salva!"), PSTR("/ntfy"));
    }

    void _handlePassword() {
        if (!_guard(true)) return;
        _pageBegin();
        _p(PSTR("<h5 class='text-info mb-3'>Senha do painel</h5>"));
        if (_cfg.webpss.length() == 0)
            _alertP(PSTR("warning"), PSTR("Defina uma senha para liberar o painel"));
        _p(PSTR("<form action='/password/save' method='POST'>"));
        _csrfField();
        _input(PSTR("Usuario"), PSTR("user"), _cfg.webusr.length() ? _cfg.webusr : String("admin"),
               PSTR("required maxlength='32'"));
        _passwordInput(PSTR("Nova senha (min. 8)"), PSTR("pass1"), PSTR("required minlength='8' maxlength='64'"));
        _passwordInput(PSTR("Confirmar senha"), PSTR("pass2"), PSTR("required minlength='8' maxlength='64'"));
        _p(PSTR("<button class='btn btn-primary w-100'>Salvar</button></form>"));
        _pageEnd();
    }

    void _handlePasswordSave() {
        if (!_guard(true)) return;
        if (!_checkCSRF()) return;
        if (!_readPanelCredentials("/password")) return;
        if (!_store.save()) {
            _errorPage(PSTR("Falha ao salvar configuracao"), PSTR("/password"));
            return;
        }
        _generateCSRF(); // rotaciona CSRF após mudança de credenciais
        _savedPage(PSTR("Senha atualizada!"), PSTR("/"));
    }

    void _go(const char* href) {
        _server.sendHeader("Location", href);
        _server.send(303, "text/plain", "");
    }

    void _handleUpdate() {
        if (!_guard()) return;
        _pageBegin(_ota.pending() ? 3 : 0);
        _otaProgressBar();

        _p(PSTR("<h5 class='text-warning mb-3'>Atualizacao</h5>"));
        _box(PSTR("Firmware"));
        _row(PSTR("Versao atual"), firmware_version);
        _row(PSTR("Automatica"), _cfg.ota_auto ? String("ligada") : String("desligada"));
        if (_ota.availableVersion()[0] != '\0')
            _row(PSTR("Disponivel"), _ota.availableVersion());
        if (_ota.detail()[0] != '\0')
            _row(PSTR("Situacao"), _ota.detail());
        _boxEnd();

        _p(PSTR("<form action='/update/auto' method='POST' class='mb-3'>"));
        _csrfField();
        _p(PSTR("<div class='form-check form-switch mb-2'>"
            "<input class='form-check-input' type='checkbox' role='switch' "
            "id='ota_auto' name='ota_auto' value='1'"));
        if (_cfg.ota_auto) _p(PSTR(" checked"));
        _p(PSTR("><label class='form-check-label' for='ota_auto'>"
            "Atualizacao automatica</label></div>"
            "<button class='btn btn-outline-light w-100'>Salvar</button></form>"));

        _p(PSTR("<form action='/update/check' method='POST' class='mb-2'>"));
        _csrfField();
        _p(PSTR("<button class='btn btn-info w-100'>Verificar agora</button></form>"));

        if (_ota.availableVersion()[0] != '\0' && !_ota.inProgress()) {
            _p(PSTR("<form action='/update/install' method='POST'>"));
            _csrfField();
            _p(PSTR("<button class='btn btn-success w-100'>Instalar esta versao</button></form>"));
        }
        _pageEnd();
    }

    void _handleUpdateAuto() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;
        _cfg.ota_auto = _server.arg("ota_auto") == "1";
        _ota.setAutomatic(_cfg.ota_auto);
        if (!_store.save()) {
            _errorPage(PSTR("Falha ao salvar configuracao"), PSTR("/update"));
            return;
        }
        _savedPage(_cfg.ota_auto
                       ? PSTR("Atualizacao automatica ligada")
                       : PSTR("Atualizacao automatica desligada"),
                   PSTR("/update"));
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
            _errorPage(PSTR("Nenhuma versao nova para instalar"), PSTR("/update"));
            return;
        }
        _ota.requestInstall();
        _go("/update");
    }

    void _deviceMark() {
        _p(PSTR("<div class='text-center mb-3'><img src='"));
        _p(_DEVICE_URL);
        _p(PSTR("' alt='ESP-01' class='img-fluid rounded' style='max-width:220px'>"
            "<div class='small mt-1' style='color:#d7dbe0'>"
            "Firmware para o modulo ESP-01</div>"
            "<div class='small' style='color:#d7dbe0'>"
            "Foto: SparkFun, CC BY 2.0</div></div>"));
    }

    void _sendKindList(bool sensors) {
        uint8_t n = sensors ? IoTService::SENSOR_COUNT : IoTService::MODULE_COUNT;
        for (uint8_t i = 0; i < n; i++) {
            if (i) _p(PSTR(","));
            char buf[4];
            snprintf(buf, sizeof(buf), "%u",
                     sensors ? IoTService::sensorKind(i) : IoTService::moduleKind(i));
            _server.sendContent(buf);
        }
    }

    void _typeOptions(bool sensors, uint8_t current) {
        uint8_t n = sensors ? IoTService::SENSOR_COUNT : IoTService::MODULE_COUNT;
        for (uint8_t i = 0; i < n; i++) {
            char value[4];
            snprintf(value, sizeof(value), "%u", i);
            _choice(value,
                    sensors ? IoTService::sensorLabel(i) : IoTService::moduleLabel(i),
                    current == i);
        }
    }

    void _pinOptions(int8_t current) {
        static const int8_t pins[] = {-1, 2, 0, 1, 3};
        static const char* values[] = {"-1", "2", "0", "1", "3"};
        static const char* labels[] = {
            "Nenhum", "GPIO2", "GPIO0", "TX (GPIO1)", "RX (GPIO3)"
        };
        for (int i = 0; i < 5; i++)
            _choice(values[i], labels[i], current == pins[i]);
    }

    void _levelOptions(bool high) {
        _choice(PSTR("1"), PSTR("Alto"), high);
        _choice(PSTR("0"), PSTR("Baixo"), !high);
    }

    void _choice(PGM_P value, PGM_P label, bool on) {
        _p(PSTR("<option value='"));
        _p(value);
        _p(on ? PSTR("' selected>") : PSTR("'>"));
        _p(label);
        _p(PSTR("</option>"));
    }

    void _selectOpen(PGM_P id) {
        _p(PSTR("<select class='" _INPUT_CLS "' id='"));
        _p(id);
        _p(PSTR("' name='"));
        _p(id);
        _p(PSTR("' onchange='iotSync()'>"));
    }

    void _divOpen(PGM_P id, bool show) {
        _p(PSTR("<div id='"));
        _p(id);
        _p(show ? PSTR("'>") : PSTR("' style='display:none'>"));
    }

    // name vai para %s: tem de estar em RAM (snprintf_P só lê o formato da flash).
    // min -55 e max 100000 passam de 128 caracteres e cortavam o '>'.
    void _number(const char* name, int value, int min, int max) {
        char buf[160];
        snprintf_P(buf, sizeof(buf),
                   PSTR("<input class='" _INPUT_CLS "' "
                        "type='number' name='%s' value='%d' min='%d' max='%d'>"),
                   name, value, min, max);
        _server.sendContent(buf);
    }

    void _handleIot() {
        if (!_guard()) return;
        _pageBegin(IoTService::allowedPin(_cfg.iot_sensor_pin) ? 5 : 0);
        _p(PSTR("<h5 class='text-info mb-3'>IoT</h5>"));
        _p(PSTR("<img src='"));
        _p(_PINOUT_URL);
        _p(PSTR("' alt='Pinos do ESP-01' class='img-fluid bg-white rounded mb-3' "
            "style='max-width:100%'>"));
        _p(PSTR("<p class='small mb-3' style='color:#d7dbe0'>"
            "GPIO0 e GPIO2 ficam em alto no boot. O sensor e o modulo usam pinos "
            "diferentes. Digital, PIR, porta, chuva, chama, gas, solo, luz, som, "
            "boia e botao ligam por nivel, ou alternam a cada pulso. O comando "
            "pode forcar o modulo ligado ou desligado. DHT, DS18B20, pulsos "
            "(a cada 10 s) e ultrassom (trig e echo no mesmo pino, em cm) comparam "
            "um valor com o limite. Rele, LED, buzzer, bomba e valvula usam um "
            "nivel. O servo vai para um angulo. Ventilador e motor usam PWM de 0 "
            "a 100, em dois patamares ou proporcional entre o limite e o limite "
            "alto. DHT e DS18B20 pedem pull-up. A leitura segue no topico ntfy ja "
            "cadastrado, com o prefixo [sensor]. O IP sai como [ip].</p>"));

        _box(PSTR("Agora"));
        _row(PSTR("Sensor"), _iot.sensorText());
        _row(PSTR("Modulo"), _iot.moduleText());
        _boxEnd();

        if (_cfg.iot_notify && _cfg.ntfy_topic[0] == '\0')
            _alertP(PSTR("warning"), PSTR("Cadastre o topico ntfy para enviar a leitura"));

        uint8_t sensorKind = IoTService::sensorKind(_cfg.iot_sensor_type);
        uint8_t moduleKind = IoTService::moduleKind(_cfg.iot_module_type);
        bool numeric = sensorKind != IoTService::KIND_DIGITAL;
        bool dht = sensorKind == IoTService::KIND_DHT11 ||
                   sensorKind == IoTService::KIND_DHT22;
        bool level = moduleKind == IoTService::MKIND_LEVEL;
        bool servo = moduleKind == IoTService::MKIND_SERVO;
        bool pwm = moduleKind == IoTService::MKIND_PWM;

        _p(PSTR("<form action='/iot/save' method='POST'>"));
        _csrfField();
        _p(PSTR("<label class='form-label small'>Pino do sensor</label>"));
        _selectOpen(PSTR("sensor"));
        _pinOptions(_cfg.iot_sensor_pin);
        _p(PSTR("</select>"));
        _p(PSTR("<label class='form-label small mt-2'>Pino do modulo</label>"));
        _selectOpen(PSTR("module"));
        _pinOptions(_cfg.iot_module_pin);
        _p(PSTR("</select>"));

        _p(PSTR("<label class='form-label small mt-2'>Comando</label>"));
        _selectOpen(PSTR("manual"));
        _choice(PSTR("0"), PSTR("Automatico"), _cfg.iot_manual == IoTService::MANUAL_AUTO);
        _choice(PSTR("1"), PSTR("Ligado"), _cfg.iot_manual == IoTService::MANUAL_ON);
        _choice(PSTR("2"), PSTR("Desligado"), _cfg.iot_manual == IoTService::MANUAL_OFF);
        _p(PSTR("</select>"));

        _p(PSTR("<label class='form-label small mt-2'>Tipo do sensor</label>"));
        _selectOpen(PSTR("sensor_type"));
        _typeOptions(true, _cfg.iot_sensor_type);
        _p(PSTR("</select>"));

        _divOpen(PSTR("iot_digital"), !numeric);
        _p(PSTR("<label class='form-label small mt-2'>Ligar o modulo quando o sensor estiver</label>"));
        _selectOpen(PSTR("sensor_high"));
        _levelOptions(_cfg.iot_sensor_high);
        _p(PSTR("</select>"));
        _p(PSTR("<label class='form-label small mt-2'>Ao pulso</label>"));
        _selectOpen(PSTR("toggle"));
        _choice(PSTR("0"), PSTR("Seguir o nivel"), !_cfg.iot_toggle);
        _choice(PSTR("1"), PSTR("Alternar"), _cfg.iot_toggle);
        _p(PSTR("</select></div>"));

        _divOpen(PSTR("iot_value"), numeric);
        _divOpen(PSTR("iot_metric"), dht);
        _p(PSTR("<label class='form-label small mt-2'>Grandeza</label>"));
        _selectOpen(PSTR("metric"));
        _choice(PSTR("0"), PSTR("Umidade"), _cfg.iot_metric == 0);
        _choice(PSTR("1"), PSTR("Temperatura"), _cfg.iot_metric == 1);
        _p(PSTR("</select></div>"));
        _p(PSTR("<label class='form-label small mt-2'>Ligar quando o valor estiver</label>"));
        _selectOpen(PSTR("above"));
        _choice(PSTR("0"), PSTR("Abaixo de"), !_cfg.iot_above);
        _choice(PSTR("1"), PSTR("Acima de"), _cfg.iot_above);
        _p(PSTR("</select>"));
        _p(PSTR("<label class='form-label small mt-2'>Limite</label>"));
        _number("threshold", _cfg.iot_threshold, -55, 100000);
        _p(PSTR("<label class='form-label small mt-2'>Margem</label>"));
        _number("margin", _cfg.iot_margin, 0, 50);
        _p(PSTR("</div>"));

        _p(PSTR("<label class='form-label small mt-2'>Tipo do modulo</label>"));
        _selectOpen(PSTR("module_type"));
        _typeOptions(false, _cfg.iot_module_type);
        _p(PSTR("</select>"));

        _divOpen(PSTR("iot_level"), level);
        _p(PSTR("<label class='form-label small mt-2'>Nivel do pino que liga o modulo</label>"));
        _selectOpen(PSTR("module_high"));
        _levelOptions(_cfg.iot_module_high);
        _p(PSTR("</select></div>"));

        _divOpen(PSTR("iot_servo"), servo);
        _p(PSTR("<label class='form-label small mt-2'>Angulo ao ligar</label>"));
        _number("angle_on", _cfg.iot_angle_on, 0, 180);
        _p(PSTR("<label class='form-label small mt-2'>Angulo ao soltar</label>"));
        _number("angle_off", _cfg.iot_angle_off, 0, 180);
        _p(PSTR("</div>"));

        _divOpen(PSTR("iot_pwm"), pwm);
        _p(PSTR("<label class='form-label small mt-2'>Potencia ao ligar (%)</label>"));
        _number("duty_on", _cfg.iot_duty_on, 0, 100);
        _p(PSTR("<label class='form-label small mt-2'>Potencia ao soltar (%)</label>"));
        _number("duty_off", _cfg.iot_duty_off, 0, 100);
        _p(PSTR("</div>"));

        _divOpen(PSTR("iot_span"), pwm && numeric);
        _p(PSTR("<label class='form-label small mt-2'>Curva da potencia</label>"));
        _selectOpen(PSTR("linear"));
        _choice(PSTR("0"), PSTR("Dois patamares"), !_cfg.iot_linear);
        _choice(PSTR("1"), PSTR("Proporcional"), _cfg.iot_linear);
        _p(PSTR("</select>"));
        _p(PSTR("<label class='form-label small mt-2'>Limite alto</label>"));
        _number("threshold_hi", _cfg.iot_threshold_hi, -55, 100000);
        _p(PSTR("</div>"));

        _p(PSTR("<div class='form-check form-switch my-3'>"
            "<input class='form-check-input' type='checkbox' role='switch' "
            "id='iot_notify' name='iot_notify' value='1'"));
        if (_cfg.iot_notify) _p(PSTR(" checked"));
        _p(PSTR("><label class='form-check-label' for='iot_notify'>"
            "Enviar leitura ao ntfy</label></div>"
            "<button class='btn btn-info w-100'>Salvar</button></form>"
            "<script>function iotSync(){var sk=["));
        _sendKindList(true);
        _p(PSTR("];var mk=["));
        _sendKindList(false);
        char js[900];
        snprintf_P(js, sizeof(js), PSTR(
            "];var s=+document.getElementById('sensor_type').value;"
            "var m=+document.getElementById('module_type').value;"
            "if(!(s>=0&&s<sk.length))s=0;"
            "if(!(m>=0&&m<mk.length))m=0;"
            "var num=sk[s]!=%u;"
            "var pwm=mk[m]==%u;"
            "document.getElementById('iot_digital').style.display=num?'none':'';"
            "document.getElementById('iot_value').style.display=num?'':'none';"
            "document.getElementById('iot_metric').style.display=(sk[s]==%u||sk[s]==%u)?'':'none';"
            "document.getElementById('iot_level').style.display=mk[m]==%u?'':'none';"
            "document.getElementById('iot_servo').style.display=mk[m]==%u?'':'none';"
            "document.getElementById('iot_pwm').style.display=pwm?'':'none';"
            "document.getElementById('iot_span').style.display=(pwm&&num)?'':'none';"
            "}iotSync();</script>"),
            IoTService::KIND_DIGITAL, IoTService::MKIND_PWM,
            IoTService::KIND_DHT11, IoTService::KIND_DHT22,
            IoTService::MKIND_LEVEL, IoTService::MKIND_SERVO);
        _server.sendContent(js);
        _pageEnd();
    }

    void _handleIotSave() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;

        // O gcc, com -fno-exceptions, ainda conta um ramo de limpeza do
        // temporário quando && ou || recebe uma String. Esse ramo não executa.
        int8_t sensor = -1;
        int8_t module = -1;
        String sensorArg = _server.arg("sensor");
        String moduleArg = _server.arg("module");
        if (!IoTService::parsePin(sensorArg, sensor)) {
            _errorPage(PSTR("Pino invalido"), PSTR("/iot"));
            return;
        }
        if (!IoTService::parsePin(moduleArg, module)) {
            _errorPage(PSTR("Pino invalido"), PSTR("/iot"));
            return;
        }
        if (sensor != -1 && sensor == module) {
            _errorPage(PSTR("Sensor e modulo precisam de pinos diferentes"), PSTR("/iot"));
            return;
        }

        uint8_t sensorType = IoTService::SENSOR_DIGITAL;
        String sensorTypeArg = _server.arg("sensor_type");
        if (sensorTypeArg.length() > 0 &&
            !IoTService::parseU8(sensorTypeArg, 0,
                                 IoTService::SENSOR_COUNT - 1, sensorType)) {
            _errorPage(PSTR("Valor invalido"), PSTR("/iot"));
            return;
        }
        uint8_t moduleType = IoTService::MODULE_LEVEL;
        String moduleTypeArg = _server.arg("module_type");
        if (moduleTypeArg.length() > 0 &&
            !IoTService::parseU8(moduleTypeArg, 0,
                                 IoTService::MODULE_COUNT - 1, moduleType)) {
            _errorPage(PSTR("Valor invalido"), PSTR("/iot"));
            return;
        }
        uint8_t metric = 0;
        String metricArg = _server.arg("metric");
        if (metricArg.length() > 0 &&
            !IoTService::parseU8(metricArg, 0, 1, metric)) {
            _errorPage(PSTR("Valor invalido"), PSTR("/iot"));
            return;
        }

        int16_t threshold = 0;
        uint8_t margin = 0;
        if (IoTService::sensorKind(sensorType) != IoTService::KIND_DIGITAL) {
            String thresholdArg = _server.arg("threshold");
            if (!IoTService::parseI16(thresholdArg, -55, 100000, threshold)) {
                _errorPage(PSTR("Valor invalido"), PSTR("/iot"));
                return;
            }
            String marginArg = _server.arg("margin");
            if (marginArg.length() > 0 &&
                !IoTService::parseU8(marginArg, 0, 50, margin)) {
                _errorPage(PSTR("Valor invalido"), PSTR("/iot"));
                return;
            }
        }

        uint8_t angleOn = 90;
        uint8_t angleOff = 0;
        if (IoTService::moduleKind(moduleType) == IoTService::MKIND_SERVO) {
            String angleOnArg = _server.arg("angle_on");
            String angleOffArg = _server.arg("angle_off");
            if (!IoTService::parseU8(angleOnArg, 0, 180, angleOn)) {
                _errorPage(PSTR("Valor invalido"), PSTR("/iot"));
                return;
            }
            if (!IoTService::parseU8(angleOffArg, 0, 180, angleOff)) {
                _errorPage(PSTR("Valor invalido"), PSTR("/iot"));
                return;
            }
        }
        uint8_t manual = IoTService::MANUAL_AUTO;
        String manualArg = _server.arg("manual");
        if (manualArg.length() > 0 &&
            !IoTService::parseU8(manualArg, 0, IoTService::MANUAL_OFF, manual)) {
            _errorPage(PSTR("Valor invalido"), PSTR("/iot"));
            return;
        }
        bool toggle = false;
        String toggleArg = _server.arg("toggle");
        if (toggleArg.length() > 0) {
            uint8_t toggleBit = 0;
            if (!IoTService::parseU8(toggleArg, 0, 1, toggleBit)) {
                _errorPage(PSTR("Valor invalido"), PSTR("/iot"));
                return;
            }
            toggle = toggleBit == 1;
        }
        bool linear = false;
        String linearArg = _server.arg("linear");
        if (linearArg.length() > 0) {
            uint8_t linearBit = 0;
            if (!IoTService::parseU8(linearArg, 0, 1, linearBit)) {
                _errorPage(PSTR("Valor invalido"), PSTR("/iot"));
                return;
            }
            linear = linearBit == 1;
        }
        if (IoTService::sensorKind(sensorType) != IoTService::KIND_DIGITAL)
            toggle = false;
        if (IoTService::sensorKind(sensorType) == IoTService::KIND_DIGITAL)
            linear = false;
        if (IoTService::moduleKind(moduleType) != IoTService::MKIND_PWM)
            linear = false;

        int16_t thresholdHi = 0;
        if (linear) {
            String hiArg = _server.arg("threshold_hi");
            if (!IoTService::parseI16(hiArg, -55, 100000, thresholdHi)) {
                _errorPage(PSTR("Valor invalido"), PSTR("/iot"));
                return;
            }
            if (thresholdHi <= threshold) {
                _errorPage(PSTR("Valor invalido"), PSTR("/iot"));
                return;
            }
        }

        uint8_t dutyOn = 100;
        uint8_t dutyOff = 0;
        if (IoTService::moduleKind(moduleType) == IoTService::MKIND_PWM) {
            String dutyOnArg = _server.arg("duty_on");
            String dutyOffArg = _server.arg("duty_off");
            if (!IoTService::parseU8(dutyOnArg, 0, 100, dutyOn)) {
                _errorPage(PSTR("Valor invalido"), PSTR("/iot"));
                return;
            }
            if (!IoTService::parseU8(dutyOffArg, 0, 100, dutyOff)) {
                _errorPage(PSTR("Valor invalido"), PSTR("/iot"));
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
        _cfg.iot_duty_on      = dutyOn;
        _cfg.iot_duty_off     = dutyOff;
        _cfg.iot_manual       = manual;
        _cfg.iot_toggle       = toggle;
        _cfg.iot_linear       = linear;
        _cfg.iot_threshold_hi = thresholdHi;
        if (!_store.save()) {
            _errorPage(PSTR("Falha ao salvar configuracao"), PSTR("/iot"));
            return;
        }
        _iot.begin();
        _savedPage(PSTR("IoT configurado"), PSTR("/iot"));
    }
};
