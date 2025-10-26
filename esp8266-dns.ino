#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <Updater.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include "secrets.h"
#include "crypto.h"

ESP8266WebServer server(80);

unsigned long checkInterval = 3600000UL;      // 1 hora
unsigned long dnsUpdateInterval = 300000UL;   // 5 min
unsigned long reconnectDelay = 5000;          // 5s entre tentativas
const int maxReconnectAttempts = 5;
const int maxRebootsBeforeWait = 3;
const unsigned long waitAfterFails = 1800000UL; // 30 min

int rebootFailCount = 0;
unsigned long lastCheck = 0;
unsigned long dnsLastUpdate = 0;
unsigned long lastReconnectAttempt = 0;
int reconnectAttempts = 0;

enum WiFiState { WIFI_OK, WIFI_DISCONNECTED, WIFI_RECONNECTING, WIFI_WAIT };
WiFiState wifiState = WIFI_OK;

unsigned long waitStart = 0;

void setup() {
  Serial.begin(115200);
  Serial.println("\nIniciando...");

  EEPROM.begin(512);
  rebootFailCount = EEPROM.read(0);
  Serial.printf("Reboots falhos anteriores: %d\n", rebootFailCount);

  if (rebootFailCount >= maxRebootsBeforeWait) {
    Serial.println("Muitas falhas consecutivas. Entrando em modo de espera não bloqueante...");
    wifiState = WIFI_WAIT;
    waitStart = millis();
  } else {
    WiFi.hostname("ESP8266_DNS");
    WiFi.begin(ssid, password);
    wifiState = WIFI_RECONNECTING;
  }

  server.on("/", handleRoot);
  server.begin();

  Serial.println("Servidor HTTP iniciado!");
}

void loop() {
  server.handleClient();

  handleWiFi();

  unsigned long now = millis();

  // Reboot diário (não bloqueante)
  if (now > 86400000UL) {
    Serial.println("Reboot diário!");
    ESP.restart();
  }

  // OTA check
  if (wifiState == WIFI_OK && (now - lastCheck >= checkInterval || lastCheck == 0)) {
    lastCheck = now;
    checkForUpdate();
  }

  // DNS check
  if (wifiState == WIFI_OK && (now - dnsLastUpdate >= dnsUpdateInterval || dnsLastUpdate == 0)) {
    dnsLastUpdate = now;
    handleDNSUpdate();
  }
}

void handleWiFi() {
  unsigned long now = millis();

  switch (wifiState) {
    case WIFI_OK:
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi desconectado.");
        wifiState = WIFI_RECONNECTING;
        reconnectAttempts = 0;
        lastReconnectAttempt = 0;
      }
      break;

    case WIFI_RECONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("Reconectado!");
        rebootFailCount = 0;
        EEPROM.write(0, rebootFailCount);
        EEPROM.commit();
        wifiState = WIFI_OK;
        break;
      }

      if (now - lastReconnectAttempt >= reconnectDelay) {
        lastReconnectAttempt = now;
        reconnectAttempts++;

        Serial.printf("Tentativa %d/%d de reconexão...\n", reconnectAttempts, maxReconnectAttempts);
        WiFi.disconnect();
        WiFi.begin(ssid, password);

        if (reconnectAttempts >= maxReconnectAttempts) {
          rebootFailCount++;
          EEPROM.write(0, rebootFailCount);
          EEPROM.commit();

          if (rebootFailCount >= maxRebootsBeforeWait) {
            Serial.println("Muitas falhas. Indo para modo espera...");
            wifiState = WIFI_WAIT;
            waitStart = millis();
          } else {
            Serial.println("Falha total, reiniciando...");
            ESP.restart();
          }
        }
      }
      break;

    case WIFI_WAIT:
      if (now - waitStart >= waitAfterFails) {
        Serial.println("Tempo de espera concluído. Tentando novamente...");
        rebootFailCount = 0;
        EEPROM.write(0, rebootFailCount);
        EEPROM.commit();
        WiFi.begin(ssid, password);
        wifiState = WIFI_RECONNECTING;
      }
      break;
  }
}

// -----------------------------
// DNS e OTA — não bloqueantes
// -----------------------------
void handleDNSUpdate() {
  String publicIP = getPublicIP();
  if (publicIP == "") return;

  String currentDNSIP = getDNSHostIP(CF_HOST);
  if (currentDNSIP == "") return;

  if (currentDNSIP != publicIP) {
    Serial.println("Atualizando DNS...");
    dnsUpdate(publicIP);
  } else {
    Serial.println("DNS já está atualizado.");
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
    Serial.printf("Falha ao acessar GitHub API. Código: %d\n", httpCode);
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
    Serial.println("Firmware já atualizado.");
    return;
  }

  int idx = payload.indexOf("\"browser_download_url\"");
  if (idx < 0) return;
  int start = payload.indexOf("https://", idx);
  int end = payload.indexOf("\"", start);
  String binUrl = payload.substring(start, end);

  Serial.println("Nova release: " + binUrl);

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
        yield(); // evita WDT reset
      }
      if (Update.end()) {
        Serial.println("Atualização concluída com sucesso!");
        ESP.restart();
      } else {
        Serial.printf("Erro na atualização: %s\n", Update.getErrorString().c_str());
      }
    }
  } else {
    Serial.printf("Falha no download. Código: %d\n", binCode);
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
      Serial.println("DNS atualizado com sucesso!");
    else
      Serial.println("Falha ao atualizar DNS.");
  } else {
    Serial.println("Erro ao atualizar DNS. Código: " + String(code));
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
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>ESP8266</title>"
                "<style>body{display:flex;justify-content:center;align-items:center;height:100vh;"
                "margin:0;font-family:Arial;}h1{font-size:3em;}</style></head>"
                "<body><h1>ESP8266</h1></body></html>";
  server.send(200, "text/html", html);
}
