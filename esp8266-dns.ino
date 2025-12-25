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

const unsigned long dnsUpdateInterval = 300000UL; // 5 min
unsigned long lastDnsUpdate = 0;

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

    char cf_token[48];
    char cf_zone[40];
    char cf_record[40];
    char cf_host[32];
};

struct OTAState {
    bool inProgress = false;
    HTTPClient http;
    WiFiClientSecure client;
    int contentLength = 0;
    size_t written = 0;
} otaState;


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
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP8266</title>

<link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.2/dist/css/bootstrap.min.css" rel="stylesheet">

</head>
<body class="bg-dark text-light">
<div class="container py-3">
)rawliteral";



const char HTML_FOOT[] PROGMEM = R"rawliteral(
<hr class="border-secondary">
<div class="text-secondary small">
Firmware: )rawliteral" firmware_version R"rawliteral(
</div>
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
    server.sendContent(
        "<div class='card bg-black border-success mb-3'>"
        "<div class='card-body text-white'>"
        "<h6 class='card-title text-success'>"
    );
    server.sendContent(title);
    server.sendContent("</h6>");
}

void htmlBoxEnd() {
    server.sendContent("</div></div>");
}




// ========================
// DNS
// ========================

String getPublicIP() {
    WiFiClient client;
    HTTPClient http;
    http.begin(client, "http://api.ipify.org");
    int httpCode = http.GET();
    String ip;
    if (httpCode == HTTP_CODE_OK){
      ip = http.getString();
      ip.trim();
    }
    http.end();
    return ip;
}


String getDNSHostIP(String host) {
    IPAddress resolvedIP;
    return WiFi.hostByName(host.c_str(), resolvedIP) ? resolvedIP.toString() : "";
}

void dnsUpdate(String ip) {
    String url = "https://api.cloudflare.com/client/v4/zones/" + String(config.cf_zone) + "/dns_records/" + String(config.cf_record);
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    http.addHeader("Authorization", "Bearer " + String(config.cf_token));
    http.addHeader("Content-Type", "application/json");
    String payload = "{\"content\":\"" + ip + "\"}";
    int code = http.PATCH(payload);
    if (code > 0) {
        String resp = http.getString();
        
        StaticJsonDocument<512> doc;
        DeserializationError err = deserializeJson(doc, resp);

        if (err) {
            addLog("DNS resposta inválida (JSON)");
        } else {
            bool success = doc["success"] | false;

            if (success) {
                addLog("DNS updated!");
            } else {
                const char* msg = doc["errors"][0]["message"] | "Erro desconhecido";
                int code = doc["errors"][0]["code"] | 0;
                addLog("DNS erro %d: %s", code, msg);
            }
        }


    } else addLog("DNS update error: %d", code);
    http.end();
}

void handleDNSUpdate() {
    String publicIP = getPublicIP();
    String currentDNSIP = getDNSHostIP(String(config.cf_host));
    addLog("PublicIP: %s", publicIP.c_str());
    addLog("CurrentDNSIP: %s", currentDNSIP.c_str());
    if (!publicIP.isEmpty() && !currentDNSIP.isEmpty() && publicIP != currentDNSIP) {
        addLog("Updating DNS...");
        dnsUpdate(publicIP);
    } else addLog("DNS already up-to-date.");
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
    
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, f);
    f.close();
    
    if (error) {
        addLog("Erro ao ler JSON: %s", error.c_str());
        return;
    }
    
    config.ssid = doc["ssid"].as<String>();
    config.pass = doc["pass"].as<String>();

    strlcpy(config.cf_token,  doc["cf_token"]  | "", sizeof(config.cf_token));
    strlcpy(config.cf_zone,   doc["cf_zone"]   | "", sizeof(config.cf_zone));
    strlcpy(config.cf_record, doc["cf_record"] | "", sizeof(config.cf_record));
    strlcpy(config.cf_host,   doc["cf_host"]   | "", sizeof(config.cf_host));

    addLog("Configuração carregada");
}

void saveConfig() {
    StaticJsonDocument<512> doc;

    doc["ssid"] = config.ssid;
    doc["pass"] = config.pass;
    
    doc["cf_token"]  = config.cf_token;
    doc["cf_zone"]   = config.cf_zone;
    doc["cf_record"] = config.cf_record;
    doc["cf_host"]   = config.cf_host;

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

        server.sendContent("<h4 class='text-warning'>Configurar Wi-Fi</h4>");

        server.sendContent("<form action='/save' method='POST'>");

        server.sendContent("<div class='mb-3'>");
        server.sendContent("<label class='form-label'>SSID</label>");
        server.sendContent("<input name='ssid' class='form-control' required>");
        server.sendContent("</div>");

        server.sendContent("<div class='mb-3'>");
        server.sendContent("<label class='form-label'>Senha</label>");
        server.sendContent("<input name='pass' type='password' class='form-control'>");
        server.sendContent("</div>");

        server.sendContent("<button class='btn btn-success w-100'>Salvar</button>");
        server.sendContent("</form>");

        pageEnd();
        return;
    }

    server.sendContent("<h4 class='text-success'>Wi-Fi Conectado</h4>");

    htmlBox("Conexão Wi-Fi");
    server.sendContent("Rede: ");
    server.sendContent(WiFi.SSID());
    server.sendContent("<br>IP: ");
    server.sendContent(WiFi.localIP().toString());
    htmlBoxEnd();

    server.sendContent(
        "<a href='/reset' class='btn btn-outline-danger w-100'>"
        "Reconfigurar Wi-Fi</a>"
    );

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

    server.sendContent("<h4 class='text-warning'>Logs</h4>");
    server.sendContent("<pre class='bg-black text-success p-3 rounded'>");

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

// Função para estimativa de temperatura via ADC (apenas indicativa)
float readChipTemp() {
    int raw = analogRead(A0);                   // valor 0-1023
    float voltage = raw * (3.3 / 1023.0);      // converte para volts
    // mapa aproximado: 0V -> 20°C, 1V -> 40°C, 2V -> 60°C etc
    float temp = voltage * 540.0;       
    return temp;
}

void handleStatus() {
    pageBegin();

    server.sendContent("<h4 class='text-info'>Status do Sistema</h4>");

    // ===== Sistema Básico =====
    htmlBox("Sistema");
    server.sendContent("Uptime: " + String(millis() / 1000) + " s<br>");
    server.sendContent("Reset: " + String(ESP.getResetReason()) + "<br>");
    server.sendContent("Chip ID: " + String(ESP.getChipId()) + "<br>");
    server.sendContent("CPU Freq: " + String(ESP.getCpuFreqMHz()) + " MHz<br>");
    server.sendContent("SDK Version: " + String(ESP.getSdkVersion()) + "<br>");
    server.sendContent("Boot Version: " + String(ESP.getBootVersion()) + "<br>");
    server.sendContent("Boot Mode: " + String(ESP.getBootMode()) + "<br>");
    // Temperatura estimada via ADC
    float chipTemp = readChipTemp();
    server.sendContent("Temp estimada (ADC): " + String(chipTemp, 1) + " °C<br>");
    htmlBoxEnd();

    // ===== Memória =====
    htmlBox("Memória");
    server.sendContent("Heap livre: " + String(ESP.getFreeHeap()) + " bytes<br>");
    server.sendContent("Maior bloco livre: " + String(ESP.getMaxFreeBlockSize()) + " bytes<br>");
    server.sendContent("Fragmentação: " + String(ESP.getHeapFragmentation()) + "%<br>");
    htmlBoxEnd();

    // ===== Flash =====
    htmlBox("Flash");
    server.sendContent("Total: " + String(ESP.getFlashChipSize()) + " bytes<br>");
    server.sendContent("Sketch: " + String(ESP.getSketchSize()) + " bytes<br>");
    server.sendContent("Livre: " + String(ESP.getFreeSketchSpace()) + " bytes<br>");
    htmlBoxEnd();

    // ===== LittleFS =====
    htmlBox("LittleFS");
    FSInfo fs_info;
    LittleFS.info(fs_info);
    server.sendContent("Total: " + String(fs_info.totalBytes) + " bytes<br>");
    server.sendContent("Usado: " + String(fs_info.usedBytes) + " bytes<br>");
    
    // Contagem manual de arquivos
    Dir dir = LittleFS.openDir("/");
    int fileCount = 0;
    while (dir.next()) fileCount++;
    server.sendContent("Arquivos: " + String(fileCount) + "<br>");
    htmlBoxEnd();

    // ===== Wi-Fi =====
    htmlBox("Wi-Fi");
    server.sendContent("SSID: " + (WiFi.isConnected() ? WiFi.SSID() : "Desconectado") + "<br>");
    server.sendContent("IP: " + (WiFi.isConnected() ? WiFi.localIP().toString() : "-") + "<br>");
    server.sendContent("Gateway: " + (WiFi.isConnected() ? WiFi.gatewayIP().toString() : "-") + "<br>");
    server.sendContent("Máscara: " + (WiFi.isConnected() ? WiFi.subnetMask().toString() : "-") + "<br>");
    server.sendContent("DNS: " + (WiFi.isConnected() ? WiFi.dnsIP().toString() : "-") + "<br>");
    server.sendContent("MAC: " + WiFi.macAddress() + "<br>");
    server.sendContent("RSSI: " + String(WiFi.RSSI()) + " dBm<br>");
    server.sendContent("Modo: " + String(WiFi.getMode()) + "<br>");
    htmlBoxEnd();

    // ===== OTA & DNS =====
    htmlBox("Serviços");
    server.sendContent("OTA em progresso: " + String(otaState.inProgress ? "Sim" : "Não") + "<br>");
    server.sendContent("Bytes OTA escritos: " + String(otaState.written) + " / " + String(otaState.contentLength) + "<br>");
    server.sendContent("Última atualização DNS: " + String((millis() - lastDnsUpdate) / 1000) + " s atrás<br>");
    htmlBoxEnd();

    pageEnd();
}

void handleReset() {
    LittleFS.remove("/config.json");
    server.send(200, "text/plain", "Config apagada. Reiniciando...");
    scheduleRestart(1000);
}

void handleCloudflare() {
    pageBegin();
    server.sendContent("<h4 class='text-info'>Cloudflare</h4>");

    bool configured = strlen(config.cf_token) > 0;

    if (!configured) {
        server.sendContent("<form action='/cloudflare/save' method='POST'>");

        server.sendContent("<div class='mb-2'><input class='form-control' name='token' placeholder='API Token' required></div>");
        server.sendContent("<div class='mb-2'><input class='form-control' name='zone' placeholder='Zone ID' required></div>");
        server.sendContent("<div class='mb-2'><input class='form-control' name='record' placeholder='Record ID' required></div>");
        server.sendContent("<div class='mb-2'><input class='form-control' name='host' placeholder='Hostname' required></div>");

        server.sendContent("<button class='btn btn-success w-100'>Salvar</button>");
        server.sendContent("</form>");
    } else {
        htmlBox("Configuração atual");
        server.sendContent("Host: ");
        server.sendContent(config.cf_host);
        server.sendContent("<br>Zone ID: ");
        server.sendContent(config.cf_zone);
        server.sendContent("<br>Record ID: ");
        server.sendContent(config.cf_record);
        server.sendContent("<br>Token: ****");
        htmlBoxEnd();
    }

    pageEnd();
}


void handleCloudflareSave() {
    strlcpy(config.cf_token,  server.arg("token").c_str(),  sizeof(config.cf_token));
    strlcpy(config.cf_zone,   server.arg("zone").c_str(),   sizeof(config.cf_zone));
    strlcpy(config.cf_record, server.arg("record").c_str(), sizeof(config.cf_record));
    strlcpy(config.cf_host,   server.arg("host").c_str(),   sizeof(config.cf_host));

    saveConfig();

    pageBegin();
    server.sendContent("<h2>Cloudflare configurado!</h2>");
    server.sendContent("<a href='/cloudflare'>Voltar</a>");
    pageEnd();
}


void setupWebServer() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/log", HTTP_GET, handleLog);
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/reset", HTTP_GET, handleReset);
    server.on("/cloudflare", HTTP_GET, handleCloudflare);
    server.on("/cloudflare/save", HTTP_POST, handleCloudflareSave);
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

bool startOTA() {
    if (otaState.inProgress) return false;

    otaState.client.setInsecure();
    if (!otaState.http.begin(otaState.client, BIN_URL)) {
        addLog("Falha ao iniciar OTA");
        return false;
    }

    int code = otaState.http.GET();
    if (code != HTTP_CODE_OK) {
        addLog("HTTP OTA erro: %d", code);
        otaState.http.end();
        return false;
    }

    otaState.contentLength = otaState.http.getSize();
    if (otaState.contentLength <= 0) {
        addLog("Tamanho do firmware inválido");
        otaState.http.end();
        return false;
    }

    if (!Update.begin(otaState.contentLength)) {
        addLog("Sem espaço para OTA");
        otaState.http.end();
        return false;
    }

    otaState.written = 0;
    otaState.inProgress = true;
    addLog("OTA iniciado: %d bytes", otaState.contentLength);
    return true;
}


void handleOTANonBlocking() {
    if (!otaState.inProgress) return;

    WiFiClient* stream = otaState.http.getStreamPtr();
    
    // Ler até 512 bytes por vez (não bloqueia)
    if (stream->available()) {
        uint8_t buffer[512];
        int len = stream->readBytes(buffer, sizeof(buffer));
        if (len > 0) {
            size_t writtenNow = Update.write(buffer, len);
            otaState.written += writtenNow;
            addLog("OTA progresso: %zu / %d bytes", otaState.written, otaState.contentLength);
        }
    }

    // Se terminou o download
    if (otaState.written >= (size_t)otaState.contentLength) {
        if (!Update.end()) {
            addLog("Erro no OTA: %s", Update.getErrorString().c_str());
        } else {
            addLog("OTA concluído com sucesso! Reiniciando...");
            scheduleRestart(1000);
        }
        otaState.http.end();
        otaState.inProgress = false;
    }
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
    memset(&config.cf_token, 0, sizeof(config.cf_token));
    memset(&config.cf_zone, 0, sizeof(config.cf_zone));
    memset(&config.cf_record, 0, sizeof(config.cf_record));
    memset(&config.cf_host, 0, sizeof(config.cf_host));

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
        if(!otaState.inProgress){
            if (ESP.getFreeHeap() < 15000 || ESP.getHeapFragmentation() > 35) {
                addLog("Memória crítica detectada");
                scheduleRestart(1000);
            }
        }
    }
    
    // Daily Restart
    if (now > 86400000UL && !restartPending) {
        addLog("Reinício diário agendado");
        scheduleRestart(1000);
    }

    if (!otaState.inProgress && !restartPending && now < OTA_DEADLINE && now - lastOTACheck >= OTA_INTERVAL) {
        lastOTACheck = now;
        String latestVersion;
        if (checkVersion(latestVersion) && latestVersion != firmware_version) {
            addLog("Nova versão encontrada: %s", latestVersion.c_str());
            startOTA();  // inicializa OTA não bloqueante
        }
    }

    // Processa OTA sem bloquear
    handleOTANonBlocking();

    if (strlen(config.cf_token) > 0 && now - lastDnsUpdate >= dnsUpdateInterval) {
        lastDnsUpdate = now;
        handleDNSUpdate();
    }
}
