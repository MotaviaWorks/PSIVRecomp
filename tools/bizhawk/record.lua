-- PSIV reference recorder for BizHawk (Genesis Plus GX core).
--
-- Load the game in EmuHawk, then this script (Tools > Lua Console, or
-- `EmuHawk --lua=record.lua rom.md`). At every logical frame -- each VInt
-- that runs while the game is waiting in WaitForVInt, which is exactly where
-- the port ends a frame -- it writes one line to the trace:
--
--   n lag pad sp | ram x16 | vram x16 | cram | vsram | sound
--
--   n     logical frame number (1 = first VInt with VInt_Flag set)
--   lag   VInts since the previous logical frame that ran with VInt_Flag
--         clear (the game was still busy; only the sound driver ran)
--   pad   pad 1 buttons the game reads in this VInt: bit 0 up, 1 down,
--         2 left, 3 right, 4 A, 5 B, 6 C, 7 Start
--   sp    the main code's stack pointer (A7 + 6: the interrupt frame is
--         already pushed at VInt entry), low 16 bits in hex
--   then the first 16 hex digits of SHA-256 over each 4 KB block of 68K RAM
--   and VRAM, over CRAM (128 bytes) and over the first 80 bytes of VSRAM.
--   The RAM block holding the stack is two hashes joined by "+": its game
--   variables ($4000-$47FF) and the live stack from sp up. In between is dead
--   stack (old interrupt frames) whose exact contents depend on cycle timing.
--   (Trace v4 hashed only from sp up, missing the battle variables.)
--   The sound driver's RAM ($5000-$55FF) is left out of its block and hashed
--   on its own as "sound": its state depends on exactly when lag VInts land
--   inside the main code, which the port can only approximate.
--   VDP_Status_Reg ($EF22-$EF23) is skipped: its $E000 block hash is two
--   hashes joined by "+", before and after it.
--   Then, for analysis only, the status word VInt saved, and (trace v5)
--   "hv:" with the VDP beam-counter values the game read since the previous
--   logical frame (see HV_READ_DONE), which the port's replay reuses.
--
-- The port replays the trace (psiv_headless --replay) and stops at the first
-- logical frame whose hashes differ.
--
-- Settings (set these globals before loading, or edit here):
--   PSIV_TRACE_OUT   trace path
--   PSIV_DUMP_AT     logical frame at which to also dump raw RAM/VRAM/CRAM/VSRAM
--   PSIV_FRAMES      stop and exit after this many emulated frames (nil = run
--                    until closed, for recording your own play)
--   PSIV_SCRIPT      scripted input: list of {frame, buttons, length} in
--                    emulated frames (nil = live input)
--   PSIV_MOVIE       scripted input by logical frame: list of {logical, buttons,
--                    length} (see movie.lua); takes precedence over PSIV_SCRIPT
--   PSIV_LOGICAL_END stop after this many logical frames
-- PSIV_ROOT (project folder) is set by the entry script (newgame.lua,
-- record_play.lua) or the PSIV_ROOT environment variable.
PSIV_ROOT = PSIV_ROOT or (os.getenv("PSIV_ROOT") or "D:/Games/PSIV Project"):gsub("\\", "/")
local OUT = PSIV_TRACE_OUT or (PSIV_ROOT .. "/build/bizhawk/reference.trace")
local DUMP_AT = PSIV_DUMP_AT
local FRAMES = PSIV_FRAMES
local SCRIPT = PSIV_SCRIPT
local MOVIE = PSIV_MOVIE
local LOGICAL_END = PSIV_LOGICAL_END
-- PSIV_BOT(logical) -> keys for that logical frame, PSIV_BOT_DONE() -> stop
-- (bot.lua: the test bot plays by itself)
local BOT, BOT_DONE = PSIV_BOT, PSIV_BOT_DONE

local VINT, VINT_FLAG = 0x524, 0xEF12
-- UpdateRNGSeed2 ($4239E) mixes the VDP's beam counter into the battle random
-- numbers: `move.w $8(a5),d0` with a5 = VDP data port reads $C00008. Its value
-- depends on cycle timing, which the port doesn't reproduce, so every value
-- read is logged ("hv:" column) and the port's replay is given the same ones.
local HV_READ_DONE = 0x423A2                    -- the instruction after the read

-- Battery save. BizHawk keeps its own copy on disk (Genesis/SaveRAM, named
-- after the game) and loads it at power-on.
--   PSIV_SRAM_IN    raw image of the "SRAM" domain to start from instead
--   PSIV_SRAM_OUT   receives the domain when the run ends
-- The save the run starts with is always kept next to the trace
-- (<trace>.start.sram), so a replay can start the port from the same one.
-- Automated runs put BizHawk's own save back before exiting, so they never
-- change it; live play (record_play.lua) keeps whatever you saved, as usual.
local AUTOMATED = FRAMES or LOGICAL_END
local SRAM_IN = PSIV_SRAM_IN or os.getenv("PSIV_SRAM_IN")
local SRAM_OUT = PSIV_SRAM_OUT or os.getenv("PSIV_SRAM_OUT")
local BASE = string.gsub(OUT, "%.trace$", "")

local function sram_read()
  local n = memory.getmemorydomainsize("SRAM")
  local bytes = memory.read_bytes_as_array(0, n, "SRAM")
  local chars = {}
  for i = 1, n do chars[i] = string.char(bytes[i]) end
  return table.concat(chars)
end
local function sram_set(data)
  local n = math.min(#data, memory.getmemorydomainsize("SRAM"))
  for i = 1, n do memory.writebyte(i - 1, data:byte(i), "SRAM") end
end
local function write_file(path, data)
  local f = assert(io.open(path, "wb"))
  f:write(data)
  f:close()
end

local SRAM_BIZHAWK = sram_read()                 -- BizHawk's own save, as loaded
if SRAM_IN and SRAM_IN ~= "" then
  local f = assert(io.open(SRAM_IN, "rb"))
  sram_set(f:read("*a"))
  f:close()
end
write_file(BASE .. ".start.sram", sram_read())
local out = assert(io.open(OUT, "w"))
out:write("# psiv reference trace v5; rom " .. gameinfo.getromhash() .. "\n")
local SOUND_LO, SOUND_HI = 0x5000, 0x5600
-- VDP_Status_Reg ($EF22): VInt's raw copy of the VDP status word. The game
-- only tests its vblank bit; the sprite collision bit depends on emulator
-- details of off-screen sprite pixels, so the word is left out of the hash
local STATUS_LO, STATUS_HI = 0xEF22, 0xEF24
-- the stack (System_Stack $FFFF4FF0, growing down) shares its 4 KB block with
-- game variables up to Vehicle_Stats ($4700-$47FF); below sp down to here is
-- dead stack, whose contents depend on where interrupts landed
local STACK_DEAD_LO = 0x4800

local logical, lag = 0, 0
local hv_reads = {}
local BUTTONS = { "Up", "Down", "Left", "Right", "A", "B", "C", "Start" }

-- PSIV_WATCH = "N:PC,PC,..." (hex PCs): during the work leading up to logical
-- frame N, log the registers whenever one of these instructions is about to
-- run, to <trace>.watch.txt -- the same lines psiv_headless_trace --watch prints
local WATCH = os.getenv("PSIV_WATCH")
if WATCH and WATCH ~= "" then
  local wf = io.open(BASE .. ".watch.txt", "w")
  local n, list = WATCH:match("^(%d+):(.*)$")
  for pc in string.gmatch(list or "", "[0-9A-Fa-f]+") do
    local addr = tonumber(pc, 16)
    event.onmemoryexecute(function()
      if logical ~= tonumber(n) - 1 then return end
      local s = { string.format("watch %d $%06X", tonumber(n), addr) }
      for r = 0, 7 do s[#s + 1] = string.format("d%d=%08X", r, emu.getregister("M68K D" .. r) & 0xFFFFFFFF) end
      for r = 0, 6 do s[#s + 1] = string.format("a%d=%08X", r, emu.getregister("M68K A" .. r) & 0xFFFFFFFF) end
      s[#s + 1] = string.format("sp=%08X", emu.getregister("M68K A7") & 0xFFFFFFFF)
      wf:write(table.concat(s, " "), "\n")
      wf:flush()
    end, addr, "psiv-watch-" .. pc)
  end
end

event.onmemoryexecute(function()
  local at = (emu.getregister("M68K A5") + 8) & 0xFFFFFF
  if at >= 0xC00008 and at <= 0xC0000F then        -- the beam counter, not RAM
    hv_reads[#hv_reads + 1] = string.format("%X", emu.getregister("M68K D0") & 0xFFFF)
  end
end, HV_READ_DONE, "psiv-hv")

local function pad_byte()
  local j = joypad.get(1)
  local b = 0
  for i, name in ipairs(BUTTONS) do
    if j[name] or j["P1 " .. name] then b = b | (1 << (i - 1)) end
  end
  return b
end

local function h(domain, addr, len)
  return string.sub(memory.hash_region(addr, len, domain), 1, 16)
end

local function dump(domain, len, path)
  local f = assert(io.open(path, "wb"))
  local bytes = memory.read_bytes_as_array(0, len, domain)
  local chunk = {}
  for i = 1, len do chunk[i] = string.char(bytes[i]) end
  f:write(table.concat(chunk))
  f:close()
end

event.onmemoryexecute(function()
  if mainmemory.read_u8(VINT_FLAG) == 0 then
    lag = lag + 1
    return
  end
  logical = logical + 1
  local sp = (emu.getregister("M68K A7") + 6) & 0xFFFF
  local parts = { string.format("%d %d %d %X |", logical, lag, pad_byte(), sp) }
  for b = 0, 15 do
    local lo, hi = b * 0x1000, b * 0x1000 + 0x1000
    if lo == SOUND_LO then lo = SOUND_HI end        -- sound driver hashed separately
    if sp > lo and sp < hi then
      -- the stack's block: game variables (battle state...) up to
      -- STACK_DEAD_LO, and the live stack from sp; dead stack in between
      local var_hi = math.min(STACK_DEAD_LO, sp)
      parts[#parts + 1] = h("68K RAM", lo, var_hi - lo) .. "+" .. h("68K RAM", sp, hi - sp)
    elseif lo <= STATUS_LO and hi >= STATUS_HI then -- skip VDP_Status_Reg
      parts[#parts + 1] = h("68K RAM", lo, STATUS_LO - lo) .. "+" .. h("68K RAM", STATUS_HI, hi - STATUS_HI)
    else
      parts[#parts + 1] = h("68K RAM", lo, hi - lo)
    end
  end
  parts[#parts + 1] = "|"
  for b = 0, 15 do parts[#parts + 1] = h("VRAM", b * 0x1000, 0x1000) end
  parts[#parts + 1] = "| " .. h("CRAM", 0, 128) .. " | " .. h("VSRAM", 0, 80)
  parts[#parts + 1] = "| " .. h("68K RAM", SOUND_LO, SOUND_HI - SOUND_LO)
  -- for analysis only (not compared): the VDP status VInt saved last frame
  parts[#parts + 1] = string.format("| %04X", mainmemory.read_u16_be(0xEF22))
  -- beam-counter values the game read since the previous logical frame
  parts[#parts + 1] = "| hv:" .. (#hv_reads > 0 and table.concat(hv_reads, ",") or "-")
  out:write(table.concat(parts, " "), "\n")
  lag = 0
  hv_reads = {}
  if DUMP_AT and logical == DUMP_AT then
    dump("68K RAM", 0x10000, BASE .. ".ram.bin")
    dump("VRAM", 0x10000, BASE .. ".vram.bin")
    dump("CRAM", 128, BASE .. ".cram.bin")
    dump("VSRAM", 128, BASE .. ".vsram.bin")
  end
end, VINT, "psiv-reference")

local KEYNAMES = { U = "Up", D = "Down", L = "Left", R = "Right", A = "A", B = "B", C = "C", S = "Start" }

local function hold(keys)
  local held = {}
  for c in string.gmatch(keys, ".") do
    if KEYNAMES[c] then held["P1 " .. KEYNAMES[c]] = true end
  end
  joypad.set(held)
end

local function finish()
  out:close()
  if SRAM_OUT and SRAM_OUT ~= "" then pcall(function() write_file(SRAM_OUT, sram_read()) end) end
end

-- live play ends whenever you close EmuHawk, and exit callbacks don't run
-- reliably then, so the save is written out as it changes instead
local sram_hash
local function sram_snapshot()
  if not (SRAM_OUT and SRAM_OUT ~= "") then return end
  local hsh = memory.hash_region(0, memory.getmemorydomainsize("SRAM"), "SRAM")
  if hsh ~= sram_hash then
    write_file(SRAM_OUT, sram_read())
    sram_hash = hsh
  end
end

if AUTOMATED then
  emu.limitframerate(false)
else
  -- live play runs at normal speed until you close EmuHawk or stop the script
  emu.limitframerate(true)
  event.onexit(finish, "psiv-finish")
end
local bot_seen, bot_keys = -1, ""
while true do
  local f = emu.framecount()
  if BOT then
    -- asked once per logical frame, like psiv_headless --bot; held through lag
    if logical ~= bot_seen then
      bot_seen = logical
      bot_keys = BOT(logical + 1)
    end
    hold(bot_keys)
  elseif MOVIE then
    -- the game reads the pad in the next VInt that finds VInt_Flag set,
    -- i.e. logical frame logical+1; lag frames ignore it
    local want, keys = logical + 1, ""
    for _, m in ipairs(MOVIE) do
      if want >= m[1] and want < m[1] + m[3] then keys = keys .. m[2] end
    end
    hold(keys)
  elseif SCRIPT then
    local held = {}
    for _, s in ipairs(SCRIPT) do
      if f >= s[1] and f < s[1] + s[3] then
        for c in string.gmatch(s[2], ".") do
          local name = ({ U = "Up", D = "Down", L = "Left", R = "Right", A = "A", B = "B", C = "C", S = "Start" })[c]
          if name then held["P1 " .. name] = true end
        end
      end
    end
    joypad.set(held)
  end
  emu.frameadvance()
  out:flush()
  if not AUTOMATED and f % 30 == 0 then pcall(sram_snapshot) end
  if FRAMES and emu.framecount() >= FRAMES then break end
  if LOGICAL_END and logical >= LOGICAL_END then break end
  if BOT_DONE and BOT_DONE() then break end
end
finish()
-- leave BizHawk as we found it: its own save (it writes the save to disk on
-- exit) and normal speed (it saves its settings on exit too)
sram_set(SRAM_BIZHAWK)
emu.limitframerate(true)
client.exit()
