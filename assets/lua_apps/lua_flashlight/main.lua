-- Lua Flashlight: drives all badge LEDs. A toggles, Up/Down set brightness.
local level = 255
local lit = true
local label = nil

local function apply()
  if lit then
    badge.led.set_all(level, level, level)
  else
    badge.led.clear()
  end
  badge.led.show()
  label:set_text(lit and ("brightness " .. level) or "off (A)")
end

function on_enter(root)
  label = badge.ui.label(root, "")
  label:align("center", 0, 40)
  local hint = badge.ui.label(root, "A toggle  Up/Down brightness")
  hint:align("center", 0, 70)
  apply()
end

function on_button(button, kind)
  local B = badge.input.BUTTON
  local K = badge.input.KIND
  if kind ~= K.PRESSED then return end
  if button == B.A then
    lit = not lit
    apply()
  elseif button == B.UP then
    level = math.min(255, level + 32)
    lit = true
    apply()
  elseif button == B.DOWN then
    level = math.max(8, level - 32)
    lit = true
    apply()
  end
end

function on_exit()
  badge.led.clear()
  badge.led.show()
end
