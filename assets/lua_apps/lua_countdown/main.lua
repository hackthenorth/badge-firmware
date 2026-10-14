-- Lua Countdown: a multi-file app. The formatting lives in fmt.lua and is
-- loaded with require("fmt"), which is sandboxed to this directory.
local fmt = require("fmt")

local remaining_ms = 5 * 60 * 1000
local running = false
local last_ms = 0
local clock = nil

local function show()
  clock:set_text(fmt.clock(remaining_ms))
end

function on_enter(root)
  clock = badge.ui.label(root, "")
  clock:align("center", 0, -20)
  clock:style({ text_font = 24 })
  local hint = badge.ui.label(root, "A start/pause  Up +1m  Down -1m")
  hint:align("center", 0, 60)
  last_ms = badge.sys.ms()
  show()
end

function on_tick()
  if not running then return end
  local now = badge.sys.ms()
  remaining_ms = remaining_ms - (now - last_ms)
  last_ms = now
  if remaining_ms <= 0 then
    remaining_ms = 0
    running = false
    badge.led.set_all(0, 255, 0)
    badge.led.show()
  end
  show()
end

function on_button(button, kind)
  if kind ~= badge.input.KIND.PRESSED then return end
  local B = badge.input.BUTTON
  if button == B.A then
    running = not running
    last_ms = badge.sys.ms()
    if not running then
      badge.led.clear()
      badge.led.show()
    end
  elseif button == B.UP then
    remaining_ms = remaining_ms + 60 * 1000
    show()
  elseif button == B.DOWN then
    remaining_ms = math.max(0, remaining_ms - 60 * 1000)
    show()
  end
end

function on_exit()
  badge.led.clear()
  badge.led.show()
end
