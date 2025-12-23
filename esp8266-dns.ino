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
  http.begin(client, GITHUB_API);
  http.addHeader("User-Agent", "ESP8266");

  if (http.GET() != 200) {
    http.end();
    return;
  }

  StaticJsonDocument<2048> doc;
  deserializeJson(doc, http.getStream());
  http.end();

  const char* tag = doc["tag_name"];
  if (!tag || strcmp(tag, firmware_version) == 0) return;

  const char* url = doc["assets"][0]["browser_download_url"];
  if (!url) return;

  WiFiClientSecure binClient;
  binClient.setInsecure();

  HTTPClient binHttp;
  binHttp.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  binHttp.begin(binClient, url);

  int code = binHttp.GET();
  if (code != 200) {
    Serial.printf("FAIL 2: HTTP %d\n", code);
    binHttp.end();
    return;
  }

  int size = binHttp.getSize();
  if (!Update.begin(size)) {
    binHttp.end();
    return;
  }

  Update.writeStream(*binHttp.getStreamPtr());

  if (Update.end()) {
    Serial.println("OTA OK, reiniciando...");
    ESP.restart();
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
