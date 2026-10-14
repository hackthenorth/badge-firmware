-- Lua Level: a bubble that follows the accelerometer.
local bubble = nil
local readout = nil

function on_enter(root)
  local frame = badge.ui.box(root, 200, 200)
  frame:align("center", 0, 0)
  frame:style({ border_width = 2, border_color = 0x888888, radius = 8 })
  bubble = badge.ui.box(root, 24, 24)
  bubble:style({ bg_color = 0x33ccff, radius = 12 })
  readout = badge.ui.label(root, "")
  readout:align("center", 0, 118)
end

local function clamp(v, lo, hi)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

function on_tick()
  local x, y = badge.sensor.accel()
  if not x then
    readout:set_text("no accelerometer")
    return
  end
  -- 1000 mg of tilt moves the bubble ~80 px; clamp inside the frame.
  local bx = clamp(math.floor(-x / 1000 * 80), -86, 86)
  local by = clamp(math.floor(y / 1000 * 80), -86, 86)
  bubble:align("center", bx, by)
  readout:set_text(string.format("%d mg, %d mg", math.floor(x), math.floor(y)))
end
