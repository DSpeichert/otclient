-- OTShield proxy discovery for servers hosted on otshosting.pl.
--
-- Activated by registering the server's subdomain in init.lua:
--   Services.otshosting = { subdomain = "myserver" }  -- myserver.ots.ovh
--
-- The module fetches https://otshosting.pl/api/proxy/<subdomain> on startup
-- and every `refreshInterval` seconds (default 300), also while connected, and
-- applies the list gracefully: new proxies are added before stale ones are
-- removed one by one, so active sessions keep flowing over the remaining
-- proxies and are never interrupted by a clear-and-readd.
--
-- See docs/proxy.md for the endpoint contract and all config keys.
OTSHosting = {}

local DEFAULT_URL = 'https://otshosting.pl/api/proxy/'
local DEFAULT_REFRESH_INTERVAL = 300 -- seconds; the API caches for up to 60s

local config = nil
local active = false
local managed = {} -- key -> { host, port, priority }
local lastRefresh = { at = nil, ok = nil, message = tr('never') }
local nextRefreshAt = nil
local refreshPending = false
local refreshEvent = nil
local updateEvent = nil
local window = nil
local topMenuButton = nil

-- one entry of the discovery response: { host = "name:port" | "wss://...", priority = n }
local function parseEntry(entry)
  if type(entry) ~= 'table' or type(entry.host) ~= 'string' or entry.host == '' then
    return nil, 'malformed entry'
  end

  local host = entry.host:trim()
  local priority = tonumber(entry.priority) or 0

  if host:lower():match('^wss?://') then
    return { host = host, port = 0, priority = priority, key = host }
  end

  local name, port = host:match('^(.+):(%d+)$')
  if not name then
    return nil, string.format('missing port in "%s"', host)
  end
  return { host = name, port = tonumber(port), priority = priority, key = name .. ':' .. port }
end

-- diff the desired list against what this module manages: add new entries
-- first, then drop stale ones one by one; untouched entries keep their
-- connection so sessions are never disturbed by a refresh
local function applyProxyList(entries)
  local desired = {}
  for _, entry in ipairs(entries) do
    local parsed, err = parseEntry(entry)
    if parsed then
      desired[parsed.key] = parsed
    else
      g_logger.warning(string.format('[otshosting] ignoring proxy entry: %s', err))
    end
  end

  local added, updated, removed = 0, 0, 0

  for key, entry in pairs(desired) do
    local current = managed[key]
    if not current then
      g_proxy.addProxy(entry.host, entry.port, entry.priority)
      managed[key] = entry
      added = added + 1
    elseif current.priority ~= entry.priority then
      g_proxy.removeProxy(current.host, current.port)
      g_proxy.addProxy(entry.host, entry.port, entry.priority)
      managed[key] = entry
      updated = updated + 1
    end
  end

  for key, current in pairs(managed) do
    if not desired[key] then
      g_proxy.removeProxy(current.host, current.port)
      managed[key] = nil
      removed = removed + 1
    end
  end

  if added > 0 or updated > 0 or removed > 0 then
    g_logger.info(string.format('[otshosting] proxy list updated: %d added, %d updated, %d removed, %d total', added,
                                updated, removed, table.size(managed)))
  end
end

local function refresh(reason)
  if not active or refreshPending then
    return
  end
  refreshPending = true

  local url = (config.url or DEFAULT_URL) .. config.subdomain
  HTTP.getJSON(url, function(data, err)
    refreshPending = false
    lastRefresh.at = os.time()

    if err then
      -- keep the current proxy list on errors, a temporarily unreachable
      -- discovery endpoint must not tear down working tunnels
      lastRefresh.ok = false
      lastRefresh.message = err
      g_logger.warning(string.format('[otshosting] proxy discovery failed (%s): %s', reason, err))
      return
    end

    if type(data) ~= 'table' or type(data.proxies) ~= 'table' then
      lastRefresh.ok = false
      lastRefresh.message = tr('unexpected response')
      g_logger.warning(string.format('[otshosting] unexpected proxy discovery response (%s)', reason))
      return
    end

    applyProxyList(data.proxies)
    lastRefresh.ok = true
    lastRefresh.message = string.format('%d %s', #data.proxies, tr('proxies'))
  end)
end

local function formatBytes(bytes)
  if bytes >= 1048576 then
    return string.format('%.1f MB', bytes / 1048576)
  elseif bytes >= 1024 then
    return string.format('%.1f KB', bytes / 1024)
  end
  return string.format('%d B', bytes)
end

local function updateWindow()
  if not window or not window:isVisible() then
    return
  end

  window:recursiveGetChildById('infoServer'):setText(string.format('%s: %s', tr('Server'), config.subdomain))

  local refreshText
  if lastRefresh.at then
    refreshText = string.format('%s: %s (%s)', tr('Last refresh'), os.date('%H:%M:%S', lastRefresh.at),
                                lastRefresh.message)
  else
    refreshText = string.format('%s: %s', tr('Last refresh'), lastRefresh.message)
  end
  local nextIn = nextRefreshAt and math.max(0, nextRefreshAt - os.time()) or 0
  window:recursiveGetChildById('infoRefresh'):setText(string.format('%s, %s %ds', refreshText, tr('next in'), nextIn))

  local statusWidget = window:recursiveGetChildById('infoRefresh')
  statusWidget:setColor(lastRefresh.ok == false and '#e08080' or '#c0c0c0')

  local list = window:recursiveGetChildById('proxyList')
  list:destroyChildren()

  local proxies = g_proxy.getProxiesStatus()
  table.sort(proxies, function(a, b)
    return a.ping < b.ping
  end)

  for _, proxy in ipairs(proxies) do
    local row = g_ui.createWidget('OtsHostingProxyRow', list)
    row:getChildById('name'):setText(proxy.webSocket and proxy.host or
                                       string.format('%s:%d', proxy.host, proxy.port))
    row:getChildById('name'):setTooltip(proxy.resolvedIp ~= '' and
                                          string.format('%s: %s', tr('Resolved'), proxy.resolvedIp) or '')
    row:getChildById('transport'):setText(proxy.webSocket and 'websocket' or 'tcp')

    local state = row:getChildById('state')
    state:setText(proxy.connected and tr('connected') or tr('connecting'))
    state:setColor(proxy.connected and '#80e080' or '#e0c080')

    row:getChildById('ping'):setText(proxy.connected and string.format('%d ms', proxy.realPing) or '-')
    row:getChildById('priority'):setText(tostring(proxy.priority))
    row:getChildById('sessions'):setText(tostring(proxy.sessions))
    row:getChildById('traffic'):setText(string.format('%s in / %s out', formatBytes(proxy.bytesReceived),
                                                      formatBytes(proxy.bytesSent)))
  end

  local empty = window:recursiveGetChildById('emptyLabel')
  empty:setVisible(#proxies == 0)
end

function OTSHosting.toggle()
  if not window then
    return
  end
  if window:isVisible() then
    window:hide()
  else
    window:show()
    window:raise()
    window:focus()
    updateWindow()
  end
  if topMenuButton then
    topMenuButton:setOn(window:isVisible())
  end
end

function OTSHosting.refreshNow()
  refresh('manual')
end

function OTSHosting.init()
  config = Services and Services.otshosting or nil
  if not config or type(config.subdomain) ~= 'string' or config.subdomain == '' then
    g_logger.debug('[otshosting] no subdomain registered in Services.otshosting, module stays idle')
    return
  end
  active = true

  local interval = tonumber(config.refreshInterval) or DEFAULT_REFRESH_INTERVAL

  -- 1) initial load + 2) periodic refresh, also while connected
  refresh('startup')
  nextRefreshAt = os.time() + interval
  refreshEvent = cycleEvent(function()
    nextRefreshAt = os.time() + interval
    refresh('periodic')
  end, interval * 1000)

  -- 3) the proxy makes the server address moot, hide it on the login screen
  if config.hideServerFields ~= false and EnterGame and EnterGame.hideServerFields then
    EnterGame.hideServerFields(config.host, config.port, config.version)
  end

  -- 4) diagnostics window
  window = g_ui.displayUI('otshosting')
  window:hide()
  window:recursiveGetChildById('refreshButton').onClick = function()
    OTSHosting.refreshNow()
  end
  window:recursiveGetChildById('closeButton').onClick = function()
    OTSHosting.toggle()
  end
  window.onVisibilityChange = function(_, visible)
    if topMenuButton then
      topMenuButton:setOn(visible)
    end
  end

  topMenuButton = modules.client_topmenu.addRightToggleButton('otshostingButton',
                                                              tr('Proxy Diagnostics') .. ' (Ctrl+Shift+P)',
                                                              '/images/topbuttons/debug', function()
    OTSHosting.toggle()
  end)

  g_keyboard.bindKeyDown('Ctrl+Shift+P', function()
    OTSHosting.toggle()
  end)

  updateEvent = cycleEvent(updateWindow, 1000)
end

function OTSHosting.terminate()
  if not active then
    return
  end
  active = false

  g_keyboard.unbindKeyDown('Ctrl+Shift+P')

  if refreshEvent then
    removeEvent(refreshEvent)
    refreshEvent = nil
  end
  if updateEvent then
    removeEvent(updateEvent)
    updateEvent = nil
  end

  if topMenuButton then
    topMenuButton:destroy()
    topMenuButton = nil
  end
  if window then
    window:destroy()
    window = nil
  end

  for _, entry in pairs(managed) do
    g_proxy.removeProxy(entry.host, entry.port)
  end
  managed = {}
end
