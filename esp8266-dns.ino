#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include "secrets.h"

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
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();

    int idx = payload.indexOf("\"browser_download_url\"");
    if (idx > 0) {
      int start = payload.indexOf("https://", idx);
      int end = payload.indexOf("\"", start);
      String binUrl = payload.substring(start, end);

      Serial.println("Nova release detectada:");
      Serial.println(binUrl);

      t_httpUpdate_return ret = ESPhttpUpdate.update(client, binUrl);

      switch (ret) {
        case HTTP_UPDATE_FAILED:
          Serial.printf("Atualização falhou. Erro (%d): %s\n",
                        ESPhttpUpdate.getLastError(),
                        ESPhttpUpdate.getLastErrorString().c_str());
          break;
        case HTTP_UPDATE_NO_UPDATES:
          Serial.println("Nenhuma atualização disponível.");
          break;
        case HTTP_UPDATE_OK:
          Serial.println("Atualizado com sucesso!");
          break;
      }
    } else {
      Serial.println("Nenhum arquivo .bin encontrado no release.");
    }
  } else {
    Serial.printf("Falha ao acessar API GitHub. Código: %d\n", httpCode);
  }

  http.end();
}
