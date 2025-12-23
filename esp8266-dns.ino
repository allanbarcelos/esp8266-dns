#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <Updater.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#ifndef firmware_version
  #define firmware_version "dev"
#endif

ESP8266WebServer server(80);

// ========================
// CONFIG
// ========================
struct Config {
  String ssid;
  String pass;
};

Config config;
unsigned long lastOTACheck = 0;
const unsigned long OTA_INTERVAL = 60000UL; // 1min

// ========================
// FILESYSTEM
// ========================
void loadConfig() {
  if (!LittleFS.exists("/config.json")) return;

  File f = LittleFS.open("/config.json", "r");
  StaticJsonDocument<256> doc;
  deserializeJson(doc, f);
  f.close();

  config.ssid = doc["ssid"] | "";
  config.pass = doc["pass"] | "";
}

void saveConfig() {
  StaticJsonDocument<256> doc;
  doc["ssid"] = config.ssid;
  doc["pass"] = config.pass;

  File f = LittleFS.open("/config.json", "w");
  serializeJson(doc, f);
  f.close();
}

// ========================
// WIFI
// ========================
void startWiFi() {
  if (config.ssid.length() == 0) {
    WiFi.softAP("ESP_Config");
    Serial.println("AP iniciado: ESP_Config");
    return;
  }

  WiFi.begin(config.ssid.c_str(), config.pass.c_str());
  Serial.print("Conectando ao Wi-Fi");

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConectado!");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nFalha, iniciando AP");
    WiFi.softAP("ESP_Config");
  }
}

// ========================
// WEB
// ========================

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>ESP8266 Config</title>
</head>
<body>
  <h2>Configurar Wi-Fi</h2>
  <form action="/save" method="POST">
    SSID:<br>
    <input name="ssid"><br><br>
    Senha:<br>
    <input name="pass" type="password"><br><br>
    <button>Salvar</button>
  </form>
  <hr>
  Firmware: %VERSION%
</body>
</html>
)rawliteral";

void handleRoot() {
  String html = FPSTR(INDEX_HTML);
  html.replace("%VERSION%", firmware_version);
  server.send(200, "text/html", html);
}


void handleSave() {
  config.ssid = server.arg("ssid");
  config.pass = server.arg("pass");
  saveConfig();

  server.send(200, "text/html",
    "<h2>Salvo!</h2><p>Reiniciando...</p>"
  );

  delay(1000);
  ESP.restart();
}

// ========================
// OTA
// ========================

void checkOTA() {
  if (WiFi.status() != WL_CONNECTED) return;

  const char* VERSION_URL =
    "https://raw.githubusercontent.com/allanbarcelos/esp8266-dns/main/firmware/version.txt";

  const char* BIN_URL =
    "https://raw.githubusercontent.com/allanbarcelos/esp8266-dns/main/firmware/firmware.bin";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;

  // ========================
  // 1️⃣ Check version
  // ========================
  Serial.println("Checking firmware version...");

  if (!http.begin(client, VERSION_URL)) return;

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Version check failed: %d\n", code);
    http.end();
    return;
  }

  String latestVersion = http.getString();
  latestVersion.trim();
  http.end();

  Serial.printf("Current: %s | Latest: %s\n",
                firmware_version,
                latestVersion.c_str());

  if (latestVersion == firmware_version) {
    Serial.println("Firmware up-to-date.");
    return;
  }

  // ========================
  // 2️⃣ Download firmware
  // ========================
  Serial.println("New firmware found. Downloading...");

  if (!http.begin(client, BIN_URL)) return;

  code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Firmware download failed: %d\n", code);
    http.end();
    return;
  }

  int contentLength = http.getSize();
  if (contentLength <= 0) {
    Serial.println("Invalid firmware size.");
    http.end();
    return;
  }

  if (!Update.begin(contentLength)) {
    Serial.println("Not enough space for OTA.");
    http.end();
    return;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t written = Update.writeStream(*stream);

  if (written != contentLength) {
    Serial.printf("Written %d/%d bytes\n", written, contentLength);
    Update.end(false);
    http.end();
    return;
  }

  if (!Update.end()) {
    Serial.printf("Update error: %s\n", Update.getErrorString().c_str());
    http.end();
    return;
  }

  Serial.println("OTA update successful! Rebooting...");
  http.end();
  delay(1000);
  ESP.restart();
}



// ========================
// SETUP / LOOP
// ========================
void setup() {
  Serial.begin(115200);
  LittleFS.begin();
  loadConfig();
  startWiFi();

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
}

void loop() {
  server.handleClient();

  if (millis() - lastOTACheck > OTA_INTERVAL) {
    lastOTACheck = millis();
    checkOTA();
  }
}
