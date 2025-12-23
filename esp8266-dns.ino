#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <Updater.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#ifndef firmware_version
  #define firmware_version "dev"
#endif

const char* GITHUB_API =
  "https://api.github.com/repos/allanbarcelos/esp8266-dns/releases/latest";

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

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  http.begin(client, github_api);
  http.addHeader("User-Agent", "ESP8266");
  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("Failed to access GitHub API. Code: %d\n", httpCode);
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  int idxVersion = payload.indexOf("\"tag_name\"");
  if (idxVersion < 0) return;

  int startVer = payload.indexOf("\"", idxVersion + 10) + 1;
  int endVer = payload.indexOf("\"", startVer);
  String latestVersion = payload.substring(startVer, endVer);

  if (latestVersion == firmware_version) {
    Serial.println("Firmware already up-to-date.");
    return;
  }

  int idx = payload.indexOf("\"browser_download_url\"");
  if (idx < 0) return;
  int start = payload.indexOf("https://", idx);
  int end = payload.indexOf("\"", start);
  String binUrl = payload.substring(start, end);

  Serial.println("New release: " + binUrl);

  WiFiClientSecure binClient;
  binClient.setInsecure();
  HTTPClient binHttp;
  binHttp.begin(binClient, binUrl);
  int binCode = binHttp.GET();

  if (binCode == HTTP_CODE_OK) {
    int contentLength = binHttp.getSize();
    if (Update.begin(contentLength)) {
      WiFiClient *stream = binHttp.getStreamPtr();
      uint8_t buf[1024];
      int bytesRead = 0;
      while (bytesRead < contentLength) {
        size_t toRead = min(sizeof(buf), (size_t)(contentLength - bytesRead));
        int c = stream->readBytes(buf, toRead);
        if (c <= 0) break;
        Update.write(buf, c);
        bytesRead += c;
        yield(); // prevents WDT reset
      }
      if (Update.end()) {
        Serial.println("Update successfully completed!");
        ESP.restart();
      } else {
        Serial.printf("Update error: %s\n", Update.getErrorString().c_str());
      }
    }
  } else {
    Serial.printf("Download failed. Code: %d\n", binCode);
  }

  binHttp.end();
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
