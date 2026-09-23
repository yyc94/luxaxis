local nativeRequire = require

local function pluginRequire(path)
  if path:sub(1, 2) == "./" and path:sub(-5) == ".luau" then
    return dofile(path:sub(3))
  end
  return nativeRequire(path)
end

local function normal(address, id, name, monitor)
  local value = { address = address, type = "normal", name = name, monitor = monitor }
  value.id = id
  return value
end

local fixtures = {
  workspaces = {
    normal("1", 1, "1", "eDP-1"),
    normal("2", 2, "2", "eDP-1"),
    normal("3", 3, "3", "HDMI-A-1"),
  },
  rules = {
    { workspaceString = "4", monitor = "eDP-1", persistent = true },
  },
  monitors = {
    { name = "eDP-1", activeWorkspace = normal("1", 1) },
    { name = "HDMI-A-1", activeWorkspace = normal("3", 3) },
  },
  clients = {
    { workspace = normal("2", 2), urgent = true },
  },
  status = { configProvider = "lua" },
}

local watchers = {}
local stateValues = {}
local dispatches = {}
local notifications = {}
local streamCallback = nil
local config = { hide_when_empty = false, invert_scroll = false }

local function stateSet(key, value)
  stateValues[key] = value
  for _, callback in ipairs(watchers[key] or {}) do
    callback(value)
  end
end

local noctalia = {
  state = {
    set = stateSet,
    get = function(key)
      return stateValues[key]
    end,
    watch = function(key, callback)
      watchers[key] = watchers[key] or {}
      table.insert(watchers[key], callback)
    end,
  },
  json = {
    decode = function(value)
      return fixtures[value]
    end,
    encode = function(_value, _pretty)
      return "{}"
    end,
  },
  string = {
    trim = function(value)
      return value:match("^%s*(.-)%s*$")
    end,
  },
  commandExists = function(name)
    return name == "hyprctl" or name == "socat"
  end,
  getenv = function(name)
    if name == "HYPRLAND_INSTANCE_SIGNATURE" then
      return "test-signature"
    end
    if name == "XDG_RUNTIME_DIR" then
      return "/tmp/runtime-test"
    end
    return nil
  end,
  fileExists = function(path)
    return path:match("%.socket2%.sock$") ~= nil
  end,
  pluginDataDir = function()
    return "/tmp/luxaxis-runtime-test"
  end,
  readFile = function()
    return nil, "not found"
  end,
  writeFile = function()
    return true
  end,
  runAsync = function(argv, callback, _timeout)
    if argv[2] == "-j" then
      callback({ exitCode = 0, stdout = argv[3] == "workspacerules" and "rules" or argv[3], stderr = "", timedOut = false })
    else
      table.insert(dispatches, argv)
      callback({ exitCode = 0, stdout = "ok", stderr = "", timedOut = false })
    end
    return true
  end,
  runStream = function(_command, callback)
    streamCallback = callback
    return true
  end,
  setUpdateInterval = function() end,
  notifyError = function(title, body)
    table.insert(notifications, title .. ": " .. tostring(body))
  end,
  notify = function() end,
  getConfig = function(key)
    return config[key]
  end,
  getColor = function()
    return "#ffffff"
  end,
  openColorPicker = function()
    return true
  end,
  copyToClipboard = function()
    return true
  end,
}

local function loadEntry(path, extra)
  local environment = {
    noctalia = noctalia,
    require = pluginRequire,
  }
  for key, value in pairs(extra or {}) do
    environment[key] = value
  end
  setmetatable(environment, { __index = _G })
  local chunk
  if setfenv ~= nil then
    chunk = assert(loadfile(path))
    setfenv(chunk, environment)
  else
    chunk = assert(loadfile(path, "t", environment))
  end
  chunk()
  return environment
end

local function fail(message)
  error(message, 2)
end

local function equal(actual, expected, message)
  if actual ~= expected then
    fail((message or "values differ") .. ": expected " .. tostring(expected) .. ", got " .. tostring(actual))
  end
end

local function test(name, body)
  local ok, err = pcall(body)
  if ok then
    io.write("ok - ", name, "\n")
    return true
  end
  io.write("not ok - ", name, "\n  ", tostring(err), "\n")
  return false
end

local service = loadEntry("service.luau")
local workspaceState = stateValues["luxaxis.workspace-state"]

local passed = 0
local failed = 0
local function run(name, body)
  if test(name, body) then
    passed = passed + 1
  else
    failed = failed + 1
  end
end

run("service publishes a usable Lua-provider snapshot", function()
  equal(workspaceState.available, true)
  equal(workspaceState.interactive, true)
  equal(workspaceState.diagnostics.configProvider, "lua")
  equal(#workspaceState.workspaces, 4)
  equal(#workspaceState.outputs["eDP-1"], 3)
  equal(#workspaceState.outputs["HDMI-A-1"], 1)
end)

run("service validates output ownership and uses Lua dispatch", function()
  local before = #dispatches
  stateSet("luxaxis.request", { action = "switch", key = "2", output = "HDMI-A-1" })
  equal(#dispatches, before, "wrong-output request must be ignored")
  stateSet("luxaxis.request", { action = "switch", key = "1", output = "eDP-1" })
  equal(#dispatches, before, "active workspace request must be ignored")
  stateSet("luxaxis.request", { action = "switch", key = "2", output = "eDP-1" })
  equal(#dispatches, before + 1)
  equal(dispatches[#dispatches][2], "dispatch")
  equal(dispatches[#dispatches][3], 'hl.dsp.focus({ workspace = "2" })')
end)

run("service uses the Hyprlang dispatcher for non-Lua configs", function()
  fixtures.status.configProvider = "hyprlang"
  service.update()
  local before = #dispatches
  stateSet("luxaxis.request", { action = "switch", key = "2", output = "eDP-1" })
  equal(#dispatches, before + 1)
  equal(dispatches[#dispatches][2], "dispatch")
  equal(dispatches[#dispatches][3], "workspace")
  equal(dispatches[#dispatches][4], "2")
  fixtures.status.configProvider = "lua"
  service.update()
end)

run("event stream refreshes relevant events only", function()
  local revision = stateValues["luxaxis.workspace-state"].revision
  streamCallback("activewindow>>kitty,title")
  equal(stateValues["luxaxis.workspace-state"].revision, revision)
  streamCallback("workspacev2>>2,2")
  equal(stateValues["luxaxis.workspace-state"].revision, revision + 1)
end)

run("snapshot failures retain the last valid state and disable interaction", function()
  local previous = stateValues["luxaxis.workspace-state"]
  local notificationCount = #notifications
  fixtures.workspaces = nil
  service.update()
  local failedState = stateValues["luxaxis.workspace-state"]
  equal(failedState.available, true)
  equal(failedState.interactive, false)
  equal(#failedState.workspaces, #previous.workspaces)
  equal(type(failedState.error), "string")
  equal(#notifications, notificationCount + 1)
  service.update()
  equal(#notifications, notificationCount + 1, "the same unresolved error must notify once")

  fixtures.workspaces = {
    normal("1", 1, "1", "eDP-1"),
    normal("2", 2, "2", "eDP-1"),
    normal("3", 3, "3", "HDMI-A-1"),
  }
  service.update()
  local recovered = stateValues["luxaxis.workspace-state"]
  equal(recovered.interactive, true)
  equal(recovered.error, nil)
  workspaceState = recovered
end)

local function uiNode(kind)
  return function(props, children)
    return { type = kind, props = props or {}, children = children or {} }
  end
end

local ui = {}
for _, kind in ipairs({ "column", "row", "scroll", "box", "label", "glyph", "image", "separator", "spacer", "progress", "button", "input", "select", "slider", "toggle" }) do
  ui[kind] = uiNode(kind)
end

local function widgetFor(output)
  local rendered = nil
  local visible = nil
  local barWidget = {
    outputName = function()
      return output
    end,
    isVertical = function()
      return false
    end,
    setVisible = function(value)
      visible = value
    end,
    render = function(value)
      rendered = value
    end,
    setTooltip = function() end,
    clearTooltip = function() end,
  }
  local environment = loadEntry("widget.luau", { ui = ui, barWidget = barWidget })
  return environment, function()
    return rendered, visible
  end
end

local internalWidget, internalView = widgetFor("eDP-1")
local externalWidget, externalView = widgetFor("HDMI-A-1")

run("each widget renders only its own output", function()
  local internalTree, internalVisible = internalView()
  local externalTree, externalVisible = externalView()
  equal(internalVisible, true)
  equal(externalVisible, true)
  equal(#internalTree.children, 5, "three workspaces plus two gaps")
  equal(#externalTree.children, 1)
  equal(internalTree.children[1].props.key, "workspace-eDP-1-1")
  equal(externalTree.children[1].props.key, "workspace-HDMI-A-1-3")
end)

run("workspace row click reaches the validated service", function()
  local tree = internalView()
  local before = #dispatches
  equal(tree.children[1].props.onClick, nil, "active row is a no-op")
  local secondWorkspace = tree.children[3]
  equal(type(secondWorkspace.props.onClick), "function")
  secondWorkspace.props.onClick()
  equal(#dispatches, before + 1)
  equal(dispatches[#dispatches][3], 'hl.dsp.focus({ workspace = "2" })')
end)

run("scroll sends one bounded step per gesture", function()
  local before = #dispatches
  internalWidget.onScroll("vertical", 1, false)
  equal(#dispatches, before)
  internalWidget.onScroll("vertical", 1, true)
  equal(#dispatches, before + 1)
  internalWidget.onScroll("vertical", -1, true)
  equal(#dispatches, before + 1, "previous from first workspace must not wrap")
end)

run("hide-empty removes only inactive empty workspaces", function()
  config.hide_when_empty = true
  internalWidget.update()
  local tree = internalView()
  equal(#tree.children, 3, "active and urgent workspaces plus one gap")
  config.hide_when_empty = false
end)

run("style panel loads and renders without mutating workspace configuration", function()
  local before = #dispatches
  local panelTree = nil
  local panel = {
    render = function(value)
      panelTree = value
    end,
    close = function() end,
  }
  local panelEntry = loadEntry("panel.luau", { ui = ui, panel = panel })
  panelEntry.onOpen(nil)
  equal(panelTree.type, "column")
  equal(#dispatches, before)

  local function findNode(node, predicate)
    if predicate(node) then
      return node
    end
    for _, child in ipairs(node.children or {}) do
      local found = findNode(child, predicate)
      if found ~= nil then
        return found
      end
    end
    return nil
  end

  local toggle = assert(findNode(panelTree, function(node)
    return node.type == "toggle"
  end))
  toggle.props.onChange("false")
  local apply = assert(findNode(panelTree, function(node)
    return node.type == "button" and node.props.text == "Apply"
  end))
  equal(apply.props.enabled, true)

  local styleRevision = stateValues["luxaxis.styles"].revision
  panelEntry.onApplyClicked()
  equal(stateValues["luxaxis.styles"].revision, styleRevision + 1)
  equal(stateValues["luxaxis.styles"].config.base.labelVisible, false)
  equal(#dispatches, before)
end)

io.write(string.format("\n%d passed, %d failed\n", passed, failed))
if failed > 0 then
  os.exit(1)
end
