#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <Updater.h>
#include <ESP8266WebServer.h>
#include "secrets.h"
#include "crypto.h"

unsigned long checkInterval = 3600000UL;  // 1 hora
unsigned long lastCheck = 0;

unsigned long dnsUpdateInterval = 300000UL;  // 1 hora
unsigned long dnsLastUpdate = 0;

ESP8266WebServer server(80);  // Servidor HTTP na porta 80

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("Iniciando...");

  WiFi.hostname("ESP8266_DNS");
  WiFi.begin(ssid, password);
  Serial.print("Conectando ao WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  server.on("/", handleRoot);  // Define rota principal
  server.begin();
  Serial.println("Servidor HTTP iniciado!");
  Serial.println("Acesse pelo navegador: http://" + WiFi.localIP().toString());

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
    String publicIP  = getPublicIP();
    if(publicIP != ""){
      Serial.println("IP público atual: " + publicIP);
      String currentDNSIP = getDNSHostIP(CF_HOST);
      Serial.println("IP atual no DNS (" + String(CF_HOST) + "): " + currentDNSIP);
      if (currentDNSIP != publicIP) {
        Serial.println("IP diferente! Atualizando DNS...");
        dnsUpdate(publicIP);
      } else {
        Serial.println("DNS já atualizado, sem mudanças.");
      }
    }
  }

  server.handleClient();

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
    // Serial.println("Resposta da Cloudflare: " + response);

    // --- Verifica se "success":true ---
    if (response.indexOf("\"success\":true") >= 0) {
      Serial.println("✅ DNS atualizado com sucesso!");
    } else {
      Serial.println("❌ Falha ao atualizar DNS (success != true).");
    }

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

String getDNSHostIP(String host) {
  IPAddress resolvedIP;
  if (WiFi.hostByName(host.c_str(), resolvedIP)) {
    return resolvedIP.toString();
  } else {
    Serial.println("Falha ao resolver host: " + host);
    return "";
  }
}


void handleRoot() {
  String html = "<!DOCTYPE html>"
                "<html>"
                "<head>"
                "<meta charset='UTF-8'>"
                "<title>ESP8266</title>"
                "<style>"
                "body { display: flex; justify-content: center; align-items: center; height: 100vh; margin: 0; font-family: Arial; }"
                "h1 { font-size: 3em; }"
                "</style>"
                "</head>"
                "<body>"
                "<h1>ESP8266</h1>"
                "</body>"
                "</html>";
  
  server.send(200, "text/html", html);
}