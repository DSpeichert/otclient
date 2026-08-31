# Proxy System

OTClient can route the game connection through one or more relay servers
("proxies") instead of connecting to the game server directly. The system
comes from OTCv8: the client keeps a pool of proxies, measures their latency
and uses the best `maxActiveProxies` (default 2) of them concurrently for each
session, with per-packet sequence numbers so a packet lost on one proxy is
delivered through another.

Engine code: `src/framework/proxy/` (`ProxyManager` = `g_proxy`, `Proxy`,
`Session`). The routing decision is in `Protocol::connect`
(`src/framework/net/protocol.cpp`).

## When the proxies are used

`g_proxy` only takes over a connection when the host the client is asked to
connect to is:

- `proxy`
- `0.0.0.0`
- `127.0.0.1` **while at least one proxy is registered**

Everything else connects directly, even if proxies are registered. A login
server that wants players to go through proxies therefore returns `"proxy"`
as the world address (`externaladdressprotected` in the HTTP login response).

## Registering proxies

### From Lua

```lua
-- plain TCP proxy
g_proxy.addProxy('proxy1.example.com', 7171, 0)
-- proxy tunnelled over a WebSocket (port argument is ignored, put it in the url)
g_proxy.addProxy('wss://proxy2.example.com/otc', 0, 20)

g_proxy.removeProxy('proxy1.example.com', 7171)
g_proxy.clear()                 -- drops every proxy AND terminates proxied sessions
g_proxy.setMaxActiveProxies(2)
g_proxy.getProxies()            -- { ["host:port" or url] = ping }
g_proxy.getProxiesDebugInfo()   -- shown by client_debug_info
g_proxy.getPing()
```

`priority` is added (in ms) to the measured ping when proxies are ranked, so
a higher value makes a proxy less preferred.

Proxies added this way (for example from `init.lua` or `otclientrc.lua`) are
static: the login flow never removes them.

### From the otshosting.pl discovery API (`modules/otshosting`)

Servers hosted on [otshosting.pl](https://otshosting.pl) expose their OTShield
proxy entry points over a public discovery endpoint:

```
GET https://otshosting.pl/api/proxy/{subdomain}
```

where `{subdomain}` is the server's subdomain label (the `myserver` part of
`myserver.ots.ovh`). The response is one flat list; every entry has exactly a
`host` and a `priority` (lower = preferred):

```json
{
  "proxies": [
    { "host": "proxy1.ots.ovh:25001", "priority": 0 },
    { "host": "wss://myserver.ots.ovh:8443/session", "priority": 0 }
  ]
}
```

`host` is either `hostname:port` (TCP proxy — each listed port accepts both
login and game traffic) or a full `wss://` url (WebSocket proxy). The same
hostname may appear multiple times with different ports; each entry is an
independent candidate. An empty array means the proxy service is disabled.

The response may also carry a `web` object next to `proxies` — it is consumed
only by the hosted web client (see below) and native clients ignore it:

```json
{
  "proxies": [ ... ],
  "web": {
    "name": "My Server",
    "clientVersion": 860,
    "login": { "type": "classic", "host": "proxy", "port": 7171 },
    "wsProxies": [
      { "host": "wss://myserver.ots.ovh:8443/session",  "priority": 0 },
      { "host": "wss://myserver.ots.ovh:8443/session2", "priority": 0 }
    ],
    "assets": {
      "things": [
        { "file": "Tibia.dat", "url": "https://client.ots.ovh/assets/myserver/<sha8>/Tibia.dat", "sha256": "<64 hex>", "size": 123 },
        { "file": "Tibia.spr", "url": "...", "sha256": "...", "size": 456 }
      ]
    }
  }
}
```

`login.type` is `classic` (tunnelled `ProtocolLogin`, `host`/`port` land in the
login screen) or `http` (`login.url` is an HTTP login endpoint). An optional
`rsa` key (decimal modulus, digits only; exponent 65537) configures servers
that use a non-standard RSA key — omitted, the standard OTServ key applies.
`wsProxies` always lists the server's own wss entry points, even when the
`proxies` list is empty. `assets` (classic servers only) lists the
`data/things/<clientVersion>/` files the web client downloads and
sha256-verifies before the first login; `null` for modern servers, which use
`modules/client_assets` instead.

The `otshosting` module consumes this endpoint. It is activated by registering
the server's subdomain in `init.lua`:

```lua
Services = {
    otshosting = {
        subdomain = "myserver",          -- required, activates the module
        refreshInterval = 300,           -- optional, seconds (API caches ≤ 60s)
        hideServerFields = true,         -- optional, default true
        host = "proxy", port = 7171,     -- optional login screen prefill
        url = "https://otshosting.pl/api/proxy/", -- optional endpoint override
    },
}
```

The module then:

1. loads the proxy list on startup,
2. refreshes it every `refreshInterval` seconds (default 5 minutes), also
   while connected. Updates are applied as a **graceful diff**: new proxies
   are registered first, then proxies that disappeared from the list are
   removed one by one — never a clear-and-readd, so active sessions keep
   flowing over the remaining proxies (the session layer already multiplexes
   over several proxies with resend). Discovery errors keep the current list
   untouched,
3. hides the server address/port fields on the login screen
   (`hideServerFields`, since the proxy makes the address moot; `host`
   defaults to leaving the current value, set it to `"proxy"` to force proxy
   routing), and
4. registers a **Proxy Diagnostics** window (top-menu button or
   `Ctrl+Shift+P`) showing every proxy's transport, state, ping, priority,
   session count and traffic, plus the last/next refresh, with a manual
   refresh button. It is backed by `g_proxy.getProxiesStatus()`, which returns
   a structured snapshot (see `meta.lua`) usable by any custom UI.

Proxies registered by the module are tracked separately: statically configured
proxies (`init.lua`, `otclientrc.lua`) are never touched by a refresh.

## Transports

### TCP

`host` + `port`: the proxy protocol runs directly on a TCP connection to the
proxy server.

### WebSocket (`ws://` / `wss://`)

`host` is a WebSocket url. The client opens it (ixwebsocket on native builds,
the browser's own WebSocket via the emscripten API in the WASM build) and
carries **exactly the same proxy protocol** inside binary WebSocket messages:
every outgoing proxy packet (2 byte size prefix included) is sent as one
binary message, and incoming messages are reassembled into proxy packets using
the size prefix, so relays are free to split or coalesce messages.

The server side therefore only needs a WebSocket → TCP unwrapper (websockify,
nginx `stream`, Cloudflare + websockify, …) in front of an ordinary proxy
server; nothing on the proxy server itself has to know about WebSockets.
The main use is reaching the proxy from networks that only allow HTTP(S)
traffic, or fronting it with a CDN that understands WebSockets.

`wss://` certificates are validated against the system CA store (ixwebsocket
default); there is no option to skip validation.

## Browser (WASM) build

The emscripten build supports **only WebSocket proxies**: TCP entries are
rejected by `g_proxy.addProxy` with a warning (a browser cannot open raw TCP
sockets), and the `otshosting` module skips them silently in web mode. The
proxy engine itself is unchanged — sessions, ranking, resend and the wire
protocol are identical — but the io context is pumped from the dispatcher
thread instead of a dedicated thread (`ProxyManager::init`), because
emscripten delivers WebSocket events on the thread that created the socket.

`Protocol::connect` applies the same routing rule as on native builds, so with
registered proxies both the classic login (port 7171) and the game connection
(port 7172) tunnel through wss — no CORS and no direct WebSocket endpoint on
the game server needed. The 7172 → 443 port rewrite used for direct browser
connections is skipped for proxy-routed hosts: the real port is carried inside
the proxy protocol's open-session packet.

The **hosted web client** (https://client.ots.ovh/s/&lt;subdomain&gt;, servers
hosted on otshosting.pl) builds on this: the page injects the subdomain via
`window.OTW` (see `browser/shell.html`), `init.lua` activates the `otshosting`
module in web mode, and the module locks the login screen to the server
described by the discovery response's `web` object, downloads classic
`.dat`/`.spr` assets into the IndexedDB-persisted write dir when needed, and
registers only the `wsProxies`. `tools/emscripten-web-serve.py` mirrors the
`/s/<subdomain>` route for local testing.

## Wire protocol (for proxy server implementers)

All integers are little endian. Every packet is `[u16 size][body]` where
`size` is the body length (>= 12).

| body | meaning |
|---|---|
| `u32 session=0, u32 packetId=0, u32 uid, u32 lastPing` | ping (client → proxy); the proxy echoes it back |
| `u32 session, u32 packetId=0, u32 port` | open `session` towards the game server on `port` |
| `u32 session, u32 packetId=0xFFFFFFFF, u32 unused` | close `session` |
| `u32 session, u32 packetId, u32 lastReceivedPacketId, u16 len, bytes[len]` | game packet `packetId` of `session` (`len`-prefixed Tibia packet); `lastReceivedPacketId` acknowledges packets in the other direction |

Session packet ids start at 1 and are resent through every active proxy until
acknowledged; the receiver reorders by id and ignores duplicates.
