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

#include "proxy.h"
#include "proxy_client.h"

ProxyManager g_proxy;

void ProxyManager::init()
{
    if (m_working)
        return;
    m_working = true;
    m_thread = std::thread([&] {
        m_io.run();
    });
}

void ProxyManager::terminate()
{
    if (!m_working)
        return;
    m_working = false;
    clear();
    m_guard.reset();
    if (!m_thread.joinable()) {
        stdext::millisleep(100);
        m_io.stop();
    }
    m_thread.join();
}

void ProxyManager::clear()
{
    for (auto& session_weak : m_sessions) {
        if (const auto session = session_weak.lock()) {
            session->terminate();
        }
    }
    m_sessions.clear();
    for (auto& proxy_weak : m_proxies) {
        if (const auto proxy = proxy_weak.lock()) {
            proxy->terminate();
        }
    }
    m_proxies.clear();
}

bool ProxyManager::isActive()
{
    return m_proxies.size() > 0;
}

namespace {
    std::string proxyKey(const ProxyPtr& proxy)
    {
        if (proxy->isWebSocket())
            return proxy->getHost();
        return proxy->getHost() + ":" + std::to_string(proxy->getPort());
    }
}

void ProxyManager::addProxy(const std::string& host, uint16_t port, int priority)
{
    // a ws:// or wss:// url carries its own port, the port argument is ignored
    const bool webSocket = Proxy::isWebSocketUrl(host);
    if (webSocket)
        port = 0;

    for (auto& proxy_weak : m_proxies) {
        if (const auto proxy = proxy_weak.lock()) {
            if (proxy->getHost() == host && proxy->getPort() == port) {
                return; // already exist
            }
        }
    }

    const auto proxy = webSocket
        ? std::make_shared<Proxy>(m_io, host, priority)
        : std::make_shared<Proxy>(m_io, host, port, priority);
    proxy->start();
    m_proxies.push_back(proxy);
}

void ProxyManager::removeProxy(const std::string& host, uint16_t port)
{
    if (Proxy::isWebSocketUrl(host))
        port = 0;

    for (auto it = m_proxies.begin(); it != m_proxies.end(); ) {
        if (const auto proxy = it->lock()) {
            if (proxy->getHost() == host && proxy->getPort() == port) {
                proxy->terminate();
                it = m_proxies.erase(it);
            } else {
                ++it;
            }
            continue;
        }
        it = m_proxies.erase(it);
    }
}

uint32_t ProxyManager::addSession(uint16_t port, std::function<void(ProxyPacketPtr)> recvCallback, std::function<void(std::error_code)> disconnectCallback)
{
    assert(recvCallback && disconnectCallback);
    const auto session = std::make_shared<Session>(m_io, port, recvCallback, disconnectCallback);
    session->start(m_maxActiveProxies);
    m_sessions.push_back(session);
    return session->getId();
}

void ProxyManager::removeSession(const uint32_t sessionId)
{
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ) {
        if (const auto session = it->lock()) {
            if (session->getId() == sessionId) {
                session->terminate();
                it = m_sessions.erase(it);
            } else {
                ++it;
            }
            continue;
        }
        it = m_sessions.erase(it);
    }
}

void ProxyManager::send(const uint32_t sessionId, ProxyPacketPtr packet)
{
    SessionPtr session = nullptr;
    for (auto& session_weak : m_sessions) {
        if (const auto tsession = session_weak.lock()) {
            if (tsession->getId() == sessionId) {
                session = tsession;
                break;
            }
        }
    }

    if (!session)
        return;

    session->onPacket(packet);
}

std::map<std::string, uint32_t> ProxyManager::getProxies()
{
    std::map<std::string, uint32_t> ret;
    for (auto& proxy_weak : m_proxies) {
        if (const auto proxy = proxy_weak.lock()) {
            ret[proxyKey(proxy)] = proxy->getRealPing();
        }
    }
    return ret;
}

std::vector<ProxyStatus> ProxyManager::getProxiesStatus()
{
    // m_proxies belongs to the calling thread but the Proxy fields are mutated
    // on the proxy io thread only, so the live proxies are collected here and
    // their fields are read on m_io: the diagnostics UI polls this every second
    // and must not race the io thread
    std::vector<ProxyPtr> proxies;
    for (auto& proxy_weak : m_proxies) {
        if (const auto proxy = proxy_weak.lock())
            proxies.push_back(proxy);
    }

    auto snapshot = [proxies] {
        std::vector<ProxyStatus> ret;
        ret.reserve(proxies.size());
        for (const auto& proxy : proxies) {
            ret.push_back(ProxyStatus{
                .host = proxy->getHost(),
                .port = proxy->getPort(),
                .webSocket = proxy->isWebSocket(),
                .connected = proxy->isConnected(),
                .ping = proxy->getPing(),
                .realPing = proxy->getRealPing(),
                .priority = static_cast<int>(proxy->getPriority()),
                .sessions = proxy->getSessionsCount(),
                .connections = proxy->getConnectionsCount(),
                .packetsSent = proxy->getPacketsSent(),
                .packetsReceived = proxy->getPacketsReceived(),
                .bytesSent = proxy->getBytesSent(),
                .bytesReceived = proxy->getBytesReceived(),
                .resolvedIp = proxy->getResolvedIp(),
            });
        }
        return ret;
    };

    if (!m_working)
        return snapshot(); // no io thread running, nothing can race

    auto promise = std::make_shared<std::promise<std::vector<ProxyStatus>>>();
    auto future = promise->get_future();
    post(m_io, [promise, snapshot] {
        promise->set_value(snapshot());
    });
    // never block the caller for long, a stalled io thread just yields an empty snapshot
    if (future.wait_for(std::chrono::milliseconds(250)) != std::future_status::ready)
        return {};
    return future.get();
}

std::map<std::string, std::string> ProxyManager::getProxiesDebugInfo()
{
    std::map<std::string, std::string> ret;
    for (auto& proxy_weak : m_proxies) {
        if (const auto proxy = proxy_weak.lock()) {
            ret[proxyKey(proxy)] = proxy->getDebugInfo();
        }
    }
    return ret;
}

int ProxyManager::getPing()
{
    uint32_t ret = 0;
    for (auto& proxy_weak : m_proxies) {
        if (const auto proxy = proxy_weak.lock()) {
            if ((proxy->getRealPing() < ret || ret == 0) && proxy->getRealPing() > 0)
                ret = proxy->getRealPing();
        }
    }
    return ret;
}