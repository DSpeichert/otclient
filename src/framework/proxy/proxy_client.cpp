//#define PROXY_DEBUG

#include <framework/global.h>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstring>

#include "proxy_client.h"

std::map<uint32_t, std::weak_ptr<Session>> g_sessions;
std::set<std::shared_ptr<Proxy>> g_proxies;
uint32_t UID = (std::chrono::high_resolution_clock::now().time_since_epoch().count()) & 0xFFFFFFFF;

void Proxy::start()
{
#ifdef PROXY_DEBUG
    std::clog << "[Proxy " << m_host << "] start" << std::endl;
#endif
    auto self(shared_from_this());
    boost::asio::post(m_io, [&, self] {
        g_proxies.insert(self);
        check();
    });
}

void Proxy::terminate()
{
    if (m_terminated)
        return;
    m_terminated = true;

#ifdef PROXY_DEBUG
    std::clog << "[Proxy " << m_host << "] terminate" << std::endl;
#endif

    auto self(shared_from_this());
    boost::asio::post(m_io, [&, self] {
        g_proxies.erase(self);
        disconnect();
        boost::system::error_code ec;
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


void Proxy::check(const boost::system::error_code& ec)
{
    if (ec || m_terminated) {
        return;
    }

    int32_t lastPing = (int32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - m_lastPingSent).count();
    if (m_state == STATE_NOT_CONNECTED) {
        connect();
    } else if (m_state == STATE_CONNECTING) { // timeout for async_connect
        if (lastPing + 50 > CHECK_INTERVAL * 5) {
            disconnect();
        }
    } else if (m_state == STATE_CONNECTED || m_state == STATE_CONNECTING_WAIT_FOR_PING) {
        if (m_waitingForPing) {
            if (lastPing + 50 > CHECK_INTERVAL* (m_state == STATE_CONNECTING_WAIT_FOR_PING ? 5 : 3)) {
#ifdef PROXY_DEBUG
                std::clog << "[Proxy " << m_host << "] ping timeout" << std::endl;
#endif
                disconnect();
            }
        } else if (m_state == STATE_CONNECTED) {
            ping();
        }
    }
    m_timer.expires_from_now(std::chrono::milliseconds(CHECK_INTERVAL));
    m_timer.async_wait(std::bind(&Proxy::check, shared_from_this(), std::placeholders::_1));
}

bool Proxy::isWebSocketUrl(const std::string& host)
{
    if (host.size() < 5)
        return false;
    std::string prefix = host.substr(0, 6);
    std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return prefix.rfind("ws://", 0) == 0 || prefix.rfind("wss://", 0) == 0;
}

void Proxy::connect()
{
#ifdef PROXY_DEBUG
    std::clog << "[Proxy " << m_host << "] connecting to " << m_host << ":" << m_port << std::endl;
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
    m_resolver = boost::asio::ip::tcp::resolver(m_io);
    auto self(shared_from_this());
    m_resolver.async_resolve(m_host, "http", [self, gen = m_generation](const boost::system::error_code& ec,
                                                                        boost::asio::ip::tcp::resolver::results_type results) {
        if (gen != self->m_generation)
            return;
        auto endpoint = boost::asio::ip::tcp::endpoint();
        if (ec || results.empty()) {
#ifdef PROXY_DEBUG
            std::clog << "[Proxy " << self->m_host << "] resolve error: " << ec.message() << std::endl;
#endif
            boost::system::error_code ecc;
            auto address = boost::asio::ip::make_address_v4(self->m_host, ecc);
            if (ecc) {
                self->m_state = STATE_NOT_CONNECTED;
                return;
            }
            endpoint = boost::asio::ip::tcp::endpoint(address, self->m_port);
        } else {
            endpoint = boost::asio::ip::tcp::endpoint(*results);
            endpoint.port(self->m_port);
        }
        self->m_resolvedIp = endpoint.address().to_string();
        self->m_socket = boost::asio::ip::tcp::socket(self->m_io);
        self->m_lastPingSent = std::chrono::high_resolution_clock::now(); // used for async_connect timeout
        self->m_socket.async_connect(endpoint, [self, endpoint, gen](const boost::system::error_code& ec) {
            if (gen != self->m_generation)
                return;
            if (ec) {
                self->m_state = STATE_NOT_CONNECTED;
                return;
            }
            boost::system::error_code ecc;
            self->m_socket.set_option(boost::asio::ip::tcp::no_delay(true), ecc);
            self->m_socket.set_option(boost::asio::socket_base::send_buffer_size(65536), ecc);
            self->m_socket.set_option(boost::asio::socket_base::receive_buffer_size(65536), ecc);
            if (ecc) {
#ifdef PROXY_DEBUG
                std::clog << "[Proxy " << self->m_host << "] connect error: " << ecc.message() << std::endl;
#endif
            }

            self->m_state = STATE_CONNECTING_WAIT_FOR_PING;
            self->readHeader();
            self->ping();
#ifdef PROXY_DEBUG
            std::clog << "[Proxy " << self->m_host << "] connected " << std::endl;
#endif
        });
    });
}

#ifndef __EMSCRIPTEN__
namespace {
    struct WebSocketUrl {
        bool secure = false;
        std::string host;
        std::string port;
        std::string target;
    };

    // ws://host[:port][/path] or wss://host[:port][/path]
    bool parseWebSocketUrl(const std::string& url, WebSocketUrl& out)
    {
        std::string scheme = url.substr(0, 6);
        std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        std::string rest;
        if (scheme.rfind("wss://", 0) == 0) {
            out.secure = true;
            rest = url.substr(6);
        } else if (scheme.rfind("ws://", 0) == 0) {
            out.secure = false;
            rest = url.substr(5);
        } else {
            return false;
        }

        const auto slash = rest.find('/');
        const std::string authority = rest.substr(0, slash);
        out.target = slash == std::string::npos ? "/" : rest.substr(slash);

        const auto colon = authority.rfind(':');
        if (colon != std::string::npos) {
            out.host = authority.substr(0, colon);
            out.port = authority.substr(colon + 1);
        } else {
            out.host = authority;
            out.port = out.secure ? "443" : "80";
        }

        if (out.host.empty() || out.port.empty())
            return false;
        for (const char c : out.port) {
            if (!std::isdigit(static_cast<unsigned char>(c)))
                return false;
        }
        return true;
    }
}
#endif

void Proxy::connectWebSocket()
{
#ifndef __EMSCRIPTEN__
    const uint32_t generation = m_generation;
    m_wsRecvBuffer.clear();
    m_ws.reset();

    WebSocketUrl url;
    if (!parseWebSocketUrl(m_host, url)) {
        g_logger.error("[Proxy " + m_host + "] invalid websocket url");
        m_state = STATE_NOT_CONNECTED;
        return;
    }

    // the Host header carries the port only when it is not the default one
    m_wsHostHeader = url.host;
    if (url.port != (url.secure ? "443" : "80"))
        m_wsHostHeader += ":" + url.port;
    m_wsTarget = url.target;

    auto conn = std::make_shared<WebSocketConnection>();
    if (url.secure) {
        // same TLS policy as the rest of the client (HttpSession / WebsocketSession):
        // the certificate is not validated, the Tibia protocol inside is encrypted anyway
        conn->sslContext = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tlsv12_client);
        conn->secure = std::make_unique<SecureWebSocketStream>(m_io, *conn->sslContext);
        conn->secure->next_layer().set_verify_mode(boost::asio::ssl::verify_peer);
        conn->secure->next_layer().set_verify_callback([](bool, boost::asio::ssl::verify_context&) { return true; });
        if (!SSL_set_tlsext_host_name(conn->secure->next_layer().native_handle(), url.host.c_str())) {
            g_logger.error("[Proxy " + m_host + "] can't set SNI host name");
            m_state = STATE_NOT_CONNECTED;
            return;
        }
    } else {
        conn->plain = std::make_unique<WebSocketStream>(m_io);
    }

    conn->withStream([&](auto& ws) {
        ws.binary(true);
        ws.read_message_max(1024 * 1024);
        ws.set_option(boost::beast::websocket::stream_base::decorator([](boost::beast::websocket::request_type& req) {
            req.set(boost::beast::http::field::user_agent, "OTClient");
        }));
    });
    m_ws = conn;

    // Every handler captures the connection so it can never be destroyed with an
    // operation in flight, and checks the generation so completions of a
    // connection that was already torn down are ignored.
    auto self(shared_from_this());
    m_resolver = boost::asio::ip::tcp::resolver(m_io);
    m_resolver.async_resolve(url.host, url.port, [self, conn, generation](const boost::system::error_code& ec,
                                                                          boost::asio::ip::tcp::resolver::results_type results) {
        if (generation != self->m_generation)
            return;
        if (ec || results.empty()) {
#ifdef PROXY_DEBUG
            std::clog << "[Proxy " << self->m_host << "] resolve error: " << ec.message() << std::endl;
#endif
            return self->disconnect();
        }
        self->m_resolvedIp = results.begin()->endpoint().address().to_string();

        conn->withStream([&](auto& ws) {
            boost::beast::get_lowest_layer(ws).expires_after(std::chrono::milliseconds(CHECK_INTERVAL * 5));
            boost::beast::get_lowest_layer(ws).async_connect(results, [self, conn, generation](const boost::system::error_code& ec,
                                                                                               const boost::asio::ip::tcp::endpoint& endpoint) {
                if (generation != self->m_generation)
                    return;
                if (ec) {
#ifdef PROXY_DEBUG
                    std::clog << "[Proxy " << self->m_host << "] connect error: " << ec.message() << std::endl;
#endif
                    return self->disconnect();
                }
                self->m_resolvedIp = endpoint.address().to_string();
                boost::system::error_code ecc;
                conn->withStream([&](auto& ws) {
                    boost::beast::get_lowest_layer(ws).socket().set_option(boost::asio::ip::tcp::no_delay(true), ecc);
                    // handshake and ping timeouts are enforced by check(), like the
                    // socket transport; Beast's stream timeouts must not overlap with
                    // websocket operations
                    boost::beast::get_lowest_layer(ws).expires_never();
                });
                self->handshakeWebSocket(generation, conn);
            });
        });
    });
#else
    g_logger.error("[Proxy " + m_host + "] WebSocket proxies are not supported in this build");
    m_state = STATE_NOT_CONNECTED;
#endif
}

#ifndef __EMSCRIPTEN__
void Proxy::handshakeWebSocket(uint32_t generation, const WebSocketConnectionPtr& conn)
{
    auto self(shared_from_this());
    auto onHandshake = [self, conn, generation](const boost::system::error_code& ec) {
        if (generation != self->m_generation)
            return;
        if (ec) {
#ifdef PROXY_DEBUG
            std::clog << "[Proxy " << self->m_host << "] websocket handshake error: " << ec.message() << std::endl;
#endif
            return self->disconnect();
        }
        self->m_wsRecvBuffer.clear();
        self->m_state = STATE_CONNECTING_WAIT_FOR_PING;
        self->readWebSocket(generation, conn);
        self->ping();
#ifdef PROXY_DEBUG
        std::clog << "[Proxy " << self->m_host << "] connected" << std::endl;
#endif
    };

    if (conn->secure) {
        conn->secure->next_layer().async_handshake(boost::asio::ssl::stream_base::client, [self, conn, generation, onHandshake](const boost::system::error_code& ec) {
            if (generation != self->m_generation)
                return;
            if (ec) {
#ifdef PROXY_DEBUG
                std::clog << "[Proxy " << self->m_host << "] ssl handshake error: " << ec.message() << std::endl;
#endif
                return self->disconnect();
            }
            conn->secure->async_handshake(self->m_wsHostHeader, self->m_wsTarget, onHandshake);
        });
    } else {
        conn->plain->async_handshake(m_wsHostHeader, m_wsTarget, onHandshake);
    }
}

void Proxy::readWebSocket(uint32_t generation, const WebSocketConnectionPtr& conn)
{
    auto self(shared_from_this());
    conn->withStream([&](auto& ws) {
        ws.async_read(conn->readBuffer, [self, conn, generation](const boost::system::error_code& ec, std::size_t) {
            if (generation != self->m_generation)
                return;
            if (ec) {
#ifdef PROXY_DEBUG
                std::clog << "[Proxy " << self->m_host << "] websocket read error: " << ec.message() << std::endl;
#endif
                return self->disconnect();
            }
            const std::string data = boost::beast::buffers_to_string(conn->readBuffer.data());
            conn->readBuffer.consume(conn->readBuffer.size());
            if (!self->onWebSocketData(data))
                return;
            self->readWebSocket(generation, conn);
        });
    });
}

// WebSocket messages are not guaranteed to map 1:1 to proxy packets (a relay
// may coalesce or split them), so the stream is reassembled using the 2 byte
// size prefix of every proxy packet, the same framing the socket transport reads.
bool Proxy::onWebSocketData(const std::string& data)
{
    m_wsRecvBuffer.append(data);

    std::size_t offset = 0;
    while (m_wsRecvBuffer.size() - offset >= 2) {
        uint16_t packetSize;
        std::memcpy(&packetSize, m_wsRecvBuffer.data() + offset, 2);
        if (packetSize < 12 || packetSize > BUFFER_SIZE) {
#ifdef PROXY_DEBUG
            std::clog << "[Proxy " << m_host << "] websocket wrong packet size " << packetSize << std::endl;
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
#endif

void Proxy::disconnect()
{
    boost::system::error_code ec;
    m_socket.close(ec);
#ifndef __EMSCRIPTEN__
    // just close the underlying socket: a websocket close handshake would need
    // the peer and pending handlers keep the stream alive until they complete
    if (m_ws) {
        m_ws->withStream([&](auto& ws) {
            boost::beast::get_lowest_layer(ws).close();
        });
        m_ws.reset();
    }
    m_wsRecvBuffer.clear();
#endif
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
    auto packet = std::make_shared<ProxyPacket>(18, 0);
    packet->at(0) = 16; // size = 12
    *(uint32_t*)(&packet->data()[10]) = UID;
    *(uint32_t*)(&packet->data()[14]) = m_ping;
    send(packet);
}

void Proxy::onPing(uint32_t packetId)
{
    if (m_state == STATE_CONNECTING_WAIT_FOR_PING) {
        m_state = STATE_CONNECTED;
    }
    m_waitingForPing = false;
    m_ping = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - m_lastPingSent).count();
}

void Proxy::addSession(uint32_t id, int port)
{
    auto packet = std::make_shared<ProxyPacket>(14, 0);
    packet->at(0) = 12; // size = 12
    *(uint32_t*)(&(packet->data()[2])) = id;
    *(uint32_t*)(&(packet->data()[10])) = port;
    send(packet);
    m_sessions += 1;
}

void Proxy::removeSession(uint32_t id)
{
    auto packet = std::make_shared<ProxyPacket>(14, 0);
    packet->at(0) = 12; // size = 12
    *(uint32_t*)(&(packet->data()[2])) = id;
    *(uint32_t*)(&(packet->data()[6])) = 0xFFFFFFFF;
    send(packet);
    m_sessions -= 1;
}

void Proxy::readHeader()
{
    auto self(shared_from_this());
    boost::asio::async_read(m_socket, boost::asio::buffer(m_buffer, 2), [self, gen = m_generation](const boost::system::error_code& ec, std::size_t bytes_transferred) {
        if (gen != self->m_generation)
            return; // read of a connection that was already torn down
        self->onHeader(ec, bytes_transferred);
    });
}

void Proxy::onHeader(const boost::system::error_code& ec, std::size_t bytes_transferred)
{
    if (ec || bytes_transferred != 2) {
#ifdef PROXY_DEBUG
        std::clog << "[Proxy " << m_host << "] onHeader error " << ec.message() << std::endl;
#endif
        return disconnect();
    }

    m_packetsRecived += 1;
    m_bytesRecived += bytes_transferred;

    uint16_t packetSize = *(uint16_t*)m_buffer;
    if (packetSize < 12 || packetSize > BUFFER_SIZE) {
#ifdef PROXY_DEBUG
        std::clog << "[Proxy " << m_host << "] onHeader wrong packet size " << packetSize << std::endl;
#endif
        return disconnect();
    }

    auto self(shared_from_this());
    boost::asio::async_read(m_socket, boost::asio::buffer(m_buffer, packetSize), [self, gen = m_generation](const boost::system::error_code& ec, std::size_t bytes_transferred) {
        if (gen != self->m_generation)
            return;
        self->onPacket(ec, bytes_transferred);
    });
}

void Proxy::onPacket(const boost::system::error_code& ec, std::size_t bytes_transferred)
{
    if (ec) {
#ifdef PROXY_DEBUG
        std::clog << "[Proxy " << m_host << "] onPacket error " << ec.message() << std::endl;
#endif
        return disconnect();
    }
    m_bytesRecived += bytes_transferred;

    if (!handlePacket(bytes_transferred))
        return;

    readHeader();
}

bool Proxy::handlePacket(std::size_t size)
{
    if (size < 12) {
#ifdef PROXY_DEBUG
        std::clog << "[Proxy " << m_host << "] handlePacket error, packet too short: " << size << std::endl;
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
        std::clog << "[Proxy " << m_host << "] onPacket, session end: " << sessionId << std::endl;
#endif
        auto it = g_sessions.find(sessionId);
        if (it != g_sessions.end()) {
            if (auto session = it->second.lock()) {
                session->terminate();
            }
        }
        return true;
    }

    uint16_t packetSize;
    std::memcpy(&packetSize, &m_buffer[12], 2);

#ifdef PROXY_DEBUG
    //std::clog << "[Proxy " << m_host << "] onPacket, session: " << sessionId << " packetId: " << packetId << " lastRecivedPacket: " << lastRecivedPacketId << " size: " << packetSize << std::endl;
#endif

    auto packet = std::make_shared<ProxyPacket>(m_buffer + 12, m_buffer + 14 + packetSize);
    auto it = g_sessions.find(sessionId);
    if (it != g_sessions.end()) {
        if (auto session = it->second.lock()) {
            session->onProxyPacket(packetId, lastRecivedPacketId, packet);
        }
    }
    return true;
}


void Proxy::send(const ProxyPacketPtr& packet)
{
    bool sendNow = m_sendQueue.empty();
    m_sendQueue.push_back(packet);
    if (sendNow) {
        writeNext();
    }
}

// writes the packet at the front of the send queue; both transports allow a
// single outstanding write, so the queue is drained from onSent one by one
void Proxy::writeNext()
{
    // the handler keeps the packet alive: disconnect() resets the queue while
    // a write may still be in flight
    const auto packet = m_sendQueue.front();
    if (m_webSocket) {
#ifndef __EMSCRIPTEN__
        if (!m_ws)
            return; // not connected, connect() resets the queue
        auto self(shared_from_this());
        const auto conn = m_ws;
        const uint32_t generation = m_generation;
        conn->withStream([&](auto& ws) {
            ws.async_write(boost::asio::buffer(packet->data(), packet->size()), [self, conn, generation, packet](const boost::system::error_code& ec, std::size_t bytes_transferred) {
                if (generation != self->m_generation)
                    return;
                self->onSent(ec, bytes_transferred);
            });
        });
#endif
        return;
    }
    auto self(shared_from_this());
    boost::asio::async_write(m_socket, boost::asio::buffer(packet->data(), packet->size()), [self, gen = m_generation, packet](const boost::system::error_code& ec, std::size_t bytes_transferred) {
        if (gen != self->m_generation)
            return;
        self->onSent(ec, bytes_transferred);
    });
}

void Proxy::onSent(const boost::system::error_code& ec, std::size_t bytes_transferred)
{
    if (ec) {
#ifdef PROXY_DEBUG
        std::clog << "[Proxy " << m_host << "] onSent error " << ec.message() << std::endl;
#endif
        return disconnect();
    }
    m_packetsSent += 1;
    m_bytesSent += bytes_transferred;
    if (m_sendQueue.empty())
        return; // queue was reset by a reconnect while this write completed
    m_sendQueue.pop_front();
    if (!m_sendQueue.empty()) {
        writeNext();
    }
}

void Session::start(int maxConnections)
{
#ifdef PROXY_DEBUG
    std::clog << "[Session " << m_id << "] start" << std::endl;
#endif
    m_maxConnections = maxConnections;
    auto self(shared_from_this());
    boost::asio::post(m_io, [&, self] {
        g_sessions[self->m_id] = self;
        m_lastPacket = std::chrono::high_resolution_clock::now();
        check(boost::system::error_code());
        if (m_useSocket) {
            readHeader();
        }
    });
}

void Session::terminate(boost::system::error_code ec)
{
    if (m_terminated)
        return;
    m_terminated = true;

#ifdef PROXY_DEBUG
    std::clog << "[Session " << m_id << "] terminate" << std::endl;
#endif

    // self must be captured by value: nothing else keeps the session alive
    // once the check() timer sees m_terminated, and this lambda dereferences
    // members (and calls m_disconnectCallback) after that point
    auto self(shared_from_this());
    boost::asio::post(m_io, [this, self, ec] {
        g_sessions.erase(m_id);
        if (m_useSocket) {
            boost::system::error_code ecc;
            m_socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ecc);
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

void Session::check(const boost::system::error_code& ec)
{
    if (ec || m_terminated) {
        return;
    }

    uint32_t lastPacket = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - m_lastPacket).count();
    if (lastPacket > TIMEOUT) {
        return terminate(boost::asio::error::timed_out);
    }

    selectProxies();

    m_timer.expires_from_now(std::chrono::milliseconds(CHECK_INTERVAL));
    m_timer.async_wait(std::bind(&Session::check, shared_from_this(), std::placeholders::_1));
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
        if (m_proxies.find(proxy) == m_proxies.end()) {
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
        bool disconnectWorst = worst_ping && worst_ping != best_ping && worst_ping->getPing() > candidate_proxy->getPing() + 20;
        if (m_proxies.size() != m_maxConnections || disconnectWorst) {
#ifdef PROXY_DEBUG
            std::clog << "[Session " << m_id << "] new proxy: " << candidate_proxy->getHost() << std::endl;
#endif
            candidate_proxy->addSession(m_id, m_port);
            m_proxies.insert(candidate_proxy);
            for (auto& packet : m_proxySendQueue) {
                candidate_proxy->send(packet.second);
            }
        }
        if ((int)m_proxies.size() > m_maxConnections) {
#ifdef PROXY_DEBUG
            std::clog << "[Session " << m_id << "] remove proxy: " << worst_ping->getHost() << std::endl;
#endif
            worst_ping->removeSession(m_id);
            m_proxies.erase(worst_ping);
        }
    }
}

void Session::onProxyPacket(uint32_t packetId, uint32_t lastRecivedPacketId, const ProxyPacketPtr& packet)
{
#ifdef PROXY_DEBUG
    std::clog << "[Session " << m_id << "] onProxyPacket, id: " << packetId << " (" << m_inputPacketId << ") last: " << lastRecivedPacketId <<
        " (" << m_outputPacketId << ") size: " << packet->size() << std::endl;
#endif
    if (packetId < m_inputPacketId) {
        return; // old packet, ignore
    }

    auto it = m_proxySendQueue.begin();
    while (it != m_proxySendQueue.end() && it->first <= lastRecivedPacketId) {
        it = m_proxySendQueue.erase(it);
    }

    m_lastPacket = std::chrono::high_resolution_clock::now();
    bool sendNow = m_sendQueue.emplace(packetId, packet).second;

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

    boost::asio::async_write(m_socket, boost::asio::buffer(packet->data(), packet->size()),
                             std::bind(&Session::onSent, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
}

void Session::readTibia12Header()
{
    auto self(shared_from_this());
    boost::asio::async_read(m_socket, boost::asio::buffer(m_buffer, 1),
                            [self](const boost::system::error_code& ec, std::size_t bytes_transferred) {
        if (ec) {
            return self->terminate();
        }
        if (self->m_buffer[0] == 0x0A) {
#ifdef PROXY_DEBUG
            std::clog << "[Session " << self->m_id << "] Tibia 12 read header finished" << std::endl;
#endif
            return self->readHeader();
        }
        self->readTibia12Header();
    });

}

void Session::readHeader()
{
    boost::asio::async_read(m_socket, boost::asio::buffer(m_buffer, 2),
                            std::bind(&Session::onHeader, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
}

void Session::onHeader(const boost::system::error_code& ec, std::size_t bytes_transferred)
{
    if (ec) {
#ifdef PROXY_DEBUG
        std::clog << "[Session " << m_id << "] onHeader error: " << ec.message() << std::endl;
#endif
        return terminate();
    }

    uint16_t packetSize = *(uint16_t*)(m_buffer);
    if (packetSize > 1024 && m_outputPacketId == 1) {
        return readTibia12Header();
    }

    if (packetSize == 0 || packetSize + 16 > BUFFER_SIZE) {
#ifdef PROXY_DEBUG
        std::clog << "[Session " << m_id << "] onHeader invalid packet size: " << packetSize << std::endl;
#endif
        return terminate();
    }

    boost::asio::async_read(m_socket, boost::asio::buffer(m_buffer + 2, packetSize),
                            std::bind(&Session::onBody, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
}

void Session::onBody(const boost::system::error_code& ec, std::size_t bytes_transferred)
{
    if (ec) {
#ifdef PROXY_DEBUG
        std::clog << "[Session " << m_id << "] onBody error: " << ec.message() << std::endl;
#endif
        return terminate();
    }

    auto packet = std::make_shared<ProxyPacket>(m_buffer, m_buffer + bytes_transferred + 2);
    onPacket(packet);

    readHeader();
}

void Session::onPacket(const ProxyPacketPtr& packet)
{
    if (!packet || packet->empty() || packet->size() + 14 > BUFFER_SIZE) {
#ifdef PROXY_DEBUG
        std::clog << "[Session " << m_id << "] onPacket error: missing packet or wrong size" << std::endl;
#endif
        return terminate();
    }

    auto self(shared_from_this());
    boost::asio::post(m_io, [this, self, packet] {
        uint32_t packetId = m_outputPacketId++;
        auto newPacket = std::make_shared<ProxyPacket>(packet->size() + 14);

        *(uint16_t*)(&(newPacket->data()[0])) = (uint16_t)packet->size() + 12;
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

void Session::onSent(const boost::system::error_code& ec, std::size_t bytes_transferred)
{
    if (ec) {
#ifdef PROXY_DEBUG
        std::clog << "[Session " << m_id << "] onSent error: " << ec.message() << std::endl;
#endif
        return terminate();
    }

    m_inputPacketId += 1;
    m_sendQueue.erase(m_sendQueue.begin());
    if (!m_sendQueue.empty() && m_sendQueue.begin()->first == m_inputPacketId) {
        boost::asio::async_write(m_socket, boost::asio::buffer(m_sendQueue.begin()->second->data(), m_sendQueue.begin()->second->size()),
                                 std::bind(&Session::onSent, shared_from_this(), std::placeholders::_1, std::placeholders::_2));
    }
}