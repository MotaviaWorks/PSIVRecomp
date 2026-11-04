-- Play an input movie (by logical frame) in BizHawk and record a reference
-- trace. Driven by tools/bizhawk/check.py --movie FILE, which sets:
--   PSIV_ROOT         project folder
--   PSIV_MOVIE_FILE   the movie (same format psiv_headless --movie reads)
--   PSIV_TRACE_OUT    where to write the trace
--   PSIV_DUMP_AT      optional logical frame for a raw memory dump
PSIV_ROOT = (os.getenv("PSIV_ROOT") or "D:/Games/PSIV Project"):gsub("\\", "/")
PSIV_TRACE_OUT = os.getenv("PSIV_TRACE_OUT")
PSIV_DUMP_AT = tonumber(os.getenv("PSIV_DUMP_AT") or "")

local function load_movie(path)
  local events, prev, last = {}, 0, 0
  for line in io.lines(path) do
    local s = line:gsub("#.*", "")
    local rel, at, keys, len = s:match("^%s*(%+?)(%d+)%s+(%S+)%s*(%d*)")
    if at then
      local start = (rel == "+" and prev or 0) + tonumber(at)
      len = tonumber(len) or 1
      events[#events + 1] = { start, keys == "-" and "" or keys, len }
      prev = start
      if start + len > last then last = start + len end
    end
  end
  return events, last
end

local ok, err = pcall(function()
  local events, last = load_movie(os.getenv("PSIV_MOVIE_FILE"))
  PSIV_MOVIE = events
  PSIV_LOGICAL_END = last + 30
  PSIV_FRAMES = PSIV_LOGICAL_END * 3 + 1200        -- safety net if the game stalls
  dofile(PSIV_ROOT .. "/tools/bizhawk/record.lua")
end)
if not ok then
  local f = io.open(PSIV_ROOT .. "/build/bizhawk/error.txt", "w")
  if f then f:write(tostring(err), "\n") f:close() end
  console.log("PSIV script error: " .. tostring(err))
  client.exit()
end
