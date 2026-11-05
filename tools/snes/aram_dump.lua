-- Dump a SNES game's sound RAM (the SPC700's 64 KB, "APURAM": the bsnes
-- core has it, Snes9x doesn't) at given frames from power-on, with a
-- screenshot each time; then quit. Run by tools/snes/sound_ram.py.
-- Env: SNES_OUT (folder), SNES_FRAMES ("600,1200,..."),
--      SNES_PRESS ("frame:Button,..." -- each held 6 frames, e.g. 900:Start)
local OUT = (os.getenv("SNES_OUT") or "."):gsub("\\", "/")
local frames, presses = {}, {}
for f in string.gmatch(os.getenv("SNES_FRAMES") or "600", "%d+") do frames[#frames + 1] = tonumber(f) end
for f, b in string.gmatch(os.getenv("SNES_PRESS") or "", "(%d+):(%a+)") do presses[#presses + 1] = {tonumber(f), b} end

local function run()
  local doms = io.open(OUT .. "/domains.txt", "w")
  for _, d in ipairs(memory.getmemorydomainlist()) do doms:write(d, " ", memory.getmemorydomainsize(d), "\n") end
  doms:close()
  emu.limitframerate(false)
  local last = frames[#frames]
  while emu.framecount() < last do
    local f = emu.framecount()
    local pad = {}
    for _, p in ipairs(presses) do if f >= p[1] and f < p[1] + 6 then pad["P1 " .. p[2]] = true end end
    joypad.set(pad)
    emu.frameadvance()
    for _, d in ipairs(frames) do
      if emu.framecount() == d then
        local t = memory.readbyterange(0, 0x10000, "APURAM")
        local o = io.open(string.format("%s/aram_%05d.bin", OUT, d), "wb")
        for i = 0, 0xFFFF do o:write(string.char(t[i] or 0)) end
        o:close()
        client.screenshot(string.format("%s/shot_%05d.png", OUT, d))
      end
    end
  end
end

local ok, err = pcall(run)
if not ok then
  local f = io.open(OUT .. "/error.txt", "w")
  if f then f:write(tostring(err), "\n") f:close() end
end
client.exit()
