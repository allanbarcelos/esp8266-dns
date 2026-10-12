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
#include "Assets.h"

// ── PROGMEM ───────────────────────────────────────────────────────────────────
// Cabeçalho dividido para permitir injeção de <meta refresh> antes de </head>.
// O CSS vem embutido: no modo AP não há internet para buscar um framework.
// Contraste AA no tema escuro; botão verde escuro para o branco passar 4.5:1.

static const char _HTML_HEAD_A[] PROGMEM =
    "<!DOCTYPE html><html lang='pt'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP8266 DNS</title>";

static const char _HTML_HEAD_B[] PROGMEM =
    "<style>"
    ":root{color-scheme:dark;--bg:#0d1117;--cd:#161b22;--ln:#30363d;--fg:#e6edf3;"
    "--mu:#9da7b3;--ok:#3fb950;--wa:#d29922;--er:#f85149;--in:#58a6ff}"
    "*{box-sizing:border-box}"
    "body{margin:0;background:var(--bg);color:var(--fg);"
    "font:15px/1.5 system-ui,-apple-system,'Segoe UI',Roboto,sans-serif}"
    "header{background:var(--cd);border-bottom:1px solid var(--ln);padding:10px 16px}"
    "header b{color:var(--ok)}"
    "nav{display:flex;flex-wrap:wrap;gap:6px;margin-top:8px}"
    "nav a{color:var(--fg);text-decoration:none;padding:3px 10px;"
    "border:1px solid var(--ln);border-radius:6px;font-size:14px}"
    "nav a:hover{border-color:var(--ok)}"
    "main,footer{max-width:640px;margin:auto;padding:16px}"
    "footer,.m{color:var(--mu);font-size:13px}"
    "h1{font-size:20px;margin:0 0 16px}"
    "h2{font-size:16px;margin:20px 0 0}"
    ".c{background:var(--cd);border:1px solid var(--ln);border-radius:8px;margin-bottom:16px}"
    ".c h2{font-size:12px;text-transform:uppercase;letter-spacing:.06em;color:var(--mu);"
    "margin:0;padding:8px 16px;border-bottom:1px solid var(--ln)}"
    ".r{display:flex;justify-content:space-between;gap:16px;padding:8px 16px;"
    "border-bottom:1px solid var(--ln)}"
    ".r:last-child{border:0}.r span{color:var(--mu)}"
    ".r b{text-align:right;overflow-wrap:anywhere}"
    "label{display:block;margin-top:12px;font-size:14px}"
    "input,select{display:block;width:100%;margin-top:4px;padding:8px 10px;"
    "background:var(--bg);color:var(--fg);border:1px solid var(--ln);border-radius:6px;font:inherit}"
    ".sw{display:flex;align-items:center;gap:8px}"
    ".sw input{width:auto;margin:0;accent-color:var(--ok)}"
    "button,.b{display:block;width:100%;margin-top:16px;padding:10px;border:0;"
    "border-radius:6px;background:#238636;color:#fff;font:inherit;font-weight:600;"
    "text-align:center;text-decoration:none;cursor:pointer}"
    ".b2{background:none;border:1px solid var(--ln);color:var(--fg)}"
    ":focus-visible{outline:2px solid var(--in);outline-offset:2px}"
    ".a{background:var(--cd);border-left:4px solid var(--in);border-radius:6px;"
    "padding:10px 14px;margin-bottom:16px}"
    ".success{border-color:var(--ok)}.warning{border-color:var(--wa)}"
    ".danger{border-color:var(--er)}"
    "progress{display:block;width:100%;accent-color:var(--ok)}"
    "pre{background:#010409;border:1px solid var(--ln);border-radius:8px;padding:12px;"
    "font-size:12px;white-space:pre-wrap;overflow-wrap:anywhere}"
    "img{max-width:100%;height:auto;border-radius:6px}"
    "figure{margin:0 0 16px;text-align:center}"
    "</style></head><body><header><b>&#9679; ESP8266 DNS</b>"
    "<nav aria-label='Menu'><a href='/'>Home</a><a href='/status'>Status</a>"
    "<a href='/log'>Logs</a><a href='/cloudflare'>Cloudflare</a><a href='/ntfy'>Ntfy</a>"
    "<a href='/iot'>IoT</a><a href='/update'>Atualizar</a><a href='/password'>Senha</a>"
    "</nav></header><main>";

static const char _HTML_FOOT_A[] PROGMEM = "</main><footer>Firmware: ";

static const char _HTML_FOOT_B[] PROGMEM = "</footer></body></html>";


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
        _server.on("/iot/now",         HTTP_GET,  [this]() { _handleIotNow(); });
        _server.on("/update",          HTTP_GET,  [this]() { _handleUpdate(); });
        _server.on("/update/auto",     HTTP_POST, [this]() { _handleUpdateAuto(); });
        _server.on("/update/check",    HTTP_POST, [this]() { _handleUpdateCheck(); });
        _server.on("/update/install",  HTTP_POST, [this]() { _handleUpdateInstall(); });
        // Imagens embutidas (Assets.h): públicas, sem dado do aparelho
        _server.on("/esp01.webp",      HTTP_GET,  [this]() {
            _asset("image/webp", ASSET_PHOTO, sizeof(ASSET_PHOTO), false);
        });
        _server.on("/pinout.svg",      HTTP_GET,  [this]() {
            _asset("image/svg+xml", ASSET_PINOUT_GZ, sizeof(ASSET_PINOUT_GZ), true);
        });
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
        _p(_HTML_HEAD_A);
        if (refreshSec > 0) {
            char meta[56];
            snprintf_P(meta, sizeof(meta),
                       PSTR("<meta http-equiv='refresh' content='%d'>"), refreshSec);
            _s(meta);
        }
        _p(_HTML_HEAD_B);
    }

    void _pageEnd() {
        _p(_HTML_FOOT_A);
        _s(firmware_version);
        _p(_HTML_FOOT_B);
    }

    // Pedaço vazio no chunked é o terminador: o core envia "0\r\n\r\n" e o
    // navegador dá a página por encerrada (o stub de teste faz o mesmo).
    // Texto fixo e números nunca são vazios; valor do usuário (SSID vazio
    // no modo AP, usuário, log) vai como String e é filtrado aqui.
    void _p(PGM_P s) { _server.sendContent_P(s); }
    void _s(const char* s) { _server.sendContent(s); }
    void _s(const String& s) {
        if (s.length()) _server.sendContent(s);
    }

    void _title(PGM_P text) {
        _p(PSTR("<h1>"));
        _p(text);
        _p(PSTR("</h1>"));
    }

    void _box(PGM_P title) {
        _p(PSTR("<section class='c'><h2>"));
        _p(title);
        _p(PSTR("</h2>"));
    }

    void _boxEnd() { _p(PSTR("</section>")); }

    // Label + valor numa linha com separador (valor sempre escapado)
    // id, se houver, deixa o script atualizar o valor sem recarregar a página
    void _row(PGM_P label, const String& value, PGM_P id = nullptr) {
        _p(PSTR("<div class='r'><span>"));
        _p(label);
        if (id) {
            _p(PSTR("</span><b id='"));
            _p(id);
            _p(PSTR("'>"));
        } else {
            _p(PSTR("</span><b>"));
        }
        _s(escape(value));
        _p(PSTR("</b></div>"));
    }

    void _csrfField() {
        _p(PSTR("<input type='hidden' name='_csrf' value='"));
        _s(_csrfToken);
        _p(PSTR("'>"));
    }

    void _alertOpen(PGM_P cls) {
        _p(PSTR("<div class='a "));
        _p(cls);
        _p(PSTR("'>"));
    }

    // Texto variável: sempre escapado
    void _alert(PGM_P cls, const String& msg) {
        _alertOpen(cls);
        _s(escape(msg));
        _p(PSTR("</div>"));
    }

    // Texto fixo do firmware (PSTR, sem caracteres especiais de HTML)
    void _alertP(PGM_P cls, PGM_P msg) {
        _alertOpen(cls);
        _p(msg);
        _p(PSTR("</div>"));
    }

    // O <label> envolve o campo: associação implícita para leitores de tela
    void _labelOpen(PGM_P label) {
        _p(PSTR("<label>"));
        _p(label);
    }

    void _input(PGM_P label, PGM_P name, const String& value, PGM_P extra) {
        _labelOpen(label);
        _p(PSTR("<input name='"));
        _p(name);
        _p(PSTR("' value='"));
        _s(escape(value));
        _p(PSTR("' "));
        _p(extra);
        _p(PSTR("></label>"));
    }

    void _passwordInput(PGM_P label, PGM_P name, PGM_P extra) {
        _labelOpen(label);
        _p(PSTR("<input type='password' autocomplete='new-password' name='"));
        _p(name);
        _p(PSTR("' "));
        _p(extra);
        _p(PSTR("></label>"));
    }

    // Caixa de seleção (liga/desliga) já marcada ou não
    void _switch(PGM_P id, PGM_P label, bool on) {
        _p(PSTR("<label class='sw'><input type='checkbox' role='switch' id='"));
        _p(id);
        _p(PSTR("' name='"));
        _p(id);
        _p(on ? PSTR("' value='1' checked>") : PSTR("' value='1'>"));
        _p(label);
        _p(PSTR("</label>"));
    }

    void _button(PGM_P text) {
        _p(PSTR("<button>"));
        _p(text);
        _p(PSTR("</button></form>"));
    }

    void _backButton(PGM_P backHref) {
        _p(PSTR("<a class='b b2' href='"));
        _p(backHref);
        _p(PSTR("'>Voltar</a>"));
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
        _p(PSTR("<div class='a' role='status'>OTA em progresso: <b>"));
        _s(pct);
        _p(PSTR("%</b><progress max='100' value='"));
        _s(pct);
        _p(PSTR("'></progress><span class='m'>"));
        _s(formatBytes((uint32_t)_ota.bytesWritten()));
        _p(PSTR(" / "));
        _s(formatBytes((uint32_t)_ota.contentLength()));
        _p(PSTR("</span></div>"));
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
                _p(PSTR("<h2>Acesso ao painel</h2>"));
                _input(PSTR("Usuario"), PSTR("user"), _cfg.webusr.length() ? _cfg.webusr : String("admin"),
                       PSTR("required maxlength='32'"));
                _passwordInput(PSTR("Senha do painel (min. 8)"), PSTR("pass1"), PSTR("required minlength='8'"));
                _passwordInput(PSTR("Confirmar senha"), PSTR("pass2"), PSTR("required minlength='8'"));
            }
            _button(PSTR("Conectar"));
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
        _title(PSTR("Log do sistema"));
        _p(PSTR("<pre>"));
        uint8_t cnt   = _log.count();
        uint8_t start = _log.startIndex();
        for (uint8_t i = 0; i < cnt; i++) {
            _s(escape(_log.entry(start + i)));
            _p(PSTR("\n"));
        }
        _p(PSTR("</pre>"));
        _pageEnd();
    }

    void _handleStatus() {
        if (!_guard()) return;
        _pageBegin(30);
        _title(PSTR("Status"));

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
        _title(PSTR("Cloudflare DDNS"));

        if (_cfg.cf_token[0])
            _alert(PSTR("success"), "Token configurado: " + maskSecret(_cfg.cf_token));
        else
            _alertP(PSTR("warning"), PSTR("Token nao configurado"));

        _p(PSTR("<form action='/cloudflare/save' method='POST'>"));
        _csrfField();

        // Token: campo vazio = mantém o atual; preenchido = substitui
        _passwordInput(PSTR("API Token <span class='m'>(deixe em branco para manter)</span>"),
                       PSTR("token"), PSTR("placeholder='Novo token' maxlength='47'"));
        _input(PSTR("Zone ID"), PSTR("zone"), _cfg.cf_zone, PSTR("maxlength='39'"));
        _input(PSTR("Hostname (exibicao)"), PSTR("host"), _cfg.cf_host, PSTR("maxlength='63'"));
        for (uint8_t i = 0; i < MAX_RECORDS; i++) {
            String val = (i < _cfg.cf_record_count) ? String(_cfg.cf_records[i]) : String();
            _p(PSTR("<label>Record ID "));
            _s(String(i + 1));
            _p(PSTR("<input maxlength='39' name='record"));
            _s(String(i));
            _p(PSTR("' value='"));
            _s(escape(val));
            _p(PSTR("'></label>"));
        }

        _button(PSTR("Salvar"));
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
        _title(PSTR("Notificacoes ntfy"));

        if (_cfg.ntfy_topic[0])
            _alert(PSTR("success"), String("Topico: ") + _cfg.ntfy_topic);
        else
            _alertP(PSTR("warning"), PSTR("Topico nao configurado - notificacoes desativadas"));

        _p(PSTR("<form action='/ntfy/save' method='POST'>"));
        _csrfField();
        _input(PSTR("Topico ntfy.sh"), PSTR("topic"), _cfg.ntfy_topic,
               PSTR("maxlength='63' pattern='[A-Za-z0-9_\\-]*' placeholder='meu-topico-secreto'"));
        _p(PSTR("<p class='m'>Enviado no boot e quando o IP mudar. "
                "Deixe em branco para desativar.</p>"));
        _button(PSTR("Salvar"));
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
        _title(PSTR("Senha do painel"));
        if (_cfg.webpss.length() == 0)
            _alertP(PSTR("warning"), PSTR("Defina uma senha para liberar o painel"));
        _p(PSTR("<form action='/password/save' method='POST'>"));
        _csrfField();
        _input(PSTR("Usuario"), PSTR("user"), _cfg.webusr.length() ? _cfg.webusr : String("admin"),
               PSTR("required maxlength='32'"));
        _passwordInput(PSTR("Nova senha (min. 8)"), PSTR("pass1"), PSTR("required minlength='8' maxlength='64'"));
        _passwordInput(PSTR("Confirmar senha"), PSTR("pass2"), PSTR("required minlength='8' maxlength='64'"));
        _button(PSTR("Salvar"));
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

    // Sai direto da flash; o cache evita reenviar a cada página. O tipo vai
    // em RAM: send_P faz String(content_type), que não lê da flash.
    void _asset(const char* type, const uint8_t* data, size_t len, bool gz) {
        _server.sendHeader("Cache-Control", "max-age=604800");
        if (gz) _server.sendHeader("Content-Encoding", "gzip");
        _server.send_P(200, type, reinterpret_cast<PGM_P>(data), len);
    }

    void _go(const char* href) {
        _server.sendHeader("Location", href);
        _server.send(303, "text/plain", "");
    }

    void _handleUpdate() {
        if (!_guard()) return;
        _pageBegin(_ota.pending() ? 3 : 0);
        _otaProgressBar();

        _title(PSTR("Atualizacao"));
        _box(PSTR("Firmware"));
        _row(PSTR("Versao atual"), firmware_version);
        _row(PSTR("Automatica"), _cfg.ota_auto ? String("ligada") : String("desligada"));
        if (_ota.availableVersion()[0] != '\0')
            _row(PSTR("Disponivel"), _ota.availableVersion());
        if (_ota.detail()[0] != '\0')
            _row(PSTR("Situacao"), _ota.detail());
        _boxEnd();

        _p(PSTR("<form action='/update/auto' method='POST'>"));
        _csrfField();
        _switch(PSTR("ota_auto"), PSTR("Atualizacao automatica"), _cfg.ota_auto);
        _p(PSTR("<button class='b2'>Salvar</button></form>"));

        _p(PSTR("<form action='/update/check' method='POST'>"));
        _csrfField();
        _button(PSTR("Verificar agora"));

        if (_ota.availableVersion()[0] != '\0' && !_ota.inProgress()) {
            _p(PSTR("<form action='/update/install' method='POST'>"));
            _csrfField();
            _button(PSTR("Instalar esta versao"));
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
        _p(PSTR("<figure><img src='/esp01.webp' alt='ESP-01' width='220' height='209'>"
                "<figcaption class='m'>Firmware para o modulo ESP-01<br>"
                "Foto: SparkFun, CC BY 2.0</figcaption></figure>"));
    }

    void _sendKindList(bool sensors) {
        uint8_t n = sensors ? IoTService::SENSOR_COUNT : IoTService::MODULE_COUNT;
        for (uint8_t i = 0; i < n; i++) {
            if (i) _p(PSTR(","));
            char buf[4];
            snprintf(buf, sizeof(buf), "%u",
                     sensors ? IoTService::sensorKind(i) : IoTService::moduleKind(i));
            _s(buf);
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

    void _selectOpen(PGM_P label, PGM_P id) {
        _labelOpen(label);
        _p(PSTR("<select id='"));
        _p(id);
        _p(PSTR("' name='"));
        _p(id);
        _p(PSTR("' onchange='iotSync()'>"));
    }

    void _selectEnd() { _p(PSTR("</select></label>")); }

    void _divOpen(PGM_P id, bool show) {
        _p(PSTR("<div id='"));
        _p(id);
        _p(show ? PSTR("'>") : PSTR("' style='display:none'>"));
    }

    // name vai para %s: tem de estar em RAM (snprintf_P só lê o formato da flash).
    // Pior caso (threshold_hi, int mínimo, -55, 100000) dá 93 bytes.
    void _number(PGM_P label, const char* name, int value, int min, int max) {
        _labelOpen(label);
        char buf[112];
        snprintf_P(buf, sizeof(buf),
                   PSTR("<input type='number' name='%s' value='%d' min='%d' max='%d'></label>"),
                   name, value, min, max);
        _s(buf);
    }

    // Leitura atual em texto puro: sensor na 1ª linha, módulo na 2ª.
    // O script da página põe em textContent, então não precisa escapar.
    void _handleIotNow() {
        if (!_guard()) return;
        String body(_iot.sensorText());
        body += '\n';
        body += _iot.moduleText();
        _server.sendHeader("Cache-Control", "no-store");
        _server.send(200, "text/plain", body);
    }

    void _handleIot() {
        if (!_guard()) return;
        _pageBegin();
        _title(PSTR("IoT"));
        _p(PSTR("<figure><img src='/pinout.svg' width='1040' height='700' "
                "alt='Pinos do ESP-01, vista de cima: GPIO0, RX (GPIO3), GPIO2, GND; "
                "VCC 3,3 V, RST, CH_PD, TX (GPIO1)'></figure>"));
        _p(PSTR("<p class='m'>"
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
        _row(PSTR("Sensor"), _iot.sensorText(), PSTR("ns"));
        _row(PSTR("Modulo"), _iot.moduleText(), PSTR("nm"));
        _boxEnd();
        // Leitura ao vivo: busca só os dois valores a cada 5 s. Recarregar a
        // página inteira descartava o que estava sendo editado no formulário.
        if (IoTService::allowedPin(_cfg.iot_sensor_pin))
            _p(PSTR("<script>setInterval(function(){fetch('/iot/now')"
                    ".then(function(r){if(r.ok)return r.text()})"
                    ".then(function(t){if(!t)return;t=t.split('\\n');"
                    "document.getElementById('ns').textContent=t[0];"
                    "document.getElementById('nm').textContent=t[1]})"
                    ".catch(function(){})},5000)</script>"));

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
        _selectOpen(PSTR("Pino do sensor"), PSTR("sensor"));
        _pinOptions(_cfg.iot_sensor_pin);
        _selectEnd();
        _selectOpen(PSTR("Pino do modulo"), PSTR("module"));
        _pinOptions(_cfg.iot_module_pin);
        _selectEnd();

        _selectOpen(PSTR("Comando"), PSTR("manual"));
        _choice(PSTR("0"), PSTR("Automatico"), _cfg.iot_manual == IoTService::MANUAL_AUTO);
        _choice(PSTR("1"), PSTR("Ligado"), _cfg.iot_manual == IoTService::MANUAL_ON);
        _choice(PSTR("2"), PSTR("Desligado"), _cfg.iot_manual == IoTService::MANUAL_OFF);
        _selectEnd();

        _selectOpen(PSTR("Tipo do sensor"), PSTR("sensor_type"));
        _typeOptions(true, _cfg.iot_sensor_type);
        _selectEnd();

        _divOpen(PSTR("iot_digital"), !numeric);
        _selectOpen(PSTR("Ligar o modulo quando o sensor estiver"), PSTR("sensor_high"));
        _levelOptions(_cfg.iot_sensor_high);
        _selectEnd();
        _selectOpen(PSTR("Ao pulso"), PSTR("toggle"));
        _choice(PSTR("0"), PSTR("Seguir o nivel"), !_cfg.iot_toggle);
        _choice(PSTR("1"), PSTR("Alternar"), _cfg.iot_toggle);
        _selectEnd();
        _p(PSTR("</div>"));

        _divOpen(PSTR("iot_value"), numeric);
        _divOpen(PSTR("iot_metric"), dht);
        _selectOpen(PSTR("Grandeza"), PSTR("metric"));
        _choice(PSTR("0"), PSTR("Umidade"), _cfg.iot_metric == 0);
        _choice(PSTR("1"), PSTR("Temperatura"), _cfg.iot_metric == 1);
        _selectEnd();
        _p(PSTR("</div>"));
        _selectOpen(PSTR("Ligar quando o valor estiver"), PSTR("above"));
        _choice(PSTR("0"), PSTR("Abaixo de"), !_cfg.iot_above);
        _choice(PSTR("1"), PSTR("Acima de"), _cfg.iot_above);
        _selectEnd();
        _number(PSTR("Limite"), "threshold", _cfg.iot_threshold, -55, 100000);
        _number(PSTR("Margem"), "margin", _cfg.iot_margin, 0, 50);
        _p(PSTR("</div>"));

        _selectOpen(PSTR("Tipo do modulo"), PSTR("module_type"));
        _typeOptions(false, _cfg.iot_module_type);
        _selectEnd();

        _divOpen(PSTR("iot_level"), level);
        _selectOpen(PSTR("Nivel do pino que liga o modulo"), PSTR("module_high"));
        _levelOptions(_cfg.iot_module_high);
        _selectEnd();
        _p(PSTR("</div>"));

        _divOpen(PSTR("iot_servo"), servo);
        _number(PSTR("Angulo ao ligar"), "angle_on", _cfg.iot_angle_on, 0, 180);
        _number(PSTR("Angulo ao soltar"), "angle_off", _cfg.iot_angle_off, 0, 180);
        _p(PSTR("</div>"));

        _divOpen(PSTR("iot_pwm"), pwm);
        _number(PSTR("Potencia ao ligar (%)"), "duty_on", _cfg.iot_duty_on, 0, 100);
        _number(PSTR("Potencia ao soltar (%)"), "duty_off", _cfg.iot_duty_off, 0, 100);
        _p(PSTR("</div>"));

        _divOpen(PSTR("iot_span"), pwm && numeric);
        _selectOpen(PSTR("Curva da potencia"), PSTR("linear"));
        _choice(PSTR("0"), PSTR("Dois patamares"), !_cfg.iot_linear);
        _choice(PSTR("1"), PSTR("Proporcional"), _cfg.iot_linear);
        _selectEnd();
        _number(PSTR("Limite alto"), "threshold_hi", _cfg.iot_threshold_hi, -55, 100000);
        _p(PSTR("</div>"));

        _switch(PSTR("iot_notify"), PSTR("Enviar leitura ao ntfy"), _cfg.iot_notify);
        _button(PSTR("Salvar"));
        _p(PSTR("<script>function iotSync(){var sk=["));
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
        _s(js);
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
