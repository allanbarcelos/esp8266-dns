# Acesso remoto

Como usar o aparelho fora de casa sem deixá-lo exposto na internet.

## A regra: nunca abra uma porta para o aparelho

O ESP-01 não tem memória para servir HTTPS. O painel e a API falam HTTP puro, com a senha em Basic Auth. Se a porta 80 do roteador apontar para ele, qualquer pessoa no caminho lê a senha, e qualquer robô que varre a internet acha o painel em poucas horas. O limite de tentativas de login atrasa quem tenta adivinhar a senha, mas não resolve isso.

Por isso o desenho é outro: o aparelho só faz conexões de saída. Ele já faz isso com o ntfy (comandos pelo celular) e com o MQTT (Home Assistant). Para chegar no painel, coloque na frente um túnel que tenha HTTPS e login próprio.

O DDNS do aparelho não atrapalha: ele só mantém o registro DNS apontando para o IP da casa. Sem porta aberta, esse IP não leva a lugar nenhum.

## Qual caminho escolher

| Você quer | Use | Precisa de |
| --- | --- | --- |
| Ligar e desligar pelo celular, sem mais nada | **ntfy** (já vem pronto) | Só o app do ntfy |
| Controlar pelo Home Assistant de fora | **Acesso remoto do Home Assistant** | O HA já acessível de fora |
| Abrir o painel de qualquer lugar, só você | **Tailscale** | Um computador sempre ligado na rede (Raspberry Pi, PC, NAS ou roteador compatível) |
| Abrir o painel num endereço próprio, com login por e-mail | **Cloudflare Tunnel + Access** | Domínio no Cloudflare e um computador sempre ligado na rede |

Os dois últimos precisam de um computador na rede, porque o ESP-01 não tem memória para rodar o túnel ele mesmo.

## ntfy

Nada a configurar além do que está na página **Ntfy** do painel. Os tópicos do ntfy.sh são públicos pelo nome, então:

- use o botão **Gerar nome aleatório** para o tópico de comandos (sem token, o painel recusa nomes com menos de 20 caracteres);
- se puder, crie um token de acesso no ntfy e informe no painel;
- não compartilhe o tópico de dados: os botões das notificações levam o tópico de comandos.

## Home Assistant

Se o Home Assistant já é acessível de fora (Home Assistant Cloud ou um acesso que você mesmo configurou com HTTPS), o aparelho vem junto: ele aparece pelo MQTT e é controlado pelo HA. O MQTT fica só na rede local, entre o aparelho e o broker. Não exponha a porta do broker na internet.

## Tailscale

O Tailscale cria uma rede privada entre os seus aparelhos. Um computador da casa anuncia a rede local, e o celular passa a enxergar o ESP-01 como se estivesse em casa.

No computador da casa (Linux):

```bash
curl -fsSL https://tailscale.com/install.sh | sh
echo 'net.ipv4.ip_forward = 1' | sudo tee /etc/sysctl.d/99-tailscale.conf
sudo sysctl -p /etc/sysctl.d/99-tailscale.conf
sudo tailscale up --advertise-routes=192.168.1.0/24
```

Troque `192.168.1.0/24` pela faixa da sua rede. Depois:

1. No painel do Tailscale, em **Machines**, abra o computador, entre em **Edit route settings** e aprove a rota.
2. Instale o Tailscale no celular e entre na mesma conta.
3. Abra `http://<ip do aparelho>` no navegador do celular.

O tráfego entre o celular e a casa vai cifrado pelo Tailscale. Do computador até o ESP-01 é HTTP na rede local, como em casa.

## Cloudflare Tunnel + Access

O `cloudflared` roda num computador da casa e abre uma conexão de saída até o Cloudflare. O Cloudflare publica o painel num endereço seu, com HTTPS, e o Access pede login antes de deixar passar.

### 1. Túnel

Pelo painel do Cloudflare (**Zero Trust → Networks → Tunnels → Create a tunnel**), escolha **Cloudflared**, dê um nome (por exemplo, `casa`) e siga o comando de instalação que ele mostra para o seu sistema. Em **Public Hostname**:

- **Subdomain / Domain:** `esp.seudominio.com`
- **Service:** `HTTP` e `192.168.1.50:80` (o IP reservado do aparelho)

Se preferir arquivo de configuração, o equivalente em `~/.cloudflared/config.yml` é:

```yaml
tunnel: <id do túnel>
credentials-file: /home/<usuario>/.cloudflared/<id do túnel>.json
ingress:
  - hostname: esp.seudominio.com
    service: http://192.168.1.50:80
  - service: http_status:404
```

com o túnel criado por `cloudflared tunnel create casa`, o DNS por `cloudflared tunnel route dns casa esp.seudominio.com` e o serviço instalado por `sudo cloudflared service install`.

Use um subdomínio diferente dos registros que o próprio aparelho atualiza no DDNS.

### 2. Login na frente (obrigatório)

Sem esta parte, o painel fica público com só o Basic Auth do aparelho. Em **Zero Trust → Access → Applications → Add an application → Self-hosted**:

- **Application domain:** `esp.seudominio.com`
- **Policy:** ação **Allow**, regra **Emails** com o seu e-mail (o Access manda um código por e-mail a cada login).

Depois disso, abrir `https://esp.seudominio.com` pede primeiro o código do Access e só então a senha do painel.

### Observações

- Entre o navegador e o Cloudflare, tudo vai em HTTPS. Do `cloudflared` até o aparelho é HTTP na rede local.
- O painel manda `Cache-Control: no-store` e `X-Frame-Options: DENY`: nada fica no cache do Cloudflare nem do navegador, e o painel não abre dentro de outro site.
- A API local (`/api/state`, `/api/cmd`) também passa pelo túnel, mas atrás do Access ela não responde a scripts. Para automação de fora, prefira o Home Assistant ou o ntfy. Se precisar mesmo da API, crie no Access um **Service Token** só para ela.

## O que não fazer

- Abrir uma porta do roteador para o aparelho, mesmo "só por um tempo".
- Publicar o túnel sem o Access na frente.
- Expor o broker MQTT na internet sem TLS e sem senha.
- Usar como senha do painel a mesma do Wi-Fi.
