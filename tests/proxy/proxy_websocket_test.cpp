// End-to-end test of the proxy session protocol tunnelled over a WebSocket:
// a fake proxy server built on ix::WebSocketServer answers pings, accepts a
// session and sends back a game packet split across WebSocket messages and
// coalesced with another packet, which exercises the stream reassembly.
#include <gtest/gtest.h>

#include <framework/proxy/proxy.h>

#include <ixwebsocket/IXWebSocketServer.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {
    std::string frame(const std::string& body)
    {
        std::string packet(2, '\0');
        const auto size = static_cast<uint16_t>(body.size());
        std::memcpy(packet.data(), &size, 2);
        return packet + body;
    }

    std::string u32(const uint32_t value)
    {
        std::string out(4, '\0');
        std::memcpy(out.data(), &value, 4);
        return out;
    }

    class FakeProxyServer
    {
    public:
        bool start()
        {
            for (int port = 38700; port < 38720; ++port) {
                m_server = std::make_unique<ix::WebSocketServer>(port, "127.0.0.1");
                m_server->setOnClientMessageCallback([this](std::shared_ptr<ix::ConnectionState>, ix::WebSocket& ws, const ix::WebSocketMessagePtr& msg) {
                    if (msg->type == ix::WebSocketMessageType::Message)
                        onData(ws, msg->str);
                });
                if (m_server->listen().first) {
                    m_server->start();
                    m_port = port;
                    return true;
                }
            }
            return false;
        }

        void stop() { if (m_server) m_server->stop(); }
        int port() const { return m_port; }

        uint32_t sessionPort()
        {
            std::lock_guard lock(m_mutex);
            return m_sessionPort;
        }

    private:
        void onData(ix::WebSocket& ws, const std::string& data)
        {
            std::lock_guard lock(m_mutex);
            m_buffer += data;
            while (m_buffer.size() >= 2) {
                uint16_t size;
                std::memcpy(&size, m_buffer.data(), 2);
                if (m_buffer.size() < 2u + size)
                    break;
                const std::string body = m_buffer.substr(2, size);
                m_buffer.erase(0, 2u + size);
                onPacket(ws, body);
            }
        }

        void onPacket(ix::WebSocket& ws, const std::string& body)
        {
            uint32_t sessionId, packetId;
            std::memcpy(&sessionId, body.data(), 4);
            std::memcpy(&packetId, body.data() + 4, 4);

            if (sessionId == 0) { // ping: echo it back
                ws.sendBinary(frame(body));
                return;
            }
            if (packetId == 0) { // open session
                std::memcpy(&m_sessionPort, body.data() + 8, 4);

                const std::string payload = "hello";
                std::string gamePacket(2, '\0');
                const auto len = static_cast<uint16_t>(payload.size());
                std::memcpy(gamePacket.data(), &len, 2);
                gamePacket += payload;

                const std::string reply = frame(u32(sessionId) + u32(1) + u32(0) + gamePacket);
                const std::string ping = frame(u32(0) + u32(0) + u32(0) + u32(0));

                // first half of the packet alone, the rest coalesced with a ping
                ws.sendBinary(reply.substr(0, 7));
                ws.sendBinary(reply.substr(7) + ping);
            }
        }

        std::unique_ptr<ix::WebSocketServer> m_server;
        int m_port = 0;
        std::mutex m_mutex;
        std::string m_buffer;
        uint32_t m_sessionPort = 0;
    };
}

TEST(ProxyWebSocketTest, SessionPacketsAreTunnelledAndReassembled)
{
    FakeProxyServer server;
    ASSERT_TRUE(server.start());

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<uint8_t> received;
    bool disconnected = false;

    ProxyManager manager;
    manager.init();
    manager.addProxy("ws://127.0.0.1:" + std::to_string(server.port()) + "/", 12345, 0);

    const auto sessionId = manager.addSession(7172,
        [&](ProxyPacketPtr packet) {
            std::lock_guard lock(mutex);
            received = *packet;
            cv.notify_all();
        },
        [&](std::error_code) {
            std::lock_guard lock(mutex);
            disconnected = true;
            cv.notify_all();
        });
    ASSERT_NE(sessionId, 0u);

    {
        std::unique_lock lock(mutex);
        const bool gotPacket = cv.wait_for(lock, std::chrono::seconds(15), [&] { return !received.empty() || disconnected; });
        ASSERT_TRUE(gotPacket) << "no packet received through the WebSocket proxy";
        ASSERT_FALSE(disconnected);
    }

    // the session receives the raw game packet: 2 byte length + payload
    ASSERT_EQ(received.size(), 7u);
    uint16_t len;
    std::memcpy(&len, received.data(), 2);
    EXPECT_EQ(len, 5);
    EXPECT_EQ(std::string(received.begin() + 2, received.end()), "hello");

    EXPECT_EQ(server.sessionPort(), 7172u);
    EXPECT_GT(manager.getProxies().size(), 0u);
    EXPECT_TRUE(manager.getProxies().contains("ws://127.0.0.1:" + std::to_string(server.port()) + "/"));

    manager.removeSession(sessionId);
    manager.terminate();
    server.stop();
}
