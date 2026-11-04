-- Record your own play as a reference trace.
--
-- In EmuHawk: load the PSIV ROM, reboot the core (the port replays from
-- power-on, so a trace must start there too), open Tools > Lua Console and
-- open this script. Play normally; every logical frame is logged to
-- build/bizhawk/play_<date>_<time>.trace until you stop the script or close
-- EmuHawk. Then check the port against it:
--
--   python tools/bizhawk/check.py --trace build/bizhawk/play_<date>_<time>.trace
--
-- The project folder comes from the PSIV_ROOT environment variable, or the
-- default below.
PSIV_ROOT = (os.getenv("PSIV_ROOT") or "D:/Games/PSIV Project"):gsub("\\", "/")
PSIV_TRACE_OUT = PSIV_ROOT .. "/build/bizhawk/play_" .. os.date("%Y%m%d_%H%M%S") .. ".trace"
console.log("recording to " .. PSIV_TRACE_OUT)
local ok, err = pcall(dofile, PSIV_ROOT .. "/tools/bizhawk/record.lua")
if not ok then console.log("PSIV script error: " .. tostring(err)) end
