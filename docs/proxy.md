# Proxy System

OTCv8 can route the login and game connection through one or more relay
servers ("proxies") instead of connecting to the game server directly. The
client keeps a pool of proxies, measures their latency and uses the best
`maxActiveProxies` (default 2) of them concurrently for each session, with
per-packet sequence numbers so a packet lost on one proxy is delivered through
another.

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
as the world address (`externaladdressprotected` in the Tibia 12 style HTTP
login response, `worldIp` of a character in the OTCv8 login.php response).

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
g_proxy.getProxiesDebugInfo()   -- reported by client_stats
g_proxy.getProxiesStatus()      -- structured snapshot, see below
g_proxy.getPing()
```

`priority` is added (in ms) to the measured ping when proxies are ranked, so
a higher value makes a proxy less preferred.

Proxies added this way (for example from `init.lua`) are static: the login
flow never removes them.

### From the login server

Every login flow of `modules/client_entergame` accepts a proxy list:

- the classic login protocol via the `LoginServerProxyList` (110) opcode,
- the OTCv8 HTTP login (`login.php`) via a top-level `proxies` array,
- the Tibia 12 style HTTP login via `playdata.proxies`:

```json
{
  "session": { "sessionkey": "...", "premiumuntil": 0 },
  "playdata": {
    "worlds": [
      { "id": 0, "name": "MyWorld", "externaladdressprotected": "proxy", "externalportprotected": 7172 }
    ],
    "characters": [ { "worldid": 0, "name": "Player" } ],
    "proxies": [
      { "host": "proxy1.example.com", "port": 7171, "priority": 0 },
      { "host": "wss://proxy2.example.com/otc", "port": 0, "priority": 20 }
    ]
  }
}
```

Every entry is registered with `g_proxy.addProxy`. The list is scoped to that
login: proxies received from a previous login are removed before the next
login attempt (including logins to classic `ip:port` servers), while
statically configured proxies are left alone. Returning an empty array or
omitting the field simply means "no proxies".

The world address must be `"proxy"` (or `0.0.0.0`) for the proxies to be used,
and the world port is the port the proxy server should open the session to on
its side.

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

The `otshosting` module consumes this endpoint. It is activated by registering
the server's subdomain in `init.lua`:

```lua
Services = {
  otshosting = {
    subdomain = "myserver",          -- required, activates the module
    refreshInterval = 300,           -- optional, seconds (API caches ≤ 60s)
    hideServerFields = true,         -- optional, default true
    host = "proxy", port = 7171, version = 1098, -- optional login screen prefill
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
3. hides the server selection on the login screen (`hideServerFields`, since
   the proxy makes the address moot). `host`/`port`/`version` prefill the
   hidden server field in the `host:port:version` format `EnterGame.doLogin`
   expects; set `host` to `"proxy"` to force proxy routing. Without a prefill
   the current server selection is kept (for example a single entry in the
   `Servers` table of `init.lua`), and
4. registers a **Proxy Diagnostics** window (top-menu button or
   `Ctrl+Shift+P`) showing every proxy's transport, state, ping, priority,
   session count and traffic, plus the last/next refresh, with a manual
   refresh button. It is backed by `g_proxy.getProxiesStatus()`, usable by any
   custom UI, which returns a list of tables with the fields `host`, `port`,
   `webSocket`, `connected`, `ping` (measured + priority), `realPing`,
   `priority`, `sessions`, `connections`, `packetsSent`, `packetsReceived`,
   `bytesSent`, `bytesReceived` and `resolvedIp`.

Proxies registered by the module are tracked separately: statically configured
proxies (`init.lua`) and proxies announced by the login server are never
touched by a refresh.

## Transports

### TCP

`host` + `port`: the proxy protocol runs directly on a TCP connection to the
proxy server.

### WebSocket (`ws://` / `wss://`)

`host` is a WebSocket url. The client opens it with Boost.Beast and carries
**exactly the same proxy protocol** inside binary WebSocket messages: every
outgoing proxy packet (2 byte size prefix included) is sent as one binary
message, and incoming messages are reassembled into proxy packets using the
size prefix, so relays are free to split or coalesce messages.

The server side therefore only needs a WebSocket → TCP unwrapper (websockify,
nginx `stream`, Cloudflare + websockify, …) in front of an ordinary proxy
server; nothing on the proxy server itself has to know about WebSockets.
The main use is reaching the proxy from networks that only allow HTTP(S)
traffic, or fronting it with a CDN that understands WebSockets.

`wss://` follows the TLS policy of the rest of the client (`HttpSession`,
`WebsocketSession`): the connection is encrypted and SNI is sent, but the
server certificate is not validated. The Tibia protocol carried inside is
encrypted on its own (RSA/XTEA).

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
