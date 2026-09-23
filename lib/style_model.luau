--!nonstrict

local M = {}

local STATE_NAMES = { "active", "urgent", "occupied", "empty" }
local STATE_SET = { active = true, urgent = true, occupied = true, empty = true }

local DEFAULT_BASE = {
  labelVisible = true,
  labelSource = "id",
  label = "",
  iconType = "none",
  icon = "",
  iconSize = 16,
  fontSize = 12,
  fontWeight = "semibold",
  foreground = "on_surface",
  fill = "#00000000",
  border = "#00000000",
  borderWidth = 0,
  radius = 8,
  paddingH = 7,
  paddingV = 3,
  minWidth = 22,
  minHeight = 22,
  contentGap = 4,
  itemGap = 6,
  opacity = 1,
}

local DEFAULT_STATES = {
  active = { fill = "primary", foreground = "on_primary" },
  urgent = { fill = "error", foreground = "on_error" },
  occupied = { fill = "secondary/0.22", foreground = "secondary" },
  empty = { fill = "surface_variant/0.30", foreground = "on_surface_variant" },
}

local ENUMS = {
  labelSource = { id = true, name = true, custom = true },
  iconType = { none = true, text = true, glyph = true, image = true },
  fontWeight = {
    thin = true,
    light = true,
    normal = true,
    medium = true,
    semibold = true,
    bold = true,
    heavy = true,
  },
}

local NUMBERS = {
  iconSize = { 8, 64 },
  fontSize = { 8, 32 },
  borderWidth = { 0, 8 },
  radius = { 0, 32 },
  paddingH = { 0, 24 },
  paddingV = { 0, 16 },
  minWidth = { 0, 160 },
  minHeight = { 0, 72 },
  contentGap = { 0, 20 },
  itemGap = { 0, 24 },
  opacity = { 0.1, 1 },
}

local STRINGS = {
  label = true,
  icon = true,
}

local COLORS = {
  foreground = true,
  fill = true,
  border = true,
}

local function copy(value)
  if type(value) ~= "table" then
    return value
  end
  local result = {}
  for key, item in pairs(value) do
    result[key] = copy(item)
  end
  return result
end

local function clamp(value, lower, upper)
  return math.max(lower, math.min(upper, value))
end

local function supportedColor(value)
  if type(value) ~= "string" or value == "" then
    return false
  end
  if value:match("^#%x%x%x%x%x%x$") or value:match("^#%x%x%x%x%x%x%x%x$") then
    return true
  end
  if value:match("^[a-z][a-z0-9_]*$") then
    return true
  end
  local role, alpha = value:match("^([a-z][a-z0-9_]*)/(%d*%.?%d+)$")
  local parsed = tonumber(alpha)
  return role ~= nil and parsed ~= nil and parsed >= 0 and parsed <= 1
end

local function sanitizeLayer(raw, warnings)
  local layer = {}
  if type(raw) ~= "table" then
    return layer
  end
  for key, value in pairs(raw) do
    if key == "labelVisible" and type(value) == "boolean" then
      layer[key] = value
    elseif ENUMS[key] ~= nil and type(value) == "string" and ENUMS[key][value] then
      layer[key] = value
    elseif NUMBERS[key] ~= nil and type(value) == "number" then
      layer[key] = clamp(value, NUMBERS[key][1], NUMBERS[key][2])
    elseif STRINGS[key] and type(value) == "string" then
      layer[key] = value
    elseif COLORS[key] and supportedColor(value) then
      layer[key] = value
    else
      table.insert(warnings, "Ignored invalid style field: " .. tostring(key))
    end
  end
  return layer
end

local function merge(target, source)
  if type(source) ~= "table" then
    return
  end
  for key, value in pairs(source) do
    target[key] = copy(value)
  end
end

function M.defaultConfig()
  return {
    schemaVersion = 1,
    base = copy(DEFAULT_BASE),
    states = copy(DEFAULT_STATES),
    workspaces = {},
  }
end

function M.sanitize(raw)
  local warnings = {}
  local result = M.defaultConfig()
  if type(raw) ~= "table" then
    if raw ~= nil then
      table.insert(warnings, "Style document was not an object")
    end
    return result, warnings
  end
  if raw.schemaVersion ~= nil and raw.schemaVersion ~= 1 then
    table.insert(warnings, "Unsupported style schema version; defaults were used")
    return result, warnings
  end

  merge(result.base, sanitizeLayer(raw.base, warnings))
  if type(raw.states) == "table" then
    for _, state in ipairs(STATE_NAMES) do
      result.states[state] = result.states[state] or {}
      merge(result.states[state], sanitizeLayer(raw.states[state], warnings))
    end
  end

  if type(raw.workspaces) == "table" then
    for key, workspace in pairs(raw.workspaces) do
      if type(key) == "string" and type(workspace) == "table" then
        local clean = { base = sanitizeLayer(workspace.base, warnings), states = {} }
        if type(workspace.states) == "table" then
          for _, state in ipairs(STATE_NAMES) do
            clean.states[state] = sanitizeLayer(workspace.states[state], warnings)
          end
        end
        result.workspaces[key] = clean
      end
    end
  end
  return result, warnings
end

function M.resolve(config, workspaceKey, state)
  local result = copy(DEFAULT_BASE)
  if type(config) ~= "table" then
    config = M.defaultConfig()
  end
  merge(result, config.base)
  if STATE_SET[state] and type(config.states) == "table" then
    merge(result, config.states[state])
  end
  local workspace = type(config.workspaces) == "table" and config.workspaces[workspaceKey] or nil
  if type(workspace) == "table" then
    merge(result, workspace.base)
    if STATE_SET[state] and type(workspace.states) == "table" then
      merge(result, workspace.states[state])
    end
  end
  return result
end

function M.copy(value)
  return copy(value)
end

function M.stateNames()
  return copy(STATE_NAMES)
end

function M.isSupportedImage(path)
  if type(path) ~= "string" or path == "" then
    return false
  end
  local lower = path:lower()
  if lower:match("^%a[%w+.-]*://") then
    return false
  end
  return lower:match("%.png$") ~= nil
    or lower:match("%.jpe?g$") ~= nil
    or lower:match("%.webp$") ~= nil
    or lower:match("%.svg$") ~= nil
end

function M.isSupportedColor(value)
  return supportedColor(value)
end

return M
