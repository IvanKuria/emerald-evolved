-- Companion of the ST_HYBRID stage in src/evo_test.c (EVO_TEST=1 builds). Selects the hybrid-only mode, prints the in-game
-- log, saves the screenshots the game asks for, and writes every gHybridDump the game publishes to OUT/<name>.bin.
-- Set by run.sh before this file runs:  MAILBOX (gEvoTest)  DUMP (gHybridDump)  OUT (directory)
local OFF_SEQ, OFF_PAD, OFF_FINISHED, OFF_LOGLEN, OFF_NAME, OFF_LOG, LOG_SIZE = 4, 7, 6, 8, 20, 52, 2048
local DUMP_SIZE = 4 + 2 + 2 + 32 + 32 + 4096 + 2048
local lastShot, lastDump, printed = 0, 0, 0

local function cstring(addr, max)
  local s = emu:readRange(addr, max)
  local z = s:find("\0", 1, true)
  return z and s:sub(1, z - 1) or s
end

callbacks:add("frame", function()
  emu:write8(MAILBOX + OFF_PAD, 3)   -- hybrid sprites only (read once, in Setup)
  if emu:read32(MAILBOX) ~= 0x31545645 then return end
  local len = emu:read32(MAILBOX + OFF_LOGLEN)
  while printed < len do
    local at = printed % LOG_SIZE
    local n = math.min(len - printed, LOG_SIZE - at)
    io.stdout:write(emu:readRange(MAILBOX + OFF_LOG + at, n))
    printed = printed + n
  end
  io.stdout:flush()
  local seq = emu:read16(MAILBOX + OFF_SEQ)
  if seq ~= lastShot then
    lastShot = seq
    emu:screenshot(OUT .. "/" .. cstring(MAILBOX + OFF_NAME, 32) .. ".png")
  end
  if emu:read32(DUMP) == 0x44425948 then
    local dseq = emu:read16(DUMP + 4)
    if dseq ~= lastDump then
      lastDump = dseq
      local f = io.open(OUT .. "/" .. cstring(DUMP + 8, 32) .. ".bin", "wb")
      f:write(emu:readRange(DUMP, DUMP_SIZE))
      f:close()
    end
  end
  local finished = emu:read8(MAILBOX + OFF_FINISHED)
  if finished ~= 0 then
    io.stdout:write(string.format("rom test finished at frame %d: %s\n", emu:currentFrame(), finished == 1 and "PASS" or "FAIL"))
    io.stdout:flush()
    os.exit(finished == 1 and 0 or 1)
  end
end)
