// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Allan Barcelos. ESP8266 IoT: https://github.com/allanbarcelos/esp8266-iot

#pragma once
#include "HttpServer.h"
#include <LittleFS.h>
#include <base64.h>
#include "Config.h"
#include "Logger.h"
#include "Version.h"
#include "ConfigStore.h"
#include "WiFiService.h"
#include "OTAService.h"
#include "IoTService.h"
#include "NtfyNotifier.h"
#include "MqttService.h"
#include "Assets.h"
#include "WebAssets.h"

// ── PROGMEM ───────────────────────────────────────────────────────────────────
// Cabeçalho dividido para permitir injeção de <meta refresh> antes de </head>.
// O CSS (web/panel.css) sai do próprio aparelho, em gzip e com cache: no
// modo AP não há internet para buscar um framework.

static const char _HTML_HEAD_A[] PROGMEM =
    "<!DOCTYPE html><html lang='pt'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>";

static const char _HTML_HEAD_B[] PROGMEM =
    "<link rel='stylesheet' href='/s.css?v=" WEB_CSS_TAG "'>"
    "</head><body><header><b>&#9679; ";

static const char _HTML_HEAD_C[] PROGMEM =
    "</b><nav aria-label='Menu'><a href='/'>Home</a><a href='/status'>Status</a>"
    "<a href='/log'>Logs</a><a href='/cloudflare'>Cloudflare</a><a href='/ntfy'>Ntfy</a><a href='/mqtt'>MQTT</a>"
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
    static constexpr size_t        STATE_JSON_LEN    = 384;
    static constexpr size_t        MIN_CMD_TOPIC     = 20;   // "Gerar" cria 28
    static constexpr uint8_t       FREE_LOGIN_FAILS  = 5;
    static constexpr unsigned long LOGIN_WAIT_MS     = 2000UL;
    static constexpr unsigned long LOGIN_WAIT_MAX_MS = 300000UL;

    WebService(Config& cfg, Logger& log, ConfigStore& store,
               WiFiService& wifi, OTAService& ota, IoTService& iot,
               NtfyNotifier* ntfy = nullptr, MqttService* mqtt = nullptr)
        : _server(80), _cfg(cfg), _log(log), _store(store),
          _wifi(wifi), _ota(ota), _iot(iot), _ntfy(ntfy), _mqtt(mqtt) {
        _csrfToken[0] = '\0';
    }

    void begin() {
        _generateCSRF();
        _server.begin(_dispatch, this);
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
    HttpServer       _server;
    Config&          _cfg;
    Logger&          _log;
    ConfigStore&     _store;
    WiFiService&     _wifi;
    OTAService&      _ota;
    IoTService&      _iot;
    NtfyNotifier*    _ntfy;
    MqttService*     _mqtt;
    uint8_t          _loginFails = 0;
    unsigned long    _loginWaitUntil = 0;

    char _csrfToken[17]; // 16 hex + null

    static void _dispatch(void* self) { static_cast<WebService*>(self)->_route(); }

    // Rota pelo caminho e pelo método; o resto é 404
    void _route() {
        const char* p = _server.path();
        if (_server.method() == HttpServer::GET) {
            if (!strcmp_P(p, PSTR("/")))           return _handleRoot();
            if (!strcmp_P(p, PSTR("/log")))        return _handleLog();
            if (!strcmp_P(p, PSTR("/status")))     return _handleStatus();
            if (!strcmp_P(p, PSTR("/cloudflare"))) return _handleCloudflare();
            if (!strcmp_P(p, PSTR("/ntfy")))       return _handleNtfy();
            if (!strcmp_P(p, PSTR("/mqtt")))       return _handleMqtt();
            if (!strcmp_P(p, PSTR("/password")))   return _handlePassword();
            if (!strcmp_P(p, PSTR("/iot")))        return _handleIot();
            if (!strcmp_P(p, PSTR("/iot/now")))    return _handleIotNow();
            // API local: mesma leitura e mesmos comandos, com token em vez do login
            if (!strcmp_P(p, PSTR("/api/state")))  return _handleApiState();
            if (!strcmp_P(p, PSTR("/update")))     return _handleUpdate();
            // Imagens embutidas (Assets.h): públicas, sem dado do aparelho
            if (!strcmp_P(p, PSTR("/esp01.webp")))
                return _asset("image/webp", ASSET_PHOTO, sizeof(ASSET_PHOTO), false);
            if (!strcmp_P(p, PSTR("/s.css")))
                return _asset("text/css", WEB_CSS_GZ, sizeof(WEB_CSS_GZ), true);
            if (!strcmp_P(p, PSTR("/iot.js")))
                return _asset("text/javascript", WEB_IOT_GZ, sizeof(WEB_IOT_GZ), true);
            if (!strcmp_P(p, PSTR("/pinout.svg")))
                return _asset("image/svg+xml", ASSET_PINOUT_GZ, sizeof(ASSET_PINOUT_GZ), true);
        } else if (_server.method() == HttpServer::POST) {
            if (!strcmp_P(p, PSTR("/save")))            return _handleSave();
            if (!strcmp_P(p, PSTR("/device/save")))     return _handleDeviceSave();
            if (!strcmp_P(p, PSTR("/cloudflare/save"))) return _handleCloudflareSave();
            if (!strcmp_P(p, PSTR("/ntfy/save")))       return _handleNtfySave();
            if (!strcmp_P(p, PSTR("/mqtt/save")))       return _handleMqttSave();
            if (!strcmp_P(p, PSTR("/password/save")))   return _handlePasswordSave();
            if (!strcmp_P(p, PSTR("/iot/save")))        return _handleIotSave();
            if (!strcmp_P(p, PSTR("/iot/cmd")))         return _handleIotCmd();
            if (!strcmp_P(p, PSTR("/iot/token")))       return _handleIotToken();
            if (!strcmp_P(p, PSTR("/iot/clear")))       return _handleIotClear();
            if (!strcmp_P(p, PSTR("/api/cmd")))         return _handleApiCmd();
            if (!strcmp_P(p, PSTR("/update/auto")))     return _handleUpdateAuto();
            if (!strcmp_P(p, PSTR("/update/check")))    return _handleUpdateCheck();
            if (!strcmp_P(p, PSTR("/update/install")))  return _handleUpdateInstall();
        }
        _server.send(404, "text/plain", "");
    }

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
        // cppcheck-suppress knownConditionTrueFalse ; o contador muda entre requisições
        if (_loginBlocked()) return false;
        // Só Basic: o navegador usaria Digest só se o aparelho pedisse
        if (!_authIs("Basic ", base64::encode(_cfg.webusr + ':' + _cfg.webpss).c_str())) {
            _loginFailed();
            _server.sendHeader("WWW-Authenticate", "Basic realm=\"ESP8266\"");
            _server.send(401, "text/plain", "");
            return false;
        }
        _loginFails = 0;
        return true;
    }

    // Senha ou token errados: os primeiros erros passam; depois, cada um
    // dobra a espera (2 s até 5 min), e nesse tempo nada é testado (429).
    // Acertar zera a conta. A conta é do aparelho, não por endereço: não há
    // memória para uma tabela, e de fora de casa o painel fica atrás de um
    // túnel com login próprio (docs/ACESSO-REMOTO.md).
    bool _loginBlocked() {
        if (_loginFails < FREE_LOGIN_FAILS || (long)(millis() - _loginWaitUntil) >= 0) return false;
        _server.send(429, "text/plain", "Muitas tentativas; aguarde");
        return true;
    }

    void _loginFailed() {
        // O primeiro pedido do navegador vem sem credencial: não é erro
        if (_server.authorization().length() == 0) return;
        if (_loginFails < 255) _loginFails++;
        if (_loginFails < FREE_LOGIN_FAILS) return;
        uint8_t doubling = _loginFails - FREE_LOGIN_FAILS;
        unsigned long wait = LOGIN_WAIT_MS << (doubling < 8 ? doubling : 8);
        _loginWaitUntil = millis() + (wait < LOGIN_WAIT_MAX_MS ? wait : LOGIN_WAIT_MAX_MS);
        if (_loginFails == FREE_LOGIN_FAILS) _log.log(PSTR("Login: erros seguidos, espera crescente"));
    }

    bool _checkCSRF() {
        if (_server.arg("_csrf") != _csrfToken) {
            _server.send(403, "text/plain", "Token CSRF invalido");
            return false;
        }
        return true;
    }

    // ── Helpers HTML ──────────────────────────────────────────────────────────

    // Páginas e API: nada em cache (Status mostra a senha do AP) e nada em
    // moldura de outro site (clickjacking)
    void _noStore() {
        _server.sendHeader("Cache-Control", "no-store");
        _server.sendHeader("X-Frame-Options", "DENY");
        _server.sendHeader("X-Content-Type-Options", "nosniff");
    }

    void _pageBegin(int refreshSec = 0, int code = 200) {
        _noStore();
        _server.setContentLength(HttpServer::UNKNOWN_LENGTH);
        _server.send(code, "text/html", "");
        _p(_HTML_HEAD_A);
        _name();
        _p(PSTR("</title>"));
        if (refreshSec > 0) {
            char meta[56];
            snprintf_P(meta, sizeof(meta),
                       PSTR("<meta http-equiv='refresh' content='%d'>"), refreshSec);
            _s(meta);
        }
        _p(_HTML_HEAD_B);
        _name();
        _p(_HTML_HEAD_C);
    }

    // Nome do aparelho no título e no topo; sem nome, o do projeto
    void _name() {
        if (_cfg.dev_name[0]) _s(escape(_cfg.dev_name));
        else _p(PSTR("ESP8266 DNS"));
    }

    void _pageEnd() {
        _p(_HTML_FOOT_A);
        _s(firmware_version);
        _p(_HTML_FOOT_B);
    }

    // Página sem tamanho conhecido: termina quando a conexão fecha
    void _p(PGM_P s) { _server.sendContent_P(s); }
    void _s(const char* s) { _server.sendContent(s); }
    void _s(const String& s) { _server.sendContent(s); }

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
            _row(PSTR("Ultima atualizacao"), _ago(_cfg.lastDnsUpdate != 0, _cfg.lastDnsUpdate));
            _boxEnd();

            _box(PSTR("Aparelho"));
            _p(PSTR("<form action='/device/save' method='POST'>"));
            _csrfField();
            _input(PSTR("Nome <span class='m'>(painel e Home Assistant)</span>"), PSTR("name"),
                   _cfg.dev_name, PSTR("maxlength='31' placeholder='ESP8266 DNS'"));
            _input(PSTR("Hostname <span class='m'>(roteador; vale na proxima conexao)</span>"),
                   PSTR("hostname"), _cfg.hostname,
                   PSTR("maxlength='32' pattern='[A-Za-z0-9]([A-Za-z0-9\\-]*[A-Za-z0-9])?'"));
            _input(PSTR("Fuso horario <span class='m'>(horas do UTC: -3, 0, 5:30)</span>"),
                   PSTR("tz"), _tzText(_cfg.tz),
                   PSTR("maxlength='6' pattern='[+\\-]?[0-9]{1,2}(:[0-5][0-9])?'"));
            _button(PSTR("Salvar"));
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

    // Nome: até 31 bytes, sem aspas, barra invertida nem controle (vai sem
    // escape no JSON do discovery). Hostname: letras, números e hífen, sem
    // hífen nas pontas (RFC 1123).
    void _handleDeviceSave() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;
        String name = _server.arg("name");
        String host = _server.arg("hostname");
        int16_t tz = 0;
        if (!parseTz(_server.arg("tz"), tz)) {
            _errorPage(PSTR("Fuso invalido (de -12 a 14 horas, como -3 ou 5:30)"), PSTR("/"));
            return;
        }
        name.trim();
        host.trim();
        bool ok = name.length() < sizeof(_cfg.dev_name);
        for (size_t i = 0; ok && i < name.length(); i++) {
            unsigned char c = (unsigned char)name.c_str()[i];
            ok = c >= 0x20 && c != '"' && c != '\\' && c != 0x7F;
        }
        if (!ok) {
            _errorPage(PSTR("Nome invalido (ate 31 caracteres, sem aspas)"), PSTR("/"));
            return;
        }
        size_t n = host.length();
        ok = n < sizeof(_cfg.hostname) && (n == 0 || (host.c_str()[0] != '-' && host.c_str()[n - 1] != '-'));
        for (size_t i = 0; ok && i < n; i++) {
            char c = host.c_str()[i];
            ok = isalnum((unsigned char)c) || c == '-';
        }
        if (!ok) {
            _errorPage(PSTR("Hostname invalido (letras, numeros e hifen)"), PSTR("/"));
            return;
        }
        strlcpy(_cfg.dev_name, name.c_str(), sizeof(_cfg.dev_name));
        strlcpy(_cfg.hostname, host.c_str(), sizeof(_cfg.hostname));
        _cfg.tz = tz;
        if (!_store.save()) {
            _errorPage(PSTR("Falha ao salvar configuracao"), PSTR("/"));
            return;
        }
        if (_cfg.hostname[0]) WiFi.hostname(_cfg.hostname);
        if (_mqtt) _mqtt->reconnect();   // o discovery leva o nome novo
        _savedPage(PSTR("Aparelho salvo!"), PSTR("/"));
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
        _row(PSTR("Hora local"), _localTime());
        _row(PSTR("Reset"),   ESP.getResetReason());
        _row(PSTR("Causa"),   ESP.getResetInfo());
        _row(PSTR("Chip ID"), String(ESP.getChipId(), HEX));
        _row(PSTR("Senha do AP " AP_SSID), _wifi.apPassword());
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

        // Consultado a cada 5 min (PublicIPResolver); reverso via DNS da rede
        _box(PSTR("IP Publico"));
        bool ip = _cfg.publicIP[0];
        _row(PSTR("IP"),      ip ? String(_cfg.publicIP) : String("-"));
        _row(PSTR("Reverso"), _cfg.publicHost[0] ? String(_cfg.publicHost) : String("-"));
        _row(PSTR("Mudou"),   _ago(ip, _cfg.ipChanged));
        _row(PSTR("Ultima verificacao"), _ago(ip, _cfg.ipChecked));
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
        _input(PSTR("Record ID"), PSTR("record"), _cfg.cf_record, PSTR("maxlength='39'"));

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
        char record[sizeof(_cfg.cf_record)];

        if (!_argInto("token", token, sizeof(token), "/cloudflare")) return;
        if (!_argInto("zone",  zone,  sizeof(zone),  "/cloudflare")) return;
        if (!_argInto("host",  host,  sizeof(host),  "/cloudflare")) return;
        if (!_argInto("record", record, sizeof(record), "/cloudflare")) return;

        if (token[0]) strlcpy(_cfg.cf_token, token, sizeof(_cfg.cf_token));
        strlcpy(_cfg.cf_zone, zone, sizeof(_cfg.cf_zone));
        strlcpy(_cfg.cf_host, host, sizeof(_cfg.cf_host));
        strlcpy(_cfg.cf_record, record, sizeof(_cfg.cf_record));

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
        if (_cfg.ntfy_cmd[0] && strlen(_cfg.ntfy_cmd) < MIN_CMD_TOPIC && !_cfg.ntfy_token[0])
            _alertP(PSTR("warning"), PSTR("Topico de comandos curto e sem token: quem adivinhar "
                                          "o nome comanda o aparelho"));
        if (_ntfy) {
            _box(PSTR("Comandos pelo ntfy"));
            _row(PSTR("Situacao"), _ntfy->commandStatus());
            _boxEnd();
        }

        _p(PSTR("<form action='/ntfy/save' method='POST'>"));
        _csrfField();
        _input(PSTR("Topico ntfy.sh"), PSTR("topic"), _cfg.ntfy_topic,
               PSTR("maxlength='63' pattern='[A-Za-z0-9_\\-]*' placeholder='meu-topico-secreto'"));
        _p(PSTR("<p class='m'>Recebe o IP no boot e quando ele mudar, e as leituras e "
                "mudancas do IoT. Deixe em branco para desativar.</p>"));
        _input(PSTR("Topico de comandos"), PSTR("cmd"), _cfg.ntfy_cmd,
               PSTR("id='cmd' maxlength='63' pattern='[A-Za-z0-9_\\-]*'"));
        _p(PSTR("<button type='button' class='b2' onclick=\"var a=new Uint8Array(12);"
                "crypto.getRandomValues(a);document.getElementById('cmd').value='cmd-'+"
                "Array.from(a,function(b){return('0'+b.toString(16)).slice(-2)}).join('')\">"
                "Gerar nome aleatorio</button>"
                "<p class='m'>Cada mensagem publicada neste topico vira um comando do atuador "
                "(ligar, desligar, pulso 5, 50%, abrir, auto). Quem souber o nome pode "
                "comandar: use um nome longo e aleatorio, diferente do topico acima, ou um "
                "token de acesso. Em branco, desativado.</p>"));
        _passwordInput(PSTR("Token de acesso do ntfy <span class='m'>(opcional; em branco mantem)"
                            "</span>"), PSTR("token"), PSTR("maxlength='47' placeholder='tk_...'"));
        _switch(PSTR("token_clear"), PSTR("Apagar o token salvo"), false);
        _button(PSTR("Salvar"));
        _pageEnd();
    }

    void _handleNtfySave() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;

        String topic = _server.arg("topic");
        String cmd = _server.arg("cmd");
        String token = _server.arg("token");
        topic.trim();
        cmd.trim();
        token.trim();
        if (topic.length() > 0 && !NtfyNotifier::isValidTopic(topic)) {
            _errorPage(PSTR("Topico invalido (use letras, numeros, _ ou -)"), PSTR("/ntfy"));
            return;
        }
        if (cmd.length() > 0 && !NtfyNotifier::isValidTopic(cmd)) {
            _errorPage(PSTR("Topico de comandos invalido (use letras, numeros, _ ou -)"),
                       PSTR("/ntfy"));
            return;
        }
        // Tópicos do ntfy.sh são públicos pelo nome: sem token, o de comandos
        // precisa ser longo o bastante para não ser adivinhado
        bool hasToken = token.length() > 0 || (_cfg.ntfy_token[0] && !_flag("token_clear"));
        if (cmd.length() > 0 && cmd.length() < MIN_CMD_TOPIC && !hasToken) {
            _errorPage(PSTR("Topico de comandos curto: use Gerar nome aleatorio ou um token"),
                       PSTR("/ntfy"));
            return;
        }
        // A resposta vai ao tópico de dados: o mesmo tópico leria a própria resposta
        if (cmd.length() > 0 && cmd == topic) {
            _errorPage(PSTR("O topico de comandos precisa ser diferente"), PSTR("/ntfy"));
            return;
        }
        bool tokenOk = token.length() < sizeof(_cfg.ntfy_token);
        for (size_t i = 0; tokenOk && i < token.length(); i++) {
            char c = token.c_str()[i];
            tokenOk = isalnum((unsigned char)c) || c == '_';
        }
        if (!tokenOk) {
            _errorPage(PSTR("Token do ntfy invalido"), PSTR("/ntfy"));
            return;
        }
        strlcpy(_cfg.ntfy_topic, topic.c_str(), sizeof(_cfg.ntfy_topic));
        strlcpy(_cfg.ntfy_cmd, cmd.c_str(), sizeof(_cfg.ntfy_cmd));
        if (_flag("token_clear")) _cfg.ntfy_token[0] = '\0';
        else if (token.length()) strlcpy(_cfg.ntfy_token, token.c_str(), sizeof(_cfg.ntfy_token));
        if (!_store.save()) {
            _errorPage(PSTR("Falha ao salvar configuracao"), PSTR("/ntfy"));
            return;
        }
        _savedPage(PSTR("Configuracao ntfy salva!"), PSTR("/ntfy"));
    }

    // ── MQTT ─────────────────────────────────────────────────────────────────

    void _handleMqtt() {
        if (!_guard()) return;
        _pageBegin();
        _title(PSTR("MQTT"));
        if (_mqtt) {
            _box(PSTR("Conexao"));
            _row(PSTR("Situacao"), _mqtt->status());
            _row(PSTR("Topicos"), String(_mqtt->base()) + "/state, /cmd, /avail");
            _boxEnd();
        }
        _p(PSTR("<form action='/mqtt/save' method='POST'>"));
        _csrfField();
        _input(PSTR("Broker <span class='m'>(host ou host:porta)</span>"), PSTR("host"),
               _cfg.mqtt_host, PSTR("maxlength='63' placeholder='192.168.1.10:1883'"));
        _input(PSTR("Usuario"), PSTR("user"), _cfg.mqtt_user,
               PSTR("maxlength='31' autocomplete='off'"));
        _passwordInput(PSTR("Senha <span class='m'>(em branco mantem)</span>"), PSTR("pass"),
                       PSTR("maxlength='63'"));
        _switch(PSTR("pass_clear"), PSTR("Apagar a senha salva"), false);
        _p(PSTR("<p class='m'>O Home Assistant encontra o aparelho sozinho (discovery). "
                "Publica o estado (o mesmo JSON da API) e recebe comandos (o mesmo "
                "texto do painel). Sem TLS: use um broker na rede local, com usuario e senha; "
                "quem publicar no broker comanda o atuador. Use o IP do broker (nomes .local "
                "nao resolvem). Em branco, desativado.</p>"));
        _button(PSTR("Salvar"));
        _pageEnd();
    }

    void _handleMqttSave() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;

        String host = _server.arg("host");
        String user = _server.arg("user");
        String pass = _server.arg("pass");
        host.trim();
        user.trim();
        char h[sizeof(_cfg.mqtt_host)];
        uint16_t port;
        if (host.length() && !MqttService::parseBroker(host.c_str(), h, sizeof(h), port)) {
            _errorPage(PSTR("Broker invalido (use host ou host:porta)"), PSTR("/mqtt"));
            return;
        }
        if (user.length() >= sizeof(_cfg.mqtt_user) || pass.length() >= sizeof(_cfg.mqtt_pass)) {
            _errorPage(PSTR("Usuario ou senha longos demais"), PSTR("/mqtt"));
            return;
        }
        strlcpy(_cfg.mqtt_host, host.c_str(), sizeof(_cfg.mqtt_host));
        strlcpy(_cfg.mqtt_user, user.c_str(), sizeof(_cfg.mqtt_user));
        if (_flag("pass_clear")) _cfg.mqtt_pass[0] = '\0';
        else if (pass.length()) strlcpy(_cfg.mqtt_pass, pass.c_str(), sizeof(_cfg.mqtt_pass));
        if (!_store.save()) {
            _errorPage(PSTR("Falha ao salvar configuracao"), PSTR("/mqtt"));
            return;
        }
        if (_mqtt) _mqtt->reconnect();
        _savedPage(PSTR("Configuracao MQTT salva!"), PSTR("/mqtt"));
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

    // Sai direto da flash; o cache evita reenviar a cada página (CSS e JS
    // levam o hash do conteúdo na URL: versão nova não usa o cache antigo).
    // O tipo vai em RAM: entra no %s do cabeçalho, que não lê da flash.
    void _asset(const char* type, const uint8_t* data, size_t len, bool gz) {
        _server.sendHeader("Cache-Control", "max-age=604800");
        if (gz) _server.sendHeader("Content-Encoding", "gzip");
        _server.send_P(200, type, reinterpret_cast<PGM_P>(data), len);
    }

    // Fuso em texto: "-3", "5:30", "0"
    static String _tzText(int16_t tz) {
        char b[8];
        int a = tz < 0 ? -tz : tz;
        if (a % 60) snprintf(b, sizeof(b), "%s%d:%02d", tz < 0 ? "-" : "", a / 60, a % 60);
        else snprintf(b, sizeof(b), "%d", tz / 60);
        return String(b);
    }

    // "HH:MM" de um minuto do dia
    static String _hhmm(uint16_t m) {
        char b[8];
        snprintf(b, sizeof(b), "%02u:%02u", m / 60u, m % 60u);
        return String(b);
    }

    String _localTime() const {
        uint16_t m;
        return _iot.localMinute(m) ? _hhmm(m) : String("relogio nao sincronizado");
    }

    // "HH:MM" (campo time) em minuto do dia
    static bool parseHhmm(const String& s, uint16_t& out) {
        const char* p = s.c_str();
        if (s.length() != 5 || p[2] != ':') return false;
        for (int i : {0, 1, 3, 4})
            if (p[i] < '0' || p[i] > '9') return false;
        int h = (p[0] - '0') * 10 + (p[1] - '0');
        int m = (p[3] - '0') * 10 + (p[4] - '0');
        if (h > 23 || m > 59) return false;
        out = (uint16_t)(h * 60 + m);
        return true;
    }

public:
    // Fuso em horas, com sinal e minutos opcionais ("-3", "+5:30", "0");
    // vazio é UTC. De -12 a +14 horas.
    static bool parseTz(String s, int16_t& out) {
        s.trim();
        const char* p = s.c_str();
        int sign = 1;
        if (*p == '+' || *p == '-') sign = *p++ == '-' ? -1 : 1;
        int h = 0, m = 0, digits = 0;
        while (*p >= '0' && *p <= '9' && digits < 3) h = h * 10 + (*p++ - '0'), digits++;
        if (*p == ':') {
            p++;
            if (p[0] < '0' || p[0] > '5' || p[1] < '0' || p[1] > '9') return false;
            m = (p[0] - '0') * 10 + (p[1] - '0');
            p += 2;
        }
        if (*p || (digits == 0 && s.length()) || digits > 2) return false;
        int v = sign * (h * 60 + m);
        if (v < -12 * 60 || v > 14 * 60) return false;
        out = (int16_t)v;
        return true;
    }

private:
    // "há quanto tempo" a partir de um millis(); "nunca" se não aconteceu
    static String _ago(bool happened, unsigned long at) {
        return happened ? formatDuration(millis() - at) + " atras" : String("nunca");
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

    // ── IoT ──────────────────────────────────────────────────────────────────

    void _option(const char* value, PGM_P label, bool on) {
        _p(PSTR("<option value='"));
        _s(value);
        _p(on ? PSTR("' selected>") : PSTR("'>"));
        _p(label);
        _p(PSTR("</option>"));
    }

    void _selectOpen(PGM_P label, PGM_P name) {
        _labelOpen(label);
        _p(PSTR("<select name='"));
        _p(name);
        _p(PSTR("'>"));
    }

    void _selectEnd() { _p(PSTR("</select></label>")); }

    // Duas opções 0/1 (nível, sim/não)
    void _pair(PGM_P label, PGM_P name, PGM_P zero, PGM_P one, bool value) {
        _selectOpen(label, name);
        _option("0", zero, !value);
        _option("1", one, value);
        _selectEnd();
    }

    void _pinSelect(PGM_P label, PGM_P name, int8_t current) {
        static const int8_t pins[] = {-1, 2, 0, 1, 3};
        static const char* const values[] = {"-1", "2", "0", "1", "3"};
        static const char* const labels[] = {
            "Escolha", "GPIO2", "GPIO0", "TX (GPIO1)", "RX (GPIO3)"
        };
        _selectOpen(label, name);
        for (int i = 0; i < 5; i++) _option(values[i], labels[i], current == pins[i]);
        _selectEnd();
    }

    void _groupOpen(PGM_P label) {
        _p(PSTR("<optgroup label='"));
        _p(label);
        _p(PSTR("'>"));
    }

    void _sensorSelect(const char* current) {
        _selectOpen(PSTR("Tipo"), PSTR("sensor"));
        for (uint8_t g = 0; g < 3; g++) {
            _groupOpen(iot::SENSOR_GROUPS[g]);
            for (const iot::SensorDef& s : iot::SENSORS)
                if (s.group == g) _option(s.slug, s.label, strcmp(s.slug, current) == 0);
            _p(PSTR("</optgroup>"));
        }
        _selectEnd();
    }

    void _actuatorSelect(const char* current) {
        _selectOpen(PSTR("Tipo"), PSTR("actuator"));
        for (uint8_t cap = iot::C_ONOFF; cap <= iot::C_COLOR; cap++) {
            _groupOpen(iot::ACTUATOR_GROUPS[cap - 1]);
            for (const iot::ActuatorDef& a : iot::ACTUATORS)
                if (a.cap == cap) _option(a.slug, a.label, strcmp(a.slug, current) == 0);
            _p(PSTR("</optgroup>"));
        }
        _selectEnd();
    }

    // name vai para %s: tem de estar em RAM (snprintf_P só lê o formato da flash).
    void _number(PGM_P label, const char* name, int value, int min, int max) {
        _labelOpen(label);
        char buf[112];
        snprintf_P(buf, sizeof(buf),
                   PSTR("<input type='number' name='%s' value='%d' min='%d' max='%d'></label>"),
                   name, value, min, max);
        _s(buf);
    }

    void _time(PGM_P label, PGM_P name, uint16_t minute) {
        _labelOpen(label);
        _p(PSTR("<input type='time' name='"));
        _p(name);
        _p(PSTR("' value='"));
        _s(_hhmm(minute));
        _p(PSTR("'></label>"));
    }

    void _fieldsetOpen(PGM_P id, PGM_P legend) {
        _p(PSTR("<fieldset id='"));
        _p(id);
        _p(PSTR("'><legend>"));
        _p(legend);
        _p(PSTR("</legend>"));
    }

    void _divOpen(PGM_P id) {
        _p(PSTR("<div id='"));
        _p(id);
        _p(PSTR("'>"));
    }

    void _radio(PGM_P value, PGM_P label, bool on) {
        _p(PSTR("<label class='sw'><input type='radio' name='role' value='"));
        _p(value);
        _p(on ? PSTR("' checked>") : PSTR("'>"));
        _p(label);
        _p(PSTR("</label>"));
    }

    // Faixa 0–N com o valor ao lado; solta o dedo e o comando vai
    void _range(PGM_P label, PGM_P id, int value, int max, PGM_P cmd, PGM_P unit) {
        char buf[40];   // "' min='0' max='180' value='180" = 30
        _labelOpen(label);
        _p(PSTR(": <output id='"));
        _p(id);
        _p(PSTR("o'>"));
        snprintf(buf, sizeof(buf), "%d", value);
        _s(buf);
        _p(PSTR("</output>"));
        _p(unit);
        _p(PSTR("<input type='range' id='"));
        _p(id);
        snprintf(buf, sizeof(buf), "' min='0' max='%d' value='%d", max, value);
        _s(buf);
        _p(PSTR("' oninput=\"$(this.id+'o').value=this.value\" onchange=\"c('"));
        _p(cmd);
        _p(PSTR("'+this.value)\"></label>"));
    }

    // Controles do cartão "Agora", de acordo com o que o atuador faz
    void _controls() {
        const iot::ActuatorDef* a = _iot.actuatorDef();
        uint8_t cap = a->cap;
        char buf[32];
        _p(PSTR("<div class='ctl'>"));
        if (cap == iot::C_POSITION) {
            _p(PSTR("<button type='button' onclick=\"c('abrir')\">Abrir</button>"
                    "<button type='button' class='b2' onclick=\"c('fechar')\">Fechar</button>"));
        } else {
            _p(PSTR("<button type='button' role='switch' id='sw' aria-checked='"));
            _p(_iot.actuatorOn() ? PSTR("true") : PSTR("false"));
            _p(PSTR("' onclick=\"c(this.getAttribute('aria-checked')=='true'?'desligar':'ligar')\">"));
            _p(a->label);
            _p(PSTR("</button>"));
        }
        _p(PSTR("<label>Pulso (s)<input type='number' id='pd' min='1' max='3600' value='"));
        snprintf(buf, sizeof(buf), "%u", a->pulse);
        _s(buf);
        _p(PSTR("'></label><button type='button' class='b2' onclick=\"c('pulso '+$('pd').value)\">"
                "Pulso</button>"));
        if (_iot.hasRule()) {
            _p(PSTR("<button type='button' class='b2' id='au' onclick=\"c('auto')\""));
            if (!_iot.manual()) _p(PSTR(" hidden"));
            _p(PSTR(">Voltar ao automatico</button>"));
        }
        _p(PSTR("</div>"));
        if (cap == iot::C_LEVEL || cap == iot::C_COLOR)
            _range(cap == iot::C_LEVEL ? PSTR("Intensidade") : PSTR("Brilho"),
                   PSTR("lv"), _iot.level(), 100, PSTR("nivel "), PSTR("%"));
        if (cap == iot::C_POSITION)
            _range(PSTR("Angulo"), PSTR("ag"), _iot.angle(), 180, PSTR("angulo "), PSTR(" graus"));
        if (cap == iot::C_COLOR) {
            snprintf(buf, sizeof(buf), "%06lx", (unsigned long)_iot.color());
            _p(PSTR("<label>Cor<input type='color' id='co' value='#"));
            _s(buf);
            _p(PSTR("' onchange=\"c('cor '+this.value)\"></label>"));
        }
    }

    void _handleIot() {
        if (!_guard()) return;
        _pageBegin();
        _title(PSTR("IoT"));
        const IoTConfig& t = _cfg.iot;
        if (_iot.unknownType())
            _alertP(PSTR("warning"), PSTR("Tipo desconhecido nesta versao: a parte fica desligada"));
        if (t.notify && _cfg.ntfy_topic[0] == '\0')
            _alertP(PSTR("warning"), PSTR("Cadastre o topico ntfy para enviar a leitura"));

        bool live = _iot.hasSensor() || _iot.hasActuator();
        if (live) {
            _box(PSTR("Agora"));
            if (_iot.hasSensor()) _row(PSTR("Sensor"), _iot.sensorText(), PSTR("ns"));
            if (_iot.hasActuator()) {
                _row(PSTR("Atuador"), _iot.actuatorText(), PSTR("na"));
                if (_iot.hasRule())
                    _row(PSTR("Modo"), _iot.manual() ? String("manual")
                                       : _iot.inHours() ? String("automatico")
                                       : String("automatico, fora do horario"),
                         PSTR("nm"));
            }
            _boxEnd();
            if (_iot.hasActuator()) _controls();
            _p(PSTR("<p id='msg' class='m' role='status' aria-live='polite'></p>"));
        }

        _p(PSTR("<figure><img src='/pinout.svg' width='1040' height='700' "
                "alt='Pinos do ESP-01, vista de cima: GPIO0, RX (GPIO3), GPIO2, GND; "
                "VCC 3,3 V, RST, CH_PD, TX (GPIO1)'></figure>"));

        bool hs = t.sensor[0] && t.sensorPin >= 0;
        bool ha = t.actuator[0] && t.actuatorPin >= 0;
        _p(PSTR("<form action='/iot/save' method='POST'>"));
        _csrfField();
        _fieldsetOpen(PSTR("fp"), PSTR("O que este aparelho faz?"));
        _radio(PSTR("n"), PSTR("Nada (IoT desligado)"), !hs && !ha);
        _radio(PSTR("s"), PSTR("So sensor: mede e avisa"), hs && !ha);
        _radio(PSTR("a"), PSTR("So atuador: obedece comandos"), !hs && ha);
        _radio(PSTR("sa"), PSTR("Sensor e atuador"), hs && ha);
        _p(PSTR("</fieldset>"));

        _fieldsetOpen(PSTR("fs"), PSTR("Sensor"));
        _sensorSelect(t.sensor);
        _pinSelect(PSTR("Pino"), PSTR("sensor_pin"), t.sensorPin);
        _divOpen(PSTR("sd"));
        _pair(PSTR("Ativo com o pino em"), PSTR("sensor_high"),
              PSTR("Nivel baixo"), PSTR("Nivel alto"), t.sensorHigh);
        _input(PSTR("Nome do estado ativo"), PSTR("sensor_on"), t.onText, PSTR("maxlength='15' placeholder='padrao do tipo'"));
        _input(PSTR("Nome do estado inativo"), PSTR("sensor_off"), t.offText,
               PSTR("maxlength='15' placeholder='padrao do tipo'"));
        _p(PSTR("</div>"));
        _divOpen(PSTR("sm"));
        _pair(PSTR("Grandeza usada na regra"), PSTR("metric"),
              PSTR("Umidade"), PSTR("Temperatura"), t.metric == 1);
        _p(PSTR("</div><p class='m' id='sh'></p></fieldset>"));

        _fieldsetOpen(PSTR("fa"), PSTR("Atuador"));
        _actuatorSelect(t.actuator);
        _pinSelect(PSTR("Pino"), PSTR("actuator_pin"), t.actuatorPin);
        _divOpen(PSTR("al"));
        _pair(PSTR("Liga com o pino em"), PSTR("actuator_high"),
              PSTR("Nivel baixo (modulos de rele comuns)"), PSTR("Nivel alto"), t.actuatorHigh);
        _p(PSTR("</div>"));
        _divOpen(PSTR("ap"));
        _number(PSTR("Intensidade ao ligar (%)"), "duty_on", t.dutyOn, 0, 100);
        _number(PSTR("Intensidade desligado (%)"), "duty_off", t.dutyOff, 0, 100);
        _p(PSTR("</div>"));
        _divOpen(PSTR("aa"));
        _number(PSTR("Angulo aberto"), "angle_on", t.angleOn, 0, 180);
        _number(PSTR("Angulo fechado"), "angle_off", t.angleOff, 0, 180);
        _p(PSTR("</div>"));
        _divOpen(PSTR("ac"));
        _number(PSTR("Quantidade de LEDs"), "leds", t.leds, 1, IoTService::MAX_LEDS);
        _p(PSTR("</div>"));
        _number(PSTR("Desligar sozinho depois de (min; 0 = nunca)"), "max_on", t.maxOn, 0, 1440);
        _pair(PSTR("Ao religar o aparelho"), PSTR("restore"),
              PSTR("Desligado"), PSTR("Ultimo comando"), t.restore);
        _p(PSTR("<p class='m' id='ah'></p></fieldset>"));

        _fieldsetOpen(PSTR("fr"), PSTR("Regra"));
        _switch(PSTR("rule"), PSTR("O sensor comanda o atuador (modo automatico)"), t.rule);
        _divOpen(PSTR("rd"));
        _pair(PSTR("Quando o sensor ficar ativo"), PSTR("toggle"),
              PSTR("Ligar enquanto estiver ativo"), PSTR("Inverter o atuador"), t.toggle);
        _p(PSTR("</div>"));
        _divOpen(PSTR("rn"));
        _pair(PSTR("Ligar quando o valor ficar"), PSTR("above"),
              PSTR("Abaixo do limite"), PSTR("Acima do limite"), t.above);
        _number(PSTR("Limite"), "threshold", t.threshold, -999, 9999);
        _number(PSTR("Margem para desligar"), "margin", t.margin, 0, 50);
        _divOpen(PSTR("rl"));
        _pair(PSTR("Intensidade"), PSTR("linear"),
              PSTR("Liga e desliga no limite"), PSTR("Proporcional ate o limite alto"), t.linear);
        _divOpen(PSTR("rh"));
        _number(PSTR("Limite alto"), "threshold_hi", t.thresholdHi, -999, 9999);
        _p(PSTR("</div>"));
        _p(PSTR("</div></div>"));
        _divOpen(PSTR("rt"));
        _switch(PSTR("window"), PSTR("So dentro de um horario (hora local)"), t.window);
        _divOpen(PSTR("rw"));
        _time(PSTR("Das"), PSTR("win_start"), t.winStart);
        _time(PSTR("Ate"), PSTR("win_end"), t.winEnd);
        _p(PSTR("<p class='m'>Fora do horario, o modo automatico deixa o atuador desligado; "
                "um comando manual vale ate auto. Ajuste o fuso na pagina inicial.</p>"));
        _p(PSTR("</div></div><p class='m' id='rs'></p></fieldset>"));

        _switch(PSTR("notify"), PSTR("Enviar leituras e mudancas ao ntfy"), t.notify);
        _button(PSTR("Salvar"));
        if (live) _apiBox();
        if (hs || ha) {
            _p(PSTR("<form action='/iot/clear' method='POST'>"));
            _csrfField();
            _p(PSTR("<button class='b2'>Descadastrar sensor e atuador</button></form>"));
        }

        _p(PSTR("<script>var SK={"));
        for (uint8_t i = 0; i < iot::SENSOR_COUNT; i++) {
            char buf[24];
            snprintf(buf, sizeof(buf), "%s%s:%u", i ? "," : "", iot::SENSORS[i].slug,
                     iot::SENSORS[i].kind);
            _s(buf);
        }
        _p(PSTR("},AC={"));
        for (uint8_t i = 0; i < iot::ACTUATOR_COUNT; i++) {
            char buf[24];
            snprintf(buf, sizeof(buf), "%s%s:%u", i ? "," : "", iot::ACTUATORS[i].slug,
                     iot::ACTUATORS[i].cap);
            _s(buf);
        }
        _p(PSTR("},T='"));
        _s(_csrfToken);
        _p(PSTR("';</script><script src='/iot.js?v=" WEB_IOT_TAG "'></script>"));
        _pageEnd();
    }

    // Lê um inteiro do formulário na faixa; senão responde 400
    bool _int(const char* name, int min, int max, int16_t& out) {
        if (IoTService::parseI16(_server.arg(name), min, max, out)) return true;
        _errorPage(PSTR("Valor invalido"), PSTR("/iot"));
        return false;
    }

    bool _u8(const char* name, int min, int max, uint8_t& out) {
        int16_t v = 0;
        if (!_int(name, min, max, v)) return false;
        out = (uint8_t)v;
        return true;
    }

    bool _flag(const char* name) { return _server.arg(name) == "1"; }

    // Nome de estado: até 15 letras, números, espaço, - _ . ,
    bool _stateText(const char* name, char* dst) {
        String v = _server.arg(name);
        v.trim();
        bool ok = v.length() < 16;
        for (size_t i = 0; ok && i < v.length(); i++) {
            char c = v.c_str()[i];
            ok = isalnum((unsigned char)c) || strchr(" -_.,", c) != nullptr;
        }
        if (!ok) {
            _errorPage(PSTR("Nome de estado invalido"), PSTR("/iot"));
            return false;
        }
        strlcpy(dst, v.c_str(), 16);
        return true;
    }

    bool _pinArg(const char* name, int8_t& out) {
        String v = _server.arg(name);   // temporário dentro do && conta um ramo a mais
        if (IoTService::parsePin(v, out) && iot::allowedPin(out)) return true;
        _errorPage(PSTR("Escolha o pino"), PSTR("/iot"));
        return false;
    }

    void _handleIotSave() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;

        String role = _server.arg("role");
        bool hs = role == "s" || role == "sa";
        bool ha = role == "a" || role == "sa";
        if (!hs && !ha && role != "n") {
            _errorPage(PSTR("Escolha o que o aparelho faz"), PSTR("/iot"));
            return;
        }

        // Parte que não está em uso guarda os ajustes e perde só tipo e pino
        IoTConfig t = _cfg.iot;
        t.sensor[0] = '\0';
        t.sensorPin = -1;
        t.actuator[0] = '\0';
        t.actuatorPin = -1;
        uint8_t kind = iot::K_NONE;
        uint8_t cap = iot::C_NONE;

        if (hs) {
            String slug = _server.arg("sensor");
            int8_t i = iot::sensorIndex(slug.c_str());
            if (i < 0) {
                _errorPage(PSTR("Tipo de sensor invalido"), PSTR("/iot"));
                return;
            }
            strlcpy(t.sensor, slug.c_str(), sizeof(t.sensor));
            kind = iot::SENSORS[i].kind;
            if (!_pinArg("sensor_pin", t.sensorPin)) return;
            if (kind == iot::K_DIGITAL) {
                t.sensorHigh = _flag("sensor_high");
                if (!_stateText("sensor_on", t.onText)) return;
                if (!_stateText("sensor_off", t.offText)) return;
            }
            if (iot::tempAndHumidity(kind) && !_u8("metric", 0, 1, t.metric)) return;
        }

        if (ha) {
            String slug = _server.arg("actuator");
            int8_t i = iot::actuatorIndex(slug.c_str());
            if (i < 0) {
                _errorPage(PSTR("Tipo de atuador invalido"), PSTR("/iot"));
                return;
            }
            strlcpy(t.actuator, slug.c_str(), sizeof(t.actuator));
            cap = iot::ACTUATORS[i].cap;
            if (!_pinArg("actuator_pin", t.actuatorPin)) return;
            if (hs && (t.actuatorPin == t.sensorPin ||
                       (kind == iot::K_SHT3X && t.actuatorPin == I2cBus::sclFor(t.sensorPin)))) {
                _errorPage(PSTR("Sensor e atuador precisam de pinos diferentes"), PSTR("/iot"));
                return;
            }
            if (cap == iot::C_ONOFF) t.actuatorHigh = _flag("actuator_high");
            if (cap == iot::C_LEVEL &&
                (!_u8("duty_on", 0, 100, t.dutyOn) || !_u8("duty_off", 0, 100, t.dutyOff)))
                return;
            if (cap == iot::C_POSITION &&
                (!_u8("angle_on", 0, 180, t.angleOn) || !_u8("angle_off", 0, 180, t.angleOff)))
                return;
            if (cap == iot::C_COLOR && !_u8("leds", 1, IoTService::MAX_LEDS, t.leds)) return;
            int16_t maxOn = 0;
            if (!_int("max_on", 0, 1440, maxOn)) return;
            t.maxOn = (uint16_t)maxOn;
            t.restore = _flag("restore");
        }

        if (hs && ha) {
            t.rule = _flag("rule");
            if (t.rule && kind == iot::K_DIGITAL) t.toggle = _flag("toggle");
            if (t.rule && kind != iot::K_DIGITAL) {
                t.above = _flag("above");
                if (!_int("threshold", -999, 9999, t.threshold)) return;
                if (!_u8("margin", 0, 50, t.margin)) return;
                t.linear = cap == iot::C_LEVEL && _flag("linear");
                if (t.linear) {
                    if (!_int("threshold_hi", -999, 9999, t.thresholdHi)) return;
                    if (t.thresholdHi <= t.threshold) {
                        _errorPage(PSTR("O limite alto precisa passar do limite"), PSTR("/iot"));
                        return;
                    }
                }
            }
            t.window = t.rule && _flag("window");
            if (t.window) {
                String from = _server.arg("win_start");   // temporário no || conta um ramo a mais
                String to = _server.arg("win_end");
                if (!parseHhmm(from, t.winStart) || !parseHhmm(to, t.winEnd)) {
                    _errorPage(PSTR("Horario invalido"), PSTR("/iot"));
                    return;
                }
                if (t.winStart == t.winEnd) {
                    _errorPage(PSTR("O horario precisa de inicio e fim diferentes"), PSTR("/iot"));
                    return;
                }
            }
        }
        t.notify = _flag("notify");
        if ((hs || ha) && !_cfg.api_token[0]) _newToken();

        _cfg.iot = t;
        if (!_store.save()) {
            _errorPage(PSTR("Falha ao salvar configuracao"), PSTR("/iot"));
            return;
        }
        _iot.begin();
        _savedPage(PSTR("IoT configurado"), PSTR("/iot"));
    }

    // Estado em JSON, ou o erro do comando
    void _sendState(uint8_t result) {
        _noStore();
        if (result != IoTService::OK) {
            const char* msg = result == IoTService::INVALID ? "comando invalido"
                            : result == IoTService::UNSUPPORTED ? "comando nao vale para este atuador"
                            : "nenhum atuador configurado";
            _jsonError(400, msg);
            return;
        }
        char buf[STATE_JSON_LEN];
        _iot.stateJson(buf, sizeof(buf));
        _server.send(200, "application/json", buf);
    }

    void _handleIotNow() {
        if (!_guard()) return;
        _sendState(IoTService::OK);
    }

    // Comando do painel (mesmo texto da API e do ntfy)
    void _handleIotCmd() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;
        String cmd = _server.arg("cmd");
        _log.log(PSTR("Painel: comando \"%.32s\""), cmd.c_str());
        _sendState(_iot.command(cmd.c_str(), millis()));
    }

    void _jsonError(int code, const char* msg) {
        _server.send(code, "application/json", String("{\"error\":\"") + msg + "\"}");
    }

    // 32 hex do RNG de hardware
    void _newToken() {
        for (int i = 0; i < 4; i++)
            snprintf(_cfg.api_token + i * 8, 9, "%08lx", (unsigned long)ESP.random());
    }

    // Authorization = esquema + segredo. Comparação em tempo constante: o
    // tempo da resposta não revela o prefixo.
    bool _authIs(const char* scheme, const char* secret) {
        String auth = _server.authorization();
        size_t s = strlen(scheme), n = strlen(secret);
        if (n == 0 || auth.length() != s + n || strncmp(auth.c_str(), scheme, s) != 0)
            return false;
        uint8_t diff = 0;
        for (size_t i = 0; i < n; i++) diff |= (uint8_t)(auth.c_str()[s + i] ^ secret[i]);
        return diff == 0;
    }

    bool _bearerOk() { return _authIs("Bearer ", _cfg.api_token); }

    // A API só existe com sensor ou atuador configurado
    bool _apiGuard() {
        _noStore();
        if (!_iot.hasSensor() && !_iot.hasActuator()) {
            _jsonError(404, "nenhum sensor ou atuador configurado");
            return false;
        }
        // cppcheck-suppress knownConditionTrueFalse ; o contador muda entre requisições
        if (_loginBlocked()) return false;
        if (!_bearerOk()) {
            _loginFailed();
            _server.sendHeader("WWW-Authenticate", "Bearer");
            _jsonError(401, "token invalido");
            return false;
        }
        _loginFails = 0;
        return true;
    }

    void _handleApiState() {
        if (!_apiGuard()) return;
        _sendState(IoTService::OK);
    }

    // Corpo em texto puro (curl -d ligar) ou campo cmd de formulário
    void _handleApiCmd() {
        if (!_apiGuard()) return;
        String cmd = _server.arg("cmd");
        if (cmd.length() == 0) cmd = _server.arg("plain");
        _log.log(PSTR("API: comando \"%.32s\""), cmd.c_str());
        _sendState(_iot.command(cmd.c_str(), millis()));
    }

    void _handleIotToken() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;
        _newToken();
        if (!_store.save()) {
            _errorPage(PSTR("Falha ao salvar configuracao"), PSTR("/iot"));
            return;
        }
        _savedPage(PSTR("Token novo gerado; o anterior deixou de valer"), PSTR("/iot"));
    }

    // Apaga sensor, atuador, regra e token. Pede confirmação numa página
    // própria: funciona sem JavaScript e o leitor de tela anuncia o aviso.
    void _handleIotClear() {
        if (!_guard()) return;
        if (!_checkCSRF()) return;
        if (!_flag("confirm")) {
            _pageBegin();
            _title(PSTR("Descadastrar"));
            _alertP(PSTR("warning"),
                    PSTR("O sensor e o atuador serao apagados com todos os ajustes e a regra. "
                         "O atuador desliga, a API local deixa de responder e o token deixa de valer."));
            _p(PSTR("<form action='/iot/clear' method='POST'>"));
            _csrfField();
            _p(PSTR("<input type='hidden' name='confirm' value='1'>"
                    "<button class='bx'>Sim, descadastrar</button></form>"));
            _backButton(PSTR("/iot"));
            _pageEnd();
            return;
        }
        _cfg.iot = IoTConfig();
        _cfg.api_token[0] = '\0';
        if (!_store.save()) {
            _errorPage(PSTR("Falha ao salvar configuracao"), PSTR("/iot"));
            return;
        }
        LittleFS.remove(IoTState::PATH);
        _iot.begin();
        _log.log(PSTR("IoT: sensor e atuador descadastrados"));
        _savedPage(PSTR("Sensor e atuador descadastrados"), PSTR("/iot"));
    }

    // Token e exemplos para integrar sem internet (scripts, Home Assistant)
    void _apiBox() {
        String ip = WiFi.localIP().toString();
        _box(PSTR("API local"));
        _p(PSTR("<div class='r'><span>Sem internet, na mesma rede: GET /api/state e "
                "POST /api/cmd com o cabecalho Authorization: Bearer e o token.</span></div>"));
        if (_cfg.api_token[0]) {
            _p(PSTR("<div class='r'><label>Token<input readonly value='"));
            _s(_cfg.api_token);
            _p(PSTR("' onclick='this.select()'></label></div><pre>curl -H 'Authorization: Bearer "));
            _s(_cfg.api_token);
            _p(PSTR("' http://"));
            _s(ip);
            _p(PSTR("/api/state\ncurl -H 'Authorization: Bearer "));
            _s(_cfg.api_token);
            _p(PSTR("' -d ligar http://"));
            _s(ip);
            _p(PSTR("/api/cmd</pre>"));
        }
        _boxEnd();
        _p(PSTR("<form action='/iot/token' method='POST'>"));
        _csrfField();
        _p(_cfg.api_token[0] ? PSTR("<button class='b2'>Gerar token novo</button></form>")
                             : PSTR("<button class='b2'>Gerar token</button></form>"));
    }
};
