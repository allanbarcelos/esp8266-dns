# Teste no aparelho

Os testes automáticos rodam no computador, com o core do Arduino simulado. Eles pegam quase tudo, mas não pegam o que só acontece no chip: tempo de handshake TLS, memória de verdade, Wi-Fi que cai, relé que faz barulho. Este roteiro é para rodar num ESP-01 antes de marcar uma versão como estável, ou depois de qualquer mudança grande.

Leva umas duas horas na primeira vez. Nas seguintes, dá para pular o que não mudou.

## Material

- ESP-01 ou ESP-01S de 1 MB e um adaptador USB-serial de 3,3 V.
- Um jumper para o GPIO0.
- Um LED com resistor (ou um módulo relé) no GPIO0, e um botão ou um fio entre o GPIO2 e o GND para servir de sensor digital.
- Chrome ou Edge no computador; um monitor serial (o do PlatformIO, o do Arduino ou `screen /dev/ttyUSB0 115200`).
- Para as partes opcionais: um domínio no Cloudflare, o app do ntfy no celular, um broker MQTT na rede (Mosquitto) e um Home Assistant.

Anote a versão testada e o resultado de cada item na tabela do fim.

## 1. Gravação pelo instalador web

1. Abra o instalador (`https://allanbarcelos.github.io/esp8266-dns/`), ligue o GPIO0 ao GND e conecte o adaptador.
2. Clique em **Instalar no ESP-01**, escolha a porta e aceite apagar o aparelho.
3. Ao terminar, tire o adaptador, solte o GPIO0 e conecte de novo.

**Esperado:** a gravação vai até 100 % sem erro. Se o instalador não achar o chip, quase sempre é o GPIO0 que não estava no GND na hora de ligar.

## 2. Senha do AP e configuração inicial

1. No instalador, clique em **Ler a senha do aparelho** e escolha a porta.
2. Em até 30 s deve aparecer uma senha de 12 caracteres (letras minúsculas e números de 2 a 9).
3. Confira no monitor serial a linha `AP ESP_Config, senha: ...`, repetida a cada 30 s.
4. No celular ou no computador, entre na rede `ESP_Config` com essa senha. Tente antes com `esp8266dns`: tem que ser recusada.
5. Abra `http://192.168.4.1`, informe o Wi-Fi da casa e defina usuário e senha do painel.

**Esperado:** o aparelho reinicia, entra na rede da casa e o AP some. A senha não aparece no log do painel.

Repita a gravação **sem** apagar o aparelho: a senha do AP tem que continuar a mesma. Depois grave **apagando**: tem que vir uma senha nova.

## 3. Painel e login

1. Descubra o IP do aparelho no roteador e reserve esse IP (reserva de DHCP).
2. Abra o painel, entre com o usuário e a senha.
3. Em **Status**, confira a linha **Senha do AP ESP_Config**.
4. Abra uma janela anônima e erre a senha 6 vezes seguidas.

**Esperado:** a partir do sexto erro aparece "Muitas tentativas; aguarde" por alguns segundos, mesmo com a senha certa. No log aparece `Login: erros seguidos, espera crescente`. Depois da espera, a senha certa entra e a conta zera.

## 4. Wi-Fi e resiliência

1. Desligue o roteador por 2 minutos e ligue de novo.
2. Tire o aparelho da tomada e ligue de novo.
3. Troque a senha do Wi-Fi no painel por uma errada e reinicie.

**Esperado:** no primeiro caso o aparelho volta sozinho, sem reiniciar por falta de internet. No segundo, volta com a mesma configuração e o atuador no estado definido em "ao religar". No terceiro, o AP `ESP_Config` volta com a **mesma** senha do passo 2, e o aparelho segue tentando a rede em segundo plano.

## 5. DNS do Cloudflare (se usar)

1. Configure token, zona e registros na página **Cloudflare**.
2. Mude o IP de um registro no painel do Cloudflare para `1.1.1.1` e reinicie o aparelho. Ele compara o IP público com o último que enviou, não com o registro, mas todo boot faz um envio.

**Esperado:** no log, `Atualizando DNS para ...` e depois `DNS atualizado!`. O registro volta para o IP da casa. Com o relógio ainda em 1970 (logo depois de ligar), o aparelho espera em vez de falhar. Depois disso, a verificação roda a cada minuto e só envia quando o IP da casa muda.

## 6. ntfy (se usar)

1. Na página **Ntfy**, informe um tópico de dados e gere um tópico de comandos com o botão.
2. Tente salvar um tópico de comandos curto (`teste`) sem token: tem que ser recusado.
3. Inscreva o celular no tópico de dados.
4. Mande `ligar` no tópico de comandos pelo app.

**Esperado:** chega o IP público no boot. O comando liga o LED em cerca de 1 s, e chega a notificação com os botões de ação. Os botões também funcionam.

## 7. IoT

1. Configure o sensor digital no GPIO2 e o relé (ou o LED) no GPIO0, com a regra ligada.
2. Acione o botão do sensor algumas vezes.
3. Pelo painel: `ligar`, `desligar`, `pulso 3`, `auto`.

**Esperado:** a regra segue o sensor; o pulso desliga sozinho depois de 3 s; o log mostra `Painel: comando "..."` para cada um.

## 8. API local

```bash
curl -H 'Authorization: Bearer <token>' http://<ip>/api/state
curl -H 'Authorization: Bearer <token>' -d ligar http://<ip>/api/cmd
curl -i -H 'Authorization: Bearer errado' http://<ip>/api/state
```

**Esperado:** o JSON de estado; o LED liga e o log mostra `API: comando "ligar"`; token errado recebe 401 (e 429 depois de 5 erros seguidos). As respostas trazem `Cache-Control: no-store` e `X-Frame-Options: DENY`.

## 9. MQTT e Home Assistant (se usar)

1. Na página **MQTT**, informe o broker (IP e porta), usuário e senha.
2. Num computador da rede: `mosquitto_sub -h <broker> -u <usuario> -P <senha> -v -t 'esp8266iot/#' -t 'homeassistant/#'`.
3. No Home Assistant, abra a integração MQTT.

**Esperado:**
- Em **MQTT**, a situação fica "conectado".
- Aparecem `.../avail online`, o discovery em `homeassistant/device/esp8266iot_<id>/config` e o estado em `.../state`.
- No HA surge o dispositivo **ESP8266 IoT** com o sensor e o atuador. Ligar e desligar pelo HA funciona nos dois sentidos.
- Tirando o aparelho da tomada, o broker publica `offline` e as entidades ficam indisponíveis.
- Publicar `ligar` **retido** no tópico de comandos e religar o aparelho não pode ligar o LED: o log mostra `MQTT: comando retido ignorado`.
- Troque o atuador no painel (por exemplo, para LED com nível): a entidade antiga some do HA e a nova aparece.

## 10. Atualização (OTA)

1. Em **Atualizar**, clique em **Verificar agora** com uma versão nova publicada.
2. Instale.

**Esperado:** a barra de progresso anda, o aparelho reinicia na versão nova e mantém a configuração. Durante o download, o MQTT cai e volta sozinho depois. Se a heap não der para o download, o aparelho reinicia em modo de recuperação, termina o OTA e volta ao normal.

## 11. Memória depois de um dia

Deixe o aparelho ligado 24 h com ntfy, MQTT e DNS ativos e confira em **Status**:

**Esperado:** heap livre acima de 15 KB, fragmentação abaixo de 50 % e nenhum reset fora o reboot diário programado. O motivo do último reset aparece em **Status**.

## Registro

| Data | Versão | Item | Resultado | Observação |
| --- | --- | --- | --- | --- |
| | | 1. Gravação | | |
| | | 2. Senha do AP | | |
| | | 3. Painel e login | | |
| | | 4. Wi-Fi | | |
| | | 5. DNS | | |
| | | 6. ntfy | | |
| | | 7. IoT | | |
| | | 8. API | | |
| | | 9. MQTT e HA | | |
| | | 10. OTA | | |
| | | 11. Memória | | |
