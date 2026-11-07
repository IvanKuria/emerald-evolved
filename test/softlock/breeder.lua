-- Breeder start check (SL_TEST build). Set before: MAILBOX, BREEDER (address of gBreederStart), SHOT (png prefix).
local K_A, K_UP = 1, 64
local st, t, n = "boot", 0, 0
local function r8(o) return emu:read8(MAILBOX + o) end
local function r16(o) return emu:read16(MAILBOX + o) end
local function shot(tag) emu:screenshot(SHOT .. tag .. ".png") end
local function state()
  return string.format("map=%d.%d x=%d y=%d party=%d [%d L%d g%d] [%d L%d g%d] [%d L%d g%d] script=%d locked=%d follower=%d",
    r8(48), r8(49), r16(12), r16(14), r8(50), r16(52), r8(58), r8(61), r16(54), r8(59), r8(62), r16(56), r8(60), r8(63), r8(7), r8(8), r8(10))
end
callbacks:add("frame", function()
  t = t + 1
  if st == "boot" then
    if r8(9) == 0 then emu:write8(BREEDER, 1) end
    local f = emu:currentFrame() % 40
    emu:setKeys((f < 3) and 8 or ((f >= 20 and f < 23) and K_A or 0))
    if r8(9) == 1 then emu:setKeys(0); st, t = "intro", 0 end
  elseif st == "intro" then
    -- press A through the welcome text, the starter bag (pick + confirm) and the Eevee text
    emu:setKeys((t % 30) < 3 and K_A or 0)
    if t == 200 then shot("_starter") end
    if t > 120 and r8(7) == 0 and r8(8) == 0 and r8(9) == 1 then n = n + 1 else n = 0 end
    if n > 60 then shot("_ready"); io.stdout:write("READY " .. state() .. "\n"); st, t = "walk", 0 end
    if t > 60 * 120 then io.stdout:write("TIMEOUT " .. state() .. "\n"); shot("_timeout"); os.exit(1) end
  elseif st == "walk" then
    emu:setKeys(t <= 20 and K_UP or 0)
    if t == 200 then shot("_door"); io.stdout:write("AFTERDOOR " .. state() .. "\n"); io.stdout:flush(); os.exit(0) end
  end
end)
