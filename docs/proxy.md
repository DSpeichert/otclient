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
