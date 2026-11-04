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
--   The RAM block holding the stack is hashed from sp up: below it is dead
--   stack (old interrupt frames) whose exact contents depend on cycle timing.
--   The sound driver's RAM ($5000-$55FF) is left out of its block and hashed
--   on its own as "sound": its state depends on exactly when lag VInts land
--   inside the main code, which the port can only approximate.
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
-- PSIV_ROOT (project folder) is set by the entry script (newgame.lua,
-- record_play.lua) or the PSIV_ROOT environment variable.
PSIV_ROOT = PSIV_ROOT or (os.getenv("PSIV_ROOT") or "D:/Games/PSIV Project"):gsub("\\", "/")
local OUT = PSIV_TRACE_OUT or (PSIV_ROOT .. "/build/bizhawk/reference.trace")
local DUMP_AT = PSIV_DUMP_AT
local FRAMES = PSIV_FRAMES
local SCRIPT = PSIV_SCRIPT

local VINT, VINT_FLAG = 0x524, 0xEF12
local out = assert(io.open(OUT, "w"))
out:write("# psiv reference trace v3; rom " .. gameinfo.getromhash() .. "\n")
local SOUND_LO, SOUND_HI = 0x5000, 0x5600

local logical, lag = 0, 0
local BUTTONS = { "Up", "Down", "Left", "Right", "A", "B", "C", "Start" }

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
    if sp > lo and sp < hi then lo = sp end         -- live stack only
    if lo == SOUND_LO then lo = SOUND_HI end        -- sound driver hashed separately
    parts[#parts + 1] = h("68K RAM", lo, hi - lo)
  end
  parts[#parts + 1] = "|"
  for b = 0, 15 do parts[#parts + 1] = h("VRAM", b * 0x1000, 0x1000) end
  parts[#parts + 1] = "| " .. h("CRAM", 0, 128) .. " | " .. h("VSRAM", 0, 80)
  parts[#parts + 1] = "| " .. h("68K RAM", SOUND_LO, SOUND_HI - SOUND_LO)
  out:write(table.concat(parts, " "), "\n")
  lag = 0
  if DUMP_AT and logical == DUMP_AT then
    local base = string.gsub(OUT, "%.trace$", "")
    dump("68K RAM", 0x10000, base .. ".ram.bin")
    dump("VRAM", 0x10000, base .. ".vram.bin")
    dump("CRAM", 128, base .. ".cram.bin")
    dump("VSRAM", 128, base .. ".vsram.bin")
  end
end, VINT, "psiv-reference")

if FRAMES then emu.limitframerate(false) end
while true do
  local f = emu.framecount()
  if SCRIPT then
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
  if FRAMES and emu.framecount() >= FRAMES then break end
end
out:close()
if FRAMES then client.exit() end
