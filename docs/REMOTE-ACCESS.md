# Remote access

How to use the device away from home without exposing it to the internet.

## The rule: never open a port to the device

The ESP-01 has no memory to serve HTTPS. The panel and the API speak plain HTTP. The panel signs in with a form and then keeps a session cookie; the API uses a Bearer token. If the router's port 80 points at it, anyone on the path can steal that cookie or token, and any bot scanning the internet finds the panel within hours. The authenticator stops a stolen password from being enough, and the login attempt limit slows guessing. An open port remains unsafe.

So the device only makes outgoing connections: to ntfy (commands from the phone), to MQTT (Home Assistant), and to its own relay Worker on Cloudflare (the panel and the API from anywhere).

## Which path to choose

| You want | Use | Needs |
| --- | --- | --- |
| The panel and the API from anywhere, at an HTTPS address | **Remote access** (built in) | A free Cloudflare account; a domain on it is optional |
| To switch things on and off from the phone, nothing else | **ntfy** (built in) | Just the ntfy app |
| To control it through Home Assistant from outside | **Home Assistant remote access** | HA already reachable from outside |
| A private network to everything at home, only you | **Tailscale** | A computer always on in the network |
| A Cloudflare Tunnel you already run for other things | **Cloudflare Tunnel** | A computer always on in the network |

## Remote access (built in)

```
browser / app ──HTTPS──> esp-pump.example.com ──> Worker + Durable Object (your Cloudflare account)
                                                     ║  WebSocket over TLS, opened by the device,
                                                     ║  authenticated by a secret only the two know
                                                   ESP-01 ── the same panel, sign-in and API
```

The device opens a connection to a Cloudflare Worker in your account and keeps it. Each request to your address goes down that connection; the device answers it with the same panel, password, authenticator and API token as at home, and the answer goes back up. Nothing is opened on the router, and nothing besides the Worker runs anywhere: no Raspberry Pi, no `cloudflared`.

The device sets the Worker up itself, with the API token: you never upload code or touch the Workers dashboard.

### 1. The token

In Cloudflare, **My Profile → API Tokens → Create Token → Create Custom Token**, with these permissions:

| Scope | Permission | For |
| --- | --- | --- |
| Account | **Workers Scripts: Edit** | the relay Worker and its workers.dev address |
| Account | **Account Settings: Read** | finding the account when there is no domain |
| Zone | **Zone: Read** | listing your domains |
| Zone | **DNS: Edit** | the DDNS record |
| Zone | **Workers Routes: Edit** | putting the Worker on your own domain |

Without a domain on Cloudflare, the two account permissions are enough. Paste the token on the panel's **Settings** page (**Cloudflare** card) and press **Save and check**: the **Cloudflare** page then lists the domains the token reaches (and the DDNS record is picked there by name, created if it does not exist). **Unregister**, on the same card, removes the token and every Cloudflare setting from the device; the Worker and the record stay in the account.

### 2. Turn it on

On the panel, **Cloudflare**, card **Worker - remote access**:

- **Address**: one of your domains, or **workers.dev** (free, no domain needed).
- **Name**: the first part of the address and the Worker's name, such as `esp-pump`. The address becomes `esp-pump.example.com`, or `esp-pump.<account>.workers.dev`. Each device gets its own name. A Worker of yours that already has that name, and is not a relay, is left alone (the panel says so).
- **workers.dev name for the account**: only if the account never had one; Cloudflare asks for it once per account.

Press **Turn on**. The device uploads the Worker (from its own firmware), with a new random secret, and puts it on the address. The page follows the steps and then shows the link and the connection state. A new workers.dev address can take a minute or two to answer the first time.

Do not use the name of the DDNS record (the one that points at your home): the Worker needs a name of its own.

**Turn off** stops the device from connecting. The Worker stays in your account and answers that the device is offline; delete it in the Cloudflare dashboard if you no longer want it (**Workers & Pages**).

### What works through it

- **The panel**, with the same sign-in. The session cookie only travels over HTTPS (browser to Cloudflare) and TLS (Cloudflare to the device). An `http://` address is sent to `https://` before anything reaches the device, the answers carry HSTS (the browser then goes straight to HTTPS for a year) and the cookie is marked `Secure` on the way out: on your own domain a typed `http://` would otherwise carry the password in clear (workers.dev is HTTPS-only already).
- **The API**, from scripts or apps anywhere:

  ```bash
  curl -H 'Authorization: Bearer <token>' https://esp-pump.example.com/api/state
  curl -H 'Authorization: Bearer <token>' -d 'level 70' https://esp-pump.example.com/api/cmd
  ```

  At home, the same calls go straight to `http://<device ip>/api/...`, faster; an app can try the local address first.
- **With the device offline** (power cut, no internet), `/api/state` answers from the last state the device sent, with `"online": false` and `"seen"` (Unix time), to whoever holds the API token. The Worker keeps only a keyed hash of the token, never the token.
- **ntfy commands and the notification buttons** keep working: while the relay is up, the Worker checks the command topic for the device (every 5 s by default; 2, 10 or 30 s on the **ntfy** page) and sends each command down the relay. If the relay stays down for more than a minute, the device goes back to its own ntfy stream, from the last message it saw.
- **MQTT** stays on the local network, unchanged.

### Limits and costs

- One request at a time reaches the device; others wait their turn at the Worker (up to 16). Expect 0.5 to 2 s per page or API call.
- Everything that checks the password, a code or the API token (sign-in, authenticator, new password, new authenticator, Reconfigure, the API) is limited to **60 requests per minute per address** at the Worker, before it reaches the device. The device keeps its own attempt limits.
- Bodies over 2 KB get 413 (the panel's forms are much smaller).
- Cloudflare's free plan includes 100,000 Worker requests a day. Polling ntfy every 5 s takes about 17,000; an app reading `/api/state` every 5 s about 17,000 more.
- The device has room for one TLS session: the relay steps aside for a few seconds while the device checks its public IP, updates DNS, posts to ntfy or downloads an update. The Worker holds requests meanwhile.
- After a firmware update that changes the relay protocol, the device uploads the new Worker by itself.

### A login in front (optional)

The panel keeps its password and authenticator, and bots meet the Worker's rate limit first. For one more layer, put **Cloudflare Access** in front of the address: **Zero Trust → Access → Applications → Add an application → Self-hosted**, the application domain being the relay's address, with a policy **Allow → Emails → your e-mail** (Access sends a code by e-mail on each login). Access needs the Zero Trust area enabled on the account, which may ask for a payment method even on the free plan.

With Access on, scripts calling the API are blocked too. Add an application rule that **bypasses** Access for `/api/*` (the API has its own token), or give the scripts an Access **Service Token**.

### Several devices

One name per device: `esp-pump`, `esp-light`, `esp-gate`, each its own Worker and address. The token can be the same.

## ntfy

Nothing to set up beyond the panel's **Ntfy** page. ntfy.sh topics are public by name, so:

- use the **Generate random name** button for the command topic (without a token, the panel refuses names shorter than 20 characters);
- if you can, create an ntfy access token and enter it on the panel;
- do not share the data topic: the notification buttons carry the command topic.

## Home Assistant

If Home Assistant is already reachable from outside (Home Assistant Cloud or access you set up yourself with HTTPS), the device comes along: it shows up through MQTT and is controlled by HA. MQTT stays on the local network, between the device and the broker. Do not expose the broker's port to the internet.

## Tailscale

Tailscale creates a private network between your devices. A computer at home advertises the local network, and the phone sees the ESP-01 as if it were at home.

On the home computer (Linux):

```bash
curl -fsSL https://tailscale.com/install.sh | sh
echo 'net.ipv4.ip_forward = 1' | sudo tee /etc/sysctl.d/99-tailscale.conf
sudo sysctl -p /etc/sysctl.d/99-tailscale.conf
sudo tailscale up --advertise-routes=192.168.1.0/24
```

Replace `192.168.1.0/24` with your network's range. Then:

1. In the Tailscale admin console, under **Machines**, open the computer, go to **Edit route settings** and approve the route.
2. Install Tailscale on the phone and sign in to the same account.
3. Open `http://<device ip>` in the phone's browser.

## Cloudflare Tunnel

If a computer at home already runs `cloudflared`, a **Public Hostname** of the tunnel can point at the device: service `HTTP`, `192.168.1.50:80` (its reserved IP), with Access in front as above. Then tell the device about that name: on the panel, at home, open **Settings** and enter it under **Other addresses**. The panel only answers to its own names (an IP address, its hostname, the DDNS record, the relay's address and that list); a page on another site that points its own name at the device gets nothing (DNS rebinding). Without this step the tunnel shows "Unknown address". Instead of the list, `cloudflared` can send the device's IP as the host (**HTTP Host Header** in the tunnel's settings, `httpHostHeader` in `config.yml`).

The built-in remote access does the same without the computer; the tunnel is only worth it if you run one anyway.

## What not to do

- Open a router port to the device, even "just for a while".
- Publish the panel through a tunnel without Access in front, or turn off the panel's password.
- Expose the MQTT broker to the internet without TLS and without a password.
- Use the Wi-Fi password as the panel password.
