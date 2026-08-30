/*
 * Copyright (c) 2010-2026 OTClient <https://github.com/edubart/otclient>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

 //#define PROXY_DEBUG

#include "proxy_client.h"

#include <framework/core/logger.h>

std::map<uint32_t, std::weak_ptr<Session>> g_sessions;
std::set<std::shared_ptr<Proxy>> g_proxies;
uint32_t UID = (std::chrono::high_resolution_clock::now().time_since_epoch().count()) & 0xFFFFFFFF;

#ifdef __EMSCRIPTEN__
namespace {
    // Emscripten hands websocket events a raw userData pointer with no lifetime
    // guarantees, so events are routed through this registry instead: it maps
    // the socket handle back to a live Proxy, and an event that arrives after
    // disconnect() removed the entry is simply dropped. Everything runs on the
    // dispatcher thread (see ProxyManager::init()), no locking needed.
    std::map<EMSCRIPTEN_WEBSOCKET_T, std::weak_ptr<Proxy>> g_emWebSockets;

    std::shared_ptr<Proxy> findEmProxy(const EMSCRIPTEN_WEBSOCKET_T socket)
    {
        const auto it = g_emWebSockets.find(socket);
        if (it == g_emWebSockets.end())
            return nullptr;
        return it->second.lock();
    }
}
#endif

void Proxy::start()
{
#ifdef PROXY_DEBUG
    g_logger.debug("[Proxy {}] start", m_host);
#endif
    auto self(shared_from_this());
    post(m_io, [&, self] {
        const std::error_code ec;
        g_proxies.insert(self);
        check(ec);
    });
}

void Proxy::terminate()
{
    if (m_terminated)
        return;
    m_terminated = true;

#ifdef PROXY_DEBUG
    g_logger.debug("[Proxy {}] terminate", m_host);
#endif

    auto self(shared_from_this());
    post(m_io, [&, self] {
        g_proxies.erase(self);
        disconnect();
        std::error_code ec;
        m_timer.cancel(ec);
    });
}

std::string Proxy::getDebugInfo()
{
    std::stringstream ss;
    ss << (m_webSocket ? "WS " : "") << "P: " << getPing() << " RP: " << getRealPing() << " In: " << m_packetsRecived << " (" << m_bytesRecived
        << ")  Out: " << m_packetsSent << " (" << m_bytesSent << ") Conns: " << m_connections << " Sess: " << m_sessions << " R: " << m_resolvedIp;
    return ss.str();
}

void Proxy::check(const std::error_code& ec)
{
    if (ec || m_terminated) {
        return;
    }

    const int32_t lastPing = static_cast<int32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - m_lastPingSent).count());
    if (m_state == STATE_NOT_CONNECTED) {
        connect();
    } else if (m_state == STATE_CONNECTING) { // timeout for async_connect
        if (lastPing + 50 > CHECK_INTERVAL * 5) {
            disconnect();
        }
    } else if (m_state == STATE_CONNECTED || m_state == STATE_CONNECTING_WAIT_FOR_PING) {
        if (m_waitingForPing) {
            if (lastPing + 50 > CHECK_INTERVAL * (m_state == STATE_CONNECTING_WAIT_FOR_PING ? 5 : 3)) {
#ifdef PROXY_DEBUG
                g_logger.debug("[Proxy {}] ping timeout", m_host);
#endif
                disconnect();
            }
        } else if (m_state == STATE_CONNECTED) {
            ping();
        }
    }
    m_timer.expires_from_now(std::chrono::milliseconds(CHECK_INTERVAL));
    m_timer.async_wait([capture0 = shared_from_this()](auto&& PH1) {
        capture0->check(std::forward<decltype(PH1)>(PH1));
    });
}

bool Proxy::isWebSocketUrl(const std::string& host)
{
    if (host.size() < 5)
        return false;
    std::string prefix = host.substr(0, 6);
    std::ranges::transform(prefix, prefix.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return prefix.starts_with("ws://") || prefix.starts_with("wss://");
}

void Proxy::connect()
{
#ifdef PROXY_DEBUG
    g_logger.debug("[Proxy {}] connecting to {}:{}", m_host, m_host, m_port);
#endif
    m_sendQueue.clear();
    m_waitingForPing = false;
    m_state = STATE_CONNECTING;
    m_connections += 1;
    m_sessions = 0;
    ++m_generation; // invalidate handlers of any previous connection
    m_lastPingSent = std::chrono::high_resolution_clock::now(); // used for connect timeout in check()

    if (m_webSocket) {
        connectWebSocket();
    } else {
        connectSocket();
    }
}

void Proxy::connectSocket()
{
    m_resolver = asio::ip::tcp::resolver(m_io);
    auto self(shared_from_this());
    m_resolver.async_resolve(m_host, "http", [self, gen = m_generation](const std::error_code& ec,
                             asio::ip::tcp::resolver::results_type results) {
        if (gen != self->m_generation)
            return;
        auto endpoint = asio::ip::tcp::endpoint();
        if (ec || results.empty()) {
#ifdef PROXY_DEBUG
            g_logger.debug("[Proxy {}] resolve error: {}", self->m_host, ec.message());
#endif
            std::error_code ecc;
            const auto address = asio::ip::make_address_v4(self->m_host, ecc);
            if (ecc) {
                self->m_state = STATE_NOT_CONNECTED;
                return;
            }
            endpoint = asio::ip::tcp::endpoint(address, self->m_port);
        } else {
            endpoint = asio::ip::tcp::endpoint(*results);
            endpoint.port(self->m_port);
        }
        self->m_resolvedIp = endpoint.address().to_string();
        self->m_socket = asio::ip::tcp::socket(self->m_io);
        self->m_lastPingSent = std::chrono::high_resolution_clock::now(); // used for async_connect timeout
        self->m_socket.async_connect(endpoint, [self, endpoint, gen](const std::error_code& ec) {
            if (gen != self->m_generation)
                return;
            if (ec) {
                self->m_state = STATE_NOT_CONNECTED;
                return;
            }
            std::error_code ecc;
            self->m_socket.set_option(asio::ip::tcp::no_delay(true), ecc);
            self->m_socket.set_option(asio::socket_base::send_buffer_size(65536), ecc);
            self->m_socket.set_option(asio::socket_base::receive_buffer_size(65536), ecc);
            if (ecc) {
#ifdef PROXY_DEBUG
                g_logger.debug("[Proxy {}] connect error: {}", self->m_host, ecc.message());
#endif
            }

            self->m_state = STATE_CONNECTING_WAIT_FOR_PING;
            self->readHeader();
            self->ping();
#ifdef PROXY_DEBUG
            g_logger.debug("[Proxy {}] connected", self->m_host);
#endif
        });
    });
}

void Proxy::connectWebSocket()
{
#ifndef __EMSCRIPTEN__
    const uint32_t generation = m_generation;
    m_wsRecvBuffer.clear();

    auto ws = std::make_shared<ix::WebSocket>();
    ws->setUrl(m_host);
    // reconnects are driven by Proxy::check(), exactly like the socket transport
    ws->disableAutomaticReconnection();
    ws->setHandshakeTimeout(CHECK_INTERVAL * 5 / 1000);

    ix::WebSocketHttpHeaders headers;
    headers["User-Agent"] = "OTClient";
    ws->setExtraHeaders(headers);

    // The callback runs on the ixwebsocket thread. All proxy state lives on m_io,
    // so every event is forwarded there. The Proxy reference obtained from the
    // weak pointer is always moved into the posted handler: if it were released
    // on the ixwebsocket thread and happened to be the last one, ~Proxy would
    // stop() the websocket and try to join the very thread it runs on.
    std::weak_ptr<Proxy> weakSelf = shared_from_this();
    ws->setOnMessageCallback([weakSelf, generation](const ix::WebSocketMessagePtr& msg) {
        if (!msg)
            return;

        auto self = weakSelf.lock();
        if (!self)
            return;

        const auto type = msg->type;
        std::string payload;
        std::string reason;
        if (type == ix::WebSocketMessageType::Message) {
            payload = msg->str;
        } else if (type == ix::WebSocketMessageType::Error) {
            reason = msg->errorInfo.reason;
        } else if (type == ix::WebSocketMessageType::Close) {
            reason = msg->closeInfo.reason;
        } else if (type != ix::WebSocketMessageType::Open) {
            return; // ping/pong/fragment, nothing to do
        }

        auto& io = self->m_io;
        post(io, [self = std::move(self), generation, type, payload = std::move(payload), reason = std::move(reason)] {
            self->onWebSocketEvent(generation, type, payload, reason);
        });
    });

    m_ws = ws;
    ws->start();
#else
    m_wsRecvBuffer.clear();

    // created on the dispatcher thread, which also pumps m_io: all callbacks
    // below fire on this same thread between pumps, so they may touch proxy
    // state directly. connect()/handshake timeouts are driven by check().
    EmscriptenWebSocketCreateAttributes attributes = {
        m_host.c_str(),
        "binary",
        EM_FALSE
    };
    m_emWs = emscripten_websocket_new(&attributes);
    if (m_emWs <= 0) {
#ifdef PROXY_DEBUG
        g_logger.debug("[Proxy {}] emscripten_websocket_new failed: {}", m_host, m_emWs);
#endif
        m_emWs = 0;
        m_state = STATE_NOT_CONNECTED;
        return;
    }
    g_emWebSockets[m_emWs] = weak_from_this();

    emscripten_websocket_set_onopen_callback(m_emWs, nullptr,
                                             [](int, const EmscriptenWebSocketOpenEvent* event, void*) -> EM_BOOL {
        if (const auto self = findEmProxy(event->socket)) {
            self->m_wsRecvBuffer.clear();
            self->m_state = STATE_CONNECTING_WAIT_FOR_PING;
            self->ping();
#ifdef PROXY_DEBUG
            g_logger.debug("[Proxy {}] connected", self->m_host);
#endif
        }
        return EM_TRUE;
    });

    emscripten_websocket_set_onmessage_callback(m_emWs, nullptr,
                                                [](int, const EmscriptenWebSocketMessageEvent* event, void*) -> EM_BOOL {
        if (event->isText || event->numBytes == 0)
            return EM_TRUE; // the proxy protocol is carried in binary messages only
        if (const auto self = findEmProxy(event->socket)) {
            self->onWebSocketData(std::string_view(reinterpret_cast<const char*>(event->data), event->numBytes));
        }
        return EM_TRUE;
    });

    emscripten_websocket_set_onerror_callback(m_emWs, nullptr,
                                              [](int, const EmscriptenWebSocketErrorEvent* event, void*) -> EM_BOOL {
        if (const auto self = findEmProxy(event->socket)) {
#ifdef PROXY_DEBUG
            g_logger.debug("[Proxy {}] websocket error", self->m_host);
#endif
            self->disconnect();
        }
        return EM_TRUE;
    });

    emscripten_websocket_set_onclose_callback(m_emWs, nullptr,
                                              [](int, const EmscriptenWebSocketCloseEvent* event, void*) -> EM_BOOL {
        if (const auto self = findEmProxy(event->socket)) {
#ifdef PROXY_DEBUG
            g_logger.debug("[Proxy {}] websocket closed", self->m_host);
#endif
            self->disconnect();
        }
        return EM_TRUE;
    });
#endif
}

#ifndef __EMSCRIPTEN__
void Proxy::onWebSocketEvent(const uint32_t generation, const ix::WebSocketMessageType type, const std::string& payload, [[maybe_unused]] const std::string& reason)
{
    if (generation != m_generation || !m_ws || m_terminated)
        return; // event from a previous connection

    switch (type) {
        case ix::WebSocketMessageType::Open:
            m_wsRecvBuffer.clear();
            m_state = STATE_CONNECTING_WAIT_FOR_PING;
            ping();
#ifdef PROXY_DEBUG
            g_logger.debug("[Proxy {}] connected", m_host);
#endif
            break;
        case ix::WebSocketMessageType::Message:
            onWebSocketData(payload);
            break;
        case ix::WebSocketMessageType::Error:
        case ix::WebSocketMessageType::Close:
#ifdef PROXY_DEBUG
            g_logger.debug("[Proxy {}] websocket {}: {}", m_host, type == ix::WebSocketMessageType::Error ? "error" : "closed", reason);
#endif
            disconnect();
            break;
        default:
            break;
    }
}
#endif

// WebSocket messages are not guaranteed to map 1:1 to proxy packets (a relay
// may coalesce or split them), so the stream is reassembled using the 2 byte
// size prefix of every proxy packet, the same framing the socket transport reads.
bool Proxy::onWebSocketData(const std::string_view data)
{
    m_wsRecvBuffer.append(data);

    std::size_t offset = 0;
    while (m_wsRecvBuffer.size() - offset >= 2) {
        uint16_t packetSize;
        std::memcpy(&packetSize, m_wsRecvBuffer.data() + offset, 2);
        if (packetSize < 12 || packetSize > BUFFER_SIZE) {
#ifdef PROXY_DEBUG
            g_logger.debug("[Proxy {}] websocket wrong packet size {}", m_host, packetSize);
#endif
            disconnect();
            return false;
        }
        if (m_wsRecvBuffer.size() - offset < 2u + packetSize)
            break; // wait for the rest of the packet

        std::memcpy(m_buffer, m_wsRecvBuffer.data() + offset + 2, packetSize);
        offset += 2u + packetSize;

        m_packetsRecived += 1;
        m_bytesRecived += 2 + packetSize;
        if (!handlePacket(packetSize))
            return false;
    }

    m_wsRecvBuffer.erase(0, offset);
    return true;
}

void Proxy::disconnect()
{
    std::error_code ec;
    m_socket.close(ec);
#ifndef __EMSCRIPTEN__
    if (m_ws) {
        // invalidate callbacks of this connection before stopping it; stop()
        // joins the ixwebsocket thread, which is safe here because proxy code
        // never runs on that thread
        const auto ws = std::move(m_ws);
        ws->stop();
    }
#else
    if (m_emWs > 0) {
        // dropping the registry entry first makes any event still queued for
        // this socket a no-op, then the socket itself is released
        g_emWebSockets.erase(m_emWs);
        emscripten_websocket_close(m_emWs, 1000, "");
        emscripten_websocket_delete(m_emWs);
        m_emWs = 0;
    }
#endif
    m_wsRecvBuffer.clear();
    ++m_generation; // pending handlers of this connection must not touch the next one
    m_sendQueue.clear();
    m_state = STATE_NOT_CONNECTED;
    m_ping = CHECK_INTERVAL * 2;
}

void Proxy::ping()
{
    m_lastPingSent = std::chrono::high_resolution_clock::now();
    m_waitingForPing = true;
    // 2 byte size + 4 byte session (0 so it's ping) + 4 byte packet num (0) + 4 byte last recived packet num + 4 byte local ping
    const auto packet = std::make_shared<ProxyPacket>(18, 0);
    packet->at(0) = 16; // size = 12
    *(uint32_t*)(&packet->data()[10]) = UID;
    *(uint32_t*)(&packet->data()[14]) = m_ping;
    send(packet);
}

void Proxy::onPing(uint32_t /*packetId*/)
{
    if (m_state == STATE_CONNECTING_WAIT_FOR_PING) {
        m_state = STATE_CONNECTED;
    }
    m_waitingForPing = false;
    m_ping = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - m_lastPingSent).count());
}

void Proxy::addSession(const uint32_t id, const int port)
{
    const auto packet = std::make_shared<ProxyPacket>(14, 0);
    packet->at(0) = 12; // size = 12
    *(uint32_t*)(&(packet->data()[2])) = id;
    *(uint32_t*)(&(packet->data()[10])) = port;
    send(packet);
    m_sessions += 1;
}

void Proxy::removeSession(const uint32_t id)
{
    const auto packet = std::make_shared<ProxyPacket>(14, 0);
    packet->at(0) = 12; // size = 12
    *(uint32_t*)(&(packet->data()[2])) = id;
    *(uint32_t*)(&(packet->data()[6])) = 0xFFFFFFFF;
    send(packet);
    m_sessions -= 1;
}

void Proxy::readHeader()
{
    async_read(m_socket, asio::buffer(m_buffer, 2), [capture0 = shared_from_this(), gen = m_generation](auto&& PH1, auto&& PH2) {
        if (gen != capture0->m_generation)
            return; // read of a connection that was already torn down
        capture0->onHeader(std::forward<decltype(PH1)>(PH1), std::forward<decltype(PH2)>(PH2));
    });
}

void Proxy::onHeader(const std::error_code& ec, const std::size_t bytes_transferred)
{
    if (ec || bytes_transferred != 2) {
#ifdef PROXY_DEBUG
        g_logger.debug("[Proxy {}] onHeader error {}", m_host, ec.message());
#endif
        return disconnect();
    }

    m_packetsRecived += 1;
    m_bytesRecived += static_cast<int>(bytes_transferred);

    uint16_t packetSize = *(uint16_t*)m_buffer;
    if (packetSize < 12 || packetSize > BUFFER_SIZE) {
#ifdef PROXY_DEBUG
        g_logger.debug("[Proxy {}] onHeader wrong packet size {}", m_host, packetSize);
#endif
        return disconnect();
    }

    async_read(m_socket, asio::buffer(m_buffer, packetSize), [capture0 = shared_from_this(), gen = m_generation](auto&& PH1, auto&& PH2) {
        if (gen != capture0->m_generation)
            return;
        capture0->onPacket(std::forward<decltype(PH1)>(PH1), std::forward<decltype(PH2)>(PH2));
    });
}

void Proxy::onPacket(const std::error_code& ec, const std::size_t bytes_transferred)
{
    if (ec) {
#ifdef PROXY_DEBUG
        g_logger.debug("[Proxy {}] onPacket error {}", m_host, ec.message());
#endif
        return disconnect();
    }
    m_bytesRecived += static_cast<int>(bytes_transferred);

    if (!handlePacket(bytes_transferred))
        return;

    readHeader();
}

bool Proxy::handlePacket(const std::size_t size)
{
    if (size < 12) {
#ifdef PROXY_DEBUG
        g_logger.debug("[Proxy {}] handlePacket error, packet too short: {}", m_host, size);
#endif
        disconnect();
        return false;
    }

    // m_buffer is a byte array, read the header fields with memcpy so the
    // reads are neither misaligned nor strict-aliasing violations
    uint32_t sessionId, packetId, lastRecivedPacketId;
    std::memcpy(&sessionId, &m_buffer[0], 4);
    std::memcpy(&packetId, &m_buffer[4], 4);
    std::memcpy(&lastRecivedPacketId, &m_buffer[8], 4);

    if (sessionId == 0) {
        onPing(packetId);
        return true;
    }
    if (packetId == 0xFFFFFFFFu) {
#ifdef PROXY_DEBUG
        g_logger.debug("[Proxy {}] onPacket, session end: {}", m_host, sessionId);
#endif
        const auto it = g_sessions.find(sessionId);
        if (it != g_sessions.end()) {
            if (const auto session = it->second.lock()) {
                session->terminate();
            }
        }
        return true;
    }

    uint16_t packetSize;
    std::memcpy(&packetSize, &m_buffer[12], 2);

#ifdef PROXY_DEBUG
    // g_logger.debug("[Proxy {}] onPacket, session: {} packetId: {} lastRecivedPacket: {} size: {}", m_host, sessionId, packetId, lastRecivedPacketId, packetSize);
#endif

    const auto packet = std::make_shared<ProxyPacket>(m_buffer + 12, m_buffer + 14 + packetSize);
    const auto it = g_sessions.find(sessionId);
    if (it != g_sessions.end()) {
        if (const auto session = it->second.lock()) {
            session->onProxyPacket(packetId, lastRecivedPacketId, packet);
        }
    }
    return true;
}

void Proxy::send(const ProxyPacketPtr& packet)
{
    if (m_webSocket) {
#ifndef __EMSCRIPTEN__
        if (!m_ws)
            return;
        // ixwebsocket serializes writes internally; sendBinary fails when the
        // connection is not open, which the socket transport reports via onSent
        const auto info = m_ws->sendBinary(std::string(packet->begin(), packet->end()));
        if (!info.success) {
#ifdef PROXY_DEBUG
            g_logger.debug("[Proxy {}] websocket send error", m_host);
#endif
            return disconnect();
        }
        m_packetsSent += 1;
        m_bytesSent += static_cast<int>(packet->size());
#else
        if (m_emWs <= 0)
            return;
        // the browser buffers writes internally; a failure means the socket is
        // closed or closing, which the socket transport reports via onSent
        if (emscripten_websocket_send_binary(m_emWs, packet->data(), packet->size()) != EMSCRIPTEN_RESULT_SUCCESS) {
#ifdef PROXY_DEBUG
            g_logger.debug("[Proxy {}] websocket send error", m_host);
#endif
            return disconnect();
        }
        m_packetsSent += 1;
        m_bytesSent += static_cast<int>(packet->size());
#endif
        return;
    }

    const bool sendNow = m_sendQueue.empty();
    m_sendQueue.push_back(packet);
    if (sendNow) {
        // the handler keeps the packet alive: disconnect() resets the queue
        // while a write may still be in flight
        async_write(m_socket, asio::buffer(packet->data(), packet->size()),
                    [capture0 = shared_from_this(), gen = m_generation, packet](auto&& PH1, auto&& PH2) {
            if (gen != capture0->m_generation)
                return;
            capture0->onSent(std::forward<decltype(PH1)>(PH1), std::forward<decltype(PH2)>(PH2));
        });
    }
}

void Proxy::onSent(const std::error_code& ec, const std::size_t bytes_transferred)
{
    if (ec) {
#ifdef PROXY_DEBUG
        g_logger.debug("[Proxy {}] onSent error {}", m_host, ec.message());
#endif
        return disconnect();
    }
    m_packetsSent += 1;
    m_bytesSent += static_cast<int>(bytes_transferred);
    if (m_sendQueue.empty())
        return; // queue was reset by a reconnect while this write completed
    m_sendQueue.pop_front();
    if (!m_sendQueue.empty()) {
        const auto packet = m_sendQueue.front();
        async_write(m_socket, asio::buffer(packet->data(), packet->size()),
                    [capture0 = shared_from_this(), gen = m_generation, packet](auto&& PH1, auto&& PH2) {
            if (gen != capture0->m_generation)
                return;
            capture0->onSent(std::forward<decltype(PH1)>(PH1), std::forward<decltype(PH2)>(PH2));
        });
    }
}

void Session::start(const int maxConnections)
{
#ifdef PROXY_DEBUG
    g_logger.debug("[Session {}] start", m_id);
#endif
    m_maxConnections = maxConnections;
    auto self(shared_from_this());
    post(m_io, [&, self] {
        g_sessions[self->m_id] = self;
        m_lastPacket = std::chrono::high_resolution_clock::now();
        check(std::error_code());
        if (m_useSocket) {
            readHeader();
        }
    });
}

void Session::terminate(std::error_code ec)
{
    if (m_terminated)
        return;
    m_terminated = true;

#ifdef PROXY_DEBUG
    g_logger.debug("[Session {}] terminate", m_id);
#endif

    // self must be captured by value: nothing else keeps the session alive
    // once the check() timer sees m_terminated, and this lambda dereferences
    // members (and calls m_disconnectCallback) after that point
    auto self(shared_from_this());
    post(m_io, [this, self, ec] {
        g_sessions.erase(m_id);
        if (m_useSocket) {
            std::error_code ecc;
            m_socket.shutdown(asio::ip::tcp::socket::shutdown_both, ecc);
            m_socket.close(ecc);
            m_timer.cancel(ecc);
        } else if (m_disconnectCallback) {
            m_disconnectCallback(ec);
        }

        for (auto& proxy : m_proxies) {
            proxy->removeSession(m_id);
        }
        m_proxies.clear();
    });
}

void Session::check(const std::error_code& ec)
{
    if (ec || m_terminated) {
        return;
    }

    const uint32_t lastPacket = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - m_lastPacket).count());
    if (lastPacket > TIMEOUT) {
        return terminate(asio::error::timed_out);
    }

    selectProxies();

    m_timer.expires_from_now(std::chrono::milliseconds(CHECK_INTERVAL));
    m_timer.async_wait([capture0 = shared_from_this()](auto&& PH1) {
        capture0->check(std::forward<decltype(PH1)>(PH1));
    });
}

void Session::selectProxies()
{
    ProxyPtr worst_ping = nullptr;
    ProxyPtr best_ping = nullptr;
    ProxyPtr candidate_proxy = nullptr;
    for (auto& proxy : g_proxies) {
        if (!proxy->isConnected()) {
            m_proxies.erase(proxy);
            continue;
        }
        if (!m_proxies.contains(proxy)) {
            if (!candidate_proxy || proxy->getPing() < candidate_proxy->getPing()) {
                candidate_proxy = proxy;
            }
            continue;
        }
        if (!best_ping || proxy->getPing() < best_ping->getPing()) {
            best_ping = proxy;
        }
        if (!worst_ping || proxy->getPing() > worst_ping->getPing()) {
            worst_ping = proxy;
        }
    }
    if (candidate_proxy) {
        // change worst to new proxy only if it has at least 20 ms better ping then worst proxy
        const bool disconnectWorst = worst_ping && worst_ping != best_ping && worst_ping->getPing() > candidate_proxy->getPing() + 20;
        if (m_proxies.size() != m_maxConnections || disconnectWorst) {
#ifdef PROXY_DEBUG
            g_logger.debug("[Session {}] new proxy: {}", m_id, candidate_proxy->getHost());
#endif
            candidate_proxy->addSession(m_id, m_port);
            m_proxies.insert(candidate_proxy);
            for (auto& packet : m_proxySendQueue) {
                candidate_proxy->send(packet.second);
            }
        }
        if (m_proxies.size() > m_maxConnections) {
#ifdef PROXY_DEBUG
            g_logger.debug("[Session {}] remove proxy: {}", m_id, worst_ping->getHost());
#endif
            worst_ping->removeSession(m_id);
            m_proxies.erase(worst_ping);
        }
    }
}

void Session::onProxyPacket(uint32_t packetId, uint32_t lastRecivedPacketId, const ProxyPacketPtr& packet)
{
#ifdef PROXY_DEBUG
    g_logger.debug("[Session {}] onProxyPacket, id: {} ({}) last: {} ({}) size: {}", m_id, packetId, m_inputPacketId, lastRecivedPacketId, m_outputPacketId, packet->size());
#endif
    if (packetId < m_inputPacketId) {
        return; // old packet, ignore
    }

    auto it = m_proxySendQueue.begin();
    while (it != m_proxySendQueue.end() && it->first <= lastRecivedPacketId) {
        it = m_proxySendQueue.erase(it);
    }

    m_lastPacket = std::chrono::high_resolution_clock::now();
    const bool sendNow = m_sendQueue.emplace(packetId, packet).second;

    if (!sendNow || packetId != m_inputPacketId) {
        return;
    }

    if (!m_useSocket) {
        while (!m_sendQueue.empty() && m_sendQueue.begin()->first == m_inputPacketId) {
            m_inputPacketId += 1;
            if (m_recvCallback) {
                m_recvCallback(packet);
            }
            m_sendQueue.erase(m_sendQueue.begin());
        }
        return;
    }

    async_write(m_socket, asio::buffer(packet->data(), packet->size()),
                [capture0 = shared_from_this()](auto&& PH1, auto&& PH2) {
        capture0->onSent(std::forward<decltype(PH1)>(PH1), std::forward<decltype(PH2)>(PH2));
    });
}

void Session::readTibia12Header()
{
    auto self(shared_from_this());
    async_read(m_socket, asio::buffer(m_buffer, 1),
                            [self](const std::error_code& ec, std::size_t /*bytes_transferred*/) {
        if (ec) {
            return self->terminate();
        }
        if (self->m_buffer[0] == 0x0A) {
#ifdef PROXY_DEBUG
            g_logger.debug("[Session {}] Tibia 12 read header finished", self->m_id);
#endif
            return self->readHeader();
        }
        self->readTibia12Header();
    });
}

void Session::readHeader()
{
    async_read(m_socket, asio::buffer(m_buffer, 2),
               [capture0 = shared_from_this()](auto&& PH1, auto&& PH2) {
        capture0->onHeader(std::forward<decltype(PH1)>(PH1), std::forward<decltype(PH2)>(PH2));
    });
}

void Session::onHeader(const std::error_code& ec, std::size_t /*bytes_transferred*/)
{
    if (ec) {
#ifdef PROXY_DEBUG
        g_logger.debug("[Session {}] onHeader error: {}", m_id, ec.message());
#endif
        return terminate();
    }

    uint16_t packetSize = *(uint16_t*)(m_buffer);
    if (packetSize > 1024 && m_outputPacketId == 1) {
        return readTibia12Header();
    }

    if (packetSize == 0 || packetSize + 16 > BUFFER_SIZE) {
#ifdef PROXY_DEBUG
        g_logger.debug("[Session {}] onHeader invalid packet size: {}", m_id, packetSize);
#endif
        return terminate();
    }

    async_read(m_socket, asio::buffer(m_buffer + 2, packetSize),
               [capture0 = shared_from_this()](auto&& PH1, auto&& PH2) {
        capture0->onBody(std::forward<decltype(PH1)>(PH1), std::forward<decltype(PH2)>(PH2));
    });
}

void Session::onBody(const std::error_code& ec, const std::size_t bytes_transferred)
{
    if (ec) {
#ifdef PROXY_DEBUG
        g_logger.debug("[Session {}] onBody error: {}", m_id, ec.message());
#endif
        return terminate();
    }

    const auto packet = std::make_shared<ProxyPacket>(m_buffer, m_buffer + bytes_transferred + 2);
    onPacket(packet);

    readHeader();
}

void Session::onPacket(const ProxyPacketPtr& packet)
{
    if (!packet || packet->empty() || packet->size() + 14 > BUFFER_SIZE) {
#ifdef PROXY_DEBUG
        g_logger.debug("[Session {}] onPacket error: missing packet or wrong size", m_id);
#endif
        return terminate();
    }

    auto self(shared_from_this());
    post(m_io, [this, self, packet] {
        const uint32_t packetId = m_outputPacketId++;
        const auto newPacket = std::make_shared<ProxyPacket>(packet->size() + 14);

        *(uint16_t*)(&(newPacket->data()[0])) = static_cast<uint16_t>(packet->size()) + 12;
        *(uint32_t*)(&(newPacket->data()[2])) = m_id;
        *(uint32_t*)(&(newPacket->data()[6])) = packetId;
        *(uint32_t*)(&(newPacket->data()[10])) = m_inputPacketId - 1;
        std::copy(packet->begin(), packet->end(), newPacket->begin() + 14);

        m_proxySendQueue[packetId] = newPacket;
        for (auto& proxy : m_proxies) {
            proxy->send(newPacket);
        }
    });
}

void Session::onSent(const std::error_code& ec, std::size_t /*bytes_transferred*/)
{
    if (ec) {
#ifdef PROXY_DEBUG
        g_logger.debug("[Session {}] onSent error: {}", m_id, ec.message());
#endif
        return terminate();
    }

    m_inputPacketId += 1;
    m_sendQueue.erase(m_sendQueue.begin());
    if (!m_sendQueue.empty() && m_sendQueue.begin()->first == m_inputPacketId) {
        async_write(m_socket, asio::buffer(m_sendQueue.begin()->second->data(), m_sendQueue.begin()->second->size()),
                    [capture0 = shared_from_this()](auto&& PH1, auto&& PH2) {
            capture0->onSent(std::forward<decltype(PH1)>(PH1), std::forward<decltype(PH2)>(PH2));
        });
    }
}
