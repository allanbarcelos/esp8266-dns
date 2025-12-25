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
const char WIFI_FORM_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt">
<head>
<meta charset="utf-8">
<title>ESP8266 Config</title>
<style>
body{font-family:Arial;margin:40px;}
input,button{width:100%;padding:8px;margin:10px 0;}
</style>
</head>
<body>
<h2>Configurar Wi-Fi</h2>
<form action="/save" method="POST">
SSID:<br>
<input name="ssid" required><br>
Senha:<br>
<input name="pass" type="password"><br>
<button type="submit">Salvar</button>
</form>
<hr>
<p>Firmware: )rawliteral" firmware_version R"rawliteral(</p>
</body>
</html>
)rawliteral";

const char WIFI_STATUS_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt">
<head>
<meta charset="utf-8">
<title>Status Wi-Fi</title>
<style>
body{font-family:Arial;margin:40px;}
.box{padding:15px;border:1px solid #ccc;}
a{display:inline-block;margin-top:15px;}
</style>
</head>
<body>
<h2>Wi-Fi Conectado</h2>
<div class="box">
<p><b>Rede:</b> %s</p>
<p><b>IP:</b> %s</p>
</div>
<a href="/reset">Reconfigurar Wi-Fi</a>
<hr>
<p>Firmware: )rawliteral" firmware_version R"rawliteral(</p>
</body>
</html>
)rawliteral";

const char LOG_HTML_HEAD[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta http-equiv="refresh" content="5">
<title>Logs ESP8266</title>
<style>
body{font-family:monospace;background:#111;color:#0f0;padding:10px;}
pre{white-space:pre-wrap;}
</style>
</head>
<body>
<h2>Logs ESP8266 DNS Updater</h2>
<pre>
)rawliteral";

const char LOG_HTML_FOOT[] PROGMEM = R"rawliteral(
</pre>
<p style="color:#888">Atualiza a cada 5 segundos</p>
</body>
</html>
)rawliteral";

const char STATUS_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt">
<head>
<meta charset="utf-8">
<title>Status do Dispositivo</title>
<meta http-equiv="refresh" content="5">
<style>
body{font-family:Arial;background:#111;color:#0f0;padding:20px;}
.box{border:1px solid #0f0;padding:15px;margin-bottom:15px;}
h2{margin-top:0;}
</style>
</head>
<body>

<h2>Status do ESP8266</h2>

<div class="box">
<b>Firmware:</b> %s<br>
<b>Uptime:</b> %lu segundos<br>
<b>Reset reason:</b> %s
</div>

<div class="box">
<b>Heap livre:</b> %u bytes<br>
<b>Heap máximo:</b> %u bytes<br>
<b>Fragmentação:</b> %u %%
</div>

<div class="box">
<b>Flash total:</b> %u bytes<br>
<b>Flash usado:</b> %u bytes<br>
<b>Sketch livre:</b> %u bytes
</div>

<div class="box">
<b>Wi-Fi SSID:</b> %s<br>
<b>IP:</b> %s<br>
<b>RSSI:</b> %d dBm
</div>

<p>Atualiza a cada 5 segundos</p>

</body>
</html>
)rawliteral";


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
        WiFi.softAP("ESP_Config");
        addLog("Modo AP iniciado: ESP_Config");
        return;
    }
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(config.ssid.c_str(), config.pass.c_str());
    
    addLog("Conectando ao Wi-Fi: %s", config.ssid.c_str());
    
    uint8_t attempts = 20;  // ~10 segundos
    while (WiFi.status() != WL_CONNECTED && attempts--) {
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

    // Se não tem SSID salvo, mostra formulário
    if (config.ssid.length() == 0 || WiFi.status() != WL_CONNECTED) {
        server.send_P(200, "text/html", WIFI_FORM_HTML);
        return;
    }

    // HTML dinâmico com SSID e IP
    char page[512];
    snprintf(
        page,
        sizeof(page),
        WIFI_STATUS_HTML,
        WiFi.SSID().c_str(),
        WiFi.localIP().toString().c_str()
    );

    server.send_P(200, "text/html", page);
}

void handleSave() {
    config.ssid = server.arg("ssid");
    config.pass = server.arg("pass");
    saveConfig();
    
    String response = "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
                      "<title>Configuração Salva</title></head><body>"
                      "<h2>Configuração salva!</h2>"
                      "<p>O dispositivo está reiniciando...</p>"
                      "</body></html>";
    
    server.send(200, "text/html", response);
    delay(1000);
    ESP.restart();
}

void handleLog() {
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    
    server.sendContent_P(LOG_HTML_HEAD);
    
    int count = logWrapped ? LOG_BUFFER_SIZE : logIndex;
    int start = logWrapped ? logIndex : 0;
    
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % LOG_BUFFER_SIZE;
        server.sendContent(logBuffer[idx]);
        server.sendContent("\n");
    }
    
    server.sendContent_P(LOG_HTML_FOOT);
}

void handleStatus() {
    char page[1024];

    uint32_t heapFree = ESP.getFreeHeap();
    uint32_t heapMax  = ESP.getMaxFreeBlockSize();
    uint32_t frag     = ESP.getHeapFragmentation();

    uint32_t flashSize = ESP.getFlashChipSize();
    uint32_t sketchSize = ESP.getSketchSize();
    uint32_t sketchFree = ESP.getFreeSketchSpace();

    snprintf(
        page,
        sizeof(page),
        STATUS_HTML,
        firmware_version,
        millis() / 1000,
        ESP.getResetReason().c_str(),
        heapFree,
        heapMax,
        frag,
        flashSize,
        sketchSize,
        sketchFree,
        WiFi.isConnected() ? WiFi.SSID().c_str() : "Desconectado",
        WiFi.isConnected() ? WiFi.localIP().toString().c_str() : "-",
        WiFi.isConnected() ? WiFi.RSSI() : 0
    );

    server.send(200, "text/html", page);
}


void setupWebServer() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/log", HTTP_GET, handleLog);
    server.on("/status", HTTP_GET, handleStatus);
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
    if (WiFi.status() != WL_CONNECTED) {
        addLog("OTA: Wi-Fi não conectado");
        return;
    }
    
    String latestVersion;
    addLog("Verificando atualizações...");
    
    if (!checkVersion(latestVersion)) {
        return;
    }
    
    Serial.printf("Versão atual: %s | Disponível: %s\n", firmware_version, latestVersion.c_str());
    
    if (latestVersion == firmware_version) {
        addLog("Firmware já está atualizado");
        return;
    }
    
    addLog("Nova versão encontrada! Baixando...");
    
    if (!downloadAndUpdateFirmware()) {
        addLog("Falha no update OTA");
        return;
    }
    
    addLog("OTA concluído com sucesso! Reiniciando...");
    delay(1000);
    ESP.restart();
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
    
    if (now > 86400000UL) ESP.restart();

    if (now < OTA_DEADLINE) {
        if (now - lastOTACheck >= OTA_INTERVAL) {
            lastOTACheck = now;
            checkOTA();
        }
    }
}