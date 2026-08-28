#include <gtest/gtest.h>

#include <framework/proxy/proxy.h>
#include <framework/proxy/proxy_client.h>

TEST(ProxyTest, DetectsWebSocketUrls)
{
    EXPECT_TRUE(Proxy::isWebSocketUrl("ws://proxy.example.com"));
    EXPECT_TRUE(Proxy::isWebSocketUrl("wss://proxy.example.com:8443/otc"));
    EXPECT_TRUE(Proxy::isWebSocketUrl("WSS://proxy.example.com"));

    EXPECT_FALSE(Proxy::isWebSocketUrl("proxy.example.com"));
    EXPECT_FALSE(Proxy::isWebSocketUrl("127.0.0.1"));
    EXPECT_FALSE(Proxy::isWebSocketUrl("http://proxy.example.com"));
    EXPECT_FALSE(Proxy::isWebSocketUrl("ws:/broken"));
    EXPECT_FALSE(Proxy::isWebSocketUrl(""));
}

TEST(ProxyTest, WebSocketProxyIsKeyedByUrlAndIgnoresPort)
{
    ProxyManager manager;

    manager.addProxy("wss://proxy.example.com/otc", 443, 10);
    manager.addProxy("wss://proxy.example.com/otc", 0, 10); // duplicate, different port argument
    manager.addProxy("proxy.example.com", 7171, 0);

    const auto proxies = manager.getProxies();
    ASSERT_EQ(proxies.size(), 2u);
    EXPECT_TRUE(proxies.contains("wss://proxy.example.com/otc"));
    EXPECT_TRUE(proxies.contains("proxy.example.com:7171"));

    manager.removeProxy("wss://proxy.example.com/otc", 8443); // port is ignored for urls
    EXPECT_EQ(manager.getProxies().size(), 1u);
    EXPECT_TRUE(manager.getProxies().contains("proxy.example.com:7171"));

    manager.removeProxy("proxy.example.com", 7171);
    EXPECT_TRUE(manager.getProxies().empty());
    EXPECT_FALSE(manager.isActive());
}

TEST(ProxyTest, ProxiesStatusExposesTransportAndConfig)
{
    ProxyManager manager;

    manager.addProxy("wss://proxy.example.com/otc", 0, 7);
    manager.addProxy("proxy.example.com", 7171, 3);

    const auto status = manager.getProxiesStatus();
    ASSERT_EQ(status.size(), 2u);

    EXPECT_EQ(status[0].host, "wss://proxy.example.com/otc");
    EXPECT_EQ(status[0].port, 0);
    EXPECT_TRUE(status[0].webSocket);
    EXPECT_EQ(status[0].priority, 7);
    EXPECT_FALSE(status[0].connected);
    EXPECT_EQ(status[0].sessions, 0);

    EXPECT_EQ(status[1].host, "proxy.example.com");
    EXPECT_EQ(status[1].port, 7171);
    EXPECT_FALSE(status[1].webSocket);
    EXPECT_EQ(status[1].priority, 3);
}
