-- Lua Stopwatch: A starts/stops, Up resets. Best time persists in badge.store.
local running = false
local start_ms = 0
local elapsed_ms = 0
local clock = nil
local best = nil

local function fmt(ms)
  local t = math.floor(ms / 100)
  local tenths = t % 10
  local s = math.floor(t / 10) % 60
  local m = math.floor(t / 600)
  return string.format("%02d:%02d.%d", m, s, tenths)
end

local function show()
  clock:set_text(fmt(elapsed_ms))
  best:set_text("best " .. fmt(badge.store.get("best", 0)))
end

function on_enter(root)
  clock = badge.ui.label(root, "00:00.0")
  clock:align("center", 0, -20)
  clock:style({ text_font = 24 })
  best = badge.ui.label(root, "")
  best:align("center", 0, 40)
  local hint = badge.ui.label(root, "A start/stop  Up reset")
  hint:align("center", 0, 70)
  show()
end

function on_tick()
  if running then
    elapsed_ms = badge.sys.ms() - start_ms
    show()
  end
end

function on_button(button, kind)
  if kind ~= badge.input.KIND.PRESSED then return end
  local B = badge.input.BUTTON
  if button == B.A then
    if running then
      running = false
      local prev = badge.store.get("best", 0)
      if prev == 0 or elapsed_ms < prev then
        badge.store.set("best", elapsed_ms)
      end
    else
      running = true
      start_ms = badge.sys.ms() - elapsed_ms
    end
    show()
  elseif button == B.UP then
    running = false
    elapsed_ms = 0
    badge.fs.write("last_reset.txt", "reset at " .. badge.sys.uptime() .. "s")
    show()
  end
end
