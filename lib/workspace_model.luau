--!nonstrict

local M = {}

local function startsWith(value, prefix)
  return type(value) == "string" and value:sub(1, #prefix) == prefix
end

local function positiveInteger(value)
  if type(value) ~= "string" or value == "" or not value:match("^%d+$") then
    return nil
  end
  local parsed = tonumber(value)
  if parsed == nil or parsed <= 0 or parsed % 1 ~= 0 then
    return nil
  end
  return parsed
end

local function integer(value)
  if type(value) == "number" and value % 1 == 0 then
    return value
  end
  return nil
end

local function parseIdentity(row, schema)
  if type(row) ~= "table" then
    return nil
  end

  if schema == "legacy" then
    local id = integer(row.id)
    if id == nil or row.address ~= nil or row.type ~= nil then
      return nil
    end
    if id >= 0 then
      local key = tostring(id)
      return {
        key = key,
        selector = key,
        kind = "numbered",
        number = id > 0 and id or nil,
        legacyId = id,
      }
    end
    local address = type(row.name) == "string" and row.name or ""
    if address == "special" or startsWith(address, "special:") then
      return { key = address, selector = address, kind = "special", legacyId = id }
    end
    if address == "" then
      return {
        key = tostring(id),
        selector = "",
        kind = "unassigned",
        legacyId = id,
      }
    end
    return {
      key = "name:" .. address,
      selector = "name:" .. address,
      kind = "named",
      legacyId = id,
    }
  end

  if type(row.address) ~= "string" or row.address == "" or type(row.type) ~= "string" then
    return nil
  end

  if row.type == "special" and row.id == nil then
    return { key = row.address, selector = row.address, kind = "special" }
  end

  if schema == "typed" and row.type == "numbered" and row.id == nil then
    local number = positiveInteger(row.address)
    if number == nil then
      return nil
    end
    return { key = row.address, selector = row.address, kind = "numbered", number = number }
  end

  if schema == "typed" and row.type == "named" and row.id == nil then
    return { key = "name:" .. row.address, selector = "name:" .. row.address, kind = "named" }
  end

  if schema == "normal" and row.type == "normal" then
    if row.id == nil then
      return { key = "name:" .. row.address, selector = "name:" .. row.address, kind = "named" }
    end
    local id = integer(row.id)
    if id == nil or id <= 0 or row.address ~= tostring(id) then
      return nil
    end
    return { key = row.address, selector = row.address, kind = "numbered", number = id }
  end

  return nil
end

local function detectSchema(rows)
  if type(rows) ~= "table" or #rows == 0 then
    return nil, "Hyprland returned no workspace identity to detect"
  end
  local matches = {}
  for _, schema in ipairs({ "legacy", "typed", "normal" }) do
    local valid = true
    for _, row in ipairs(rows) do
      if parseIdentity(row, schema) == nil then
        valid = false
        break
      end
    end
    if valid then
      table.insert(matches, schema)
    end
  end
  if #matches ~= 1 then
    return nil, "Hyprland workspace identity schema is mixed or ambiguous"
  end
  return matches[1]
end

local function previousOrdinals(previous)
  local ordinals = {}
  local nextOrdinal = 0
  if type(previous) == "table" and type(previous.workspaces) == "table" then
    for _, workspace in ipairs(previous.workspaces) do
      if type(workspace.key) == "string" and type(workspace.ordinal) == "number" then
        ordinals[workspace.key] = workspace.ordinal
        nextOrdinal = math.max(nextOrdinal, workspace.ordinal + 1)
      end
    end
  end
  return ordinals, nextOrdinal
end

local function addPersistentRules(workspaces, seen, rules, schema, ordinals, nextOrdinal)
  if type(rules) ~= "table" then
    return nextOrdinal
  end
  for _, rule in ipairs(rules) do
    if type(rule) == "table" and rule.enabled ~= false then
      local selector = type(rule.workspaceString) == "string" and rule.workspaceString or ""
      local identity
      if startsWith(selector, "name:") and #selector > 5 then
        identity = {
          key = selector,
          selector = selector,
          kind = "named",
          legacyId = schema == "legacy" and -1 or nil,
        }
      else
        local number = positiveInteger(selector)
        if number ~= nil then
          identity = {
            key = tostring(number),
            selector = tostring(number),
            kind = "numbered",
            number = number,
            legacyId = schema == "legacy" and number or nil,
          }
        end
      end

      if identity ~= nil and seen[identity.key] then
        local defaultName = type(rule.defaultName) == "string" and rule.defaultName or ""
        if defaultName ~= "" and identity.kind == "numbered" then
          for _, workspace in ipairs(workspaces) do
            if workspace.key == identity.key and (workspace.name == "" or workspace.name == identity.key) then
              workspace.name = defaultName
              break
            end
          end
        end
      elseif identity ~= nil and rule.persistent == true then
        local ordinal = ordinals[identity.key]
        if ordinal == nil then
          ordinal = nextOrdinal
          nextOrdinal = nextOrdinal + 1
        end
        local address = identity.kind == "named" and selector:sub(6) or selector
        local defaultName = type(rule.defaultName) == "string" and rule.defaultName or ""
        table.insert(workspaces, {
          key = identity.key,
          selector = identity.selector,
          kind = identity.kind,
          number = identity.number,
          legacyId = identity.legacyId,
          name = defaultName ~= "" and defaultName or address,
          output = type(rule.monitor) == "string" and rule.monitor or "",
          ordinal = ordinal,
          runtime = false,
          active = false,
          urgent = false,
          occupied = false,
        })
        seen[identity.key] = true
      end
    end
  end
  return nextOrdinal
end

local function workspaceLess(a, b)
  if a.legacyId ~= nil and b.legacyId ~= nil and a.legacyId ~= b.legacyId then
    return a.legacyId < b.legacyId
  end
  if a.kind ~= b.kind then
    return a.kind == "named"
  end
  if a.kind == "named" then
    if a.ordinal ~= b.ordinal then
      return a.ordinal > b.ordinal
    end
    return a.key < b.key
  end
  if a.number ~= b.number then
    if a.number == nil then
      return false
    end
    if b.number == nil then
      return true
    end
    return a.number < b.number
  end
  return a.key < b.key
end

function M.buildSnapshot(workspaceRows, ruleRows, monitorRows, clientRows, previous)
  if type(workspaceRows) ~= "table" or type(monitorRows) ~= "table" or type(clientRows) ~= "table" then
    return nil, "Hyprland snapshot members must be JSON arrays"
  end

  local schema, schemaError = detectSchema(workspaceRows)
  if schema == nil then
    return nil, schemaError
  end
  if type(previous) == "table" and previous.schema ~= nil and previous.schema ~= schema then
    return nil, "Hyprland workspace identity schema changed during the session"
  end

  local ordinals, nextOrdinal = previousOrdinals(previous)
  local workspaces = {}
  local byKey = {}
  local seen = {}

  for _, row in ipairs(workspaceRows) do
    local identity = parseIdentity(row, schema)
    if identity == nil then
      return nil, "Hyprland returned a malformed workspace identity"
    end
    if identity.kind ~= "special" and identity.kind ~= "unassigned" then
      local ordinal = ordinals[identity.key]
      if ordinal == nil then
        ordinal = nextOrdinal
        nextOrdinal = nextOrdinal + 1
      end
      local workspace = {
        key = identity.key,
        selector = identity.selector,
        kind = identity.kind,
        number = identity.number,
        legacyId = identity.legacyId,
        name = type(row.name) == "string" and row.name or identity.key,
        output = type(row.monitor) == "string" and row.monitor or "",
        ordinal = ordinal,
        runtime = true,
        active = false,
        urgent = false,
        occupied = false,
      }
      table.insert(workspaces, workspace)
      byKey[workspace.key] = workspace
      seen[workspace.key] = true
    end
  end

  nextOrdinal = addPersistentRules(workspaces, seen, ruleRows, schema, ordinals, nextOrdinal)
  byKey = {}
  for _, workspace in ipairs(workspaces) do
    byKey[workspace.key] = workspace
  end

  for _, monitor in ipairs(monitorRows) do
    if type(monitor) == "table" and type(monitor.name) == "string" and type(monitor.activeWorkspace) == "table" then
      local identity = parseIdentity(monitor.activeWorkspace, schema)
      if identity == nil then
        return nil, "Hyprland returned a malformed active workspace identity"
      end
      local workspace = byKey[identity.key]
      if workspace ~= nil and workspace.output == monitor.name then
        workspace.active = true
      end
    end
  end

  for _, client in ipairs(clientRows) do
    if type(client) == "table" and type(client.workspace) == "table" then
      local identity = parseIdentity(client.workspace, schema)
      if identity == nil then
        return nil, "Hyprland returned a malformed client workspace identity"
      end
      local workspace = byKey[identity.key]
      if workspace ~= nil then
        workspace.occupied = true
        if client.urgent == true then
          workspace.urgent = true
        end
      end
    end
  end

  table.sort(workspaces, workspaceLess)
  local outputs = {}
  for _, workspace in ipairs(workspaces) do
    if workspace.output ~= "" then
      outputs[workspace.output] = outputs[workspace.output] or {}
      table.insert(outputs[workspace.output], workspace.key)
    end
  end

  return {
    schema = schema,
    workspaces = workspaces,
    outputs = outputs,
    nextOrdinal = nextOrdinal,
  }
end

function M.stateName(workspace)
  if workspace.active then
    return "active"
  elseif workspace.urgent then
    return "urgent"
  elseif workspace.occupied then
    return "occupied"
  end
  return "empty"
end

function M.forOutput(snapshot, output, hideWhenEmpty)
  local result = {}
  if type(snapshot) ~= "table" or type(snapshot.workspaces) ~= "table" or type(output) ~= "string" then
    return result
  end
  for _, workspace in ipairs(snapshot.workspaces) do
    local empty = not workspace.active and not workspace.urgent and not workspace.occupied
    if workspace.output == output and not (hideWhenEmpty and empty) then
      table.insert(result, workspace)
    end
  end
  return result
end

function M.stepTarget(snapshot, output, direction, hideWhenEmpty)
  local visible = M.forOutput(snapshot, output, hideWhenEmpty)
  local activeIndex
  for index, workspace in ipairs(visible) do
    if workspace.active then
      activeIndex = index
      break
    end
  end
  if activeIndex == nil then
    return nil, "No active visible workspace on output"
  end
  local targetIndex = activeIndex + (direction < 0 and -1 or 1)
  if targetIndex < 1 or targetIndex > #visible then
    return nil, "Workspace boundary reached"
  end
  return visible[targetIndex]
end

function M.find(snapshot, key)
  if type(snapshot) ~= "table" or type(snapshot.workspaces) ~= "table" then
    return nil
  end
  for _, workspace in ipairs(snapshot.workspaces) do
    if workspace.key == key then
      return workspace
    end
  end
  return nil
end

return M
