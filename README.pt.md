# 📘 Firmware ESP8266 — DDNS Cloudflare + OTA

[English](README.md) | Português

Firmware para **ESP-01 (ESP8266, 1 MB)** que funciona como agente autônomo de DNS dinâmico:

1. **Mantém registros A do Cloudflare** apontando para o IP público residencial.
2. **Notifica o IP público via [ntfy.sh](https://ntfy.sh)** no boot e sempre que ele mudar, com ou sem Cloudflare configurado.
3. **Atualiza o próprio firmware (OTA)** a partir dos GitHub Releases, com **assinatura RSA verificada** no aparelho.
4. Oferece um **painel web local** (autenticado) para configuração e diagnóstico.
5. Tem **watchdogs** de Wi-Fi, memória e reinício diário.

---

## 🧩 Arquitetura

Cada serviço é um header com dependências injetadas por referência, composto em camadas por `App.h`. O `.ino` só chama `app.begin()` e `app.tick()`. Todos os `tick(now)` são não bloqueantes.

| Arquivo              | Responsabilidade                                                               |
| -------------------- | ------------------------------------------------------------------------------ |
| `App.h`              | Composição dos serviços, `setup()`/`loop()`                                    |
| `Config.h`           | Estrutura de configuração e estado em runtime; credenciais do AP              |
| `ConfigStore.h`      | Persistência em `/config.json` (LittleFS) com **escrita atômica**             |
| `Logger.h`           | Buffer circular de logs (Serial + painel)                                      |
| `Clock.h`            | Relógio via SNTP (necessário para validar certificados TLS)                   |
| `WiFiService.h`      | Conexão, modo de configuração (AP), reconexão e watchdogs                      |
| `PublicIPResolver.h` | IPv4 público via HTTPS (raízes de `CloudflareCA.h`), com cache de 60 s       |
| `CloudflareDNS.h`    | PATCH dos registros via API Cloudflare (TLS validado com `CloudflareCA.h`)    |
| `NtfyNotifier.h`     | Notificação do IP público; resolve o IP por conta própria                     |
| `OTAService.h`       | Checagem de versão e download do firmware assinado                            |
| `WebService.h`       | Painel HTTP (Basic Auth + CSRF + escape de HTML)                               |
| `Version.h`          | `firmware_version` (injetado pelo CI; `"dev"` em builds locais)               |

---

## 🚀 Primeiro uso

1. Grave o firmware via serial (veja [Build local](#-build-local)).
2. Sem Wi-Fi configurado, o ESP cria a rede **`ESP_Config`** (senha padrão **`esp8266dns`**, altere no build com `-DAP_PASSWORD=\"...\"`).
3. Acesse `http://192.168.4.1`, informe o Wi-Fi e **defina usuário e senha do painel** (mínimo de 8 caracteres).
4. Após reiniciar, acesse o IP local do ESP e configure **Cloudflare** (token, Zone ID, Record IDs) e, se quiser, o tópico **ntfy**.

> O painel exige senha. Sem senha definida, todas as páginas redirecionam para `/password`.
> Se o Wi-Fi configurado falhar no boot, o ESP sobe o AP **e** continua tentando a rede em segundo plano: ao conectar, o AP é desligado.

### Token do Cloudflare

Crie um token com escopo mínimo: **Zone → DNS → Edit**, restrito à zona usada.

---

## 🌐 Atualização de DNS

A cada 60 s (com Wi-Fi conectado e relógio sincronizado):

1. Obtém o IP público por HTTPS. O corpo precisa ser um único IPv4 e o certificado precisa encadear nas raízes de `CloudflareCA.h`.
2. Se o IP for diferente do **último IP sincronizado** (em memória), envia `PATCH` para cada registro.
3. Só marca como sincronizado quando **todos** os registros são atualizados. Em caso de falha, tenta de novo no próximo ciclo.

A busca tenta `api4.ipify.org`, depois `icanhazip.com`, depois `ifconfig.co`. Os três respondem com um IPv4 puro e apresentam a cadeia WE1 → GTS Root R4, a mesma que `api.cloudflare.com` já valida nos aparelhos em campo. Nenhuma raiz nova é adicionada. A primeira resposta válida fica em cache por 60 s e é compartilhada: DNS e ntfy abrem uma sessão TLS por ciclo.

O ntfy lê esse resultado por conta própria. Um tópico sem token Cloudflare continua resolvendo e notificando.

Não dependemos do DNS resolvido localmente: ele tem cache/TTL e, em registros *proxied*, devolve IPs da Cloudflare. Após cada boot há um `PATCH` inicial, que é idempotente.

A conexão com `api.cloudflare.com` valida o certificado contra as raízes em `CloudflareCA.h` (GTS Root R1/R4, ISRG Root X1/X2).

---

## 🔄 OTA assinado

```text
GitHub Releases ──(vN)──► ESP8266
  firmware.signed.bin = gzip(firmware) + assinatura RSA-2048/SHA-256
```

- De hora em hora (até ~23 h de uptime), consulta `releases/latest`. Só instala tags `vN` **maiores** que a atual, o que impede downgrade.
- O build com `public.key` faz o `Updater` **verificar a assinatura** em `Update.end()` e gravar só o gzip. A instalação manual baixa o mesmo arquivo da automática: `firmware.signed.bin` se a chave está no build, `firmware.bin` se não está. O eboot lê o tamanho descomprimido nos 4 últimos bytes; no arquivo assinado esses bytes são o tamanho da assinatura, e gravá-lo sem remover o trailer corrompe o sketch.
- O PlatformIO embute `public.key` antes de compilar (`pre:tools/embed_pubkey.py`), como o arduino-cli no CI. O link falha se o ELF não tiver o verificador e essa chave. O teste nativo não define `ARDUINO_SIGNING` e por isso exige `firmware.bin`.
- A imagem é **comprimida com gzip** (o bootloader descomprime). Sem isso, firmware atual + novo não cabem juntos na área de sketch de 1 MB.
- O linker `ld/eagle.flash.1m64.ota.ld` encolhe o IROM em **400 KB**. Esse buraco fica contra o sistema de arquivos e o sketch nunca o ocupa; firmware maior que o teto falha no link. O `package_ota.py` também recusa gzip/assinatura maior que a reserva.
- O download é abortado se o Wi-Fi cair, em erro de escrita ou após 60 s sem dados. A sessão TLS com o GitHub usa 16 KB (o GitHub não negocia MFLN; 4 KB travam o handshake). O GET do asset segue os 302 num buffer fixo de headers (o `String` por linha do HTTPClient com 16 KB de TLS gerava Exception 29). No CDN o aparelho tenta MFLN e cai para 4 KB se o host aceitar. DNS e ntfy param durante o download. Falha transitória retenta com backoff; se a heap não bastar, grava `/ota.pending` e reinicia em modo recuperação (sem IoT) para concluir o OTA.

### Assets do release

| Arquivo               | Uso                                                                              |
| --------------------- | -------------------------------------------------------------------------------- |
| `firmware.signed.bin` | gzip + assinatura: build que embute `public.key`                                |
| `firmware.bin`        | gzip puro: build sem a chave, para o eboot ler o tamanho certo                  |

### Chave de assinatura

- `public.key` fica versionada no repositório.
- A chave privada **nunca** vai para o repositório (está no `.gitignore`). No CI ela vem do secret **`OTA_SIGNING_KEY`** (conteúdo PEM).
- **Guarde um backup da chave privada.** Sem ela, aparelhos com o firmware novo não aceitam mais atualizações OTA e só podem ser regravados via serial.

Para gerar um novo par:

```bash
openssl genrsa -out private.key 2048
openssl rsa -in private.key -outform PEM -pubout -out public.key
gh secret set OTA_SIGNING_KEY < private.key
```

---

## 🛡️ Resiliência

| Mecanismo                | Comportamento                                                                  |
| ------------------------ | ------------------------------------------------------------------------------ |
| Wi-Fi caiu               | Checagem a cada 15 s, reconexão; 5 falhas seguidas → reinício                 |
| WAN inacessível          | A busca do IP falha, é registrada e tenta de novo em 60 s. O reinício fica reservado à perda de associação Wi-Fi (5 falhas) e ao ciclo diário. |
| Memória crítica          | Heap < 15 KB ou fragmentação > 50 % em 3 checagens seguidas (10 s) → reinício |
| Reinício diário          | Após 24 h de uptime (adiado durante OTA)                                       |
| Modo de configuração     | Nenhum watchdog derruba o AP enquanto ele está ativo                           |

---

## 🧪 Testes

Testes nativos (Unity + PlatformIO) com stubs do core Arduino em `test/stubs`. O CI exige **100 % de cobertura** de linhas, funções e branches dos headers do firmware.

```bash
python -m venv venv && . venv/bin/activate
pip install -r requirements.txt

pio test -e native        # qualquer compilador do host
./test/coverage.sh        # requer gcc (Linux) — mesmo comando do CI
```

---

## 🤖 CI/CD (`.github/workflows/ci.yml`)

| Job       | Quando                              | O que faz                                                                    |
| --------- | ----------------------------------- | ---------------------------------------------------------------------------- |
| `plan`    | sempre                              | Decide se haverá release e calcula a próxima tag `vN` (ordenação numérica)   |
| `test`    | sempre                              | Testes + cobertura 100 %                                                      |
| `build`   | sempre                              | Compila com arduino-cli, confere verificador de assinatura e limites de flash |
| `release` | push na `main` alterando firmware   | Comprime, assina, **verifica a assinatura** e publica o release              |

Mudanças só em testes, docs ou workflow não geram release. Para forçar, use *Run workflow* com `release: true`.

---

## 🔌 IoT

A página **IoT** configura um sensor e um módulo na ESP-01, sem editar o firmware. Os pinos possíveis são GPIO0, GPIO2, TX e RX. O desenho fica em `docs/esp01-pinout.png` e o painel carrega essa imagem do repositório. A página inicial mostra a foto do módulo em `docs/esp-01.jpg` (SparkFun Electronics, CC BY 2.0).

O sensor pode ser digital, PIR, porta, chuva, chama, gás, solo, luz, som, boia ou botão. Por padrão seguem o nível. No modo alternar, cada pulso inverte o módulo e ele permanece. DHT11, DHT22, DS18B20, pulsos a cada 10 s ou ultrassom HC-SR04 no mesmo pino (trig e echo juntos, em cm) ligam o módulo acima ou abaixo de um limite, com margem para não oscilar. O módulo pode ser relé, LED, buzzer, bomba ou válvula (nível fixo), servo (um ângulo para ligado e outro para desligado) ou ventilador e motor em PWM de 0 a 100. O PWM proporcional varia a potência entre o limite e o limite alto. O comando manual força ligado ou desligado e ignora o sensor até voltar ao automático. DHT e DS18B20 pedem resistor de pull-up. O servo usa fonte de 5 V separada e o GND em comum.

Sem as chaves novas no `/config.json`, o comando fica automático, o botão segue o nível, o PWM fica em dois patamares, sensor e módulo ficam digitais e a função continua desligada até o usuário escolher os pinos. A leitura segue no tópico ntfy já cadastrado como `[sensor]`. O aviso de IP no mesmo tópico usa `[ip]`.

---

## 📋 Log remoto (implementação futura)

Ainda não está no firmware. O anel local (`Logger.h`, 10 linhas de 128 caracteres, Serial e painel) permanece o limite da RAM. A cópia segue pelo [ntfy.sh](https://ntfy.sh), no tópico já cadastrado, com o prefixo `[log]`. O IP já sai como `[ip]` e o sensor como `[sensor]`.

- Um POST reúne várias linhas, no mesmo intervalo de 60 s do aviso de IP, e só quando há heap e bloco livres para o TLS. O ESP continua falando com esse único host.
- Sem a chave nova no `/config.json`, o envio fica desligado. Aparelhos já em campo seguem só com o log local.
- O ntfy.sh público retém cada mensagem por 12 horas. Para arquivo sem prazo, `ntfy subscribe` nesse tópico grava a saída num arquivo.
- A linha `Reset:` logo após o boot entra nesse envio e registra a causa do ciclo anterior. Uma queda no meio do TLS não chega a ser publicada.

---

## 🔧 Build local

O ESP-01 não tem reset automático: para gravar, ligue o **GPIO0 ao GND** e reconecte o adaptador. Depois da gravação, solte o GPIO0 e reconecte de novo para rodar o firmware.

**PlatformIO** (o `platformio.ini` já embute a `public.key`, o linker script do OTA e tira o `float` do `printf`):

```bash
pio run -e esp8266                                   # compila
pio run -e esp8266 -t erase  --upload-port COM4      # opcional: apaga a flash (inclusive a configuração)
pio run -e esp8266 -t upload --upload-port COM4      # depois de apagar, volte ao modo de gravação
pio device monitor -b 115200 -p COM4
```

**arduino-cli** (mesmas flags do CI):

```bash
arduino-cli config init   # pule se já tiver configuração do arduino-cli
arduino-cli config add board_manager.additional_urls https://arduino.esp8266.com/stable/package_esp8266com_index.json
arduino-cli core update-index
arduino-cli core install esp8266:esp8266@3.1.2
arduino-cli lib install "ArduinoJson@6.21.6"
python3 tools/ota_flash_ld.py
arduino-cli compile --fqbn esp8266:esp8266:generic:eesz=1M64 \
  --build-property build.flash_ld=eagle.flash.1m64.ota.ld \
  --build-property "build.float=" \
  --build-property "build.extra_flags=-DARDUINO_SIGNING=1" \
  --output-dir build
arduino-cli upload  --fqbn esp8266:esp8266:generic:eesz=1M64 -p /dev/ttyUSB0 --input-dir build
arduino-cli monitor -p /dev/ttyUSB0 --config 115200
```

Builds locais têm versão `dev` e atualizam para o último release na primeira checagem OTA (se a atualização automática estiver ligada).

---

## 📄 Licença

[MIT](LICENSE) © 2026 Allan Barcelos. Todo arquivo de código traz um cabeçalho SPDX (`SPDX-License-Identifier: MIT` e a linha de copyright); `python3 tools/license_headers.py` acrescenta o cabeçalho em arquivos novos e o workflow **License headers** recusa uma mudança sem ele. Componentes de terceiros e os pedidos de cortesia estão no [NOTICE](NOTICE). Para relatar uma vulnerabilidade, veja o [SECURITY.md](SECURITY.md).
