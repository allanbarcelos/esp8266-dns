#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <Updater.h>
#include "secrets.h"
#include "crypto.h"

unsigned long checkInterval = 3600000UL;  // 1 hora
unsigned long lastCheck = 0;

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
  if (millis() - lastCheck >= checkInterval || lastCheck == 0) {
    lastCheck = millis();
    checkForUpdate();
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
