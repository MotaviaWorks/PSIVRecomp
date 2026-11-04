-- Record your own play as a reference trace.
--
-- Easiest: python tools/bizhawk/check.py --play  (opens EmuHawk with this
-- script; play, close the window, and the port replays your session).
--
-- By hand: in EmuHawk load the Phantasy Star IV ROM, open Tools > Lua Console
-- and open this script. It restarts the game from power-on itself (a trace
-- must start there, because the port replays from reset), then you play at
-- normal speed. Every logical frame is logged to
-- build/bizhawk/play_<date>_<time>.trace until you close EmuHawk or stop the
-- script. The battery save the game started with is kept next to it
-- (.start.sram) and the one it ended with too (.bizhawk.sram), so Continue
-- works and saving is checked. Then check the port against it:
--
--   python tools/bizhawk/check.py --trace build/bizhawk/play_<date>_<time>.trace
--
-- The project folder comes from the PSIV_ROOT environment variable, or the
-- default below.
PSIV_ROOT = (os.getenv("PSIV_ROOT") or "D:/Games/PSIV Project"):gsub("\\", "/")
PSIV_TRACE_OUT = os.getenv("PSIV_TRACE_OUT")
if not PSIV_TRACE_OUT or PSIV_TRACE_OUT == "" then
  PSIV_TRACE_OUT = PSIV_ROOT .. "/build/bizhawk/play_" .. os.date("%Y%m%d_%H%M%S") .. ".trace"
end
PSIV_TRACE_OUT = PSIV_TRACE_OUT:gsub("\\", "/")
PSIV_SRAM_OUT = (string.gsub(PSIV_TRACE_OUT, "%.trace$", "")) .. ".bizhawk.sram"
PSIV_SRAM_IN = ""                    -- live play starts from BizHawk's own save

-- reboot first: rebooting the core drops Lua memory callbacks, so the
-- recorder must install its hooks afterwards
client.reboot_core()
console.log("recording to " .. PSIV_TRACE_OUT)
local ok, err = pcall(dofile, PSIV_ROOT .. "/tools/bizhawk/record.lua")
if not ok then console.log("PSIV script error: " .. tostring(err)) end
