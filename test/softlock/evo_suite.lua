-- Companion of src/realgen_test.c (REALGEN_TEST=1 builds only). Prints the in-game test log, saves the screenshots
-- the game asks for, and exits when the game reports the result. Set by rom_test.sh before this file runs:
--   MAILBOX  address of gRealgenTest     SHOTS  directory for screenshots
local OFF_SEQ, OFF_FINISHED, OFF_LOGLEN, OFF_NAME, OFF_LOG, LOG_SIZE = 4, 6, 8, 20, 52, 2048
local lastSeq, printed = 0, 0

local function cstring(addr, max)
  local s = emu:readRange(addr, max)
  local z = s:find("\0", 1, true)
  return z and s:sub(1, z - 1) or s
end

callbacks:add("frame", function()
  if emu:read32(MAILBOX) ~= 0x31545645 then return end
  local len = emu:read32(MAILBOX + OFF_LOGLEN)
  while printed < len do   -- ring buffer
    local at = printed % LOG_SIZE
    local n = math.min(len - printed, LOG_SIZE - at)
    io.stdout:write(emu:readRange(MAILBOX + OFF_LOG + at, n))
    printed = printed + n
  end
  io.stdout:flush()
  local seq = emu:read16(MAILBOX + OFF_SEQ)
  if seq ~= lastSeq then
    lastSeq = seq
    emu:screenshot(SHOTS .. "/" .. cstring(MAILBOX + OFF_NAME, 32) .. ".png")
  end
  local finished = emu:read8(MAILBOX + OFF_FINISHED)
  if finished ~= 0 then
    io.stdout:write(string.format("rom test finished at frame %d: %s\n", emu:currentFrame(), finished == 1 and "PASS" or "FAIL"))
    io.stdout:flush()
    os.exit(finished == 1 and 0 or 1)
  end
end)
