#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <Updater.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <stdarg.h>

#ifndef firmware_version
#define firmware_version "dev"
#endif

// ========================
// CONSTANTES
// ========================
const char* VERSION_URL = "https://raw.githubusercontent.com/allanbarcelos/esp8266-dns/main/firmware/version.txt";
const char* BIN_URL     = "https://raw.githubusercontent.com/allanbarcelos/esp8266-dns/main/firmware/firmware.bin";
const unsigned long OTA_INTERVAL = 60000UL;  // 1 minuto
const unsigned long OTA_DEADLINE = 82800000UL;  // ~23h00min
const uint16_t LOG_BUFFER_SIZE = 10;
const uint16_t LOG_LINE_SIZE = 128;

bool restartPending = false;
unsigned long restartAt = 0;

bool otaInProgress = false;

// ========================
// DECLARAÇÕES DE OBJETOS
// ========================
ESP8266WebServer server(80);

// ========================
// ESTRUTURAS DE DADOS
// ========================
struct Config {
    String ssid;
    String pass;
};

// ========================
// VARIÁVEIS GLOBAIS
// ========================
Config config;
unsigned long lastOTACheck = 0;
char logBuffer[LOG_BUFFER_SIZE][LOG_LINE_SIZE];
int logIndex = 0;
bool logWrapped = false;

// ========================
// PROGMEM HTML
// ========================
const char HTML_HEAD[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP8266</title>
<style>
body{
    font-family:Arial;
    background:#111;
    color:#0f0;
    padding:20px;
}
h2{margin-top:0;}
.box{
    border:1px solid #0f0;
    padding:15px;
    margin-bottom:15px;
}
input,button{
    width:100%;
    padding:8px;
    margin:8px 0;
}
a{
    color:#0f0;
    text-decoration:none;
}
a:hover{text-decoration:underline;}
.footer{
    margin-top:20px;
    color:#888;
    font-size:12px;
}
</style>
</head>
<body>
)rawliteral";

const char HTML_FOOT[] PROGMEM = R"rawliteral(
<div class="footer">
Firmware: )rawliteral" firmware_version R"rawliteral(
</div>
</body>
</html>
)rawliteral";

// ========================
// COMPONENTS
// ========================

void pageBegin() {
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    server.sendContent_P(HTML_HEAD);
}

void pageEnd() {
    server.sendContent_P(HTML_FOOT);
}

void htmlBox(const char* title) {
    server.sendContent("<div class='box'><b>");
    server.sendContent(title);
    server.sendContent("</b><br>");
}

void htmlBoxEnd() {
    server.sendContent("</div>");
}


// ========================
// Schedule Function
// ========================

void scheduleRestart(unsigned long ms = 1000) {
    if (restartPending) return;
    addLog("Reinício agendado em %lu ms", ms);
    restartPending = true;
    restartAt = millis() + ms;
}

// ========================
// FUNÇÕES DE LOG
// ========================
void addLog(const char *format, ...) {
    char msg[LOG_LINE_SIZE];
    va_list args;
    
    va_start(args, format);
    vsnprintf(msg, sizeof(msg), format, args);
    va_end(args);
    
    snprintf(
        logBuffer[logIndex],
        LOG_LINE_SIZE,
        "[%lus] %s",
        millis() / 1000,
        msg
    );
    
    Serial.println(logBuffer[logIndex]);
    
    logIndex++;
    if (logIndex >= LOG_BUFFER_SIZE) {
        logIndex = 0;
        logWrapped = true;
    }
}

// ========================
// FUNÇÕES DO FILESYSTEM
// ========================
void loadConfig() {
    if (!LittleFS.exists("/config.json")) {
        addLog("Arquivo de config não encontrado");
        return;
    }
    
    File f = LittleFS.open("/config.json", "r");
    if (!f) {
        addLog("Erro ao abrir config.json");
        return;
    }
    
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, f);
    f.close();
    
    if (error) {
        addLog("Erro ao ler JSON: %s", error.c_str());
        return;
    }
    
    config.ssid = doc["ssid"].as<String>();
    config.pass = doc["pass"].as<String>();
    addLog("Configuração carregada");
}

void saveConfig() {
    StaticJsonDocument<256> doc;
    doc["ssid"] = config.ssid;
    doc["pass"] = config.pass;
    
    File f = LittleFS.open("/config.json", "w");
    if (!f) {
        addLog("Erro ao salvar config");
        return;
    }
    
    serializeJson(doc, f);
    f.close();
    addLog("Configuração salva");
}

// ========================
// FUNÇÕES DE WIFI
// ========================
void startWiFi() {
    if (config.ssid.length() == 0) {
        WiFi.mode(WIFI_AP);
        WiFi.softAP("ESP_Config");
        addLog("Modo AP iniciado: ESP_Config");
        return;
    }
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(config.ssid.c_str(), config.pass.c_str());
    
    addLog("Conectando ao Wi-Fi: %s", config.ssid.c_str());
    
    uint8_t attempts = 20;  // ~10 segundos
    while (WiFi.status() != WL_CONNECTED && attempts--) {
        yield();
        delay(500);
        Serial.print(".");
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        addLog("Conectado! IP: %s", WiFi.localIP().toString().c_str());
    } else {
        addLog("Falha na conexão. Iniciando modo AP");
        WiFi.mode(WIFI_AP);
        WiFi.softAP("ESP_Config");
    }
}

// ========================
// HANDLERS WEB
// ========================
void handleRoot() {
    pageBegin();

    if (config.ssid.length() == 0 || WiFi.status() != WL_CONNECTED) {

        server.sendContent("<h2>Configurar Wi-Fi</h2>");
        server.sendContent("<form action='/save' method='POST'>");
        server.sendContent("SSID:<br><input name='ssid' required>");
        server.sendContent("Senha:<br><input name='pass' type='password'>");
        server.sendContent("<button>Salvar</button>");
        server.sendContent("</form>");

        pageEnd();
        return;
    }

    server.sendContent("<h2>Wi-Fi Conectado</h2>");
    htmlBox("Conexão");
    server.sendContent("Rede: ");
    server.sendContent(WiFi.SSID());
    server.sendContent("<br>IP: ");
    server.sendContent(WiFi.localIP().toString());
    htmlBoxEnd();

    server.sendContent("<a href='/reset'>Reconfigurar Wi-Fi</a>");

    pageEnd();
}


void handleSave() {
    config.ssid = server.arg("ssid");
    config.pass = server.arg("pass");
    saveConfig();

    pageBegin();
    server.sendContent("<h2>Configuração salva!</h2>");
    server.sendContent("<p>Reiniciando...</p>");
    pageEnd();

    scheduleRestart(1000);
}


void handleLog() {
    pageBegin();
    server.sendContent("<h2>Logs</h2><pre>");

    int count = logWrapped ? LOG_BUFFER_SIZE : logIndex;
    int start = logWrapped ? logIndex : 0;

    for (int i = 0; i < count; i++) {
        int idx = (start + i) % LOG_BUFFER_SIZE;
        server.sendContent(logBuffer[idx]);
        server.sendContent("\n");
    }

    server.sendContent("</pre>");
    pageEnd();
}


void handleStatus() {
    pageBegin();
    server.sendContent("<h2>Status do ESP8266</h2>");

    htmlBox("Sistema");
    server.sendContent("Uptime: ");
    server.sendContent(String(millis()/1000));
    server.sendContent(" s<br>Reset: ");
    server.sendContent(ESP.getResetReason());
    htmlBoxEnd();

    htmlBox("Memória");
    server.sendContent("Heap livre: ");
    server.sendContent(String(ESP.getFreeHeap()));
    server.sendContent(" bytes<br>Fragmentação: ");
    server.sendContent(String(ESP.getHeapFragmentation()));
    server.sendContent("%");
    htmlBoxEnd();

    htmlBox("Flash");
    server.sendContent("Total: ");
    server.sendContent(String(ESP.getFlashChipSize()));
    server.sendContent("<br>Sketch: ");
    server.sendContent(String(ESP.getSketchSize()));
    server.sendContent("<br>Livre: ");
    server.sendContent(String(ESP.getFreeSketchSpace()));
    htmlBoxEnd();

    htmlBox("Wi-Fi");
    server.sendContent("SSID: ");
    server.sendContent(WiFi.isConnected() ? WiFi.SSID() : "Desconectado");
    server.sendContent("<br>IP: ");
    server.sendContent(WiFi.isConnected() ? WiFi.localIP().toString() : "-");
    server.sendContent("<br>RSSI: ");
    server.sendContent(String(WiFi.RSSI()));
    server.sendContent(" dBm");
    htmlBoxEnd();

    pageEnd();
}

void handleReset() {
    LittleFS.remove("/config.json");
    server.send(200, "text/plain", "Config apagada. Reiniciando...");
    scheduleRestart(1000);
}

void setupWebServer() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/log", HTTP_GET, handleLog);
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/reset", HTTP_GET, handleReset);
    server.begin();
    addLog("Servidor web iniciado na porta 80");
}

// ========================
// FUNÇÕES OTA
// ========================
bool checkVersion(String& latestVersion) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    
    String url = String(VERSION_URL) + "?t=" + String(millis());
    if (!http.begin(client, url)) {
        addLog("Falha ao conectar para verificar versão");
        return false;
    }
    
    // HEADERS ANTI-CACHE
    http.addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    http.addHeader("Pragma", "no-cache");
    http.addHeader("Expires", "0");

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        addLog("Erro HTTP na versão: %d", code);
        http.end();
        return false;
    }
    
    latestVersion = http.getString();
    latestVersion.trim();
    http.end();
    
    return true;
}

bool downloadAndUpdateFirmware() {
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.useHTTP10(true);          // CRÍTICO
    http.setTimeout(20000);

    if (!http.begin(client, BIN_URL)) {
        addLog("Falha ao conectar para download");
        return false;
    }
    
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        addLog("Erro HTTP no download: %d", code);
        http.end();
        return false;
    }
    
    int contentLength = http.getSize();
    if (contentLength <= 0) {
        addLog("Tamanho do firmware inválido");
        http.end();
        return false;
    }
    
    addLog("Tamanho do binário: %d bytes", contentLength);
    
    if (!Update.begin(contentLength)) {
        addLog("Espaço insuficiente para OTA");
        Update.printError(Serial);
        http.end();
        return false;
    }
    
    WiFiClient* stream = http.getStreamPtr();
    size_t written = Update.writeStream(*stream);
    
    if (written != (size_t)contentLength) {
        addLog("Escrita parcial: %zu / %d bytes", written, contentLength);
        Update.end(false);
        http.end();
        return false;
    }
    
    if (!Update.end()) {
        addLog("Erro no update: %s", Update.getErrorString().c_str());
        http.end();
        return false;
    }
    
    http.end();
    return true;
}

void checkOTA() {

    if (otaInProgress || restartPending) return;

    otaInProgress = true;

    if (WiFi.status() != WL_CONNECTED) {
        addLog("OTA: Wi-Fi não conectado");
        otaInProgress = false;
        return;
    }
    
    String latestVersion;
    addLog("Verificando atualizações...");
    
    if (!checkVersion(latestVersion)) {
        otaInProgress = false;
        return;
    }
    
    Serial.printf("Versão atual: %s | Disponível: %s\n", firmware_version, latestVersion.c_str());
    
    if (latestVersion == firmware_version) {
        addLog("Firmware já está atualizado");
        otaInProgress = false;
        return;
    }
    
    addLog("Nova versão encontrada! Baixando...");
    
    if (!downloadAndUpdateFirmware()) {
        addLog("Falha no update OTA");
        otaInProgress = false;
        return;
    }
    
    addLog("OTA concluído com sucesso! Reiniciando...");
    otaInProgress = false;
    scheduleRestart(1000);
}

// ========================
// SETUP E LOOP
// ========================
void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n\n=== Inicializando ESP8266 DNS Updater ===");
    Serial.printf("Versão: %s\n", firmware_version);
    
    // Inicializar LittleFS
    if (!LittleFS.begin()) {
        Serial.println("ERRO: Falha ao montar LittleFS");
        addLog("Falha ao montar LittleFS");
    } else {
        addLog("LittleFS inicializado");
    }
    
    // Carregar configurações
    loadConfig();
    
    // Conectar Wi-Fi
    startWiFi();
    
    // Configurar servidor web
    setupWebServer();
    
    addLog("Sistema inicializado");
}

void loop() {
    server.handleClient();
    unsigned long now = millis();

    // Executa reboot sem bloquear
    if (restartPending && (long)(now - restartAt) >= 0) {
        ESP.restart();
    }

    // Watchdog de memória
    if (!restartPending) {
       if (ESP.getFreeHeap() < 15000 || ESP.getHeapFragmentation() > 35) {
           addLog("Memória crítica detectada");
           scheduleRestart(1000);
       }
    }
    
    // Daily Restart
    if (now > 86400000UL && !restartPending) {
        addLog("Reinício diário agendado");
        scheduleRestart(1000);
    }


    if (now < OTA_DEADLINE && now - lastOTACheck >= OTA_INTERVAL) {
        lastOTACheck = now;
        checkOTA();
    }
}
