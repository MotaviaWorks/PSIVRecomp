-- Automated reference run: boot, skip the intro, start a new game, advance the
-- opening dialogue and walk a little. Records build/bizhawk/newgame.trace and
-- closes EmuHawk. Set the environment variable PSIV_DUMP_AT to also dump raw
-- memory at that logical frame (default 100). Run it through
-- tools/bizhawk/check.py, which sets PSIV_ROOT.
PSIV_ROOT = (os.getenv("PSIV_ROOT") or "D:/Games/PSIV Project"):gsub("\\", "/")
PSIV_TRACE_OUT = PSIV_ROOT .. "/build/bizhawk/newgame.trace"
PSIV_DUMP_AT = tonumber(os.getenv("PSIV_DUMP_AT") or "") or 100
PSIV_FRAMES = 2600
PSIV_SCRIPT = {
  { 300, "S", 4 }, { 470, "S", 4 }, { 610, "C", 4 }, { 700, "C", 4 }, { 780, "C", 4 },
  { 860, "C", 4 }, { 960, "C", 4 }, { 1060, "C", 4 }, { 1160, "C", 4 }, { 1260, "C", 4 },
  { 1360, "C", 4 }, { 1460, "C", 4 }, { 1600, "R", 40 }, { 1700, "D", 30 }, { 1800, "C", 4 },
  { 1900, "L", 60 }, { 2000, "C", 4 }, { 2100, "U", 40 }, { 2200, "C", 4 }, { 2300, "R", 50 },
}
local ok, err = pcall(dofile, PSIV_ROOT .. "/tools/bizhawk/record.lua")
if not ok then
  -- never leave an automated run looping in attract mode
  local f = io.open(PSIV_ROOT .. "/build/bizhawk/error.txt", "w")
  if f then f:write(tostring(err), "\n") f:close() end
  console.log("PSIV script error: " .. tostring(err))
  client.exit()
end
