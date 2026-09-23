--!nonstrict

local workspaceModel = require("./lib/workspace_model.luau")
local styleModel = require("./lib/style_model.luau")

local WORKSPACE_STATE_KEY = "luxaxis.workspace-state"
local STYLE_STATE_KEY = "luxaxis.styles"
local REQUEST_KEY = "luxaxis.request"
local COMMAND_TIMEOUT_MS = 3000

local SNAPSHOT_COMMANDS = {
  { key = "workspaces", argv = { "hyprctl", "-j", "workspaces" }, array = true },
  { key = "rules", argv = { "hyprctl", "-j", "workspacerules" }, array = true },
  { key = "monitors", argv = { "hyprctl", "-j", "monitors" }, array = true },
  { key = "clients", argv = { "hyprctl", "-j", "clients" }, array = true },
  { key = "status", argv = { "hyprctl", "-j", "status" }, array = false },
}

local RELEVANT_EVENTS = {
  workspace = true,
  workspacev2 = true,
  createworkspace = true,
  createworkspacev2 = true,
  destroyworkspace = true,
  destroyworkspacev2 = true,
  moveworkspace = true,
  moveworkspacev2 = true,
  focusedmon = true,
  focusedmonv2 = true,
  openwindow = true,
  closewindow = true,
  movewindow = true,
  movewindowv2 = true,
  urgent = true,
  renameworkspace = true,
  changeworkspaceid = true,
  configreloaded = true,
}

local snapshot = nil
local snapshotRevision = 0
local configProvider = "unknown"
local lastError = nil
local lastNotifiedError = nil
local refreshInFlight = false
local refreshPending = false
local eventStreamStarted = false
local eventSocketPath = nil

local styles = styleModel.defaultConfig()
local styleRevision = 0
local styleWarnings = {}
local styleError = nil
local stylePath = nil

local hyprctlAvailable = noctalia.commandExists("hyprctl")
local socatAvailable = noctalia.commandExists("socat")
local signature = noctalia.getenv("HYPRLAND_INSTANCE_SIGNATURE")
local sessionAvailable = type(signature) == "string" and signature ~= ""

local function notifyNewError(message)
  if type(message) ~= "string" or message == "" or message == lastNotifiedError then
    return
  end
  lastNotifiedError = message
  noctalia.notifyError("Luxaxis", message)
end

local function publishWorkspaceState()
  local source = snapshot or { schema = nil, workspaces = {}, outputs = {} }
  noctalia.state.set(WORKSPACE_STATE_KEY, {
    available = snapshot ~= nil,
    interactive = snapshot ~= nil
      and lastError == nil
      and hyprctlAvailable
      and socatAvailable
      and sessionAvailable
      and eventStreamStarted,
    error = lastError,
    schema = source.schema,
    workspaces = source.workspaces,
    outputs = source.outputs,
    revision = snapshotRevision,
    diagnostics = {
      configProvider = configProvider,
      eventStreamStarted = eventStreamStarted,
      eventSocketPath = eventSocketPath,
      refreshInFlight = refreshInFlight,
      hyprctlAvailable = hyprctlAvailable,
      socatAvailable = socatAvailable,
      sessionAvailable = sessionAvailable,
    },
  })
end

local function setWorkspaceError(message)
  lastError = message
  publishWorkspaceState()
  notifyNewError(message)
end

local function clearWorkspaceError()
  if not hyprctlAvailable then
    lastError = "hyprctl is not available"
  elseif not socatAvailable then
    lastError = "socat is not available"
  elseif not sessionAvailable then
    lastError = "HYPRLAND_INSTANCE_SIGNATURE is not available"
  elseif not eventStreamStarted then
    lastError = "The Hyprland event stream is not running"
  else
    lastError = nil
    lastNotifiedError = nil
  end
end

local function publishStyles()
  noctalia.state.set(STYLE_STATE_KEY, {
    config = styles,
    revision = styleRevision,
    warnings = styleWarnings,
    error = styleError,
  })
end

local function setStyleError(message)
  styleError = message
  publishStyles()
  notifyNewError(message)
end

local function loadStyles()
  local dataDir, dataDirError = noctalia.pluginDataDir()
  if type(dataDir) ~= "string" or dataDir == "" then
    setStyleError("Cannot open the Luxaxis data directory: " .. tostring(dataDirError or "unknown error"))
    return
  end

  stylePath = dataDir .. "/styles.json"
  if not noctalia.fileExists(stylePath) then
    styles = styleModel.defaultConfig()
    styleWarnings = {}
    styleError = nil
    styleRevision = styleRevision + 1
    publishStyles()
    return
  end

  local raw, readError = noctalia.readFile(stylePath)
  if raw == nil then
    setStyleError("Cannot read workspace styles: " .. tostring(readError or "unknown error"))
    return
  end
  local decoded, decodeError = noctalia.json.decode(raw)
  if type(decoded) ~= "table" then
    setStyleError("Cannot decode workspace styles: " .. tostring(decodeError or "invalid JSON"))
    return
  end

  styles, styleWarnings = styleModel.sanitize(decoded)
  styleError = nil
  styleRevision = styleRevision + 1
  publishStyles()
end

local function saveStyles(value)
  if type(value) ~= "table" then
    setStyleError("Rejected an invalid workspace style document")
    return
  end
  if stylePath == nil then
    local dataDir, dataDirError = noctalia.pluginDataDir()
    if type(dataDir) ~= "string" or dataDir == "" then
      setStyleError("Cannot open the Luxaxis data directory: " .. tostring(dataDirError or "unknown error"))
      return
    end
    stylePath = dataDir .. "/styles.json"
  end

  local clean, warnings = styleModel.sanitize(value)
  local encoded, encodeError = noctalia.json.encode(clean, true)
  if encoded == nil then
    setStyleError("Cannot encode workspace styles: " .. tostring(encodeError or "unknown error"))
    return
  end
  local wrote, writeError = noctalia.writeFile(stylePath, encoded .. "\n")
  if not wrote then
    setStyleError("Cannot save workspace styles: " .. tostring(writeError or "unknown error"))
    return
  end

  styles = clean
  styleWarnings = warnings
  styleError = nil
  styleRevision = styleRevision + 1
  publishStyles()
end

local function resultError(name, result)
  if result == nil then
    return name .. " did not return a result"
  end
  if result.timedOut then
    return name .. " timed out"
  end
  if result.exitCode ~= 0 then
    local detail = noctalia.string.trim(result.stderr or "")
    if detail == "" then
      detail = "exit code " .. tostring(result.exitCode)
    end
    return name .. " failed: " .. detail
  end
  return nil
end

local requestRefresh

local function finishRefresh(rawResults)
  refreshInFlight = false

  local decoded = {}
  for _, command in ipairs(SNAPSHOT_COMMANDS) do
    local result = rawResults[command.key]
    local commandError = resultError("hyprctl " .. command.key, result)
    if commandError ~= nil then
      setWorkspaceError(commandError)
      if refreshPending then
        refreshPending = false
        requestRefresh()
      end
      return
    end

    local value, decodeError = noctalia.json.decode(result.stdout or "")
    if type(value) ~= "table" or (command.array and #value == 0 and command.key ~= "clients" and command.key ~= "rules") then
      setWorkspaceError("Invalid JSON from hyprctl " .. command.key .. ": " .. tostring(decodeError or "unexpected shape"))
      if refreshPending then
        refreshPending = false
        requestRefresh()
      end
      return
    end
    decoded[command.key] = value
  end

  local nextSnapshot, modelError = workspaceModel.buildSnapshot(
    decoded.workspaces,
    decoded.rules,
    decoded.monitors,
    decoded.clients,
    snapshot
  )
  if nextSnapshot == nil then
    setWorkspaceError(modelError or "Cannot build the Hyprland workspace state")
  else
    snapshot = nextSnapshot
    snapshotRevision = snapshotRevision + 1
    configProvider = decoded.status.configProvider == "lua" and "lua" or "hyprlang"
    clearWorkspaceError()
    publishWorkspaceState()
  end

  if refreshPending then
    refreshPending = false
    requestRefresh()
  end
end

requestRefresh = function()
  if not hyprctlAvailable then
    setWorkspaceError("hyprctl is not available")
    return
  end
  if not sessionAvailable then
    setWorkspaceError("HYPRLAND_INSTANCE_SIGNATURE is not available")
    return
  end
  if refreshInFlight then
    refreshPending = true
    return
  end

  refreshInFlight = true
  publishWorkspaceState()
  local remaining = #SNAPSHOT_COMMANDS
  local results = {}

  local function complete(key, result)
    if results[key] ~= nil then
      return
    end
    results[key] = result
    remaining = remaining - 1
    if remaining == 0 then
      finishRefresh(results)
    end
  end

  for _, command in ipairs(SNAPSHOT_COMMANDS) do
    local key = command.key
    local started = noctalia.runAsync(command.argv, function(result)
      complete(key, result)
    end, COMMAND_TIMEOUT_MS)
    if not started then
      complete(key, {
        exitCode = -1,
        stdout = "",
        stderr = "process could not be started",
        timedOut = false,
      })
    end
  end
end

local function luaQuote(value)
  return '"' .. value:gsub("\\", "\\\\"):gsub('"', '\\"'):gsub("\n", "\\n"):gsub("\r", "\\r") .. '"'
end

local function dispatchWorkspace(workspace)
  if snapshot == nil or lastError ~= nil or type(workspace) ~= "table" then
    return
  end
  if workspace.active == true then
    return
  end

  local argv
  if configProvider == "lua" then
    argv = {
      "hyprctl",
      "dispatch",
      "hl.dsp.focus({ workspace = " .. luaQuote(workspace.selector) .. " })",
    }
  else
    argv = { "hyprctl", "dispatch", "workspace", workspace.selector }
  end

  local started = noctalia.runAsync(argv, function(result)
    local dispatchError = resultError("Workspace switch", result)
    if dispatchError ~= nil then
      setWorkspaceError(dispatchError)
      return
    end
    requestRefresh()
  end, COMMAND_TIMEOUT_MS)
  if not started then
    setWorkspaceError("Workspace switch process could not be started")
  end
end

local function handleRequest(value)
  if type(value) ~= "table" or type(value.action) ~= "string" then
    return
  end
  if value.action == "refresh" then
    requestRefresh()
    return
  end
  if value.action == "save-styles" then
    saveStyles(value.config)
    return
  end
  if snapshot == nil or lastError ~= nil then
    return
  end

  if value.action == "switch" then
    if type(value.key) ~= "string" or type(value.output) ~= "string" then
      return
    end
    local workspace = workspaceModel.find(snapshot, value.key)
    if workspace ~= nil and workspace.output == value.output then
      dispatchWorkspace(workspace)
    end
  elseif value.action == "step" then
    if type(value.output) ~= "string" or type(value.direction) ~= "number" then
      return
    end
    local direction = value.direction < 0 and -1 or 1
    local target = workspaceModel.stepTarget(
      snapshot,
      value.output,
      direction,
      noctalia.getConfig("hide_when_empty") == true
    )
    if target ~= nil then
      dispatchWorkspace(target)
    end
  end
end

local function shellQuote(value)
  return "'" .. value:gsub("'", "'\"'\"'") .. "'"
end

local function resolveEventSocket()
  if not sessionAvailable then
    return nil
  end
  local candidates = {}
  local runtimeDir = noctalia.getenv("XDG_RUNTIME_DIR")
  if type(runtimeDir) == "string" and runtimeDir ~= "" then
    table.insert(candidates, runtimeDir .. "/hypr/" .. signature .. "/.socket2.sock")
  end
  table.insert(candidates, "/tmp/hypr/" .. signature .. "/.socket2.sock")
  for _, candidate in ipairs(candidates) do
    if noctalia.fileExists(candidate) then
      return candidate
    end
  end
  return candidates[1]
end

local function onEventLine(line)
  local separator = type(line) == "string" and line:find(">>", 1, true) or nil
  if separator == nil then
    return
  end
  local event = line:sub(1, separator - 1)
  if RELEVANT_EVENTS[event] then
    requestRefresh()
  end
end

local function startEventStream()
  if eventStreamStarted then
    return
  end
  if not socatAvailable then
    setWorkspaceError("socat is not available")
    return
  end
  eventSocketPath = resolveEventSocket()
  if eventSocketPath == nil then
    setWorkspaceError("Cannot resolve the Hyprland event socket")
    return
  end

  local socket = shellQuote(eventSocketPath)
  local command = "while true; do socat -u UNIX-CONNECT:" .. socket .. " - 2>/dev/null; sleep 1; done"
  eventStreamStarted = noctalia.runStream(command, onEventLine)
  if not eventStreamStarted then
    setWorkspaceError("Cannot start the Hyprland event stream")
  end
  publishWorkspaceState()
end

function update()
  requestRefresh()
end

function onEnable()
  startEventStream()
  requestRefresh()
end

function onOutputsChanged()
  requestRefresh()
end

function onConfigChanged()
  requestRefresh()
end

noctalia.state.watch(REQUEST_KEY, handleRequest)
noctalia.setUpdateInterval(30000)
loadStyles()
publishWorkspaceState()
startEventStream()
requestRefresh()
