# O que portar do Tasmota

Notas de 13/10/2026. O Tasmota é o firmware mais provado no ESP8266. Este aparelho é outro produto: um ESP-01 de 1 MB, um sensor e um atuador, DNS no Cloudflare, ntfy, MQTT e OTA assinado. O que vale trazer de lá é o pedaço pequeno que resolve um caso real neste hardware. O motor de regras, a lista de drivers e o OTA por URL não cabem, e o OTA por URL ainda enfraquece o que este firmware já faz melhor.

A folga atual é de 1.280 bytes no teto de 500.000. Nada da lista abaixo entra antes do servidor web enxuto, que deve liberar de 5 a 8 KB (ver [ESTADO-E-PROXIMOS-PASSOS.md](ESTADO-E-PROXIMOS-PASSOS.md)). Nome, hostname e teleperiod são os únicos que talvez caibam antes, se o texto novo no painel for mínimo.

## O que portar

Em ordem.

### 1. Fuso horário e uma janela

O relógio daqui é UTC (`configTime` com deslocamento zero). Sem fuso, "ligar às 7h" fica errado o ano inteiro. O Tasmota guarda deslocamento, horário de verão e até 16 timers com máscara de dias.

Aqui basta o deslocamento em horas e uma janela de início e fim, cercando só a regra automática. Fora dela, `auto` significa desligado. Comando manual continua valendo até `auto`. Com a janela ligada e o relógio ainda em 1970, o atuador fica desligado.

### 2. Um botão

O Tasmota trata botão, interruptor, toque, segurar e vários cliques, em vários GPIOs (`SwitchMode`). O sensor e o atuador já usam dois pinos; sobram dois. O que falta é um pino opcional: segurado desliga, um toque passa para manual desligado até `auto`. A regra de acompanhar o sensor já existe. Os outros modos não entram.

### 3. SHT3x, um driver só

Na lista de sensores do Tasmota, o que mais substitui o DHT é o SHT30/SHT31. O driver de lá é genérico, com gerenciador I2C, e não cabe. O que se porta é o protocolo: dois pinos, leitura de 6 bytes, conta em décimos, no mesmo formato do DHT. SDA e SCL em GPIO0 e GPIO2, com pull-up: o barramento em repouso fica alto, que é o nível de boot desses pinos. O atuador vai para TX ou RX. O DHT permanece no catálogo. BME280, BH1750, medidor de energia e o resto da lista ficam de fora.

### 4. Segundo Wi-Fi

O Tasmota tenta um SSID e, se falhar, outro. O `WiFiService` guarda uma rede só. A lógica cabe nele. O custo real são os campos no painel, em texto na flash. Serve quando o roteador troca ou existe um ponto de reserva.

### 5. Nome do aparelho e hostname DHCP

Não há nome em `Config`. O MQTT e o Home Assistant usam o chip ID. O Tasmota separa o nome que aparece no Home Assistant do hostname que aparece no roteador. Dois campos curtos. O RSSI já está no painel.

### 6. TelePeriod

O Tasmota republica o estado de tempos em tempos, mesmo sem mudança. Aqui o MQTT sai na conexão e quando algo muda. Um intervalo fixo, por exemplo 5 minutos, evita entidade indisponível no Home Assistant se uma mensagem se perde. O JSON de estado já existe.

## O que não portar

| Peça do Tasmota | Por que fica de fora |
| --- | --- |
| Linguagem de regras (Rule1–3, IF, variáveis, backlog) | É outro produto. Come a folga inteira e duplica a regra que o painel já escreve em frase. |
| OTA por URL aberta | Qualquer arquivo naquele endereço seria instalado. Aqui a imagem só entra com assinatura RSA. |
| Dezenas de drivers, energia, infravermelho, visor, Zigbee | Pedem pinos, barramento ou flash que esta placa não tem. O segundo relé continua sendo outro ESP-01. |
| mDNS | Já foi medido em cerca de 21 KB. O aparelho se acha pelo IP reservado no roteador. |
| Sono profundo | O ESP-01 não tem o GPIO16 de despertar. O painel, o MQTT e o OTA precisam do rádio ligado. |
| Console web com a linguagem de comandos inteira | O painel e os comandos `ligar` e `pulso 5` já cobrem o atuador. |

## Ordem em relação ao roteiro atual

Os itens 5 e 6 de [ESTADO-E-PROXIMOS-PASSOS.md](ESTADO-E-PROXIMOS-PASSOS.md) (MQTT com TLS e o certificado do GitHub, cerca de 1,5 KB cada) disputam os mesmos bytes. TLS de nuvem e o certificado do GitHub não aumentam o que o aparelho consegue automatizar. A assinatura já impede uma imagem falsa. Alguém no caminho só consegue atrasar a atualização.

Se a meta for o que o Tasmota faz e este aparelho ainda não faz, a ordem é: teste no ESP-01, servidor web enxuto, fuso e janela, botão, SHT3x. Nome, segundo Wi-Fi e teleperiod entram no que sobrar. O certificado do GitHub volta quando houver espaço.
