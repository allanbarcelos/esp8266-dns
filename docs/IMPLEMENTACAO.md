# O que podia ser implementado, e o que foi

Situação em 13/10/2026. A lista saiu dos documentos do mesmo dia: [Estado e próximos passos](ESTADO-E-PROXIMOS-PASSOS.md), [O que portar do Tasmota](PORTAR-DO-TASMOTA.md) e [Bateria e painel solar](BATERIA-E-PAINEL.md), mais o log remoto do README. A rodada implementou os itens 5, 6, 7, 8, 10 e 11, um commit por item. O 7 foi feito primeiro porque liberou o espaço para os outros.

Depois da rodada o firmware tem **494.912 bytes**, abaixo do teto de 500.000 (sobram 5.088). Antes do servidor enxuto eram 499.312. São 417 testes nativos, com 100 % de funções e ramos.

## Não mudam o firmware

| # | Item | Esforço | Implementado | Commit |
|---|---|---|---|---|
| 1 | Teste no ESP-01 com o roteiro de [TESTE-HARDWARE.md](TESTE-HARDWARE.md): nada da rodada de hoje rodou num aparelho de verdade | 2–4 h + correções | Não | — |
| 2 | Teste de ponta a ponta no CI com Mosquitto e Home Assistant (hoje manual) | ~1 dia | Não | — |
| 3 | Medir as travas do laço principal: aviso no log quando um ciclo passar de ~200 ms | ½ dia | Não | — |

## Cabiam antes do servidor enxuto, com o texto do painel mínimo

| # | Item | Flash / esforço | Implementado | Commit |
|---|---|---|---|---|
| 4 | Canal estável de release: o OTA automático só instala versões marcadas como estáveis | ~0,2 KB | Não | — |
| 5 | TelePeriod no MQTT: o estado sai de novo a cada 5 min, mesmo sem mudança | +16 B | Sim | `dd7640d` |
| 6 | Nome do aparelho (painel e Home Assistant) e hostname do DHCP | +1,8 KB | Sim | `be023bb` |

## Dependiam do servidor web enxuto

| # | Item | Flash / esforço | Implementado | Commit |
|---|---|---|---|---|
| 7 | Servidor web enxuto no lugar do `ESP8266WebServer`, sem perder rota, login, CSRF, API nem arquivo embutido | **−12,3 KB** | Sim | `f173944` |
| 8 | Fuso horário e janela de horário na regra automática; hora local no Status | +2,6 KB | Sim | `18cfe91` |
| 9 | Botão físico num pino livre (segurar desliga, toque passa para manual) | pequeno | Não | — |
| 10 | Sensor SHT30/SHT31 por I2C (GPIO0 com GPIO2, ou RX com TX) | +1,1 KB | Sim | `28e917f` |
| 11 | Medidor de bateria MAX17048 e, no canto direito do topo do painel: na fonte, na bateria (%) ou carregando pelo solar | +2,4 KB | Sim, só o MAX17048; o INA219 ficou de fora | `98588a9` |
| 12 | Segundo Wi-Fi de reserva | o formulário | Não | — |
| 13 | Diagnóstico em campo: último travamento e heap, RSSI e tempo ligado no MQTT | ~1 dia | Não | — |
| 14 | Validar o certificado do GitHub no OTA | ~1,5 KB | Não | — |
| 15 | MQTT com TLS para um broker na nuvem (um ou outro com os comandos do ntfy) | ~1,5 KB, ~2 dias | Não | — |
| 16 | Log remoto pelo ntfy (`[log]` no tópico de dados) | pequeno | Não | — |

## Decisão sem código

| # | Item | Esforço | Implementado | Commit |
|---|---|---|---|---|
| 17 | Nome do projeto (`esp8266-dns` ou `esp8266-iot`) e identificadores em inglês na API e no MQTT, com os atuais como sinônimos | ½–1 dia | Não | — |

## Notas sobre o que entrou

- **7, servidor enxuto.** `HttpServer.h` atende um cliente por vez e fecha a conexão no fim de cada resposta. O corpo cru continua em `plain`, então `curl -d ligar` funciona igual. Os testes do painel agora falam HTTP de verdade com o servidor, por um socket simulado.
- **8, janela de horário.** Fora da janela, e com o relógio ainda em 1970, o modo automático deixa o atuador desligado. Um comando manual continua valendo até `auto`. A API e o MQTT trazem `"hours"` no atuador quando a janela está ligada.
- **10, SHT3x.** O pino escolhido é o SDA; o SCL é o do par. O I2C é feito por software (`I2cBus.h`), sem a biblioteca Wire. O painel não deixa o atuador ficar no SCL.
- **11, bateria.** A situação vem da taxa de carga do MAX17048. Subindo, a bateria está carregando. Caindo, ou abaixo de 98 % sem carregar, o aparelho está na bateria. Cheia e parada, ou sem medidor configurado, ele está na fonte. Pela taxa, o medidor não distingue o painel solar de uma fonte: qualquer carga aparece como "Carregando". A carga ainda não vai à API nem ao MQTT.
- **Ainda não verificado no aparelho:** os itens 7, 8, 10 e 11 dependem de hardware, rede e navegador reais. Vale passar pelo item 1 antes de publicar.
