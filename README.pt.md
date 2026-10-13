# 📘 ESP8266 IoT — DDNS Cloudflare, OTA e IoT local

[English](README.md) | Português

Firmware para o **ESP-01 (ESP8266, 1 MB)**. Um aparelho faz três trabalhos:

1. **DNS dinâmico**: mantém os registros A do Cloudflare apontando para o IP público residencial e envia esse IP pelo [ntfy.sh](https://ntfy.sh).
2. **IoT**: um sensor e um atuador, cada um opcional. Operado pelo painel, por uma **API local** que funciona sem internet e por **comandos do ntfy** entregues em cerca de um segundo.
3. **Manutenção sozinho**: atualização OTA assinada pelo GitHub Releases, watchdogs de Wi-Fi e memória e reinício diário.

O painel web é autossuficiente (CSS e imagens ficam no firmware): funciona no modo de configuração e em redes sem internet.

---

## 🧩 Arquitetura

Cada serviço é um header com dependências injetadas por referência, compostas em camadas pelo `App.h`. O `.ino` só chama `app.begin()` e `app.tick()`. Todo `tick(now)` é não bloqueante.

| Arquivo              | Responsabilidade                                                                  |
| -------------------- | --------------------------------------------------------------------------------- |
| `App.h`              | Composição dos serviços, `setup()`/`loop()`, uma sessão TLS por vez              |
| `Config.h`           | Configuração e estado em runtime; credenciais do AP; `IoTConfig`                 |
| `ConfigStore.h`      | `/config.json` (LittleFS) com **escrita atômica**, número de versão e migrações  |
| `Logger.h`           | Buffer circular de linhas de log (Serial + painel)                               |
| `Clock.h`            | Relógio via SNTP (necessário para validar certificados TLS)                      |
| `TlsTrim.h`          | Deixa fora do firmware as cifras e curvas do BearSSL que não são usadas (só no sketch) |
| `WiFiService.h`      | Conexão, modo de configuração (AP), reconexão e watchdogs                        |
| `PublicIPResolver.h` | IPv4 público via HTTPS (raízes em `CloudflareCA.h`), em cache por 60 s          |
| `CloudflareDNS.h`    | PATCH dos registros pela API do Cloudflare                                       |
| `NtfyNotifier.h`     | ntfy nos dois sentidos: publica IP e eventos do IoT; assina o tópico de comandos |
| `OTAService.h`       | Checagem de versão e download do firmware assinado                               |
| `IoTCatalog.h`       | Catálogo de sensores e atuadores, por identificador estável e capacidade        |
| `IoTCommand.h`       | Interpretador de comandos de texto, o mesmo no painel, na API e no ntfy         |
| `IoTService.h`       | Leitura do sensor, saída do atuador, regra, pulso, desligamento de segurança, JSON |
| `IoTState.h`         | Último comando manual (`/iot.state`), para "ao religar"                          |
| `Ws2812.h`           | Driver WS2812 de uma cor, sem biblioteca                                         |
| `WebService.h`       | Painel HTTP (Basic Auth + CSRF + escape de HTML) e API local (token Bearer)     |
| `Assets.h`           | Imagens do painel embutidas no firmware (geradas por `tools/embed_assets.py`)   |
| `WebAssets.h`        | CSS e JavaScript do painel, em gzip (gerados de `web/` por `tools/embed_web.py`) |
| `Version.h`          | `firmware_version` (injetado pelo CI; `"dev"` em build local)                    |

---

## 🚀 Primeiro uso

1. Grave pela serial (veja [Build local](#-build-local)).
2. Sem Wi-Fi configurado, o ESP cria a rede **`ESP_Config`** (senha padrão **`esp8266dns`**, trocável no build com `-DAP_PASSWORD=\"...\"`).
3. Abra `http://192.168.4.1`, informe o Wi-Fi e **defina usuário e senha do painel** (mínimo de 8 caracteres).
4. Depois do reinício, abra o IP local do ESP. Configure **Cloudflare**, **ntfy** e **IoT** conforme precisar; cada parte é opcional.

> O painel exige senha. Até ela ser definida, toda página redireciona para `/password`.
> Se o Wi-Fi configurado falhar no boot, o ESP sobe o AP **e** continua tentando a rede em segundo plano. Quando conecta, o AP é desligado.

Reserve o IP do aparelho no roteador (reserva de DHCP): o painel e a API local são acessados pelo IP. A descoberta por nome via mDNS (`.local`) foi medida e não cabe (veja [Orçamento de flash](#-orçamento-de-flash)).

### Token do Cloudflare

Crie um token de privilégio mínimo: **Zone → DNS → Edit**, restrito à zona em uso.

---

## 🔌 IoT

O ESP-01 expõe quatro pinos utilizáveis (GPIO0, GPIO2, TX/GPIO1, RX/GPIO3) e nenhuma entrada analógica: o aparelho controla **um sensor e um atuador**, ambos digitais. O desenho dos pinos está na página IoT (`docs/esp01-pinout.svg`, embutido).

### O que o aparelho faz

A página IoT começa por uma escolha:

| Papel | Comportamento |
| --- | --- |
| **Só sensor** | Lê e publica no ntfy (`[sensor] …`). |
| **Só atuador** | Obedece comandos do painel, da API local e do ntfy. |
| **Sensor e atuador** | Os dois; com a **regra** ligada, o sensor comanda o atuador no modo automático. Um comando manual vale até `auto`. |
| **Nada** | IoT desligado; os pinos ficam livres. |

Cada escolha mostra só os campos que importam. Cada tipo de sensor e de atuador mostra uma dica de ligação de uma linha (pull-up, alimentação, GPIO0/GPIO2 no boot).

### Sensores

| Identificador | Sensor | Leitura |
| --- | --- | --- |
| `dht22`, `dht11` | Temperatura e umidade | °C e % (a regra usa a grandeza escolhida) |
| `ds18b20` | Temperatura (1-Wire) | °C |
| `pir` | Presença | presença / livre |
| `contato` | Porta ou janela (reed) | aberta / fechada |
| `botao` | Botão ou interruptor | pressionado / solto |
| `digital` | Saída digital genérica (chuva, gás, chama, solo, boia, som) | nomes de estado escolhidos por você |
| `distancia` | HC-SR04 / JSN-SR04T (trig e echo no mesmo pino) | cm |
| `pulsos` | Vazão ou pulsos (YF-S201) | pulsos a cada 10 s |

### Atuadores, por capacidade

A capacidade define o controle mostrado no painel e os comandos aceitos.

| Capacidade | Identificadores | Controle no painel | Comandos |
| --- | --- | --- | --- |
| **Liga/desliga** | `rele`, `bomba`, `valvula`, `fechadura`, `buzzer` | interruptor + pulso | `ligar`, `desligar`, `alternar`, `pulso 5` |
| **Intensidade (PWM)** | `led`, `ventilador`, `motor` | interruptor + faixa 0–100 % + pulso | os acima + `50%`, `nivel 50` |
| **Posição** | `servo` | abrir/fechar + faixa 0–180° + pulso | `abrir`, `fechar`, `angulo 90`, `pulso 3` |
| **Cor** | `fita` (WS2812, até 60 LEDs) | interruptor + seletor de cor + brilho | os acima + `cor #ff8800`, `brilho 40` |

Todo atuador aceita também `auto` (volta à regra) e `status` (não muda nada; devolve o estado). Maiúsculas e minúsculas tanto faz, e há sinônimos em inglês (`on`, `off`, `toggle`, `pulse`, `level`, `angle`, `color`, `open`, `close`). `abrir`/`fechar` são ligar/desligar: o servo vai para o ângulo aberto ou fechado.

Para cada atuador:

- **Pulso**: liga por N segundos (1–3600) e desliga. Um pulso nunca é gravado como estado a restaurar: uma fechadura não reabre sozinha depois de uma queda de energia. Com a regra, o fim do pulso volta ao automático.
- **Desligamento de segurança**: "desligar sozinho depois de N minutos" (0 = nunca), para bombas, válvulas e aquecedores. Com a regra, o atuador fica no manual depois, senão a regra religaria na hora.
- **Ao religar o aparelho**: desligado, ou o último comando manual (`/iot.state`).

### Regra (sensor e atuador)

- **Sensor digital**: acompanhar (ligado enquanto ativo) ou inverter (cada acionamento inverte o atuador).
- **Sensor numérico**: liga acima ou abaixo de um limite, com margem para desligar (histerese), para o relé não ficar oscilando.
- **Intensidade proporcional**: um atuador PWM acompanha a leitura de forma linear entre o limite e um limite alto.

O formulário escreve a regra como frase ("Liga o ventilador quando o valor ficar acima de 28 °C e desliga ao voltar abaixo de 26 °C") antes de salvar.

### Cartão "Agora"

Mostra a leitura e o estado do atuador, com o controle de verdade da capacidade dele. Os comandos saem sem recarregar a página e os valores se atualizam a cada 5 s (`/iot/now`), sem perder o que está sendo editado no formulário. Os valores são anunciados a leitores de tela (`aria-live`); o estado é sempre escrito, nunca só por cor.

### Descadastrar

**Descadastrar sensor e atuador**, no fim da página IoT, abre uma página de confirmação (funciona sem JavaScript) antes de agir. Confirmado, apaga o sensor, o atuador, a regra, o estado salvo (`/iot.state`) e o token da API: o atuador desliga e a API local deixa de responder até um novo cadastro, que gera um token novo.

---

## 🖧 API local

A mesma leitura e os mesmos comandos, para scripts e automação residencial na rede local, **sem internet**. As rotas só existem com sensor ou atuador configurado (404 sem eles).

```bash
curl -H 'Authorization: Bearer <token>' http://192.168.1.50/api/state
curl -H 'Authorization: Bearer <token>' -d ligar http://192.168.1.50/api/cmd
curl -H 'Authorization: Bearer <token>' -d 'cmd=pulso 5' http://192.168.1.50/api/cmd
```

```json
{"api":1,
 "sensor":{"type":"dht22","status":"ok","text":"24.5 C, 61.0%","temperature":24.5,"humidity":61.0},
 "actuator":{"type":"ventilador","capability":"level","on":true,"mode":"auto","text":"ligado 60%","level":60}}
```

- O token (32 caracteres hexadecimais do gerador aleatório de hardware) é separado da senha do painel. Ele é criado quando o IoT é configurado pela primeira vez e aparece na página IoT com estes exemplos; **Gerar token novo** invalida o anterior. A comparação é feita em tempo constante.
- `sensor` ou `actuator` vem `null` quando não está em uso. `status` é `ok`, `waiting` ou `error`. Campos extras por tipo: `active`, `temperature`, `humidity`, `distance`, `pulses`, `level`, `angle`, `color`, `pulse` (segundos restantes).
- Um comando que não é entendido ou não serve ao atuador devolve **400** com `{"error":"..."}`; nada muda.
- O Home Assistant lê com o sensor `rest` e envia comandos com `rest_command`.

> O painel e a API usam HTTP puro (o ESP-01 não tem memória para um servidor TLS). Use-os numa rede confiável e não redirecione a porta 80 para o aparelho.

---

## 📣 ntfy

### Tópico de dados (envio)

- `[ip] 1.2.3.4` no boot e sempre que o IP público muda. Não depende do Cloudflare.
- `[sensor] …` e `[atuador] …` com **Enviar leituras e mudanças ao ntfy** ligado. Mudanças e respostas a comandos saem em segundos; leituras numéricas periódicas, no máximo uma vez por minuto (limite do ntfy.sh).
- Com tópico de comandos configurado, cada notificação do IoT traz até **três botões de ação**, que publicam um comando de volta: Ligar/Desligar/Pulso, Desligar/50 %/100 % ou Abrir/Fechar/Pulso, conforme o atuador. Com a regra ligada, o terceiro botão é **Auto**. Dá para operar pela notificação do celular, sem digitar.

### Tópico de comandos (recebimento)

Defina um **tópico de comandos** na página ntfy (o botão gera um nome aleatório). O aparelho assina o tópico com uma conexão contínua (`GET /<tópico>/json`) e cada mensagem vira um comando, entregue em cerca de um segundo. Respostas a comandos inválidos vão ao tópico de dados como `[erro] …`.

- **Uma sessão TLS por vez.** O ESP-01 tem memória para uma única sessão TLS. Antes de publicar, ou quando o Cloudflare ou o OTA vão usar a rede, o fluxo fecha. Ao reabrir, ele pede `since=<último id>`, e o ntfy (que guarda as mensagens por 12 horas) entrega o que chegou no intervalo. Nada se perde; no pior caso, um comando espera alguns segundos.
- **O certificado do ntfy.sh é validado** (ISRG Root X1, já embutida), no fluxo de comandos e no envio: um comando forjado no caminho acionaria equipamento de verdade, e o envio leva o token e, nos botões de ação, o tópico de comandos. Até o relógio sincronizar nada é enviado; a mensagem espera a próxima tentativa.
- Comandos com mais de 10 minutos são ignorados. Conexões que falham tentam de novo de 5 s até 5 min. Um fluxo sem nada por 2 minutos é reaberto (o ntfy manda keepalive a cada 45 s).
- Tópicos do ntfy.sh são públicos pelo nome: quem souber o tópico de comandos pode mandar comandos. Use um nome longo e aleatório, diferente do tópico de dados, ou um **token de acesso do ntfy** (usado no envio e na assinatura). Os botões de ação levam o tópico de comandos (e o token, se houver): mantenha o tópico de dados privado também.

---

## 🔁 Compatibilidade e novos módulos

Os aparelhos instalados se atualizam sozinhos, então o firmware nunca quebra uma configuração existente:

- **Identificadores estáveis.** A configuração grava um sensor ou atuador pelo identificador (`"actuator":"rele"`), nunca pela posição numa tabela. Identificadores nunca são renomeados nem reaproveitados; nomes de exibição e ordem podem mudar à vontade.
- **Configuração com versão.** O `/config.json` tem `"v"`. Ao carregar, uma versão antiga passa pelas migrações em sequência. A versão 1 (tipos gravados pela posição na tabela) vira a versão 2 no primeiro boot, e o original fica em `/config.v1.json`. Campo que falta recebe o padrão; campo desconhecido é ignorado. Na v1 o módulo só funcionava com sensor; depois da migração, um módulo sem sensor continua parado, então nenhum pino passa a ser acionado com a atualização. O "comando" da v1 (forçar ligado/desligado) vira o estado manual guardado.
- **Tipo desconhecido, parada segura.** Se um OTA que voltou atrás deixar um firmware mais antigo lendo uma configuração mais nova, o tipo desconhecido deixa aquela parte desligada (atuador solto), mostra um aviso na página IoT e **o arquivo não é regravado**. Atualizar de novo volta tudo.
- **Contrato da API.** Campos e comandos só são acrescentados; os existentes mantêm nome e significado. `"api":1` marca a versão.

Um módulo novo que usa uma capacidade existente é **uma linha** no `IoTCatalog.h`; painel, comandos, API, ntfy e botões de ação passam a funcionar sem outro código:

```cpp
{"aquecedor", _A_HEATER, C_ONOFF, 60},   // identificador, nome, capacidade, pulso sugerido (s)
```

Só uma capacidade nova (um motor de passo, por exemplo) pede código: uma saída no `IoTService` e um controle no `WebService`. O CI testa as migrações com arquivos de configuração reais de cada versão.

---

## 🌐 Atualização do DNS

A cada 60 s (com Wi-Fi conectado e relógio sincronizado):

1. Busca o IP público via HTTPS. A resposta precisa ser um único IPv4 e o certificado precisa chegar às raízes do `CloudflareCA.h`.
2. Se for diferente do **último IP sincronizado** (guardado em memória), envia um `PATCH` para cada registro.
3. Só marca como sincronizado quando **todos** os registros dão certo. Em qualquer falha, tenta de novo no ciclo seguinte.

A busca tenta `api4.ipify.org`, depois `icanhazip.com`, depois `ifconfig.co`. Os três respondem só o IPv4 e apresentam a cadeia WE1 → GTS Root R4, a mesma que o `api.cloudflare.com` já valida nos aparelhos em campo. A primeira resposta válida fica em cache por 60 s e é compartilhada: DNS e ntfy abrem uma sessão TLS por ciclo.

O firmware não depende da resolução DNS local: ela tem cache por TTL e devolve IPs do Cloudflare em registros *proxied*. Cada boot faz um `PATCH` inicial, que é idempotente. A conexão com `api.cloudflare.com` valida o certificado pelas raízes do `CloudflareCA.h` (GTS Root R1/R4, ISRG Root X1/X2).

---

## 🔄 OTA assinado

```text
GitHub Releases ──(vN)──► ESP8266
  firmware.signed.bin = gzip(firmware) + assinatura RSA-2048/SHA-256
```

- A cada hora (até ~23 h ligado) consulta `releases/latest`. Só instala tags `vN` **maiores** que a atual, o que impede downgrade.
- Um build que embute `public.key` faz o `Updater` do core **verificar a assinatura** em `Update.end()` e gravar só o gzip. A instalação manual baixa o mesmo arquivo da checagem por hora: `firmware.signed.bin` quando a chave está no build, `firmware.bin` quando não está. O bootloader lê o tamanho descomprimido nos 4 bytes finais; no arquivo assinado esses bytes são o tamanho da assinatura, e gravar esse final corromperia o sketch.
- O gzip é completado (campo FNAME) até um múltiplo de 4 bytes: o `Updater` do core faz o hash em leituras de 4 bytes e falha em silêncio com um final de outro tamanho.
- O PlatformIO embute `public.key` antes de compilar (`pre:tools/embed_pubkey.py`), como o arduino-cli faz no CI. O link falha se o ELF não tiver o verificador e essa chave.
- O linker script `ld/eagle.flash.1m64.ota.ld` encurta a IROM em **400 KB**. Esse espaço fica junto ao sistema de arquivos e o sketch nunca o ocupa; um firmware que passar do limite falha no link. O `package_ota.py` também recusa um gzip/assinatura maior que a reserva.
- O download é abortado se o Wi-Fi cair, uma escrita na flash falhar ou nenhum dado chegar por 60 s. O TLS do GitHub usa buffer de 16 KB. DNS, ntfy e o fluxo de comandos do ntfy pausam durante o download. Se a heap não comportar a sessão, o aparelho grava `/ota.pending` e reinicia em modo de recuperação (IoT desligado) para concluir o OTA. Uma versão recusada por `Update.end()` fica marcada em `/ota.rejected` e só uma instalação manual a baixa de novo.

### Chave de assinatura

- `public.key` fica no repositório.
- A chave privada **nunca** vai para o repositório (está no `.gitignore`). O CI lê do segredo **`OTA_SIGNING_KEY`** (conteúdo PEM).
- **Faça backup da chave privada.** Sem ela, os aparelhos com o firmware novo recusam todo OTA e só podem ser regravados pela serial.

```bash
openssl genrsa -out private.key 2048
openssl rsa -in private.key -outform PEM -pubout -out public.key
gh secret set OTA_SIGNING_KEY < private.key
```

---

## 📏 Orçamento de flash

A flash de 1 MB guarda o firmware, uma **reserva OTA de 400 KB** (o próximo firmware, comprimido) e um **LittleFS de 64 KB** para a configuração. Toda função precisa caber na área de programa e, comprimida, na reserva OTA. Nenhuma funcionalidade ou correção pode levar o firmware além de **500.000 bytes**: o `tools/firmware_limit.py` faz o build do PlatformIO e o CI falharem acima disso.

| Item | Tamanho |
| --- | --- |
| Firmware | ~496 KB de 549 KB (cerca de 56 KB livres); **limite de 500.000 bytes** |
| Pacote OTA assinado | ~356 KB de 400 KB (cerca de 44 KB livres) |
| Imagens embutidas | foto de 5,4 KB (WebP) + pinos de 1,2 KB (SVG com gzip) |
| Sistema de arquivos da configuração | 64 KB, sem mudança |

Pequeno de propósito: texto das páginas na flash (`PSTR`), sem `printf` de float, uma sessão TLS por vez, TLS só com ECDHE + AES-GCM (CBC, CCM, 3DES, ChaCha20, X25519 e certificado de cliente ficam fora do link pelo `TlsTrim.h`, cerca de 27 KB; todos os servidores que o aparelho usa negociam ECDHE + AES-128-GCM), lwIP sem extras (`ip=lm2n`, cerca de 10 KB: sem NAT, fragmentação IP nem AutoIP; o SNTP guarda um servidor só, então o `Clock` alterna entre `pool.ntp.org` e `time.google.com` até sincronizar), login do painel só Basic (o código de Digest do core fica de fora), driver WS2812 sem biblioteca, tabela de palavras dos comandos na flash. O **mDNS** foi medido em cerca de 21 KB e não cabe; use uma reserva de DHCP.

---

## 🛡️ Resiliência

| Mecanismo            | Comportamento                                                                 |
| -------------------- | ----------------------------------------------------------------------------- |
| Wi-Fi perdido        | Checado a cada 15 s e reconectado; 5 falhas seguidas → reinício              |
| Sem internet         | As buscas falham, ficam no log e tentam de novo; painel, API, sensor, atuador e regra seguem funcionando. Reinício só por perda da associação Wi-Fi e no reinício diário. |
| Memória crítica      | Heap < 15 KB ou fragmentação > 50 % em 3 checagens seguidas (10 s) → reinício |
| Reinício diário      | Depois de 24 h ligado (adiado durante o OTA)                                  |
| Modo de configuração | Nenhum watchdog derruba o AP enquanto ele está ativo                          |

---

## 🧪 Testes

Os testes nativos (Unity + PlatformIO) usam stubs do core Arduino em `test/stubs`. O CI exige **100 % de cobertura de linhas, funções e ramos** dos headers do firmware.

```bash
python -m venv venv && . venv/bin/activate
pip install -r requirements.txt

pio test -e native        # qualquer compilador do host
./test/coverage.sh        # requer gcc (Linux) — o mesmo comando do CI
```

---

## 🤖 CI/CD

| Job (`ci.yml`) | Quando | O que faz |
| --- | --- | --- |
| `plan` | sempre, primeiro | Decide: há firmware novo (desde a última tag `vN`)? só mudaram testes? qual versão? publica? |
| `check` | sempre | Regras do repositório: todo arquivo de código tem o cabeçalho de licença SPDX |
| `build` | firmware novo | CSS/JS embutidos em dia, compila com arduino-cli, firmware dentro de **500.000 bytes**, verificador de assinatura, tamanho do pacote OTA |
| `test` | depois do `build`, ou sozinho quando só mudaram testes | Testes + 100 % de cobertura |
| `release` | push na `main` com firmware novo | Comprime, assina, **verifica a assinatura** e publica o release |

A esteira roda `plan → check → build → test → release`; se um job falha, os seguintes não rodam. Sem firmware novo (documentação, workflow, ferramentas), `build`, `test` e `release` são ignorados. Para forçar um release, use *Run workflow* com `release: true`.

---

## 📋 Log remoto (trabalho futuro)

Ainda não está no firmware. O buffer local (`Logger.h`, 10 linhas de 128 caracteres, Serial e painel) continua sendo o limite de RAM. A cópia iria pelo ntfy no tópico de dados, com o prefixo `[log]`, várias linhas por POST no mesmo intervalo de 60 s do aviso de IP.

---

## 🔧 Build local

O ESP-01 não tem reset automático: para gravar, ligue **GPIO0 ao GND** e reconecte o adaptador. Depois de gravar, solte o GPIO0 e reconecte de novo para rodar o firmware.

**PlatformIO** (o `platformio.ini` já embute o `public.key`, o linker script do OTA e tira o `printf` de float):

```bash
pio run -e esp8266                                   # compila
pio run -e esp8266 -t erase  --upload-port COM4      # opcional: apaga a flash (inclusive a config)
pio run -e esp8266 -t upload --upload-port COM4      # volte ao modo de gravação depois de apagar
pio device monitor -b 115200 -p COM4
```

**arduino-cli** (as mesmas flags do CI):

```bash
arduino-cli config init   # pule se já tiver uma configuração do arduino-cli
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

Builds locais têm versão `dev` e atualizam para o último release na primeira checagem OTA (se a atualização automática estiver ligada). Depois de mudar `docs/esp-01.jpg` ou `docs/esp01-pinout.svg`, rode `python3 tools/embed_assets.py` (requer Pillow) e faça commit do `Assets.h`.

O estilo do painel e o script da página IoT ficam em `web/panel.css` e `web/iot.js`. O aparelho entrega os dois em gzip (`/s.css`, `/iot.js`): o navegador descomprime e guarda no cache, e a URL leva um hash do conteúdo, então uma versão nova nunca usa cópia antiga. Depois de editar, rode `python3 tools/embed_web.py` e faça commit do `WebAssets.h`; o CI roda `python3 tools/embed_web.py --check`.

---

## 📄 Licença

[MIT](LICENSE) © 2026 Allan Barcelos. Todo arquivo de código traz um cabeçalho SPDX (`SPDX-License-Identifier: MIT` e a linha de copyright); `python3 tools/license_headers.py` acrescenta o cabeçalho em arquivos novos e o job `check` do CI recusa uma mudança sem ele. Componentes de terceiros e os pedidos de cortesia estão no [NOTICE](NOTICE). Para relatar uma vulnerabilidade, veja o [SECURITY.md](SECURITY.md).
