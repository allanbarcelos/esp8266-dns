# Bateria e painel solar

Notas de 13/10/2026. Dá para alimentar o aparelho com bateria e painel solar sem usar nenhum GPIO. A porcentagem de carga também cabe sem tirar os pinos do sensor e do atuador, mas isso usa os dois pinos que sobram, e o firmware ainda não lê carga.

O ESP-01 só tem quatro GPIOs: GPIO0, GPIO2, TX e RX. O sensor fica com um, o atuador com outro. Os dois restantes são o único lugar para medir a bateria. O pino analógico do chip não sai no conector, então um divisor de tensão direto no módulo não serve.

## Só energia, sem medir nada

A bateria e o painel ficam no caminho da alimentação, antes do pino de 3,3 V. O firmware de hoje liga do mesmo jeito e não precisa de mudança.

- A célula passa por um carregador e por um regulador de 3,3 V. A tensão da célula, até 4,2 V, não entra no VCC.
- O painel entra no carregador, não num GPIO. Para painel, o carregador tem de ser feito para fonte solar (por exemplo o CN3791). O TP4056 espera 5 V fixos de USB e se comporta mal com painel.
- Um capacitor de algumas centenas de µF na saída do regulador segura o pico do Wi-Fi, que passa de 300 mA. Sem isso o aparelho reinicia ao conectar.

Este firmware deixa o rádio acordado o tempo todo. A conta fecha com uma célula de uns 2000 mAh e um painel de cerca de 1 a 2 W num lugar que toma sol. Uma célula pequena ou um painel de interior não sustenta a noite.

## Carga na tela, sem mexer no sensor nem no atuador

Os dois pinos livres viram um I2C, e nele entra um medidor. Sensor e atuador continuam nos pinos deles.

| Chip | O que informa | Pinos |
| --- | --- | --- |
| MAX17048 | Tensão da célula e carga em % | os dois livres (SDA e SCL) |
| INA219, no mesmo barramento | Tensão e corrente; dá para ver se o painel está carregando | os mesmos dois |

O par melhor para esse barramento é TX e RX: não são pinos de boot. GPIO0 e GPIO2 ficam com o sensor e o atuador, altos no reset. O custo é perder o log da serial enquanto o I2C estiver nesses pinos.

Esse arranjo gasta os dois pinos que sobram. O botão de parada previsto em [PORTAR-DO-TASMOTA.md](PORTAR-DO-TASMOTA.md) não cabe junto, a menos que o sensor passe a ser um SHT31. Aí o medidor de bateria divide o mesmo I2C do sensor, o atuador fica no pino dele e o quarto pino volta a ser o botão.

O que o firmware teria de ganhar, e hoje não tem: ler o medidor, mostrar a carga no painel e publicá-la na API e no MQTT. É um driver pequeno, da ordem do SHT31, e esbarra na mesma folga de 1.280 bytes. Entra depois do servidor web enxuto descrito em [ESTADO-E-PROXIMOS-PASSOS.md](ESTADO-E-PROXIMOS-PASSOS.md).
