local workspaceModel = dofile("lib/workspace_model.luau")
local styleModel = dofile("lib/style_model.luau")

local passed = 0
local failed = 0

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
    passed = passed + 1
    io.write("ok - ", name, "\n")
  else
    failed = failed + 1
    io.write("not ok - ", name, "\n  ", tostring(err), "\n")
  end
end

local function legacy(id, name, monitor)
  return { id = id, name = name, monitor = monitor }
end

local function typed(address, kind, name, monitor)
  return { address = address, type = kind, name = name, monitor = monitor }
end

local function normal(address, id, name, monitor)
  local value = { address = address, type = "normal", name = name, monitor = monitor }
  value.id = id
  return value
end

test("legacy schema excludes special workspaces", function()
  local snapshot, err = workspaceModel.buildSnapshot({
    legacy(1, "1", "eDP-1"),
    legacy(-7, "dev", "eDP-1"),
    legacy(-99, "special:scratchpad", "eDP-1"),
  }, {}, {
    { name = "eDP-1", activeWorkspace = legacy(1, "1") },
  }, {})
  equal(err, nil)
  equal(snapshot.schema, "legacy")
  equal(#snapshot.workspaces, 2)
  equal(snapshot.workspaces[1].key, "name:dev")
  equal(snapshot.workspaces[2].key, "1")
end)

test("legacy unassigned clients do not invalidate a snapshot", function()
  local snapshot, err = workspaceModel.buildSnapshot({
    legacy(1, "1", "eDP-1"),
  }, {}, {
    { name = "eDP-1", activeWorkspace = legacy(1, "1") },
  }, {
    { workspace = { id = -1, name = "" }, urgent = false },
  })
  equal(err, nil)
  equal(#snapshot.workspaces, 1)
end)

test("typed schema computes active occupied and urgent state", function()
  local snapshot, err = workspaceModel.buildSnapshot({
    typed("1", "numbered", "1", "eDP-1"),
    typed("2", "numbered", "2", "eDP-1"),
  }, {}, {
    { name = "eDP-1", activeWorkspace = typed("1", "numbered") },
  }, {
    { workspace = typed("2", "numbered"), urgent = true },
  })
  equal(err, nil)
  equal(workspaceModel.stateName(snapshot.workspaces[1]), "active")
  equal(workspaceModel.stateName(snapshot.workspaces[2]), "urgent")
end)

test("normal schema distinguishes empty runtime from persistent synthesis", function()
  local snapshot, err = workspaceModel.buildSnapshot({
    normal("1", 1, "1", "eDP-1"),
  }, {
    { workspaceString = "2", monitor = "eDP-1", persistent = true },
    { workspaceString = "3", monitor = "eDP-1", persistent = false },
    { workspaceString = "r[4-8]", monitor = "eDP-1", persistent = true },
  }, {
    { name = "eDP-1", activeWorkspace = normal("1", 1) },
  }, {})
  equal(err, nil)
  equal(#snapshot.workspaces, 2, "only runtime and exact persistent workspaces are visible")
  equal(snapshot.workspaces[1].runtime, true)
  equal(snapshot.workspaces[2].key, "2")
  equal(snapshot.workspaces[2].runtime, false)
  equal(workspaceModel.stateName(snapshot.workspaces[2]), "empty")
end)

test("workspace rules may label runtime numbered workspaces", function()
  local snapshot = assert(workspaceModel.buildSnapshot({
    normal("1", 1, "1", "eDP-1"),
  }, {
    { workspaceString = "1", defaultName = "Main", persistent = false },
  }, {
    { name = "eDP-1", activeWorkspace = normal("1", 1) },
  }, {}))
  equal(snapshot.workspaces[1].name, "Main")
end)

test("output filtering and hide-empty preserve active empty workspace", function()
  local snapshot = assert(workspaceModel.buildSnapshot({
    normal("1", 1, "1", "eDP-1"),
    normal("2", 2, "2", "eDP-1"),
    normal("3", 3, "3", "HDMI-A-1"),
  }, {}, {
    { name = "eDP-1", activeWorkspace = normal("1", 1) },
    { name = "HDMI-A-1", activeWorkspace = normal("3", 3) },
  }, {}))
  local visible = workspaceModel.forOutput(snapshot, "eDP-1", true)
  equal(#visible, 1)
  equal(visible[1].key, "1")
end)

test("stepping stops at output boundaries without wrapping", function()
  local snapshot = assert(workspaceModel.buildSnapshot({
    normal("1", 1, "1", "eDP-1"),
    normal("2", 2, "2", "eDP-1"),
  }, {}, {
    { name = "eDP-1", activeWorkspace = normal("1", 1) },
  }, {}))
  local previous, previousError = workspaceModel.stepTarget(snapshot, "eDP-1", -1, false)
  equal(previous, nil)
  equal(previousError, "Workspace boundary reached")
  local nextWorkspace = assert(workspaceModel.stepTarget(snapshot, "eDP-1", 1, false))
  equal(nextWorkspace.key, "2")
end)

test("state priority is active urgent occupied empty", function()
  equal(workspaceModel.stateName({ active = true, urgent = true, occupied = true }), "active")
  equal(workspaceModel.stateName({ active = false, urgent = true, occupied = true }), "urgent")
  equal(workspaceModel.stateName({ active = false, urgent = false, occupied = true }), "occupied")
  equal(workspaceModel.stateName({}), "empty")
end)

test("style inheritance applies all four layers", function()
  local config = styleModel.defaultConfig()
  config.base.foreground = "base"
  config.states.active.foreground = "state"
  config.workspaces["1"] = {
    base = { foreground = "workspace", radius = 3 },
    states = { active = { foreground = "workspace-state" } },
  }
  local resolved = styleModel.resolve(config, "1", "active")
  equal(resolved.foreground, "workspace-state")
  equal(resolved.radius, 3)
end)

test("style sanitizer clamps numbers and rejects unsupported images", function()
  local config, warnings = styleModel.sanitize({ base = { fontSize = 200, mystery = true } })
  equal(config.base.fontSize, 32)
  equal(#warnings, 1)
  equal(styleModel.isSupportedImage("~/icon.WEBP"), true)
  equal(styleModel.isSupportedImage("~/icon.gif"), false)
  equal(styleModel.isSupportedImage("file:///tmp/icon.png"), false)
  equal(styleModel.isSupportedColor("primary/0.4"), true)
  equal(styleModel.isSupportedColor("#12xz00"), false)
end)

io.write(string.format("\n%d passed, %d failed\n", passed, failed))
if failed > 0 then
  os.exit(1)
end
