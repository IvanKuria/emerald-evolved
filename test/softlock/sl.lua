-- Companion of src/sl_test.c. Set before this file: MAILBOX (address of gSlTest), SCENARIO, SHOT (png path).
-- Faces the TV, presses A, keeps pressing A while a script runs, then checks the player can walk away.
local K_A, K_B, K_UP, K_DOWN = 1, 2, 64, 128
local st, t, sawScript, idle, startY = "boot", 0, false, 0, nil
local function r8(o) return emu:read8(MAILBOX + o) end
local function r16s(o) local v = emu:read16(MAILBOX + o); if v >= 32768 then v = v - 65536 end; return v end
local function done(res, why)
  emu:screenshot(SHOT)
  io.stdout:write(string.format("RESULT scenario=%d %s %s frame=%d script=%d locked=%d field=%d follower=%d x=%d y=%d sc=%08x nat=%08x st=%06x\n",
    SCENARIO, res, why, emu:currentFrame(), r8(7), r8(8), r8(9), r8(10), r16s(12), r16s(14), emu:read32(MAILBOX+20), emu:read32(MAILBOX+24), emu:read32(MAILBOX+28)))
  io.stdout:flush(); os.exit(res == "PASS" and 0 or 1)
end
callbacks:add("frame", function()
  t = t + 1
  if st == "boot" then
    emu:write16(MAILBOX + 4, SCENARIO)
    local f = emu:currentFrame() % 40
    emu:setKeys((f < 3) and 8 or ((f >= 20 and f < 23) and K_A or 0))   -- Start / A through the intro and title
    if r8(6) >= 1 then emu:setKeys(0) end
    if emu:read32(MAILBOX) == 0x54534C53 and r8(6) == 2 then st, t = "settle", 0 end
    if emu:currentFrame() > 20000 then done("ERROR", "never-reached-field") end
  elseif st == "settle" then
    if SCENARIO >= 1024 then
      if emu:read32(MAILBOX + 44) >= 256 then
        emu:screenshot(SHOT)
        io.stdout:write(string.format("UNIT legendary_min_perfect_ivs=%d roamer_same=%d free_pal_before=%d free_pal_after=%d follower=%d\n",
          emu:read32(MAILBOX + 32), emu:read32(MAILBOX + 36), emu:read32(MAILBOX + 40), emu:read32(MAILBOX + 44) - 256, r8(10)))
        io.stdout:flush(); os.exit(0)
      end
      return
    end
    if t == 40 then io.stdout:write(string.format("settle follower=%d dbg=%d x=%d y=%d\n", r8(10), r8(11), r16s(12), r16s(14))); emu:screenshot((SHOT:gsub("%.png$", "_start.png"))) end
    if t > 40 then st, t = "face", 0 end
  elseif st == "face" then
    emu:setKeys(t <= 6 and K_UP or 0)
    if t > 30 then st, t = "talk", 0; startY = r16s(14) end
  elseif st == "talk" then
    emu:setKeys(t <= 4 and K_A or 0)
    if r8(7) == 1 then sawScript = true end
    if t > 30 then st, t = "mash", 0 end
  elseif st == "mash" then
    if DEBUGSHOTS and t % 60 == 1 and t < 1300 then emu:screenshot((SHOT:gsub("%.png$", string.format("_m%04d.png", t)))) end
    if r8(7) == 1 or r8(8) == 1 then sawScript = true; idle = 0 else idle = idle + 1 end
    -- keep the dialogue going while it runs; stop pressing once the field is free so A doesn't restart the TV
    emu:setKeys((idle == 0 and (t % 24) < 3) and K_A or 0)
    if idle > 30 then st, t = "walk", 0 end
    if t > 60 * 90 then done("FAIL", sawScript and "softlock-script-never-ended" or "no-script") end
  elseif st == "walk" then
    emu:setKeys(t <= 24 and K_DOWN or 0)
    if t > 60 then
      if r16s(14) ~= startY then done("PASS", sawScript and "ok" or "ok-noscript") else done("FAIL", "cannot-move") end
    end
  end
end)
