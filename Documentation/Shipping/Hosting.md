# Hosting

How players reach a game that hosts, and how to let browser players in from an https page.

## Ports and addresses

A game hosts when its script calls `Net.startServer(port)` (or `Net.startDedicatedServer(port)`).
The host then listens on two ports:

- `port`, over UDP: desktop and Steam Deck players.
- `port + 1`, a WebSocket gateway: browser players, who have no UDP. Plain `ws://`.

A joining script calls `Net.connect(address, port)` with the host's game port. The address
decides how it joins:

| Address | Desktop / Deck | Browser |
|---|---|---|
| `192.168.1.20`, `play.example.com` | UDP to `port` | `ws://` to `port + 1`, or `wss://` when the page itself is https |
| `ws://play.example.com` | WebSocket to `port + 1` | the same |
| `wss://play.example.com:443/path` | secure WebSocket to the port named | the same |

A `ws://` or `wss://` address without a port of its own goes to `port + 1`, the gateway. Name the
port when something else answers there: a TLS proxy on 443 is reached as
`wss://play.example.com:443`, not `wss://play.example.com`.

## Browser players from an https page

A browser on an https page (a game on a website) may only open `wss://`, and only to a server
whose certificate a public authority issued for its domain name. A self-signed certificate, or a
bare IP address, is refused without a reason the game can see. So browser players from an https
page can join only a host with a domain name and a real certificate. Players on a LAN, or a page
served over plain http, use `ws://` and need none of this.

The way to give a host that certificate is a TLS reverse proxy in front of its gateway: the proxy
holds the certificate (and renews it: they last 90 days or less), takes `wss://` on 443, and
passes plain WebSocket traffic to the gateway on the same machine.

**Caddy** fetches and renews a Let's Encrypt certificate by itself. With the game hosting on 7000
(gateway on 7001), the whole `Caddyfile` is:

```
play.example.com {
    reverse_proxy 127.0.0.1:7001
}
```

**nginx**, with a certificate from certbot:

```
server {
    listen 443 ssl;
    server_name play.example.com;
    ssl_certificate     /etc/letsencrypt/live/play.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/play.example.com/privkey.pem;

    location / {
        proxy_pass http://127.0.0.1:7001;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_read_timeout 1h; # a quiet game must not be cut off
    }
}
```

Then:

- Point the domain's DNS at the machine and open 80 and 443 (Caddy and certbot prove the domain
  over 80). Close the gateway port (7001) to the outside: only the proxy needs it.
- Leave the UDP port (7000) open if desktop and Deck players join the same host directly.
- Browser players join `wss://play.example.com:443`. Masquerade's Join field takes that address
  as typed.

## A certificate in the host itself

A game with native code can hand the gateway a certificate and key directly (`WebSocketTls`,
passed to `NetworkManager::HostServer`); every browser connection is then TLS before its upgrade.
The host reads the files once, as it starts, so renewing means restarting it. A script cannot do
this, and there is no player setting or command-line flag for it yet: a dedicated-server launch
of the player, with its TLS files, is planned for 0.2. Until then the proxy is the way.

## Who hosts today

A host is a running game whose script has called `Net.startServer`, usually from its menu: a
player's desktop build, or play in editor (which hosts and joins in two Game tabs). The player
has no headless or command-line hosting yet (the same 0.2 item), so a public server today is a
desktop player running on the server machine, hosting from its menu.
