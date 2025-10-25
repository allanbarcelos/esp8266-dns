#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <Updater.h>
#include "secrets.h"
#include "crypto.h"

unsigned long checkInterval = 3600000UL;  // 1 hora
unsigned long lastCheck = 0;

unsigned long dnsUpdateInterval = 300000UL;  // 1 hora
unsigned long dnsLastUpdate = 0;

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("Iniciando...");

  WiFi.begin(ssid, password);
  Serial.print("Conectando ao WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nConectado!");
  Serial.print("Chip ID: ");
  Serial.println(ESP.getChipId(), HEX);
}

void loop() {

  if (millis() >= 86400000UL) {
    Serial.println("Reboot diário!");
    ESP.restart();
  }
  
  if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi desconectado. Reiniciando...");
      ESP.restart();
  }

  if (millis() - lastCheck >= checkInterval || lastCheck == 0) {
    lastCheck = millis();
    checkForUpdate();
  }

  if (millis() - dnsLastUpdate >= dnsUpdateInterval || dnsLastUpdate == 0) {
    dnsLastUpdate = millis();
    String ip = getPublicIP();
    if(ip != ""){
      Serial.println("Atualizando DNS para o IP público: " + ip);
      dnsUpdate(ip);
    }
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
    Serial.printf("Falha ao acessar API GitHub. Código: %d\n", httpCode);
    http.end();
    return;
  }

  String payload = http.getString();

  // Pegar a tag da release
  int idxVersion = payload.indexOf("\"tag_name\"");
  if (idxVersion < 0) {
      Serial.println("Não foi possível detectar a versão.");
      http.end();
      return;
  }
  int startVer = payload.indexOf("\"", idxVersion + 10) + 1;
  int endVer = payload.indexOf("\"", startVer);
  String latestVersion = payload.substring(startVer, endVer);

  Serial.print("Versão atual do firmware: "); Serial.println(firmware_version);
  Serial.print("Última versão disponível: "); Serial.println(latestVersion);

  // Verifica se a versão é a mesma
  if (latestVersion == firmware_version) {
      Serial.println("Firmware já está atualizado.");
      http.end();
      return;
  }

  // Pega URL do .bin
  int idx = payload.indexOf("\"browser_download_url\"");
  if (idx < 0) {
    Serial.println("Nenhum arquivo .bin encontrado no release.");
    http.end();
    return;
  }

  int start = payload.indexOf("https://", idx);
  int end = payload.indexOf("\"", start);
  String binUrl = payload.substring(start, end);

  Serial.println("Nova release detectada:");
  Serial.println(binUrl);

  // ---- Download manual e OTA em blocos ----
  HTTPClient binHttp;
  binHttp.begin(client, binUrl);
  int binCode = binHttp.GET();

  // Seguir redirect se GitHub responder 302
  if (binCode == HTTP_CODE_MOVED_PERMANENTLY || binCode == HTTP_CODE_FOUND) {
    String redirectUrl = binHttp.getLocation();
    Serial.printf("Redirecionando para: %s\n", redirectUrl.c_str());
    binHttp.end();
    binHttp.begin(client, redirectUrl);
    binCode = binHttp.GET();
  }

  if (binCode == HTTP_CODE_OK) {
      int contentLength = binHttp.getSize();
      if (Update.begin(contentLength)) {
          WiFiClient* stream = binHttp.getStreamPtr();
          uint8_t buf[1024]; // buffer de 1 KB
          int bytesRead = 0;

          while (bytesRead < contentLength) {
              size_t toRead = min(sizeof(buf), (size_t)(contentLength - bytesRead));
              int c = stream->readBytes(buf, toRead);
              if (c <= 0) break;
              decryptBuffer(buf, c);   // descriptografia
              Update.write(buf, c);    // grava no flash
              bytesRead += c;
          }

          if (Update.end()) {
              Serial.println("Atualização concluída com sucesso!");
              ESP.restart();
          } else {
              Serial.printf("Falha na atualização: %s\n", Update.getErrorString().c_str());
          }
      }
  } else {
      Serial.printf("Falha ao baixar o .bin. Código: %d\n", binCode);
  }

  binHttp.end();
  http.end();
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

  int httpResponseCode = http.PATCH(payload);

  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.println("Resultado da atualização: " + response);
  } else {
    Serial.println("Erro ao atualizar DNS. Código: " + String(httpResponseCode));
  }

  http.end();
}

String getPublicIP() {
  WiFiClient client;
  HTTPClient http;

  http.begin(client, "http://api.ipify.org"); // Retorna apenas o IP em texto puro
  int httpCode = http.GET();
  String ip = "";

  if (httpCode == HTTP_CODE_OK) {
    ip = http.getString();
    ip.trim();
  } else {
    Serial.println("Falha ao obter IP público. Código: " + String(httpCode));
  }

  http.end();
  return ip;
}


