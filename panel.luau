--!nonstrict

local styleModel = require("./lib/style_model.luau")

local WORKSPACE_STATE_KEY = "luxaxis.workspace-state"
local STYLE_STATE_KEY = "luxaxis.styles"
local REQUEST_KEY = "luxaxis.request"

local SCOPE_IDS = { "base", "state", "workspace", "workspace-state" }
local SCOPE_LABELS = { "Base", "State", "Workspace", "Workspace + state" }
local STATE_IDS = { "active", "urgent", "occupied", "empty" }
local STATE_LABELS = { "Active", "Urgent", "Occupied", "Empty" }
local LABEL_SOURCE_IDS = { "id", "name", "custom" }
local LABEL_SOURCE_LABELS = { "ID", "Name", "Custom" }
local ICON_TYPE_IDS = { "none", "text", "glyph", "image" }
local ICON_TYPE_LABELS = { "None", "Text / emoji", "Glyph", "Image" }
local WEIGHT_IDS = { "thin", "light", "normal", "medium", "semibold", "bold", "heavy" }
local WEIGHT_LABELS = { "Thin", "Light", "Normal", "Medium", "Semibold", "Bold", "Heavy" }

local NUMBER_FIELDS = {
  { key = "iconSize", label = "Icon size", min = 8, max = 64, step = 1 },
  { key = "fontSize", label = "Font size", min = 8, max = 32, step = 1 },
  { key = "borderWidth", label = "Border width", min = 0, max = 8, step = 1 },
  { key = "radius", label = "Corner radius", min = 0, max = 32, step = 1 },
  { key = "paddingH", label = "Horizontal padding", min = 0, max = 24, step = 1 },
  { key = "paddingV", label = "Vertical padding", min = 0, max = 16, step = 1 },
  { key = "minWidth", label = "Minimum width", min = 0, max = 160, step = 1 },
  { key = "minHeight", label = "Minimum height", min = 0, max = 72, step = 1 },
  { key = "contentGap", label = "Content gap", min = 0, max = 20, step = 1 },
  { key = "itemGap", label = "Workspace gap", min = 0, max = 24, step = 1 },
  { key = "opacity", label = "Opacity", min = 0.1, max = 1, step = 0.05 },
}

local snapshot = { available = false, workspaces = {}, diagnostics = {} }
local styleState = { config = styleModel.defaultConfig(), revision = 0, warnings = {} }
local savedStyles = styleModel.defaultConfig()
local draft = styleModel.defaultConfig()
local dirty = false
local awaitingSave = false
local scopeIndex = 1
local stateIndex = 1
local selectedWorkspaceKey = nil
local inputRevision = 0

local render

local function request(value)
  noctalia.state.set(REQUEST_KEY, value)
end

local function indexOf(values, target)
  for index, value in ipairs(values) do
    if value == target then
      return index
    end
  end
  return 1
end

local function selectedState()
  return STATE_IDS[stateIndex] or "active"
end

local function selectedScope()
  return SCOPE_IDS[scopeIndex] or "base"
end

local function workspaceChoices()
  local labels = {}
  local keys = {}
  for _, workspace in ipairs(snapshot.workspaces or {}) do
    table.insert(keys, workspace.key)
    table.insert(labels, (workspace.output or "?") .. " / " .. (workspace.name or workspace.key))
  end
  if #keys > 0 and indexOf(keys, selectedWorkspaceKey) == 1 and keys[1] ~= selectedWorkspaceKey then
    selectedWorkspaceKey = keys[1]
  end
  return labels, keys
end

local function ensureWorkspace(key)
  if type(key) ~= "string" or key == "" then
    return nil
  end
  if type(draft.workspaces[key]) ~= "table" then
    draft.workspaces[key] = { base = {}, states = {} }
  end
  local workspace = draft.workspaces[key]
  workspace.base = type(workspace.base) == "table" and workspace.base or {}
  workspace.states = type(workspace.states) == "table" and workspace.states or {}
  for _, state in ipairs(STATE_IDS) do
    workspace.states[state] = type(workspace.states[state]) == "table" and workspace.states[state] or {}
  end
  return workspace
end

local function targetLayer(create)
  local scope = selectedScope()
  local state = selectedState()
  if scope == "base" then
    return draft.base
  end
  if scope == "state" then
    return draft.states[state]
  end
  if selectedWorkspaceKey == nil then
    return nil
  end
  local workspace = create and ensureWorkspace(selectedWorkspaceKey) or draft.workspaces[selectedWorkspaceKey]
  if type(workspace) ~= "table" then
    return nil
  end
  if scope == "workspace" then
    return workspace.base
  end
  return type(workspace.states) == "table" and workspace.states[state] or nil
end

local function editorStyle()
  local scope = selectedScope()
  if scope == "base" then
    return styleModel.resolve({ base = draft.base, states = {}, workspaces = {} }, "__preview", "__none")
  end
  if scope == "state" then
    return styleModel.resolve({ base = draft.base, states = draft.states, workspaces = {} }, "__preview", selectedState())
  end
  if scope == "workspace" then
    local workspace = selectedWorkspaceKey ~= nil and draft.workspaces[selectedWorkspaceKey] or nil
    return styleModel.resolve({
      base = draft.base,
      states = {},
      workspaces = workspace ~= nil and { [selectedWorkspaceKey] = workspace } or {},
    }, selectedWorkspaceKey or "__preview", "__none")
  end
  return styleModel.resolve(draft, selectedWorkspaceKey or "__preview", selectedState())
end

local function previewStyle()
  return styleModel.resolve(draft, selectedWorkspaceKey or "__preview", selectedState())
end

local function changeTarget()
  inputRevision = inputRevision + 1
  render()
end

local function setField(key, value)
  local layer = targetLayer(true)
  if layer == nil then
    return
  end
  layer[key] = value
  dirty = true
  render()
end

local function selectorRow(label, options, values, current, key)
  local selected = indexOf(values, current) - 1
  return ui.row({ gap = 10, align = "center" }, {
    ui.label({ text = label, color = "on_surface_variant", fontSize = 12, flexGrow = 1 }),
    ui.select({
      key = "select-" .. key .. "-" .. tostring(inputRevision),
      options = options,
      selectedIndex = selected,
      width = 220,
      onChange = function(index)
        local value = values[(tonumber(index) or 0) + 1]
        if value ~= nil then
          setField(key, value)
        end
      end,
    }),
  })
end

local function textField(label, field, value, placeholder)
  return ui.column({ gap = 5 }, {
    ui.label({ text = label, color = "on_surface_variant", fontSize = 12 }),
    ui.input({
      key = "input-" .. field .. "-" .. tostring(inputRevision),
      value = value or "",
      placeholder = placeholder or "",
      onChange = function(nextValue)
        setField(field, nextValue)
      end,
    }),
  })
end

local function numberField(definition, style)
  local value = tonumber(style[definition.key]) or definition.min
  local display = definition.key == "opacity" and string.format("%.2f", value) or tostring(math.floor(value + 0.5))
  return ui.column({ gap = 4 }, {
    ui.row({ align = "center" }, {
      ui.label({ text = definition.label, color = "on_surface_variant", fontSize = 12, flexGrow = 1 }),
      ui.label({ text = display, color = "on_surface", fontSize = 12 }),
    }),
    ui.slider({
      key = "slider-" .. definition.key,
      min = definition.min,
      max = definition.max,
      step = definition.step,
      value = value,
      onChange = function(nextValue)
        local parsed = tonumber(nextValue)
        if parsed ~= nil then
          setField(definition.key, parsed)
        end
      end,
    }),
  })
end

local function colorField(label, field, value)
  local current = value or "#00000000"
  local previewColor = styleModel.isSupportedColor(current) and current or "#00000000"
  local pickerColor = current
  if current:sub(1, 1) ~= "#" then
    local role = current:match("^([a-z][a-z0-9_]*)")
    pickerColor = role ~= nil and noctalia.getColor(role) or nil
    pickerColor = pickerColor or "#000000"
  end
  return ui.column({ gap = 5 }, {
    ui.label({ text = label, color = "on_surface_variant", fontSize = 12 }),
    ui.row({ gap = 8, align = "center" }, {
      ui.box({ width = 30, height = 30, radius = 5, fill = previewColor, border = "outline", borderWidth = 1 }),
      ui.input({
        key = "color-" .. field .. "-" .. tostring(inputRevision),
        value = current,
        flexGrow = 1,
        onChange = function(nextValue)
          setField(field, nextValue)
        end,
      }),
      ui.button({
        glyph = "color-picker",
        tooltip = "Pick " .. label:lower(),
        onClick = function()
          noctalia.openColorPicker(pickerColor, function(color)
            if color ~= nil then
              inputRevision = inputRevision + 1
              setField(field, color)
            end
          end)
        end,
      }),
    }),
  })
end

local function previewIcon(style)
  if style.iconType == "text" and style.icon ~= "" then
    return ui.label({
      text = style.icon,
      fontSize = style.iconSize,
      color = style.foreground,
      baseline = "pictographic",
      maxLines = 1,
    })
  end
  if style.iconType == "glyph" and style.icon ~= "" then
    return ui.glyph({ name = style.icon, size = style.iconSize, color = style.foreground })
  end
  if style.iconType == "image" and styleModel.isSupportedImage(style.icon) and noctalia.fileExists(style.icon) then
    return ui.image({ path = style.icon, width = style.iconSize, height = style.iconSize, fit = "contain" })
  end
  return nil
end

local function selectedWorkspace()
  for _, workspace in ipairs(snapshot.workspaces or {}) do
    if workspace.key == selectedWorkspaceKey then
      return workspace
    end
  end
  return nil
end

local function previewLabel(workspace, style)
  if style.labelSource == "custom" then
    return style.label or ""
  end
  if style.labelSource == "name" then
    return workspace ~= nil and (workspace.name or workspace.key) or "Workspace"
  end
  if workspace ~= nil then
    return workspace.number ~= nil and tostring(workspace.number) or workspace.selector
  end
  return "1"
end

local function previewBand()
  local style = previewStyle()
  local foreground = styleModel.isSupportedColor(style.foreground) and style.foreground or "on_surface"
  local fill = styleModel.isSupportedColor(style.fill) and style.fill or "#00000000"
  local border = styleModel.isSupportedColor(style.border) and style.border or "#00000000"
  style.foreground = foreground
  local workspace = selectedWorkspace()
  local children = {}
  local icon = previewIcon(style)
  if icon ~= nil then
    table.insert(children, icon)
  end
  if style.labelVisible then
    table.insert(children, ui.label({
      text = previewLabel(workspace, style),
      fontSize = style.fontSize,
      fontWeight = style.fontWeight,
      color = foreground,
      maxLines = 1,
    }))
  end
  return ui.row({
    align = "center",
    justify = "center",
    minHeight = 86,
    fill = "surface_variant/0.25",
    radius = 6,
  }, {
    ui.row({
      gap = style.contentGap,
      paddingH = style.paddingH,
      paddingV = style.paddingV,
      align = "center",
      justify = "center",
      fill = fill,
      border = border,
      borderWidth = style.borderWidth,
      radius = style.radius,
      minWidth = style.minWidth,
      minHeight = style.minHeight,
      opacity = style.opacity,
    }, children),
  })
end

local function scopeControls()
  local buttons = {}
  for index, label in ipairs(SCOPE_LABELS) do
    table.insert(buttons, ui.button({
      key = "scope-" .. SCOPE_IDS[index],
      text = label,
      selected = scopeIndex == index,
      flexGrow = 1,
      onClick = function()
        scopeIndex = index
        changeTarget()
      end,
    }))
  end

  local rows = { ui.row({ gap = 6 }, buttons) }
  local scope = selectedScope()
  if scope == "state" or scope == "workspace-state" then
    table.insert(rows, ui.row({ gap = 10, align = "center" }, {
      ui.label({ text = "State", color = "on_surface_variant", fontSize = 12, flexGrow = 1 }),
      ui.select({
        options = STATE_LABELS,
        selectedIndex = stateIndex - 1,
        width = 220,
        onChange = function(index)
          stateIndex = math.max(1, math.min(#STATE_IDS, (tonumber(index) or 0) + 1))
          changeTarget()
        end,
      }),
    }))
  end
  if scope == "workspace" or scope == "workspace-state" then
    local labels, keys = workspaceChoices()
    table.insert(rows, ui.row({ gap = 10, align = "center" }, {
      ui.label({ text = "Workspace", color = "on_surface_variant", fontSize = 12, flexGrow = 1 }),
      ui.select({
        options = labels,
        selectedIndex = math.max(0, indexOf(keys, selectedWorkspaceKey) - 1),
        placeholder = "No workspace",
        enabled = #keys > 0,
        width = 300,
        onChange = function(index)
          selectedWorkspaceKey = keys[(tonumber(index) or 0) + 1]
          changeTarget()
        end,
      }),
    }))
  end
  return ui.column({ gap = 8 }, rows)
end

local function editorBody()
  local style = editorStyle()
  local rows = {
    ui.row({ gap = 10, align = "center" }, {
      ui.label({ text = "Show label", color = "on_surface_variant", fontSize = 12, flexGrow = 1 }),
      ui.toggle({
        checked = style.labelVisible,
        onChange = function(value)
          setField("labelVisible", value == "true")
        end,
      }),
    }),
    selectorRow("Label source", LABEL_SOURCE_LABELS, LABEL_SOURCE_IDS, style.labelSource, "labelSource"),
  }

  if style.labelSource == "custom" then
    table.insert(rows, textField("Custom label", "label", style.label, "Workspace label"))
  end

  table.insert(rows, selectorRow("Icon type", ICON_TYPE_LABELS, ICON_TYPE_IDS, style.iconType, "iconType"))
  if style.iconType ~= "none" then
    local label = style.iconType == "image" and "Image path" or (style.iconType == "glyph" and "Glyph name" or "Text / emoji")
    table.insert(rows, textField(label, "icon", style.icon, ""))
  end

  if style.iconType == "image" and style.icon ~= "" then
    if styleModel.isSupportedImage(style.icon) and noctalia.fileExists(style.icon) then
      table.insert(rows, ui.image({ path = style.icon, height = 88, fit = "contain", radius = 4 }))
    else
      table.insert(rows, ui.label({
        text = "Image must be an existing PNG, JPEG, WebP, or SVG path",
        color = "error",
        fontSize = 11,
      }))
    end
  end

  table.insert(rows, selectorRow("Font weight", WEIGHT_LABELS, WEIGHT_IDS, style.fontWeight, "fontWeight"))
  table.insert(rows, ui.separator({ spacing = 4 }))
  table.insert(rows, colorField("Foreground", "foreground", style.foreground))
  table.insert(rows, colorField("Fill", "fill", style.fill))
  table.insert(rows, colorField("Border", "border", style.border))
  table.insert(rows, ui.separator({ spacing = 4 }))
  for _, definition in ipairs(NUMBER_FIELDS) do
    if definition.key ~= "iconSize" or style.iconType ~= "none" then
      table.insert(rows, numberField(definition, style))
    end
  end
  return rows
end

local function resetLayer()
  local scope = selectedScope()
  local state = selectedState()
  local defaults = styleModel.defaultConfig()
  if scope == "base" then
    draft.base = styleModel.copy(defaults.base)
  elseif scope == "state" then
    draft.states[state] = styleModel.copy(defaults.states[state])
  elseif selectedWorkspaceKey ~= nil then
    local workspace = ensureWorkspace(selectedWorkspaceKey)
    if scope == "workspace" then
      workspace.base = {}
    else
      workspace.states[state] = {}
    end
  end
  dirty = true
  changeTarget()
end

local function diagnosticsRows()
  local diagnostics = snapshot.diagnostics or {}
  local rows = {
    ui.label({ text = "Diagnostics", fontWeight = "bold", fontSize = 13 }),
    ui.label({
      text = "Schema: " .. tostring(snapshot.schema or "unknown")
        .. " | Provider: " .. tostring(diagnostics.configProvider or "unknown")
        .. " | Stream: " .. (diagnostics.eventStreamStarted and "running" or "stopped"),
      color = "on_surface_variant",
      fontSize = 11,
    }),
  }
  if type(snapshot.error) == "string" and snapshot.error ~= "" then
    table.insert(rows, ui.label({ text = snapshot.error, color = "error", fontSize = 11 }))
  end
  if type(styleState.error) == "string" and styleState.error ~= "" then
    table.insert(rows, ui.label({ text = styleState.error, color = "error", fontSize = 11 }))
  end
  for _, warning in ipairs(styleState.warnings or {}) do
    table.insert(rows, ui.label({ text = warning, color = "on_surface_variant", fontSize = 11 }))
  end
  return rows
end

local function copyDiagnostics()
  local value = {
    error = snapshot.error,
    schema = snapshot.schema,
    revision = snapshot.revision,
    diagnostics = snapshot.diagnostics,
    workspaces = snapshot.workspaces,
    styleError = styleState.error,
    styleWarnings = styleState.warnings,
  }
  local encoded = noctalia.json.encode(value, true)
  if encoded ~= nil and noctalia.copyToClipboard(encoded, "text/plain") then
    noctalia.notify("Luxaxis", "Diagnostics copied")
  end
end

render = function()
  local header = ui.row({ gap = 8, align = "center" }, {
    ui.glyph({ name = "layout-dashboard", size = 20, color = "primary" }),
    ui.label({ text = "Workspace styles", fontSize = 16, fontWeight = "bold", flexGrow = 1 }),
    ui.button({
      glyph = "refresh",
      tooltip = "Refresh Hyprland state",
      onClick = function()
        request({ action = "refresh" })
      end,
    }),
    ui.button({ glyph = "close", tooltip = "Close", onClick = "onCloseClicked" }),
  })

  local body = {
    previewBand(),
    scopeControls(),
    ui.separator({}),
  }
  for _, row in ipairs(editorBody()) do
    table.insert(body, row)
  end
  table.insert(body, ui.separator({}))
  for _, row in ipairs(diagnosticsRows()) do
    table.insert(body, row)
  end
  table.insert(body, ui.button({
    text = "Copy diagnostics",
    glyph = "clipboard",
    variant = "outline",
    onClick = copyDiagnostics,
  }))

  local footer = ui.row({ gap = 8, align = "center" }, {
    ui.button({ text = "Reset layer", variant = "ghost", onClick = resetLayer }),
    ui.spacer({ flexGrow = 1 }),
    ui.button({ text = "Cancel", variant = "outline", enabled = dirty, onClick = "onCancelClicked" }),
    ui.button({ text = awaitingSave and "Applying..." or "Apply", variant = "primary", enabled = dirty and not awaitingSave, onClick = "onApplyClicked" }),
  })

  panel.render(ui.column({ flexGrow = 1, gap = 10 }, {
    header,
    ui.scroll({ flexGrow = 1, gap = 10 }, body),
    footer,
  }))
end

function onApplyClicked()
  if not dirty or awaitingSave then
    return
  end
  awaitingSave = true
  request({ action = "save-styles", config = styleModel.copy(draft) })
  render()
end

function onCancelClicked()
  draft = styleModel.copy(savedStyles)
  dirty = false
  awaitingSave = false
  changeTarget()
end

function onCloseClicked()
  panel.close()
end

function onOpen(_context)
  snapshot = noctalia.state.get(WORKSPACE_STATE_KEY) or snapshot
  styleState = noctalia.state.get(STYLE_STATE_KEY) or styleState
  if type(styleState.config) == "table" then
    savedStyles = styleModel.copy(styleState.config)
    draft = styleModel.copy(savedStyles)
  end
  dirty = false
  awaitingSave = false
  workspaceChoices()
  inputRevision = inputRevision + 1
  request({ action = "refresh" })
  render()
end

noctalia.state.watch(WORKSPACE_STATE_KEY, function(value)
  if type(value) == "table" then
    snapshot = value
    workspaceChoices()
    render()
  end
end)

noctalia.state.watch(STYLE_STATE_KEY, function(value)
  if type(value) ~= "table" or type(value.config) ~= "table" then
    return
  end
  local revisionChanged = value.revision ~= styleState.revision
  styleState = value
  savedStyles = styleModel.copy(value.config)
  if awaitingSave then
    if type(value.error) == "string" and value.error ~= "" then
      awaitingSave = false
    elseif revisionChanged then
      draft = styleModel.copy(savedStyles)
      dirty = false
      awaitingSave = false
      inputRevision = inputRevision + 1
    end
  elseif not dirty then
    draft = styleModel.copy(savedStyles)
  end
  render()
end)
