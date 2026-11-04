-- Reference sound log: play every music ID in BizHawk and log the 68000's
-- writes to the YM2612 and PSG, frame by frame.
--
-- For each ID $81-$B4: power on, at frame 30 write $FE (stop all) to
-- Sound_Index ($FF500A), at frame 32 write the ID, run PSIV_JUKE_FRAMES more
-- frames. Writes to $A04000-$A04003 (YM2612 parts I/II) and $C00011 (PSG) are
-- logged to build/bizhawk/juke_<ID>.log as "tick chip reg value", where tick
-- counts runs of the 68000 sound driver (UpdateSound, $D0008) -- the same
-- tick psiv_headless --ymlog records, so lag frames don't misalign anything.
-- Only the 68000's writes are visible here; the Z80's are not compared.
-- Run through tools/soundcheck.py --bizhawk, which sets PSIV_ROOT.
PSIV_ROOT = (os.getenv("PSIV_ROOT") or "D:/Games/PSIV Project"):gsub("\\", "/")
local FRAMES = tonumber(os.getenv("PSIV_JUKE_FRAMES") or "") or 1800
local ONLY = os.getenv("PSIV_JUKE_ID")                 -- optional single ID, hex

local function run()
  local out
  local tick = 0
  local addr = { 0, 0 }
  event.onmemoryexecute(function() tick = tick + 1 end, 0xD0008, "sound-tick")
  local function on_ym(a, v)
    local port = a - 0xA04000
    if port & 1 == 0 then
      addr[(port >> 1) + 1] = v
    elseif out then
      out:write(string.format("%d ym%d %02X %02X\n", tick, port >> 1, addr[(port >> 1) + 1], v))
    end
  end
  for p = 0, 3 do
    event.onmemorywrite(function(a, v) on_ym(a, v) end, 0xA04000 + p, "ym" .. p)
  end
  event.onmemorywrite(function(a, v)
    if out then out:write(string.format("%d psg %02X\n", tick, v)) end
  end, 0xC00011, "psg")

  -- EmuHawk was started with the ROM, so this is power-on. (Rebooting the core
  -- here would drop the write callbacks registered above.)
  emu.limitframerate(false)
  while emu.framecount() < 29 do emu.frameadvance() end
  -- every ID starts from the same frame-29 state (kept in memory, not in
  -- BizHawk's savestate slots)
  local start = memorysavestate.savecorestate()

  local first, last = 0x81, 0xB4
  if ONLY then first = tonumber(ONLY, 16); last = first end
  for id = first, last do
    memorysavestate.loadcorestate(start)
    out = assert(io.open(string.format("%s/build/bizhawk/juke_%02X.log", PSIV_ROOT, id), "w"))
    while emu.framecount() < 30 do emu.frameadvance() end
    mainmemory.write_u8(0x500A, 0xFE)
    while emu.framecount() < 32 do emu.frameadvance() end
    mainmemory.write_u8(0x500A, id)
    while emu.framecount() < 32 + FRAMES do emu.frameadvance() end
    out:close()
    out = nil
  end
end

local ok, err = pcall(run)
if not ok then
  local f = io.open(PSIV_ROOT .. "/build/bizhawk/error.txt", "w")
  if f then f:write(tostring(err), "\n") f:close() end
  console.log("PSIV script error: " .. tostring(err))
end
emu.limitframerate(true)          -- BizHawk saves its settings on exit
client.exit()
