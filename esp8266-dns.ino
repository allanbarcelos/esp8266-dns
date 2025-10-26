#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <Updater.h>
#include <ESP8266WebServer.h>
#include <ArduinoWebsockets.h>
#include "secrets.h"
#include "crypto.h"

using namespace websockets;

unsigned long checkInterval = 3600000UL;  // 1 hora
unsigned long lastCheck = 0;

unsigned long dnsUpdateInterval = 300000UL;  // 1 hora
unsigned long dnsLastUpdate = 0;

ESP8266WebServer server(80);  // Servidor HTTP na porta 80
WebsocketsServer wsServer;  // WebSocket na porta 81


// Timers para loops não bloqueantes
unsigned long lastHTTP = 0;
unsigned long lastWS = 0;
unsigned long lastSendStats = 0;

// --- Variáveis para cálculo de CPU ---
unsigned long measureStart = 0;
unsigned long idleTime = 0;
float cpuLoad = 0;

// --- HTML da página principal ---
const char htmlMain[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<title>ESP8266 - Status</title>
<style>
body { font-family: Arial; text-align: center; background: #f0f0f0; margin-top: 50px; }
.card { background: white; display: inline-block; padding: 20px; border-radius: 10px; box-shadow: 0 0 10px #ccc; }
h1 { color: #333; }
</style>
</head>
<body>
  <div class="card">
    <h1>Status do ESP8266</h1>
    <p><b>Memória livre:</b> <span id="memoria">0</span> bytes</p>
    <p><b>Uso de CPU:</b> <span id="cpu">0</span>%</p>
  </div>

  <script>
    var ws = new WebSocket('ws://' + window.location.hostname + ':81');
    ws.onmessage = function(event) {
      let data = JSON.parse(event.data);
      document.getElementById('memoria').innerText = data.memoria;
      document.getElementById('cpu').innerText = data.cpu.toFixed(1);
    };
  </script>
</body>
</html>
)rawliteral";

// ------------------------------------------------------------------------


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

  Serial.print("Local IP: ");
  Serial.println(WiFi.localIP());

  // Página principal
  server.on("/", handleRoot);
  server.begin();
  Serial.println("Servidor HTTP iniciado!");

  // WebSocket
  wsServer.listen(81); // Porta 81
  Serial.println("WebSocket iniciado na porta 81");

  measureStart = millis();

  Serial.println("\nConectado!");
  Serial.print("Chip ID: ");
  Serial.println(ESP.getChipId(), HEX);
}

void loop() {
    unsigned long now = millis();

    // --- Loop HTTP Server ---
    if (now - lastHTTP >= 10) {  // checa HTTP a cada 10 ms
        server.handleClient();
        lastHTTP = now;
    }

    // --- Loop WebSocket ---
    if (now - lastWS >= 10) {  // checa WebSocket a cada 10 ms
        wsServer.poll();

        // Aceita clientes WebSocket ativos
        auto client = wsServer.accept();
        if (client.available()) {
            auto msg = client.readBlocking();
            Serial.println("Mensagem recebida: " + msg.data());
            // Aqui você pode processar mensagens do cliente
        }
        lastWS = now;
    }

    // --- Envio de CPU/memória para WebSocket ---
    if (now - lastSendStats >= 1000) {  // a cada 1 segundo
        sendStatsToClients();
        lastSendStats = now;
    }

    // --- Checagem de atualização de firmware ---
    if (now - lastCheck >= checkInterval || lastCheck == 0) {
        lastCheck = now;
        checkForUpdate();
    }

    // --- Checagem de atualização de DNS ---
    if (now - dnsLastUpdate >= dnsUpdateInterval || dnsLastUpdate == 0) {
        dnsLastUpdate = now;
        String publicIP  = getPublicIP();
        if (publicIP != "") {
            String currentDNSIP = getDNSHostIP(CF_HOST);
            if (currentDNSIP != publicIP) dnsUpdate(publicIP);
        }
    }

    // --- Reinício diário ou se WiFi cair ---
    if (now >= 86400000UL) {
        Serial.println("Reboot diário!");
        ESP.restart();
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi desconectado. Reiniciando...");
        ESP.restart();
    }

    // Simulação de CPU idle
    delay(0);
    idleTime++;
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
  server.send_P(200, "text/html", htmlMain);
}

void sendStatsToClients() {
    unsigned long total = millis() - measureStart;
    if (total > 0) {
        cpuLoad = 100.0 * (1.0 - ((float)idleTime / total));
    }
    idleTime = 0;
    measureStart = millis();

    int freeMem = ESP.getFreeHeap();
    String msg = "{\"memoria\":" + String(freeMem) + ",\"cpu\":" + String(cpuLoad) + "}";

    // Envia para todos os clientes WebSocket conectados
    for (auto &client : wsServer.availableClients()) {
        if (client.available()) {
            client.send(msg);
        }
    }
}