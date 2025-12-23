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

// Constantes OTA
const char* VERSION_URL = "https://raw.githubusercontent.com/allanbarcelos/esp8266-dns/main/firmware/version.txt";
const char* BIN_URL     = "https://raw.githubusercontent.com/allanbarcelos/esp8266-dns/main/firmware/firmware.bin";
const unsigned long OTA_INTERVAL = 60000UL;  // 1 minuto

ESP8266WebServer server(80);

struct Config {
  String ssid;
  String pass;
};
Config config;

unsigned long lastOTACheck = 0;

// HTML com versão em PROGMEM
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pt">
<head>
  <meta charset="utf-8">
  <title>ESP8266 Config</title>
  <style>body{font-family:Arial;margin:40px;}input{width:100%;padding:8px;margin:10px 0;}</style>
</head>
<body>
  <h2>Configurar Wi-Fi</h2>
  <form action="/save" method="POST">
    SSID:<br>
    <input name="ssid" required><br><br>
    Senha:<br>
    <input name="pass" type="password"><br><br>
    <button type="submit">Salvar</button>
  </form>
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


// ========================
// LOG
// ========================
#define LOG_BUFFER_SIZE 10
#define LOG_LINE_SIZE   128 

char logBuffer[LOG_BUFFER_SIZE][LOG_LINE_SIZE];
int logIndex = 0;
bool logWrapped = false;

void addLog(const char *format, ...) {
  char msg[LOG_LINE_SIZE];

  va_list args;
  va_start(args, format);
  vsnprintf(msg, sizeof(msg), format, args);
  va_end(args);

  snprintf(logBuffer[logIndex], LOG_LINE_SIZE,`
           "[%lus] %s", millis() / 1000, msg);

  Serial.println(logBuffer[logIndex]);

  logIndex++;
  if (logIndex >= LOG_BUFFER_SIZE) {
    logIndex = 0;
    logWrapped = true;
  }
}



// ========================
// FILESYSTEM
// ========================
void loadConfig() {
  if (!LittleFS.exists("/config.json")) return;
  
  File f = LittleFS.open("/config.json", "r");
  if (!f) return;
  
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, f) == DeserializationError::Ok) {
    config.ssid = doc["ssid"].as<String>();
    config.pass = doc["pass"].as<String>();
  }
  f.close();
}

void saveConfig() {
  StaticJsonDocument<256> doc;
  doc["ssid"] = config.ssid;
  doc["pass"] = config.pass;
  
  File f = LittleFS.open("/config.json", "w");
  if (f) {
    serializeJson(doc, f);
    f.close();
  }
}

// ========================
// WIFI
// ========================
void startWiFi() {
  if (config.ssid.length() == 0) {
    WiFi.softAP("ESP_Config");
    addLog("\nAP mode: ESP_Config");
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(config.ssid.c_str(), config.pass.c_str());
  
  Serial.print("Conectando ao Wi-Fi");
  uint8_t attempts = 20;  // ~10 segundos
  while (WiFi.status() != WL_CONNECTED && attempts--) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nConectado! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    addLog("\nFalha na conexão → AP mode");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("ESP_Config");
  }
}

// ========================
// WEB HANDLERS
// ========================
void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleSave() {
  config.ssid = server.arg("ssid");
  config.pass = server.arg("pass");
  saveConfig();
  
  server.send(200, "text/html",
    "<h2>Configuração salva!</h2><p>O dispositivo está reiniciando...</p>");
  delay(1000);
  ESP.restart();
}

// ========================
// OTA UPDATE
// ========================
void checkOTA() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();  // Aceita certificados auto-assinados/inválidos
  HTTPClient http;

  addLog("\nVerificando versão do firmware...");

  // --- Verifica versão ---
  if (!http.begin(client, VERSION_URL)) {
    addLog("Falha ao conectar para versão");
    return;
  }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Erro na versão: %d\n", code);
    http.end();
    return;
  }
  String latestVersion = http.getString();
  latestVersion.trim();
  http.end();

  Serial.printf("Atual: %s | Disponível: %s\n", firmware_version, latestVersion.c_str());
  if (latestVersion == firmware_version) {
    addLog("Firmware já está atualizado.");
    return;
  }

  // --- Download e update ---
  addLog("Nova versão encontrada! Baixando firmware...");
  if (!http.begin(client, BIN_URL)) {
    addLog("Falha ao conectar para download");
    return;
  }

  code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Erro no download: %d\n", code);
    http.end();
    return;
  }

  int contentLength = http.getSize();
  if (contentLength <= 0) {
    addLog("Tamanho do firmware inválido");
    http.end();
    return;
  }

  Serial.printf("Tamanho do binário: %d bytes\n", contentLength);

  if (!Update.begin(contentLength)) {
    addLog("Espaço insuficiente para OTA");
    Update.printError(Serial);
    http.end();
    return;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t written = Update.writeStream(*stream);

  if (written != contentLength) {
    Serial.printf("Escrita parcial: %zu / %d bytes\n", written, contentLength);
    Update.end(false);
    http.end();
    return;
  }

  if (!Update.end()) {
    Serial.printf("Erro no update: %s\n", Update.getErrorString().c_str());
    http.end();
    return;
  }

  http.end();
  addLog("OTA concluído com sucesso! Reiniciando...");
  delay(1000);
  ESP.restart();
}

// ========================
// SETUP / LOOP
// ========================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  if (!LittleFS.begin()) {
    addLog("Falha ao montar LittleFS");
  }
  
  loadConfig();
  startWiFi();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);


  server.on("/log", HTTP_GET, []() {

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
  });


  server.begin();
  addLog("Servidor web iniciado");
}

void loop() {
  server.handleClient();

  if (millis() - lastOTACheck >= OTA_INTERVAL) {
    lastOTACheck = millis();
    checkOTA();
  }
}