#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x501
#endif

#include <cstdint>
#include <boost/asio.hpp>
#ifndef __EMSCRIPTEN__
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket/ssl.hpp>
#endif
#include <memory>
#include <string>
#include <list>
#include <thread>
#include <functional>
#include <map>
#include <chrono>
#include <chrono>
#include <vector>
#include <random>
#include <set>

using ProxyPacket = std::vector<uint8_t>;
using ProxyPacketPtr = std::shared_ptr<ProxyPacket>;

class Session;
using SessionPtr = std::shared_ptr<Session>;

class Proxy : public std::enable_shared_from_this<Proxy> {
    static constexpr int CHECK_INTERVAL = 2500; // also timeout for ping
    static constexpr int BUFFER_SIZE = 65535;
    enum ProxyState {
        STATE_NOT_CONNECTED,
        STATE_CONNECTING,
        STATE_CONNECTING_WAIT_FOR_PING,
        STATE_CONNECTED
    };
public:
    // proxy reached over a plain TCP socket
    Proxy(boost::asio::io_context& io, const std::string& host, uint16_t port, int priority)
        : m_io(io), m_timer(io), m_socket(io), m_resolver(io), m_state(STATE_NOT_CONNECTED)
    {
        m_host = host;
        m_port = port;
        m_priority = priority;
    }

    // proxy reached over a WebSocket (ws:// or wss:// url). The proxy session
    // protocol is carried unchanged inside binary WebSocket messages, so the
    // server side only needs to unwrap WebSocket -> TCP (websockify, nginx, ...)
    // in front of a regular proxy server.
    Proxy(boost::asio::io_context& io, const std::string& url, int priority)
        : m_io(io), m_timer(io), m_socket(io), m_resolver(io), m_state(STATE_NOT_CONNECTED)
    {
        m_host = url;
        m_port = 0;
        m_priority = priority;
        m_webSocket = true;
    }

    static bool isWebSocketUrl(const std::string& host);

    // thread-safe
    void start();
    void terminate();

    uint32_t getPing() { return m_ping + m_priority; }
    uint32_t getRealPing() { return m_ping; }
    uint32_t getPriority() { return m_priority; }
    bool isConnected() { return m_state == STATE_CONNECTED; }
    bool isWebSocket() const { return m_webSocket; }
    std::string getHost() { return m_host; } // full url for WebSocket proxies
    uint16_t getPort() { return m_port; } // always 0 for WebSocket proxies
    std::string getDebugInfo();
    bool isActive() { return m_sessions > 0; }
    int getSessionsCount() { return m_sessions; }
    int getConnectionsCount() { return m_connections; }
    int getPacketsSent() { return m_packetsSent; }
    int getPacketsReceived() { return m_packetsRecived; }
    int getBytesSent() { return m_bytesSent; }
    int getBytesReceived() { return m_bytesRecived; }
    std::string getResolvedIp() { return m_resolvedIp; }

    // not thread-safe
    void addSession(uint32_t id, int m_port);
    void removeSession(uint32_t id);

    void send(const ProxyPacketPtr& packet);

private:
    void check(const boost::system::error_code& ec = boost::system::error_code());
    void connect();
    void connectSocket();
    void connectWebSocket();
    void disconnect();

    void ping();
    void onPing(uint32_t packetId);

    // plain socket transport
    void readHeader();
    void onHeader(const boost::system::error_code& ec, std::size_t bytes_transferred);
    void onPacket(const boost::system::error_code& ec, std::size_t bytes_transferred);
    void onSent(const boost::system::error_code& ec, std::size_t bytes_transferred);

    // WebSocket transport (Boost.Beast, runs on m_io like the socket transport)
#ifndef __EMSCRIPTEN__
    using WebSocketStream = boost::beast::websocket::stream<boost::beast::tcp_stream>;
    using SecureWebSocketStream = boost::beast::websocket::stream<boost::beast::ssl_stream<boost::beast::tcp_stream>>;

    // everything a single WebSocket connection owns; kept alive by the pending
    // handlers (they capture it) so a stream is never destroyed with operations
    // in flight, and the ssl context always outlives the ssl stream
    struct WebSocketConnection {
        std::shared_ptr<boost::asio::ssl::context> sslContext;
        std::unique_ptr<WebSocketStream> plain;
        std::unique_ptr<SecureWebSocketStream> secure;
        boost::beast::flat_buffer readBuffer;

        template<typename Fn>
        void withStream(Fn&& fn)
        {
            if (secure)
                fn(*secure);
            else
                fn(*plain);
        }
    };
    using WebSocketConnectionPtr = std::shared_ptr<WebSocketConnection>;

    void handshakeWebSocket(uint32_t generation, const WebSocketConnectionPtr& conn);
    void readWebSocket(uint32_t generation, const WebSocketConnectionPtr& conn);
    bool onWebSocketData(const std::string& data);
#endif
    void writeNext();

    // handles a complete proxy packet stored in m_buffer, returns false when the proxy got disconnected
    bool handlePacket(std::size_t size);

    boost::asio::io_context& m_io;
    boost::asio::steady_timer m_timer;
    boost::asio::ip::tcp::socket m_socket;
    boost::asio::ip::tcp::resolver m_resolver;

#ifndef __EMSCRIPTEN__
    WebSocketConnectionPtr m_ws;
    std::string m_wsHostHeader;
    std::string m_wsTarget;
    std::string m_wsRecvBuffer;
#endif
    // bumped on every connect/disconnect; async handlers capture it and ignore
    // completions that belong to a previous connection, otherwise a dying
    // read handler can tear down the next connection attempt over and over
    uint32_t m_generation = 0;
    bool m_webSocket = false;

    ProxyState m_state;

    std::string m_host;
    uint16_t m_port;

    bool m_terminated = false;

    uint8_t m_buffer[BUFFER_SIZE];
    std::list<ProxyPacketPtr> m_sendQueue;

    bool m_waitingForPing;
    uint32_t m_ping = 0;
    int m_priority = 0;
    std::chrono::time_point<std::chrono::high_resolution_clock> m_lastPingSent;

    int m_packetsRecived = 0;
    int m_packetsSent = 0;
    int m_bytesRecived = 0;
    int m_bytesSent = 0;
    int m_connections = 0;
    int m_sessions = 0;
    std::string m_resolvedIp;
};

using ProxyPtr = std::shared_ptr<Proxy>;

class Session : public std::enable_shared_from_this<Session> {
    static constexpr int CHECK_INTERVAL = 500;
    static constexpr int BUFFER_SIZE = 65535;
    static constexpr int TIMEOUT = 30000;
public:
    Session(boost::asio::io_context& io, boost::asio::ip::tcp::socket socket, int port)
        : m_io(io), m_timer(io), m_socket(std::move(socket))
    {
        m_id = (std::chrono::high_resolution_clock::now().time_since_epoch().count()) & 0xFFFFFFFF;
        if (m_id == 0) m_id = 1;
        m_port = port;
        m_useSocket = true;
    }

    Session(boost::asio::io_context& io, int port, std::function<void(ProxyPacketPtr)> recvCallback, std::function<void(boost::system::error_code)> disconnectCallback)
        : m_io(io), m_timer(io), m_socket(io)
    {
        m_id = (std::chrono::high_resolution_clock::now().time_since_epoch().count()) & 0xFFFFFFFF;
        if (m_id == 0) m_id = 1;
        m_port = port;
        m_useSocket = false;
        m_recvCallback = recvCallback;
        m_disconnectCallback = disconnectCallback;
    }

    // thread safe
    uint32_t getId() { return m_id; }
    void start(int maxConnections = 3);
    void terminate(boost::system::error_code ec = boost::asio::error::eof);
    void onPacket(const ProxyPacketPtr& packet);

    // not thread safe
    void onProxyPacket(uint32_t packetId, uint32_t lastRecivedPacketId, const ProxyPacketPtr& packet);

private:
    void check(const boost::system::error_code& ec);
    void selectProxies();

    void readTibia12Header();
    void readHeader();
    void onHeader(const boost::system::error_code& ec, std::size_t bytes_transferred);
    void onBody(const boost::system::error_code& ec, std::size_t bytes_transferred);
    void onSent(const boost::system::error_code& ec, std::size_t bytes_transferred);

    boost::asio::io_context& m_io;
    boost::asio::steady_timer m_timer;
    boost::asio::ip::tcp::socket m_socket;

    uint32_t m_id;
    uint16_t m_port;
    bool m_useSocket;
    bool m_terminated = false;

    std::function<void(ProxyPacketPtr)> m_recvCallback = nullptr;
    std::function<void(boost::system::error_code)> m_disconnectCallback = nullptr;

    std::set<ProxyPtr> m_proxies;

    int m_maxConnections;

    uint8_t m_buffer[BUFFER_SIZE];
    std::map<uint32_t, ProxyPacketPtr> m_sendQueue;
    std::map<uint32_t, ProxyPacketPtr> m_proxySendQueue;

    uint32_t m_inputPacketId = 1;
    uint32_t m_outputPacketId = 1;

    std::chrono::time_point<std::chrono::high_resolution_clock> m_lastPacket;
};