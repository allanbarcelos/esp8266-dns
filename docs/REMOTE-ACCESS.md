# Remote access

How to use the device away from home without exposing it to the internet.

## The rule: never open a port to the device

The ESP-01 has no memory to serve HTTPS. The panel and the API speak plain HTTP, with the password in Basic Auth. If the router's port 80 points at it, anyone on the path reads the password, and any bot scanning the internet finds the panel within hours. The login attempt limit slows down password guessing, but does not solve this.

So the design is different: the device only makes outgoing connections. It already does that with ntfy (commands from the phone) and MQTT (Home Assistant). To reach the panel, put a tunnel with HTTPS and its own login in front of it.

The device's DDNS does not get in the way: it only keeps the DNS record pointing at the home IP. With no open port, that IP leads nowhere.

## Which path to choose

| You want | Use | Needs |
| --- | --- | --- |
| To switch things on and off from the phone, nothing else | **ntfy** (built in) | Just the ntfy app |
| To control it through Home Assistant from outside | **Home Assistant remote access** | HA already reachable from outside |
| To open the panel from anywhere, only you | **Tailscale** | A computer always on in the network (Raspberry Pi, PC, NAS or a compatible router) |
| To open the panel at your own address, with e-mail login | **Cloudflare Tunnel + Access** | A domain on Cloudflare and a computer always on in the network |

The last two need a computer in the network, because the ESP-01 has no memory to run the tunnel itself.

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

Traffic between the phone and home is encrypted by Tailscale. From the computer to the ESP-01 it is HTTP on the local network, as at home.

## Cloudflare Tunnel + Access

`cloudflared` runs on a home computer and opens an outgoing connection to Cloudflare. Cloudflare publishes the panel at your address, with HTTPS, and Access asks for a login before letting anyone through.

### 1. Tunnel

In the Cloudflare dashboard (**Zero Trust → Networks → Tunnels → Create a tunnel**), choose **Cloudflared**, give it a name (for example `home`) and follow the install command it shows for your system. Under **Public Hostname**:

- **Subdomain / Domain:** `esp.yourdomain.com`
- **Service:** `HTTP` and `192.168.1.50:80` (the device's reserved IP)

If you prefer a configuration file, the equivalent in `~/.cloudflared/config.yml` is:

```yaml
tunnel: <tunnel id>
credentials-file: /home/<user>/.cloudflared/<tunnel id>.json
ingress:
  - hostname: esp.yourdomain.com
    service: http://192.168.1.50:80
  - service: http_status:404
```

with the tunnel created by `cloudflared tunnel create home`, the DNS by `cloudflared tunnel route dns home esp.yourdomain.com` and the service installed by `sudo cloudflared service install`.

Use a subdomain different from the records the device itself updates through DDNS.

### 2. Login in front (required)

Without this part, the panel is public with only the device's Basic Auth. In **Zero Trust → Access → Applications → Add an application → Self-hosted**:

- **Application domain:** `esp.yourdomain.com`
- **Policy:** action **Allow**, rule **Emails** with your e-mail (Access sends a code by e-mail on each login).

After that, opening `https://esp.yourdomain.com` first asks for the Access code and only then for the panel password.

### Notes

- Between the browser and Cloudflare, everything is HTTPS. From `cloudflared` to the device it is HTTP on the local network.
- The panel sends `Cache-Control: no-store` and `X-Frame-Options: DENY`: nothing stays in the Cloudflare or browser cache, and the panel does not open inside another site.
- The local API (`/api/state`, `/api/cmd`) also goes through the tunnel, but behind Access it does not answer scripts. For automation from outside, prefer Home Assistant or ntfy. If you really need the API, create an Access **Service Token** just for it.

## What not to do

- Open a router port to the device, even "just for a while".
- Publish the tunnel without Access in front.
- Expose the MQTT broker to the internet without TLS and without a password.
- Use the Wi-Fi password as the panel password.
