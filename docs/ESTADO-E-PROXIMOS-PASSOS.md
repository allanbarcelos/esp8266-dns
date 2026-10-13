# Estado do firmware e próximos passos

Situação em 13/10/2026, depois da rodada de trabalho que vai do commit `93d7bd9` ao `3e95259`.

## Onde estamos

Um ESP-01 de 1 MB que mantém o DNS do Cloudflare apontando para a casa, lê um sensor e comanda um atuador, e se deixa controlar pelo painel web, por uma API local, pelo ntfy e pelo MQTT, com o Home Assistant achando o aparelho sozinho. Atualiza sozinho por OTA assinado e instala pelo navegador.

| | |
| --- | --- |
| Firmware | 498.720 bytes de 500.000 (folga de 1.280) |
| Pacote OTA assinado | ~358 KB de 400 KB |
| RAM estática | 37.808 bytes |
| Testes nativos | 370, com 100 % de linhas, funções e ramos |
| No CI | sem nenhum aviso de compilação, ASan e UBSan, cppcheck, fuzzing de 5 entradas |

## O que mudou nesta rodada

### Segurança

- **ntfy:** o envio de notificações passou a validar o certificado. Antes ia sem validação, e o token e o tópico de comandos podiam ser lidos por quem estivesse no caminho (`93d7bd9`).
- **SECURITY.md** corrigido: o download do OTA não valida o certificado do GitHub. A proteção ali é a assinatura RSA da imagem (`0a1c054`).
- **Senha do AP de configuração** sorteada por aparelho no primeiro boot, no lugar da `esp8266dns` igual para todos. Sai na serial, no painel e no instalador web (`4d219b1`).
- **Painel e API:** espera crescente depois de 5 logins errados; tópico de comandos do ntfy com 20 caracteres no mínimo sem token; cabeçalhos contra cache e clickjacking; o log diz de onde veio cada comando (`3e95259`).

### Funcionalidades

- **Instalador web** no GitHub Pages, com o ESP Web Tools: grava o ESP-01 pelo navegador, sem PlatformIO (`b54b4d0`).
- **MQTT na rede local:** estado, comandos e disponibilidade com testamento, num cliente próprio sem biblioteca (`14444f7`).
- **Discovery do Home Assistant:** o aparelho aparece sozinho, com as entidades certas para cada tipo de sensor e atuador, e as antigas somem quando o módulo é trocado (`29047ab`).

### Espaço na flash

O MQTT e o discovery não cabiam. Para caberem sem tirar nada que o usuário vê:

- um só leitor de JSON, tudo analisado no lugar como `char*` (−4,7 KB, `4272ca1`);
- raízes TLS em DER, geradas de `ca/roots.pem` (−1,5 KB, `2c31926`);
- foto do painel em WebP qualidade 40 e discovery sem campos que o HA não usa (−0,7 KB, `4ed23fc`).

### Qualidade

- `-Wall -Wextra -Werror` nos testes (`dd34b8b`).
- Suíte inteira de novo com AddressSanitizer e UndefinedBehaviorSanitizer (`9bdef25`).
- cppcheck no job `check` (`abfb58f`).
- Fuzzing com libFuzzer de tudo que vem de fora: pacotes MQTT, comandos, resposta do Cloudflare, fluxo do ntfy e `config.json` (`458cefb`).

### Documentação

- [Teste no aparelho](TESTE-HARDWARE.md): roteiro para rodar num ESP-01 antes de chamar uma versão de estável (`34f2e2a`).
- [Acesso remoto](ACESSO-REMOTO.md): como usar de fora de casa sem abrir porta no roteador (`88c2bb4`).

## Como foi verificado

- Testes nativos, sanitizers, cppcheck e fuzzing em toda mudança, como no CI.
- O MQTT rodou contra um Mosquitto real: autenticação, senha errada, comandos, testamento na queda e comando retido na reconexão.
- O discovery rodou contra um Home Assistant real, com cinco combinações de sensor e atuador, ida e volta pelos serviços do HA e nenhum aviso no log dele. Esse teste foi feito à mão, num ambiente montado para isso; não está no CI.
- O instalador web e o botão que lê a senha do AP foram testados no Chrome e no Edge, com a serial simulada.

**Ainda não verificado:** nada desta rodada rodou num ESP-01 de verdade. O primeiro passo é o [roteiro de teste no aparelho](TESTE-HARDWARE.md).

## Decisões que valem lembrar

- **Limite de 500.000 bytes.** Regra do projeto, conferida no CI. Toda mudança cabe dentro dele.
- **A foto do ESP-01 fica.** Se faltar espaço, ela não é a primeira coisa a sair.
- **Nunca abrir porta para o aparelho.** O ESP-01 não serve HTTPS; o acesso de fora passa por túnel com login próprio.
- **Conta de erros de login por aparelho, não por endereço.** Alguém na rede consegue atrasar o login do dono por alguns minutos, mas não adivinhar a senha. Uma tabela por endereço não cabe na memória.
- **MQTT sem TLS.** A única sessão TLS que cabe já é disputada pelo DNS, pelo ntfy e pelo OTA.

## Próximos passos

Em ordem, com uma estimativa de esforço.

1. **Rodar o teste no aparelho** e corrigir o que aparecer. De 2 a 4 horas de teste, mais o que precisar de correção.
2. **Canal estável de release.** O OTA automático instala só versões marcadas como estáveis; as outras ficam como beta, para quem quiser. Importa mais agora que o aparelho pode ser controlado de longe. Cerca de 1 dia; ~0,2 KB de flash.
3. **Medir as travas do laço principal.** Conexões TLS e a do MQTT seguram o laço por alguns segundos, e nesse tempo o painel não responde e o pulso do atuador pode passar do pedido. Primeiro medir (um aviso no log quando um ciclo passar de, por exemplo, 200 ms), depois decidir. Meio dia para medir.
4. **Servidor web enxuto.** Trocar o servidor do core por um feito para as poucas rotas do painel deve liberar de 5 a 8 KB. É o que destrava os itens 5 e 6, e devolve folga para correções. De 3 a 5 dias; os testes do painel cobrem a troca.
5. **MQTT com TLS para um broker na nuvem** (opcional, depende do item 4). Controle pela internet sem túnel. Seria uma escolha ou outra com o fluxo de comandos do ntfy, porque só uma sessão TLS cabe. Cerca de 2 dias; ~1,5 KB.
6. **Validar o certificado do GitHub no OTA** (depende do item 4). Hoje a assinatura protege a imagem, mas alguém no meio da conexão consegue bloquear atualizações. Meio dia; ~1,5 KB.
7. **Diagnóstico em campo.** Guardar o último travamento (endereço e pilha) e mostrar no painel; publicar heap, sinal do Wi-Fi e tempo ligado pelo MQTT. Cerca de 1 dia; o tamanho depende de quanto entrar.
8. **Teste de ponta a ponta no CI.** Levar para o CI o teste com Mosquitto e Home Assistant que hoje é manual. Cerca de 1 dia; não muda o firmware.
9. **Nome e idioma.** Decidir entre `esp8266-dns` e `esp8266-iot` (hoje os dois aparecem) e oferecer identificadores em inglês na API e no MQTT, mantendo os atuais como sinônimos. De meio dia a 1 dia.

O que vale trazer do Tasmota para este aparelho, e o que não vale: [PORTAR-DO-TASMOTA.md](PORTAR-DO-TASMOTA.md). Os itens 1 a 3 de lá (fuso e janela, botão, SHT3x) disputam a folga do item 4 com os itens 5 e 6 desta lista.
