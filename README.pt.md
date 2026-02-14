# 📘 **Documentação Técnica — Firmware ESP8266 DDNS + OTA**

[English](README.md) | Português

## 📋 **Resumo do Projeto**

O objetivo do firmware é permitir que um **ESP8266** atue como um agente autônomo de atualização de IP dinâmico e manutenção remota.
Ele:

1. **Atualiza automaticamente o IP público** do servidor no **Cloudflare DNS**, garantindo que o domínio sempre aponte para o IP residencial atual.
2. **Gerencia conexão Wi-Fi** com tolerância a falhas e mecanismos de *backoff* (espera progressiva).
3. **Realiza atualização de firmware OTA (Over-The-Air)** diretamente via **GitHub Releases**, com suporte a **criptografia** do binário.
4. Possui **servidor HTTP local** para diagnóstico básico.
5. Implementa **mecanismos anti-bloqueio e anti-loop**, garantindo alta resiliência.

---

## ⚙️ **Componentes Principais**

| Módulo                | Função                                                              |
| --------------------- | ------------------------------------------------------------------- |
| `ESP8266WiFi.h`       | Conexão e gerenciamento Wi-Fi                                       |
| `ESP8266HTTPClient.h` | Comunicação HTTP e HTTPS                                            |
| `Updater.h`           | Gerenciamento de atualização OTA                                    |
| `EEPROM.h`            | Armazenamento não volátil do contador de falhas                     |
| `ESP8266WebServer.h`  | Servidor HTTP local                                                 |
| `secrets.h`           | Configurações sensíveis (SSID, tokens Cloudflare, GitHub API, etc.) |
| `crypto.h`            | Funções de decriptação do binário OTA                               |

---

## 🧩 **Arquitetura Geral**

```
+----------------------------+
|         Usuário            |
|  (Acesso via domínio DNS)  |
+-------------+--------------+
              |
              v
+----------------------------+
|         Cloudflare         |
| (DNS aponta p/ IP público) |
+-------------+--------------+
              ^
              |
     +--------+--------+
     |     ESP8266     |
     |------------------|
     |  WiFi Manager    |
     |  OTA Updater     |
     |  DNS Updater     |
     |  Web Server      |
     +--------+---------+
              |
              v
      Rede residencial (IP dinâmico)
```

---

## 🔄 **Fluxo Geral de Execução**

1. **Inicialização (`setup()`)**

   * Lê o contador de falhas no EEPROM.
   * Define se deve entrar em modo “espera” (após falhas consecutivas).
   * Inicializa WiFi e o servidor HTTP.

2. **Loop Principal (`loop()`)**

   * Executa o **servidor HTTP** (`server.handleClient()`).
   * Mantém o **estado da conexão WiFi**.
   * Executa **checagem OTA** e **atualização DNS** de forma não bloqueante.
   * Reinicia diariamente para liberar memória.

3. **Gerenciamento de Wi-Fi (`handleWiFi()`)**

   * Controla reconexões automáticas.
   * Usa contadores e EEPROM para evitar reinicializações infinitas.
   * Modo “WAIT” impede o ESP de entrar em loop de reboots.

4. **Atualização OTA (`checkForUpdate()`)**

   * Consulta o **GitHub API** para obter a versão mais recente.
   * Se houver nova versão → baixa o `.bin` → decripta com `decryptBuffer()` → executa `Update.write()`.
   * Após sucesso, reinicia o ESP.

5. **Atualização DNS (`handleDNSUpdate()`)**

   * Obtém IP público via `api.ipify.org`.
   * Resolve o IP atual do domínio via `WiFi.hostByName()`.
   * Se diferentes → atualiza o registro DNS via **API Cloudflare**.

---

## 🧠 **Diagrama de Estados — WiFi**

```text
┌───────────────────┐
│     WIFI_OK       │
│ (Conectado)       │
└───┬───────────────┘
    │
    │ WiFi desconectado
    ▼
┌───────────────────┐
│ WIFI_RECONNECTING │
│ (Tentando ligar)  │
└───┬───────────────┘
    │
    ├─ Reconectado com sucesso → WIFI_OK
    │
    ├─ Falha repetida → incrementa contador
    │
    └─ Excesso de falhas → WIFI_WAIT
        │
        ▼
┌───────────────────┐
│    WIFI_WAIT      │
│ (Espera 30 min)   │
└───┬───────────────┘
    │ Tempo esgotado
    ▼
┌───────────────────┐
│ WIFI_RECONNECTING │
└───────────────────┘
```

---

## 🌐 **Fluxo da Atualização DNS**

```text
┌─────────────────────────────┐
│ handleDNSUpdate()           │
├─────────────────────────────┤
│ 1. Obter IP público (ipify) │
│ 2. Resolver IP atual (DNS)  │
│ 3. Comparar IPs             │
│ 4. Se diferente → PATCH via │
│    Cloudflare API           │
│ 5. Loga sucesso/falha       │
└─────────────────────────────┘
```

---

## 🔄 **Fluxo OTA Seguro (GitHub → ESP8266)**

```text
┌──────────────────────────────┐
│ checkForUpdate()             │
├──────────────────────────────┤
│ 1. Consulta GitHub API       │
│    → extrai tag e binário    │
│ 2. Compara versão local      │
│ 3. Baixa o .bin via HTTPS    │
│ 4. Decripta com crypto.h     │
│ 5. Grava via Update.write()  │
│ 6. Reinicia ESP8266          │
└──────────────────────────────┘
```

---

## 💾 **Persistência de Estado (EEPROM)**

| Endereço | Dado              | Descrição                                           |
| -------- | ----------------- | --------------------------------------------------- |
| 0        | `rebootFailCount` | Quantidade de falhas consecutivas de reconexão WiFi |

**Função:**
Evita ciclos infinitos de reboot, entrando em *modo de espera não bloqueante* após `maxRebootsBeforeWait` falhas.

---

## 🔐 **Segurança**

| Mecanismo                      | Descrição                                         |
| ------------------------------ | ------------------------------------------------- |
| **HTTPS via WiFiClientSecure** | Comunicação criptografada com GitHub e Cloudflare |
| **Token Cloudflare (Bearer)**  | Autenticação segura na API                        |
| **Descriptografia OTA**        | Evita instalação de binários maliciosos           |
| **Contadores EEPROM**          | Protege contra loops e DoS por falhas de WiFi     |

---

## 🌍 **Servidor HTTP Local**

* Porta: **80**
* Endpoint: `/`
* Resposta: HTML simples com título “ESP8266”
  (Usado apenas como verificação de vida / debug)

---

## ⚡ **Tempos e Intervalos**

| Função              | Intervalo  | Descrição                                      |
| ------------------- | ---------- | ---------------------------------------------- |
| `checkInterval`     | 1 hora     | Checagem de nova versão OTA                    |
| `dnsUpdateInterval` | 5 minutos  | Atualização do registro DNS                    |
| `reconnectDelay`    | 5 segundos | Tempo entre tentativas de reconexão            |
| `waitAfterFails`    | 30 minutos | Tempo em modo de espera após falhas sucessivas |
| `daily reboot`      | 24 horas   | Reboot preventivo                              |

---

## 🧩 **Principais Variáveis**

| Variável                      | Tipo          | Função                       |
| ----------------------------- | ------------- | ---------------------------- |
| `WifiConnState`               | Enum          | Estado atual do WiFi         |
| `rebootFailCount`             | int           | Contador de falhas no EEPROM |
| `checkInterval`               | unsigned long | Intervalo OTA                |
| `dnsUpdateInterval`           | unsigned long | Intervalo DNS                |
| `lastCheck` / `dnsLastUpdate` | unsigned long | Controle de tempo            |
| `reconnectAttempts`           | int           | Tentativas de reconexão      |

---

## 🧰 **Dependências Externas (secrets.h)**

O arquivo `secrets.h` deve conter:

```cpp
const char* ssid = "MinhaRedeWiFi";
const char* password = "SenhaDaRede";

const char* firmware_version = "v1.0.0";
const char* github_api = "https://api.github.com/repos/usuario/repositorio/releases/latest";

#define CF_ZONE "ZONE_ID"
#define CF_RECORD "RECORD_ID"
#define CF_TOKEN "API_TOKEN"
#define CF_HOST "dominio.exemplo.com"
```

---

## 📈 **Diagrama de Sequência — Atualização DNS + OTA**

```text
ESP8266         Cloudflare         GitHub
   |                 |                 |
   |--- GET ipify -->|                 |  (obter IP público)
   |<-- resposta ----|                 |
   |--- GET DNS ---->|                 |  (resolve IP atual)
   |<-- resposta ----|                 |
   |--- PATCH DNS --->|                 |  (atualiza se diferente)
   |<-- OK ----------|                 |
   |---------------------------------->|  (verifica GitHub API)
   |<----------------------------------|  (recebe JSON release)
   |---------------------------------->|  (baixa .bin criptografado)
   |<----------------------------------|  (stream OTA)
   |   Decripta + Update.write()       |
   |--- Reinicia --------------------->|
```

---

## 🧱 **Boas Práticas Implementadas**

✅ Evita *watchdog resets* com `yield()` no loop de OTA.
✅ EEPROM usada de forma eficiente e não bloqueante.
✅ WiFi reconectado de forma escalonada (sem travar o sistema).
✅ Operações HTTP são **não bloqueantes**, sempre retornando controle ao loop.
✅ Código preparado para **ambientes remotos e ininterruptos (headless IoT)**.

---

## 🧩 **Extensões Possíveis**

* **Adição de API local** (ex: `/status`) retornando JSON com IP atual, uptime, versão e estado WiFi.
* **Criptografia AES real** no `crypto.h` (ex: AES-CTR).
* **Sincronização NTP** para timestamps reais.
* **LED indicador** (ex: piscando para reconexão, fixo para online).
* **Modo de configuração Wi-Fi via portal (WiFiManager)**.

---

## 🏁 **Resumo Final**

| Função                    | Descrição                                  |
| ------------------------- | ------------------------------------------ |
| **DDNS Automático**       | Mantém o IP atualizado no Cloudflare       |
| **OTA Seguro via GitHub** | Atualiza firmware remotamente              |
| **WiFi Resiliente**       | Reconexão automática com espera controlada |
| **Servidor Local**        | Diagnóstico mínimo                         |
| **Falha Tolerante**       | Usa EEPROM para evitar loops de falha      |