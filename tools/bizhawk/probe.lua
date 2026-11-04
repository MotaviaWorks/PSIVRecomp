-- Probe: does BizHawk's Genesis core run Lua execute callbacks, and what do
-- the memory domains and hash_region look like? Writes probe.txt next to the
-- project's build/bizhawk folder and exits.
local OUT = "D:/Games/PSIV Project/build/bizhawk/probe.txt"
local out = io.open(OUT, "w")
local function log(s)
  if out then out:write(s, "\n") out:flush() end
  console.log(s)
end

local okall, errall = pcall(function()
  log("system: " .. emu.getsystemid())
  log("rom: " .. gameinfo.getromname() .. "  hash: " .. gameinfo.getromhash())
  for _, d in ipairs(memory.getmemorydomainlist()) do
    log("domain: " .. d .. " size " .. memory.getmemorydomainsize(d))
  end

  local VINT = 0x524
  local hits, flagged = 0, 0
  local first = {}
  local ok, err = pcall(function()
    event.onmemoryexecute(function()
      hits = hits + 1
      local flag = mainmemory.read_u8(0xEF12)
      if flag ~= 0 then flagged = flagged + 1 end
      if #first < 8 then
        first[#first + 1] = string.format("frame %d flag %d", emu.framecount(), flag)
      end
    end, VINT, "vint")
  end)
  log("onmemoryexecute registered: " .. tostring(ok) .. " " .. tostring(err))

  emu.limitframerate(false)
  for _ = 1, 600 do emu.frameadvance() end

  log(string.format("after %d frames: VInt executed %d times, %d with VInt_Flag set",
    emu.framecount(), hits, flagged))
  for _, s in ipairs(first) do log("  " .. s) end
  local okh, h = pcall(memory.hash_region, 0, 0x1000, "68K RAM")
  log("hash_region(68K RAM, 0, 4096): " .. tostring(okh) .. " " .. tostring(h))
end)
if not okall then log("ERROR: " .. tostring(errall)) end
if out then out:close() end
emu.limitframerate(true)          -- BizHawk saves its settings on exit
client.exit()
