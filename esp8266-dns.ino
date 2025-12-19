#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <Updater.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <stdarg.h>

// ============================================
// DEFINIÇÕES E CONSTANTES
// ============================================

// Firmware
const char* github_api = "https://api.github.com/repos/allanbarcelos/esp8266-dns/releases/latest";

#ifndef firmware_version
  #define firmware_version "dev"
#endif

// LOG
#define LOG_BUFFER_SIZE 10
#define LOG_LINE_SIZE   128 

String logBuffer[LOG_BUFFER_SIZE];
int logIndex = 0;
bool logWrapped = false;

// Intervalos de tempo (em milissegundos)
const unsigned long CHECK_INTERVAL = 3600000UL;       // 1 hora
const unsigned long DNS_UPDATE_INTERVAL = 300000UL;   // 5 minutos
const unsigned long RECONNECT_DELAY = 5000;           // 5 segundos
const unsigned long WAIT_AFTER_FAILS = 1800000UL;     // 30 minutos
const unsigned long WIFI_TEST_TIMEOUT = 10000;        // 10 segundos
const unsigned long DAILY_REBOOT_INTERVAL = 86400000UL; // 24 horas
unsigned long bootTime;

// Limites de tentativas
const int MAX_RECONNECT_ATTEMPTS = 5;
const int MAX_REBOOTS_BEFORE_WAIT = 3;

// Variáveis globais
unsigned long lastCheck = 0;
unsigned long dnsLastUpdate = 0;
unsigned long lastReconnectAttempt = 0;
unsigned long waitStart = 0;
unsigned long wifiTestStart = 0;

int rebootFailCount = 0;
int reconnectAttempts = 0;

bool testingWiFi = false;
String testSSID = "";
String testPass = "";

// Enumerações
enum WifiConnectionState { 
  WIFI_OK, 
  WIFI_DISCONNECTED, 
  WIFI_RECONNECTING, 
  WIFI_WAIT 
};
WifiConnectionState wifiConnectionState = WIFI_OK;

// Estruturas de dados
struct Config {
  String wifi_ssid;
  String wifi_pass;
  String CF_TOKEN;
  String CF_ZONE;
  String CF_RECORD;
  String CF_HOST;
  String web_user;
  String web_pass;
};

Config config;

// Objetos globais
ESP8266WebServer server(80);

// ============================================
// PROTÓTIPOS DE FUNÇÕES
// ============================================

// Configuração e inicialização
void setup();
void initializeFileSystem();
void loadConfiguration();
void saveConfiguration();
void initializeWiFi();

// Gerenciamento de Wi-Fi
void handleWiFi();
void handleWiFiTest();
void attemptReconnection();
void enterWaitMode();
void resetFailureCounter();

// Servidor Web
void handleRoot();
void handleSave();
void startWiFiTest(const String& ssid, const String& password);

// DNS e OTA
void handleDNSUpdate();
void checkForUpdate();
void updateDNSRecord(const String& ipAddress);
String getPublicIP();
String getDNSRecordIP(const String& hostname);

// Utilitários
void performDailyReboot();

//
bool authenticate();
bool hasWebPassword();
void showCreatePasswordPage();

// Log
void addLog(const char *format, ...);

// ============================================
// CONFIGURAÇÃO E INICIALIZAÇÃO
// ============================================

void setup() {
  Serial.begin(115200);
  addLog("Iniciando ESP8266 DNS Updater...");

  //
  bootTime = millis();

  // Inicializa sistemas
  initializeFileSystem();
  loadConfiguration();
  
  // Inicializa EEPROM e carrega contador de falhas
  EEPROM.begin(512);
  rebootFailCount = EEPROM.read(0);
  addLog("Tentativas de reinício falhas anteriores: %d", rebootFailCount);

  // Verifica se deve entrar em modo de espera
  if (rebootFailCount >= MAX_REBOOTS_BEFORE_WAIT) {
    addLog("Muitas falhas consecutivas. Entrando em modo de espera...");
    wifiConnectionState = WIFI_WAIT;
    waitStart = millis();
  } else {
    initializeWiFi();
  }

  // Configura rotas do servidor web
  server.on("/", handleRoot);
  server.on("/save", handleSave);

  server.on("/setpass", HTTP_POST, []() {

    if (hasWebPassword()) {
      server.send(403, "text/plain", "Senha já definida");
      return;
    }

    if (!server.hasArg("user") || !server.hasArg("pass")) {
      server.send(400, "text/plain", "Dados inválidos");
      return;
    }

    config.web_user = server.arg("user");
    config.web_pass = server.arg("pass");

    saveConfiguration();

    server.send(200, "text/html",
      "<h2>Senha definida com sucesso!</h2>"
      "<p>Recarregue a página para acessar.</p>"
    );
  });


  server.on("/log", []() {

    if (hasWebPassword()) {
      if (!authenticate()) return;
    }

    String html =
      "<!DOCTYPE html><html><head>"
      "<meta charset='UTF-8'>"
      "<meta http-equiv='refresh' content='5'>"  // auto refresh
      "<title>Logs ESP8266</title>"
      "<style>"
      "body{font-family:monospace;background:#111;color:#0f0;padding:10px;}"
      "pre{white-space:pre-wrap;}"
      "</style>"
      "</head><body>"
      "<h2>Logs ESP8266 DNS Updater</h2>"
      "<pre>";

    if (logWrapped) {
      for (int i = logIndex; i < LOG_BUFFER_SIZE; i++) {
        html += logBuffer[i] + "\n";
      }
    }

    for (int i = 0; i < logIndex; i++) {
      html += logBuffer[i] + "\n";
    }

    html +=
      "</pre>"
      "<p style='color:#888'>Atualiza a cada 5 segundos</p>"
      "</body></html>";

    server.send(200, "text/html", html);
  });


  server.begin();
  
  addLog("Servidor HTTP inicializado na porta 80");
}

void initializeFileSystem() {
  if (!LittleFS.begin()) {
    addLog("Falha ao montar sistema de arquivos LittleFS");
    return;
  }
  addLog("Sistema de arquivos LittleFS montado com sucesso");
}

void loadConfiguration() {
  if (!LittleFS.exists("/config.json")) {
    addLog("Arquivo de configuração não encontrado");
    return;
  }

  File configFile = LittleFS.open("/config.json", "r");
  if (!configFile) {
    addLog("Falha ao abrir arquivo de configuração");
    return;
  }

  StaticJsonDocument<512> jsonDoc;
  DeserializationError error = deserializeJson(jsonDoc, configFile);
  configFile.close();

  if (error) {
    addLog("Erro ao analisar configuração: %s", error.c_str());
    return;
  }

  // Carrega valores da configuração
  config.wifi_ssid = jsonDoc["wifi_ssid"].as<String>();
  config.wifi_pass = jsonDoc["wifi_pass"].as<String>();
  config.CF_TOKEN = jsonDoc["CF_TOKEN"].as<String>();
  config.CF_ZONE = jsonDoc["CF_ZONE"].as<String>();
  config.CF_RECORD = jsonDoc["CF_RECORD"].as<String>();
  config.CF_HOST = jsonDoc["CF_HOST"].as<String>();

  config.web_user = jsonDoc["web_user"] | "";
  config.web_pass = jsonDoc["web_pass"] | "";

  addLog("Configuração carregada com sucesso");
}

void saveConfiguration() {
  StaticJsonDocument<512> jsonDoc;
  
  jsonDoc["wifi_ssid"] = config.wifi_ssid;
  jsonDoc["wifi_pass"] = config.wifi_pass;
  jsonDoc["CF_TOKEN"] = config.CF_TOKEN;
  jsonDoc["CF_ZONE"] = config.CF_ZONE;
  jsonDoc["CF_RECORD"] = config.CF_RECORD;
  jsonDoc["CF_HOST"] = config.CF_HOST;

  jsonDoc["web_user"] = config.web_user;
  jsonDoc["web_pass"] = config.web_pass;

  File configFile = LittleFS.open("/config.json", "w");
  if (!configFile) {
    addLog("Falha ao abrir arquivo para salvar configuração");
    return;
  }
  
  serializeJson(jsonDoc, configFile);
  configFile.close();
  
  addLog("Configuração salva com sucesso");
}

void initializeWiFi() {
  if (config.wifi_ssid.length() > 0) {
    WiFi.hostname("ESP8266_DNS");
    WiFi.begin(config.wifi_ssid.c_str(), config.wifi_pass.c_str());
    wifiConnectionState = WIFI_RECONNECTING;
  } else {
    WiFi.softAP("ESP_Config");
    addLog("AP criado. SSID: ESP_Config, IP: %s", 
                  WiFi.softAPIP().toString().c_str());
    wifiConnectionState = WIFI_DISCONNECTED;
  }
}

// ============================================
// LOOP PRINCIPAL
// ============================================

void loop() {
  unsigned long currentTime = millis();
  
  // Processa requisições do servidor web
  server.handleClient();
  
  // Gerencia conexão Wi-Fi
  handleWiFi();
  handleWiFiTest();
  
  // Reboot diário
  if (millis() - bootTime >= DAILY_REBOOT_INTERVAL) {
    performDailyReboot();
  }
  
  // Verifica atualizações OTA
  if (wifiConnectionState == WIFI_OK && 
      (currentTime - lastCheck >= CHECK_INTERVAL || lastCheck == 0)) {
    lastCheck = currentTime;
    checkForUpdate();
  }
  
  // Atualiza registro DNS se necessário
  if (wifiConnectionState == WIFI_OK && 
      (currentTime - dnsLastUpdate >= DNS_UPDATE_INTERVAL || dnsLastUpdate == 0)) {
    dnsLastUpdate = currentTime;
    handleDNSUpdate();
  }
}

// ============================================
// GERENCIAMENTO DE WI-FI
// ============================================

void handleWiFi() {
  unsigned long currentTime = millis();
  
  switch (wifiConnectionState) {
    case WIFI_OK:
      if (WiFi.status() != WL_CONNECTED) {
        addLog("Conexão Wi-Fi perdida");
        wifiConnectionState = WIFI_RECONNECTING;
        reconnectAttempts = 0;
        lastReconnectAttempt = 0;
      }
      break;
      
    case WIFI_RECONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        addLog("Reconectado com sucesso!");
        resetFailureCounter();
        wifiConnectionState = WIFI_OK;
        break;
      }
      
      if (currentTime - lastReconnectAttempt >= RECONNECT_DELAY) {
        attemptReconnection();
      }
      break;
      
    case WIFI_WAIT:
      if (currentTime - waitStart >= WAIT_AFTER_FAILS) {
        addLog("Tempo de espera concluído. Tentando reconectar...");
        resetFailureCounter();
        WiFi.begin(config.wifi_ssid.c_str(), config.wifi_pass.c_str());
        wifiConnectionState = WIFI_RECONNECTING;
      }
      break;
      
    case WIFI_DISCONNECTED:
      // Modo AP ativo, aguardando configuração
      break;
  }
}

void attemptReconnection() {
  unsigned long currentTime = millis();
  lastReconnectAttempt = currentTime;
  reconnectAttempts++;
  
  addLog("Tentativa de reconexão %d/%d...", 
                reconnectAttempts, MAX_RECONNECT_ATTEMPTS);
  
  WiFi.disconnect();
  WiFi.begin(config.wifi_ssid.c_str(), config.wifi_pass.c_str());
  
  if (reconnectAttempts >= MAX_RECONNECT_ATTEMPTS) {
    rebootFailCount++;
    EEPROM.write(0, rebootFailCount);
    EEPROM.commit();
    
    if (rebootFailCount >= MAX_REBOOTS_BEFORE_WAIT) {
      addLog("Muitas falhas. Entrando em modo de espera...");
      enterWaitMode();
    } else {
      addLog("Falha total nas reconexões. Reiniciando...");
      ESP.restart();
    }
  }
}

void enterWaitMode() {
  wifiConnectionState = WIFI_WAIT;
  waitStart = millis();
}

void resetFailureCounter() {
  rebootFailCount = 0;
  EEPROM.write(0, rebootFailCount);
  EEPROM.commit();
}

// ============================================
// TESTE DE WI-FI (ASSÍNCRONO)
// ============================================

void handleWiFiTest() {
  if (!testingWiFi) return;

  unsigned long currentTime = millis();

  // ===== SUCESSO =====
  if (WiFi.status() == WL_CONNECTED) {
    testingWiFi = false;

    addLog("Conectado ao Wi-Fi! IP: %s",
                  WiFi.localIP().toString().c_str());

    // Salva configuração
    config.wifi_ssid = testSSID;
    config.wifi_pass = testPass;
    saveConfiguration();

    // Restaura AP + servidor
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("ESP_Config");

    wifiConnectionState = WIFI_DISCONNECTED;
    return;
  }

  // ===== TIMEOUT =====
  if (currentTime - wifiTestStart > WIFI_TEST_TIMEOUT) {
    testingWiFi = false;

    addLog("Falha ao conectar ao Wi-Fi (timeout)");

    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("ESP_Config");

    wifiConnectionState = WIFI_DISCONNECTED;
  }
}


void startWiFiTest(const String& ssid, const String& password) {
  addLog("Iniciando teste Wi-Fi para SSID: %s", ssid.c_str());
  
  testSSID = ssid;
  testPass = password;
  
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  
  wifiTestStart = millis();
  testingWiFi = true;
}

bool hasWebPassword() {
  return config.web_user.length() > 0 && config.web_pass.length() > 0;
}

// ============================================
// SERVIDOR WEB
// ============================================

void showCreatePasswordPage() {
  server.send(200, "text/html",
    "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
    "<title>Criar Senha</title></head><body>"
    "<h2>Defina a senha de acesso</h2>"
    "<form action='/setpass' method='POST'>"
    "Usuário:<br><input name='user' required><br><br>"
    "Senha:<br><input name='pass' type='password' placeholder='********' required><br><br>"
    "<button type='submit'>Salvar</button>"
    "</form>"
    "<hr>"
    "<p style='font-size:12px;color:#777;'>"
    "Firmware version: " firmware_version
    "</p>"
    "</body></html>"
  );
}


void handleRoot() {

  if (hasWebPassword()) {
    if (!authenticate()) return;
  } else {
    showCreatePasswordPage();
    return;
  }

  String html = R"(
    <!DOCTYPE html>
    <html>
    <head>
      <meta charset='UTF-8'>
      <meta name='viewport' content='width=device-width, initial-scale=1'>
      <title>Configuração ESP8266 DNS</title>
      <style>
        body { font-family: Arial, sans-serif; max-width: 600px; margin: 0 auto; padding: 20px; }
        h1 { color: #333; }
        label { display: block; margin-top: 15px; }
        input { width: 100%; padding: 8px; margin-top: 5px; box-sizing: border-box; }
        button { background: #4CAF50; color: white; padding: 12px 20px; border: none; cursor: pointer; margin-top: 20px; }
        button:hover { background: #45a049; }
        .form-group { margin-bottom: 15px; }
      </style>
    </head>
    <body>
      <h1>Configuração ESP8266 DNS Updater</h1>
      <form action='/save' method='POST'>
        <div class='form-group'>
          <label>SSID Wi-Fi:</label>
          <input type='text' name='wifi_ssid' value=')" + config.wifi_ssid + R"(' required>
        </div>
        <div class='form-group'>
          <label>Senha Wi-Fi:</label>
          <input type='password' name='wifi_pass' placeholder='********' required>
        </div>
        <div class='form-group'>
          <label>Cloudflare Token:</label>
          <input type='text' name='CF_TOKEN' value=')" + config.CF_TOKEN + R"('>
        </div>
        <div class='form-group'>
          <label>Cloudflare Zone ID:</label>
          <input type='text' name='CF_ZONE' value=')" + config.CF_ZONE + R"('>
        </div>
        <div class='form-group'>
          <label>Cloudflare Record ID:</label>
          <input type='text' name='CF_RECORD' value=')" + config.CF_RECORD + R"('>
        </div>
        <div class='form-group'>
          <label>Hostname:</label>
          <input type='text' name='CF_HOST' value=')" + config.CF_HOST + R"('>
        </div>
        <button type='submit'>Salvar e Testar Conexão</button>
      </form>
      <hr>
      <p style="font-size:12px;color:#777;text-align:center;">
        Firmware version: {{VERSION}}
      </p>
    </body>
    </html>
  )";

  html.replace("{{VERSION}}", firmware_version);
  
  server.send(200, "text/html", html);
}

void handleSave() {

  if (hasWebPassword()) {
    if (!authenticate()) return;
  }

  if (!server.hasArg("wifi_ssid")) {
    server.send(400, "text/html", "<h2>SSID é obrigatório!</h2>");
    return;
  }
  
  // Coleta dados do formulário
  String ssid = server.arg("wifi_ssid");
  String password = server.arg("wifi_pass");
  
  if (password.length() == 0) {
    password = config.wifi_pass; // mantém a atual
  }

  // Atualiza configurações do Cloudflare
  if (server.hasArg("CF_TOKEN"))  config.CF_TOKEN  = server.arg("CF_TOKEN");
  if (server.hasArg("CF_ZONE"))   config.CF_ZONE   = server.arg("CF_ZONE");
  if (server.hasArg("CF_RECORD")) config.CF_RECORD = server.arg("CF_RECORD");
  if (server.hasArg("CF_HOST"))   config.CF_HOST   = server.arg("CF_HOST");
  
  // Inicia teste assíncrono de Wi-Fi
  startWiFiTest(ssid, password);
  
  // Responde imediatamente
  server.send(200, "text/html",
    "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Testando</title></head><body>"
    "<h1>Testando conexão Wi-Fi...</h1>"
    "<p>O ESP está tentando se conectar à rede informada.</p>"
    "<p>Aguarde alguns segundos e atualize a página para ver o resultado.</p>"
    "</body></html>");
}

bool authenticate() {
  if (config.web_user.length() == 0 || config.web_pass.length() == 0) {
    return true; // primeira configuração
  }

  if (!server.authenticate(
        config.web_user.c_str(),
        config.web_pass.c_str())) {
    server.requestAuthentication(
      DIGEST_AUTH,
      "ESP8266",
      "Autenticação necessária");
    return false;
  }
  return true;
}


// ============================================
// ATUALIZAÇÃO DNS E OTA
// ============================================

void handleDNSUpdate() {
  String publicIP = getPublicIP();
  if (publicIP.isEmpty()) {
    addLog("Falha ao obter IP público");
    return;
  }
  
  String currentDNSIP = getDNSRecordIP(config.CF_HOST);
  if (currentDNSIP.isEmpty()) {
    addLog("Falha ao obter IP do DNS");
    return;
  }
  
  if (currentDNSIP != publicIP) {
    addLog("IPs diferentes. Atualizando DNS...");
    updateDNSRecord(publicIP);
  } else {
    addLog("DNS já está atualizado");
  }
}

void checkForUpdate() {
  if (WiFi.status() != WL_CONNECTED) return;

  addLog("Free heap before OTA: %d", ESP.getFreeHeap());

  // ===== 1. CONSULTA API DO GITHUB (HTTPS) =====
  WiFiClientSecure apiClient;
  apiClient.setInsecure();

  HTTPClient apiHttp;
  apiHttp.begin(apiClient, github_api);
  apiHttp.addHeader("User-Agent", "ESP8266");

  int httpCode = apiHttp.GET();
  if (httpCode != HTTP_CODE_OK) {
    addLog("GitHub API error: %d", httpCode);
    apiHttp.end();
    return;
  }

  String payload = apiHttp.getString();
  apiHttp.end();

  // ===== 2. OBTÉM VERSÃO =====
  int idxVersion = payload.indexOf("\"tag_name\"");
  if (idxVersion < 0) return;

  int startVer = payload.indexOf("\"", idxVersion + 10) + 1;
  int endVer   = payload.indexOf("\"", startVer);
  String latestVersion = payload.substring(startVer, endVer);

  if (latestVersion == firmware_version) {
    addLog("Firmware already up-to-date.");
    return;
  }

  // ===== 3. OBTÉM URL DO BIN =====
  int idx = payload.indexOf("\"browser_download_url\"");
  if (idx < 0) return;

  int start = payload.indexOf("https://", idx);
  int end   = payload.indexOf("\"", start);
  String binUrl = payload.substring(start, end);

  // CONVERTE PARA HTTP (IMPORTANTE)
  binUrl.replace("https://", "http://");

  addLog("New firmware: %s", binUrl.c_str());

  // ===== 4. LIBERA MEMÓRIA =====
  server.stop();
  delay(500);

  // ===== 5. DOWNLOAD DO BIN (HTTP) =====
  WiFiClient binClient;
  HTTPClient binHttp;

  binHttp.begin(binClient, binUrl);
  int binCode = binHttp.GET();

  if (binCode != HTTP_CODE_OK) {
    addLog("Firmware download failed: %d", binCode);
    binHttp.end();
    return;
  }

  int contentLength = binHttp.getSize();
  if (!Update.begin(contentLength)) {
    addLog("Not enough space for OTA");
    binHttp.end();
    return;
  }

  WiFiClient *stream = binHttp.getStreamPtr();
  size_t written = Update.writeStream(*stream);

  if (written == contentLength && Update.end()) {
    addLog("OTA successful, rebooting...");
    ESP.restart();
  } else {
    addLog("OTA failed: %s", Update.getErrorString().c_str());
  }

  binHttp.end();
}

void updateDNSRecord(const String& ipAddress) {
  String url = "https://api.cloudflare.com/client/v4/zones/" + 
               config.CF_ZONE + "/dns_records/" + config.CF_RECORD;
  
  WiFiClientSecure client;
  client.setInsecure();
  
  HTTPClient http;
  http.begin(client, url);
  http.addHeader("Authorization", "Bearer " + config.CF_TOKEN);
  http.addHeader("Content-Type", "application/json");
  
  String payload = "{\"content\":\"" + ipAddress + "\"}";
  int httpCode = http.PATCH(payload);
  
  if (httpCode > 0) {
    String response = http.getString();
    if (response.indexOf("\"success\":true") >= 0) {
      addLog("DNS atualizado com sucesso!");
    } else {
      addLog("Cloudflare response: %s", response.c_str());
    }
  } else {
    addLog("Erro ao atualizar DNS. Código: %d", httpCode);
  }
  
  http.end();
}

String getPublicIP() {
  WiFiClient client;
  HTTPClient http;
  
  http.begin(client, "http://api.ipify.org");
  int httpCode = http.GET();
  
  String ipAddress = "";
  if (httpCode == HTTP_CODE_OK) {
    ipAddress = http.getString();
    ipAddress.trim();
  } else {
    addLog("Falha ao obter IP público. Código: %d", httpCode);
  }
  
  http.end();
  return ipAddress;
}

String getDNSRecordIP(const String& hostname) {
  IPAddress resolvedIP;
  if (WiFi.hostByName(hostname.c_str(), resolvedIP)) {
    return resolvedIP.toString();
  }
  return "";
}

void addLog(const char *format, ...) {
  char buffer[LOG_LINE_SIZE];

  va_list args;
  va_start(args, format);
  vsnprintf(buffer, LOG_LINE_SIZE, format, args);
  va_end(args);

  String line = "[" + String(millis() / 1000) + "s] " + String(buffer);

  logBuffer[logIndex] = line;
  logIndex++;

  if (logIndex >= LOG_BUFFER_SIZE) {
    logIndex = 0;
    logWrapped = true;
  }

  Serial.println(line); // mantém saída no Serial
}

// ============================================
// UTILITÁRIOS
// ============================================

void performDailyReboot() {
  addLog("Reinício diário programado!");
  ESP.restart();
}