#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <Updater.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <EEPROM.h>
// #include "secrets.h"
// #include "crypto.h"

ESP8266WebServer server(80);

unsigned long checkInterval = 3600000UL;      // 1 hour
unsigned long dnsUpdateInterval = 300000UL;   // 5 minutes
unsigned long reconnectDelay = 5000;          // 5 seconds between reconnect attempts
const int maxReconnectAttempts = 5;
const int maxRebootsBeforeWait = 3;
const unsigned long waitAfterFails = 1800000UL; // 30 minutes

int rebootFailCount = 0;
unsigned long lastCheck = 0;
unsigned long dnsLastUpdate = 0;
unsigned long lastReconnectAttempt = 0;
int reconnectAttempts = 0;

enum WifiConnState { WIFI_OK, WIFI_DISCONNECTED, WIFI_RECONNECTING, WIFI_WAIT };
WifiConnState WifiConnState = WIFI_OK;

// ---- Variáveis para teste assíncrono de Wi-Fi ----
bool testingWiFi = false;
unsigned long wifiTestStart = 0;
String testSSID = "";
String testPass = "";
const unsigned long wifiTestTimeout = 10000; // 10 segundos
//

unsigned long waitStart = 0;

struct Config {
  String wifi_ssid;
  String wifi_pass;
  String CF_TOKEN;
  String CF_ZONE;
  String CF_RECORD;
  String CF_HOST;
};

Config config;


void setup() {
  Serial.begin(115200);
  Serial.println("\nStarting...");

  // Inicializa LittleFS e carrega config
  loadConfig();

  EEPROM.begin(512);
  rebootFailCount = EEPROM.read(0);
  Serial.printf("Previous failed reboots: %d\n", rebootFailCount);

  if (rebootFailCount >= maxRebootsBeforeWait) {
    Serial.println("Too many consecutive failures. Entering non-blocking wait mode...");
    WifiConnState = WIFI_WAIT;
    waitStart = millis();
  } else {
    if (config.wifi_ssid.length() > 0) {
      WiFi.hostname("ESP8266_DNS");
      WiFi.begin(config.wifi_ssid.c_str(), config.wifi_pass.c_str());
      WifiConnState = WIFI_RECONNECTING;
    } else {
      Serial.println("Nenhuma configuração Wi-Fi encontrada. Criando AP de configuração...");
      WiFi.softAP("ESP_Config");
    }

  }

  server.on("/", handleRoot);
  server.on("/save", handleSave);

  server.begin();

  Serial.println("HTTP server started!");
}

void loop() {
  server.handleClient();

  handleWiFi();
  handleWiFiTest();  

  unsigned long now = millis();

  // Daily reboot (non-blocking)
  if (now > 86400000UL) {
    Serial.println("Daily reboot!");
    ESP.restart();
  }

  // OTA check
  if (WifiConnState == WIFI_OK && (now - lastCheck >= checkInterval || lastCheck == 0)) {
    lastCheck = now;
    checkForUpdate();
  }

  // DNS update check
  if (WifiConnState == WIFI_OK && (now - dnsLastUpdate >= dnsUpdateInterval || dnsLastUpdate == 0)) {
    dnsLastUpdate = now;
    handleDNSUpdate();
  }
}

void handleWiFi() {
  unsigned long now = millis();

  switch (WifiConnState) {
    case WIFI_OK:
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi disconnected.");
        WifiConnState = WIFI_RECONNECTING;
        reconnectAttempts = 0;
        lastReconnectAttempt = 0;
      }
      break;

    case WIFI_RECONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("Reconnected!");
        rebootFailCount = 0;
        EEPROM.write(0, rebootFailCount);
        EEPROM.commit();
        WifiConnState = WIFI_OK;
        break;
      }

      if (now - lastReconnectAttempt >= reconnectDelay) {
        lastReconnectAttempt = now;
        reconnectAttempts++;

        Serial.printf("Reconnect attempt %d/%d...\n", reconnectAttempts, maxReconnectAttempts);
        WiFi.disconnect();
        WiFi.begin(config.wifi_ssid.c_str(), config.wifi_pass.c_str());


        if (reconnectAttempts >= maxReconnectAttempts) {
          rebootFailCount++;
          EEPROM.write(0, rebootFailCount);
          EEPROM.commit();

          if (rebootFailCount >= maxRebootsBeforeWait) {
            Serial.println("Too many failures. Going to wait mode...");
            WifiConnState = WIFI_WAIT;
            waitStart = millis();
          } else {
            Serial.println("Total failure, restarting...");
            ESP.restart();
          }
        }
      }
      break;

    case WIFI_WAIT:
      if (now - waitStart >= waitAfterFails) {
        Serial.println("Wait time completed. Trying again...");
        rebootFailCount = 0;
        EEPROM.write(0, rebootFailCount);
        EEPROM.commit();
        WiFi.begin(config.wifi_ssid.c_str(), config.wifi_pass.c_str());
        WifiConnState = WIFI_RECONNECTING;
      }
      break;
  }
}

// -----------------------------
// DNS and OTA — non-blocking
// -----------------------------
void handleDNSUpdate() {
  String publicIP = getPublicIP();
  if (publicIP == "") return;

  String currentDNSIP = getDNSHostIP(CF_HOST);
  if (currentDNSIP == "") return;

  if (currentDNSIP != publicIP) {
    Serial.println("Updating DNS...");
    dnsUpdate(publicIP);
  } else {
    Serial.println("DNS is already up-to-date.");
  }
}

void checkForUpdate() {
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
        decryptBuffer(buf, c);
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

void dnsUpdate(String ip) {
  String url = "https://api.cloudflare.com/client/v4/zones/" + String(CF_ZONE) + "/dns_records/" + String(CF_RECORD);
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, url);
  http.addHeader("Authorization", "Bearer " + String(CF_TOKEN));
  http.addHeader("Content-Type", "application/json");
  String payload = "{\"content\":\"" + ip + "\"}";
  int code = http.PATCH(payload);
  if (code > 0) {
    String resp = http.getString();
    if (resp.indexOf("\"success\":true") >= 0)
      Serial.println("DNS successfully updated!");
    else
      Serial.println("Failed to update DNS.");
  } else {
    Serial.println("Error updating DNS. Code: " + String(code));
  }
  http.end();
}

String getPublicIP() {
  WiFiClient client;
  HTTPClient http;
  http.begin(client, "http://api.ipify.org");
  int httpCode = http.GET();
  String ip = "";
  if (httpCode == HTTP_CODE_OK) {
    ip = http.getString();
    ip.trim();
  }
  http.end();
  return ip;
}

String getDNSHostIP(String host) {
  IPAddress resolvedIP;
  if (WiFi.hostByName(host.c_str(), resolvedIP)) return resolvedIP.toString();
  return "";
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Configuração ESP8266</title></head><body>";
  html += "<h1>Configuração</h1><form action='/save' method='POST'>";
  html += "SSID: <input name='wifi_ssid' value='" + config.wifi_ssid + "'><br>";
  html += "Senha Wi-Fi: <input type='password' name='wifi_pass' value='" + config.wifi_pass + "'><br>";
  html += "CF_TOKEN: <input name='CF_TOKEN' value='" + config.CF_TOKEN + "'><br>";
  html += "CF_ZONE: <input name='CF_ZONE' value='" + config.CF_ZONE + "'><br>";
  html += "CF_RECORD: <input name='CF_RECORD' value='" + config.CF_RECORD + "'><br>";
  html += "CF_HOST: <input name='CF_HOST' value='" + config.CF_HOST + "'><br>";
  html += "<input type='submit' value='Salvar'>";
  html += "</form></body></html>";
  server.send(200, "text/html", html);
}


void handleSave() {
  if (!server.hasArg("wifi_ssid")) {
    server.send(400, "text/html", "<h2>SSID ausente!</h2>");
    return;
  }

  // Guarda dados do formulário
  testSSID = server.arg("wifi_ssid");
  testPass = server.arg("wifi_pass");

  if (server.hasArg("CF_TOKEN"))  config.CF_TOKEN  = server.arg("CF_TOKEN");
  if (server.hasArg("CF_ZONE"))   config.CF_ZONE   = server.arg("CF_ZONE");
  if (server.hasArg("CF_RECORD")) config.CF_RECORD = server.arg("CF_RECORD");
  if (server.hasArg("CF_HOST"))   config.CF_HOST   = server.arg("CF_HOST");

  // Inicia teste assíncrono
  Serial.printf("Iniciando teste Wi-Fi para SSID: %s\n", testSSID.c_str());
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(testSSID.c_str(), testPass.c_str());

  wifiTestStart = millis();
  testingWiFi = true;

  // Responde imediatamente
  server.send(200, "text/html",
    "<h1>Testando conexão Wi-Fi...</h1>"
    "<p>O ESP vai tentar se conectar. Aguarde alguns segundos e atualize a página.</p>");
}


void handleWiFiTest() {
  if (!testingWiFi) return;

  if (WiFi.status() == WL_CONNECTED) {
    testingWiFi = false;
    Serial.printf("Conectado ao Wi-Fi! IP: %s\n", WiFi.localIP().toString().c_str());

    // Atualiza config e salva
    config.wifi_ssid = testSSID;
    config.wifi_pass = testPass;
    saveConfig();

    server.send(200, "text/html",
      "<h1>Wi-Fi conectado com sucesso!</h1>"
      "<p>Configuração salva.</p>"
      "<p>IP: " + WiFi.localIP().toString() + "</p>"
      "<p>Reinicie o ESP para aplicar.</p>");
    WiFi.disconnect(true);
    WiFi.softAP("ESP_Config");
  }

  if (millis() - wifiTestStart > wifiTestTimeout) {
    testingWiFi = false;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Falha ao conectar ao Wi-Fi informado (timeout).");
      server.send(200, "text/html",
        "<h1>Falha ao conectar!</h1>"
        "<p>SSID ou senha incorretos.</p>"
        "<p>Retorne e tente novamente.</p>");
      WiFi.disconnect(true);
      WiFi.softAP("ESP_Config");
    }
  }
}




// ---------------------------------------------------------------------------------------------------

void loadConfig() {
  if (!LittleFS.begin()) {
    Serial.println("Falha ao montar LittleFS");
    return;
  }

  if (!LittleFS.exists("/config.json")) {
    Serial.println("Arquivo de configuração não existe.");
    return;
  }

  File file = LittleFS.open("/config.json", "r");
  if (!file) return;

  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error) {
    Serial.println("Erro ao ler config: " + String(error.c_str()));
    return;
  }

  config.wifi_ssid = doc["wifi_ssid"].as<String>();
  config.wifi_pass = doc["wifi_pass"].as<String>();
  config.CF_TOKEN = doc["CF_TOKEN"].as<String>();
  config.CF_ZONE = doc["CF_ZONE"].as<String>();
  config.CF_RECORD = doc["CF_RECORD"].as<String>();
  config.CF_HOST = doc["CF_HOST"].as<String>();
}

void saveConfig() {
  StaticJsonDocument<512> doc;
  doc["wifi_ssid"] = config.wifi_ssid;
  doc["wifi_pass"] = config.wifi_pass;
  doc["CF_TOKEN"] = config.CF_TOKEN;
  doc["CF_ZONE"] = config.CF_ZONE;
  doc["CF_RECORD"] = config.CF_RECORD;
  doc["CF_HOST"] = config.CF_HOST;

  File file = LittleFS.open("/config.json", "w");
  if (!file) {
    Serial.println("Falha ao abrir arquivo para salvar config");
    return;
  }
  serializeJson(doc, file);
  file.close();
}
