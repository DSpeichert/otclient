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
-- In the hosted web client (Services.otshosting.web = true, injected by
-- browser/shell.html through init.lua) the module additionally consumes the
-- `web` object of the discovery response: it locks the login screen to the
-- configured server, keeps only wss:// proxies (TCP cannot work in a browser)
-- and downloads the server's classic .dat/.spr assets when needed.
--
-- See docs/proxy.md for the endpoint contract and all config keys.
OTSHosting = Controller:new()

local DEFAULT_URL = 'https://otshosting.pl/api/proxy/'
local DEFAULT_REFRESH_INTERVAL = 300 -- seconds; the API caches for up to 60s

local config = nil
local active = false
local webMode = false -- hosted web client: wss-only proxies + managed login screen
local appliedWebSignature = nil
local assetInstallToken = 0 -- invalidates an in-flight install when config changes
local managed = {} -- key -> { host, port, priority }
local lastRefresh = { at = nil, ok = nil, message = tr('never') }
local nextRefreshAt = nil
local refreshPending = false
local window = nil
local topMenuButton = nil
local gameButton = nil

-- one entry of the discovery response: { host = "name:port" | "wss://...", priority = n }
-- returns entry | nil, error-message | nil, nil (silently skipped)
local function parseEntry(entry)
    if type(entry) ~= 'table' or type(entry.host) ~= 'string' or entry.host == '' then
        return nil, 'malformed entry'
    end

    local host = entry.host:trim()
    local priority = tonumber(entry.priority) or 0

    if host:lower():match('^wss?://') then
        return { host = host, port = 0, priority = priority, key = host }
    end

    if webMode then
        return nil -- TCP proxies cannot work in a browser, skip without noise
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
        elseif err then
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

local function setLoginLock(message)
    if EnterGame and EnterGame.setManagedLock then
        EnterGame.setManagedLock(message)
    end
end

-- installs the server's classic .dat/.spr files listed in web.assets.things
-- into /data/things/<version>/ (the PhysFS write dir overlay, persisted to
-- IndexedDB by the browser); a marker file with the installed hashes makes the
-- download one-time per browser until the server operator uploads new files
local function installClassicAssets(version, files, token)
    local markerPath = string.format('/data/things/%d/otshosting-assets.json', version)
    local installed = {}
    local index = 0
    local count = #files

    local function fail(message)
        g_logger.error(string.format('[otshosting] asset install failed: %s', message))
        setLoginLock(string.format('%s: %s', tr('Game asset download failed'), message))
    end

    local function step()
        if token ~= assetInstallToken then
            return -- a newer configuration superseded this install
        end

        index = index + 1
        local entry = files[index]
        if not entry then
            if not g_resources.writeFileContents(markerPath, json.encode(installed)) then
                return fail(string.format('cannot write %s', markerPath))
            end
            g_logger.info(string.format('[otshosting] installed %d game asset(s) for version %d', count, version))
            setLoginLock(nil)
            return
        end

        local label = string.format('%s (%d/%d): %s', tr('Downloading game assets'), index, count, entry.file)
        setLoginLock(label)

        local downloadName = string.format('otshosting_%d_%s', version, entry.file)
        HTTP.download(entry.url, downloadName, function(path, checksum, err)
            if token ~= assetInstallToken then
                return
            end
            if err then
                return fail(string.format('%s: %s', entry.file, err))
            end

            local ok, contents = pcall(g_resources.readFileContents, '/downloads/' .. downloadName)
            if not ok or not contents or #contents == 0 then
                return fail(string.format('%s: downloaded file is unreadable', entry.file))
            end
            if type(entry.sha256) == 'string' and entry.sha256 ~= '' and g_crypt.sha256(contents) ~= entry.sha256 then
                return fail(string.format('%s: checksum mismatch', entry.file))
            end

            g_resources.makeDir('/data/things')
            g_resources.makeDir(string.format('/data/things/%d', version))
            if not g_resources.writeFileContents(string.format('/data/things/%d/%s', version, entry.file), contents) then
                return fail(string.format('%s: cannot write file', entry.file))
            end

            installed[entry.file] = entry.sha256 or ''
            step()
        end, function(progress, speed)
            if token == assetInstallToken then
                setLoginLock(string.format('%s (%d%%)', label, progress))
            end
        end)
    end

    step()
end

-- skips the download when the marker written by installClassicAssets already
-- matches every desired file hash
local function ensureClassicAssets(web)
    local version = tonumber(web.clientVersion)
    local things = web.assets and web.assets.things
    if not version or type(things) ~= 'table' or #things == 0 then
        setLoginLock(nil)
        return
    end

    local files = {}
    for _, entry in ipairs(things) do
        if type(entry) ~= 'table' or type(entry.file) ~= 'string' or type(entry.url) ~= 'string' then
            return setLoginLock(tr('Invalid game asset configuration'))
        end
        -- plain file names only, they are joined into a data path below
        if entry.file:match('[/\\]') then
            return setLoginLock(tr('Invalid game asset configuration'))
        end
        table.insert(files, entry)
    end

    assetInstallToken = assetInstallToken + 1

    local markerPath = string.format('/data/things/%d/otshosting-assets.json', version)
    if g_resources.fileExists(markerPath) then
        local ok, current = pcall(json.decode, g_resources.readFileContents(markerPath))
        if ok and type(current) == 'table' then
            local upToDate = true
            for _, entry in ipairs(files) do
                if current[entry.file] ~= (entry.sha256 or '') then
                    upToDate = false
                    break
                end
            end
            if upToDate then
                setLoginLock(nil)
                return
            end
        end
    end

    installClassicAssets(version, files, assetInstallToken)
end

-- locks the login screen to the server described by the discovery response's
-- `web` object; reapplied only when the configuration actually changes
local function applyWebConfig(web)
    if type(web) ~= 'table' or type(web.login) ~= 'table' then
        setLoginLock(tr('This server does not have the web client enabled.'))
        return
    end

    local ok, signature = pcall(json.encode, web)
    if ok and signature == appliedWebSignature then
        return
    end
    appliedWebSignature = ok and signature or nil

    local login = web.login
    local cfg
    if login.type == 'http' and type(login.url) == 'string' then
        cfg = {
            host = login.url,
            port = tonumber(login.port) or 443,
            clientVersion = tonumber(web.clientVersion),
            httpLogin = true,
        }
    else
        cfg = {
            host = login.host or 'proxy',
            port = tonumber(login.port) or 7171,
            clientVersion = tonumber(web.clientVersion),
            httpLogin = false,
        }
    end

    if EnterGame and EnterGame.applyManagedServer then
        EnterGame.applyManagedServer(cfg)
    end

    -- servers with a non-standard RSA key advertise its decimal modulus;
    -- g_game.chooseRsa leaves custom keys alone, so this survives doLogin.
    -- When the key disappears from the config, fall back to the standard one.
    if type(web.rsa) == 'string' and web.rsa:match('^%d+$') then
        g_game.setRsa(web.rsa)
    elseif g_game.getRsa() ~= OTSERV_RSA and g_game.getRsa() ~= CIPSOFT_RSA then
        g_game.setRsa(OTSERV_RSA)
    end

    ensureClassicAssets(web)
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
            if webMode and appliedWebSignature == nil then
                -- never configured yet: replace the "Configuring server..."
                -- lock with the actual problem; the cycle event keeps retrying
                setLoginLock(string.format('%s (%s)', tr('Cannot reach the server configuration service'), err))
            end
            return
        end

        if type(data) ~= 'table' or type(data.proxies) ~= 'table' then
            lastRefresh.ok = false
            lastRefresh.message = tr('unexpected response')
            g_logger.warning(string.format('[otshosting] unexpected proxy discovery response (%s)', reason))
            return
        end

        -- the web client prefers the dedicated wsProxies list (it always
        -- carries the wss endpoints, even for servers without a proxy tier);
        -- the plain proxies list is the fallback, filtered by parseEntry
        local entries = data.proxies
        if webMode and type(data.web) == 'table' and type(data.web.wsProxies) == 'table' then
            entries = data.web.wsProxies
        end

        applyProxyList(entries)
        if webMode then
            applyWebConfig(data.web)
        end
        lastRefresh.ok = true
        lastRefresh.message = string.format('%d %s', #entries, tr('proxies'))
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

function OTSHosting:toggle()
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
    local visible = window:isVisible()
    if topMenuButton then
        topMenuButton:setOn(visible)
    end
    if gameButton then
        gameButton:setOn(visible)
    end
end

function OTSHosting:refreshNow()
    refresh('manual')
end

function OTSHosting:onInit()
    config = Services and Services.otshosting or nil
    if not config or type(config.subdomain) ~= 'string' or config.subdomain == '' then
        g_logger.debug('[otshosting] no subdomain registered in Services.otshosting, module stays idle')
        return
    end
    active = true
    webMode = config.web == true and g_platform.isBrowser()

    local interval = tonumber(config.refreshInterval) or DEFAULT_REFRESH_INTERVAL

    -- the hosted web client is unusable until the discovery response tells it
    -- which server to lock onto, so logins wait for the first refresh
    if webMode then
        setLoginLock(tr('Configuring server...'))
    end

    -- 1) initial load + 2) periodic refresh, also while connected
    refresh('startup')
    nextRefreshAt = os.time() + interval
    self:cycleEvent(function()
        nextRefreshAt = os.time() + interval
        refresh('periodic')
    end, interval * 1000)

    -- 3) the proxy makes the server address moot, hide it on the login screen
    if config.hideServerFields ~= false and EnterGame then
        EnterGame.hideServerFields(config.host, config.port)
    end

    -- 4) diagnostics window
    window = g_ui.displayUI('otshosting')
    window:hide()
    window:recursiveGetChildById('refreshButton').onClick = function()
        OTSHosting:refreshNow()
    end
    window:recursiveGetChildById('closeButton').onClick = function()
        OTSHosting:toggle()
    end
    window.onVisibilityChange = function(_, visible)
        if topMenuButton then
            topMenuButton:setOn(visible)
        end
        if gameButton then
            gameButton:setOn(visible)
        end
    end

    topMenuButton = modules.client_topmenu.addTopRightToggleButton('otshostingButton',
                                                                   tr('Proxy Diagnostics') .. ' (Ctrl+Shift+P)',
                                                                   '/images/topbuttons/debug', function()
        OTSHosting:toggle()
    end)

    -- the top menu toggles panel is only shown on the login screen; in game the
    -- icons live in the main panel, so register a second button there
    gameButton = modules.client_topmenu.addRightGameToggleButton('otshostingGameButton',
                                                                 tr('Proxy Diagnostics') .. ' (Ctrl+Shift+P)',
                                                                 '/images/options/analyzers', function()
        OTSHosting:toggle()
    end)

    Keybind.new('Misc.', 'Toggle Proxy Diagnostics', 'Ctrl+Shift+P', '')
    Keybind.bind('Misc.', 'Toggle Proxy Diagnostics', {{
        type = KEY_DOWN,
        callback = function()
            OTSHosting:toggle()
        end
    }})

    self:cycleEvent(updateWindow, 1000)
end

function OTSHosting:onTerminate()
    if not active then
        return
    end
    active = false

    if webMode then
        assetInstallToken = assetInstallToken + 1 -- abort any in-flight install
        appliedWebSignature = nil
        setLoginLock(nil)
        webMode = false
    end

    Keybind.delete('Misc.', 'Toggle Proxy Diagnostics')

    if topMenuButton then
        topMenuButton:destroy()
        topMenuButton = nil
    end
    if gameButton then
        gameButton:destroy()
        gameButton = nil
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
