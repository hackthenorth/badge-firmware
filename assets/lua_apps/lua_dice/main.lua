-- Lua Dice: shake the badge (or press A) to roll.
local faces = { "1", "2", "3", "4", "5", "6" }
local die = nil
local total = nil
local rolls = 0

local function roll()
  local v = badge.sys.random(6) + 1
  rolls = rolls + 1
  die:set_text(faces[v])
  die:style({ text_font = 24 })
  total:set_text("rolls: " .. rolls)
  badge.store.set("rolls", rolls)
end

function on_enter(root)
  rolls = badge.store.get("rolls", 0)
  die = badge.ui.label(root, "?")
  die:align("center", 0, -20)
  die:style({ text_font = 24 })
  total = badge.ui.label(root, "rolls: " .. rolls)
  total:align("center", 0, 60)
  local hint = badge.ui.label(root, "shake or A to roll")
  hint:align("center", 0, 90)
end

function on_tick()
  if badge.sensor.shake() then roll() end
end

function on_button(button, kind)
  if kind == badge.input.KIND.PRESSED and button == badge.input.BUTTON.A then
    roll()
  end
end
