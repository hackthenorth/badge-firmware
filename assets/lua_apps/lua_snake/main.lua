-- Lua Snake: arrows steer, eat to grow, A restarts after game over.
local COLS, ROWS, CELL = 12, 9, 20
local OX, OY = 40, 16

local segs = {}
local seg_widgets = {}
local dir = { x = 1, y = 0 }
local food = { x = 8, y = 4 }
local food_w = nil
local score_label = nil
local ui_root = nil
local score = 0
local over = false
local last_step = 0

local function cell_xy(c, r)
  return OX + c * CELL, OY + r * CELL
end

local function make_segment(x, y)
  local w = badge.ui.box(ui_root, CELL - 2, CELL - 2)
  w:set_pos(x + 1, y + 1)
  w:style({ bg_color = 0x44ff44, radius = 4 })
  return w
end

local function place_food()
  food.x = badge.sys.random(COLS)
  food.y = badge.sys.random(ROWS)
  local x, y = cell_xy(food.x, food.y)
  food_w:set_pos(x, y)
end

local function restart()
  for _, w in ipairs(seg_widgets) do w:delete() end
  segs = { { x = 2, y = 4 }, { x = 1, y = 4 } }
  seg_widgets = {}
  for _, s in ipairs(segs) do
    local x, y = cell_xy(s.x, s.y)
    table.insert(seg_widgets, make_segment(x, y))
  end
  dir = { x = 1, y = 0 }
  score = 0
  over = false
  score_label:set_text("score 0")
  place_food()
  last_step = badge.sys.ms()
end

function on_enter(root)
  ui_root = root
  local field = badge.ui.box(root, COLS * CELL + 4, ROWS * CELL + 4)
  field:set_pos(OX - 2, OY - 2)
  field:style({ border_width = 2, border_color = 0x888888 })
  food_w = badge.ui.box(root, CELL - 2, CELL - 2)
  food_w:style({ bg_color = 0xff4444, radius = 4 })
  score_label = badge.ui.label(root, "score 0")
  score_label:align("center", 0, 100)
  restart()
end

function on_tick()
  if over then return end
  local now = badge.sys.ms()
  if now - last_step < 180 then return end
  last_step = now

  local head = { x = segs[1].x + dir.x, y = segs[1].y + dir.y }
  if head.x < 0 or head.y < 0 or head.x >= COLS or head.y >= ROWS then
    over = true
    score_label:set_text("game over " .. score .. " (A)")
    return
  end
  for _, s in ipairs(segs) do
    if s.x == head.x and s.y == head.y then
      over = true
      score_label:set_text("game over " .. score .. " (A)")
      return
    end
  end
  table.insert(segs, 1, head)
  local hx, hy = cell_xy(head.x, head.y)
  table.insert(seg_widgets, 1, make_segment(hx, hy))
  if head.x == food.x and head.y == food.y then
    score = score + 1
    score_label:set_text("score " .. score)
    local prev = badge.store.get("best", 0)
    if score > prev then badge.store.set("best", score) end
    place_food()
  else
    table.remove(segs)
    local tail = table.remove(seg_widgets)
    tail:delete()
  end
end

function on_button(button, kind)
  if kind ~= badge.input.KIND.PRESSED then return end
  local B = badge.input.BUTTON
  if over then
    if button == B.A then restart() end
    return
  end
  if button == B.UP and dir.y == 0 then dir = { x = 0, y = -1 } end
  if button == B.DOWN and dir.y == 0 then dir = { x = 0, y = 1 } end
  if button == B.LEFT and dir.x == 0 then dir = { x = -1, y = 0 } end
  if button == B.RIGHT and dir.x == 0 then dir = { x = 1, y = 0 } end
end
