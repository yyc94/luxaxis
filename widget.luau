--!nonstrict

local workspaceModel = require("./lib/workspace_model.luau")
local styleModel = require("./lib/style_model.luau")

local WORKSPACE_STATE_KEY = "luxaxis.workspace-state"
local STYLE_STATE_KEY = "luxaxis.styles"
local REQUEST_KEY = "luxaxis.request"

local snapshot = {
  available = false,
  interactive = false,
  workspaces = {},
  outputs = {},
}
local styles = styleModel.defaultConfig()
local hasGlobalTooltip = false

local function request(value)
  noctalia.state.set(REQUEST_KEY, value)
end

local function workspaceLabel(workspace, style)
  if style.labelSource == "custom" then
    return style.label or ""
  end
  if style.labelSource == "name" then
    return workspace.name or workspace.key
  end
  if workspace.number ~= nil then
    return tostring(workspace.number)
  end
  return workspace.selector or workspace.key
end

local function iconNode(style)
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
    return ui.glyph({
      name = style.icon,
      size = style.iconSize,
      color = style.foreground,
    })
  end
  if style.iconType == "image" and styleModel.isSupportedImage(style.icon) then
    return ui.image({
      path = style.icon,
      width = style.iconSize,
      height = style.iconSize,
      fit = "contain",
    })
  end
  return nil
end

local function workspaceNode(workspace, output, interactive)
  local state = workspaceModel.stateName(workspace)
  local style = styleModel.resolve(styles, workspace.key, state)
  local children = {}
  local icon = iconNode(style)
  if icon ~= nil then
    table.insert(children, icon)
  end
  if style.labelVisible then
    table.insert(children, ui.label({
      text = workspaceLabel(workspace, style),
      fontSize = style.fontSize,
      fontWeight = style.fontWeight,
      color = style.foreground,
      maxLines = 1,
      textAlign = "center",
    }))
  end

  local tooltip = (workspace.name or workspace.key) .. " - " .. state
  if not interactive and type(snapshot.error) == "string" and snapshot.error ~= "" then
    tooltip = tooltip .. "\n" .. snapshot.error
  end

  local key = workspace.key
  local active = workspace.active == true
  return ui.row({
    key = "workspace-" .. output .. "-" .. key,
    gap = style.contentGap,
    paddingH = style.paddingH,
    paddingV = style.paddingV,
    align = "center",
    justify = "center",
    fill = style.fill,
    border = style.border,
    borderWidth = style.borderWidth,
    radius = style.radius,
    minWidth = style.minWidth,
    minHeight = style.minHeight,
    opacity = interactive and style.opacity or style.opacity * 0.55,
    tooltip = tooltip,
    onClick = interactive and not active and function()
      request({ action = "switch", key = key, output = output })
    end or nil,
  }, children)
end

local function appendGap(children, vertical, size, index)
  if index <= 1 or size <= 0 then
    return
  end
  if vertical then
    table.insert(children, ui.spacer({ key = "gap-" .. tostring(index), height = size }))
  else
    table.insert(children, ui.spacer({ key = "gap-" .. tostring(index), width = size }))
  end
end

local function updateGlobalTooltip()
  if type(snapshot.error) == "string" and snapshot.error ~= "" then
    barWidget.setTooltip(snapshot.error)
    hasGlobalTooltip = true
  elseif hasGlobalTooltip then
    barWidget.clearTooltip()
    hasGlobalTooltip = false
  end
end

local function render()
  updateGlobalTooltip()
  local output = barWidget.outputName()
  if type(output) ~= "string" or output == "" then
    barWidget.setVisible(true)
    barWidget.render(ui.glyph({ name = "alert-triangle", color = "error" }))
    return
  end

  if snapshot.available ~= true then
    barWidget.setVisible(true)
    barWidget.render(ui.glyph({ name = "alert-triangle", color = "error" }))
    return
  end

  local hideWhenEmpty = noctalia.getConfig("hide_when_empty") == true
  local visible = workspaceModel.forOutput(snapshot, output, hideWhenEmpty)
  if #visible == 0 then
    barWidget.setVisible(false)
    return
  end

  barWidget.setVisible(true)
  local vertical = barWidget.isVertical()
  local children = {}
  for index, workspace in ipairs(visible) do
    local state = workspaceModel.stateName(workspace)
    local style = styleModel.resolve(styles, workspace.key, state)
    appendGap(children, vertical, style.itemGap, index)
    table.insert(children, workspaceNode(workspace, output, snapshot.interactive == true))
  end

  if vertical then
    barWidget.render(ui.column({ align = "center" }, children))
  else
    barWidget.render(ui.row({ align = "center" }, children))
  end
end

function update()
  render()
end

function onScroll(_axis, steps, startsGesture)
  if not startsGesture or snapshot.interactive ~= true or steps == 0 then
    return
  end
  local output = barWidget.outputName()
  if type(output) ~= "string" or output == "" then
    return
  end

  local direction = steps < 0 and -1 or 1
  if noctalia.getConfig("invert_scroll") == true then
    direction = -direction
  end
  request({
    action = "step",
    output = output,
    direction = direction,
  })
end

noctalia.state.watch(WORKSPACE_STATE_KEY, function(value)
  if type(value) == "table" then
    snapshot = value
    render()
  end
end)

noctalia.state.watch(STYLE_STATE_KEY, function(value)
  if type(value) == "table" and type(value.config) == "table" then
    styles = value.config
    render()
  end
end)

snapshot = noctalia.state.get(WORKSPACE_STATE_KEY) or snapshot
local styleState = noctalia.state.get(STYLE_STATE_KEY)
if type(styleState) == "table" and type(styleState.config) == "table" then
  styles = styleState.config
end
render()
