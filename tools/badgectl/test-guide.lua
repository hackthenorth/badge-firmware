-- Host-side execution of the complete guide examples with deterministic I/O.
-- This checks Lua/control flow, not the firmware's native bindings or hardware.
local file, slug = arg[1], arg[2]
local function run(available)
  local now, writes, led_shows, sends = 0, 0, 0, 0
  local saved, objects, recv = {}, {}, nil
  local leds = {}
  for i=1,6 do leds[i] = {0, 0, 0} end
  local last_card = available and {uid="01234567", sak=0, atqa=4} or nil
  local function integer(x) assert(type(x) == "number" and x % 1 == 0) end
  local function set_led(index, r, g, b)
    integer(index); assert(index >= 1 and index <= 6)
    for _,v in ipairs({r,g,b}) do integer(v); assert(v >= 0 and v <= 255) end
    leds[index] = {r,g,b}
  end
  local function set_all(r, g, b)
    for i=1,6 do set_led(i, r, g, b) end
  end
  local function object(kind)
    local obj = {kind=kind}
    function obj:set_text(text) assert(self.kind == "label" and type(text) == "string"); self.text = text end
    function obj:align(name, x, y) assert(type(name) == "string"); integer(x); integer(y) end
    function obj:style(props)
      for k in pairs(props) do
        assert(({text_font=true, text_align=true, bg_color=true, radius=true, border_color=true, border_width=true})[k], k)
      end
    end
    function obj:set_pos(x, y) integer(x); integer(y); assert(x >= 0 and y >= 0 and x < 320 and y < 240) end
    function obj:set_color(rgb) integer(rgb) end
    function obj:hidden(value) assert(type(value) == "boolean") end
    table.insert(objects, obj)
    return obj
  end
  local B = {A=1, B=2, START=3, UP=4, DOWN=5, LEFT=6, RIGHT=7}
  local badge = {
    ui = {
      label = function(_, text) local o=object("label"); o:set_text(text); return o end,
      box = function(_, w, h) integer(w); integer(h); return object("box") end,
    },
    input = {BUTTON=B, KIND={PRESSED=1, RELEASED=2}},
    sys = {ms=function() return now end, random=function(n) return n-1 end},
    store = {
      get_int=function(k, default) return saved[k] or default end,
      set_int=function(k, v) integer(v); writes=writes+1; saved[k]=v end,
    },
    led = {
      set=set_led, set_all=set_all, count=function() return 6 end,
      clear=function() set_all(0,0,0) end,
      show=function() led_shows=led_shows+1 end,
    },
    sensor = {accel=function() if available then return 100.5, -200.2, 999 end; return nil, "unavailable" end},
    nfc = {
      enable=function() return available end, disable=function() end,
      clear=function() end, card=function() return last_card end,
    },
    radio = {
      enable=function() return available end, disable=function() end,
      on_recv=function(fn) recv=fn end,
      send=function(payload) assert(#payload > 0 and #payload <= 44); sends=sends+1; return sends ~= 2 end,
    },
  }
  -- Deliberately omit io/os/package/load/pcall and other stripped globals.
  local env = {badge=badge, math=math, string=string, table=table, utf8=utf8,
               ipairs=ipairs, pairs=pairs, tostring=tostring, tonumber=tonumber, type=type}
  assert(loadfile(file, "t", env))()
  env.on_enter({})
  local function has(text)
    for _,o in ipairs(objects) do if o.text and o.text:find(text, 1, true) then return true end end
    return false
  end
  local function press(button, kind) if env.on_button then env.on_button(button, kind or 1) end end
  local function tick(ms) now=ms; if env.on_tick then env.on_tick() end end
  if slug == "demo_counter" then
    press(B.A, 2); assert(has("0"))
    press(B.A); press(B.A); press(B.B); assert(has("1")); assert(writes == 0)
  elseif slug == "demo_reaction" then
    press(B.A); press(B.A); assert(has("Too soon"))
    press(B.A); tick(3499); assert(has("Wait for green"))
    tick(3500); assert(has("GO!"))
    now=3623; press(B.A); assert(saved.best_ms == 123)
    press(B.A); tick(7123); now=7400; press(B.A); assert(saved.best_ms == 123)
    press(B.B); assert(has("A to start"))
  elseif slug == "demo_level" then
    tick(0); press(B.A); tick(100); press(B.B); tick(200)
    assert(has(available and "mg" or "unavailable"))
  elseif slug == "demo_light" then
    press(B.A); assert(has("Off")); press(B.A)
    for _=1,30 do press(B.UP); press(B.RIGHT) end
    assert(has("255"))
    for _=1,30 do press(B.DOWN); press(B.LEFT) end
    assert(has("White / 8"))
  elseif slug == "demo_led_tour" then
    local function only_lit(index)
      for i=1,6 do
        local lit = leds[i][1] + leds[i][2] + leds[i][3] > 0
        assert(lit == (i == index), "unexpected lit LED " .. i)
      end
    end
    only_lit(2); assert(has("Upper right"))
    local shows, widget_count = led_shows, #objects
    press(B.A, 2); tick(50); tick(100)
    assert(led_shows == shows, "no redraw before next chase step")
    for step,index in ipairs({3,4,5,6,1,2}) do
      tick(step * 150); only_lit(index)
    end
    shows = led_shows
    tick(10000); only_lit(3)
    assert(led_shows == shows + 1, "skip missed animation frames")
    press(B.A); assert(has("Breathe"))
    tick(11000)
    for i=1,6 do assert(leds[i][2] == 160 and leds[i][3] == 160) end
    tick(12000)
    for i=1,6 do assert(leds[i][2] == 40 and leds[i][3] == 40) end
    press(B.B); only_lit(nil); shows = led_shows
    tick(13000); assert(led_shows == shows, "off stops animation")
    press(B.B); press(B.A); assert(has("Locate"))
    press(B.LEFT); press(B.LEFT); only_lit(1); assert(has("Upper left"))
    press(B.RIGHT); only_lit(2); assert(has("Upper right"))
    for _=1,30 do press(B.DOWN) end
    assert(leds[2][2] == 32)
    for _=1,30 do press(B.UP) end
    assert(leds[2][2] == 255 and #objects == widget_count)
  elseif slug == "demo_nfc" then
    tick(0); assert(has(available and "01234567" or "unavailable"))
    press(B.A); tick(200)
  elseif slug == "demo_radio" then
    if available then
      recv("AA:BB:CC:DD:EE:FF", -42, "WRONG:hi"); assert(has("No messages"))
      recv("AA:BB:CC:DD:EE:FF", -42, "HELLO1:hi"); assert(has("1 frames"))
      press(B.A); assert(sends == 1 and has("queued"))
      press(B.A); assert(sends == 1)
      now=1000; press(B.A); assert(sends == 2 and has("Send failed"))
    else
      assert(has("Radio unavailable")); press(B.A); assert(sends == 0)
    end
  end
  local old_writes = writes
  for i=1,100 do tick(now+20) end
  assert(writes == old_writes, "tick must not write flash")
  if env.on_exit then env.on_exit() end
  for i=1,6 do
    assert(leds[i][1]+leds[i][2]+leds[i][3] == 0, "LED cleanup " .. i)
  end
  if slug == "demo_counter" then assert(saved.count == 1) end
  if slug == "demo_light" then assert(saved.color == 1 and saved.level == 8) end
  if slug == "demo_radio" then assert(recv == nil) end
end
run(true)
run(false)
print("PASS: " .. slug .. " Lua lifecycle, controls, persistence and unavailable hardware paths")
