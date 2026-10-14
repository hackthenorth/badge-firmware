-- Time formatting helper, loaded via sandboxed require("fmt").
local fmt = {}

function fmt.clock(ms)
  if ms < 0 then ms = 0 end
  local t = math.floor(ms / 1000)
  local s = t % 60
  local m = math.floor(t / 60)
  return string.format("%02d:%02d", m, s)
end

return fmt
