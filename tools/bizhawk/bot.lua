-- The test bot (runtime/testbot.c) in BizHawk: it plays by itself from a
-- battery save -- Continue, walk the world map back and forth until a battle,
-- fight each battle with planned commands (attacks, techniques, skills,
-- items, a macro, running, a mix), save in slot 1 -- while record.lua writes
-- the reference trace. The port then replays the trace: tools/battle_test.py
-- --bizhawk, or tools/bizhawk/check.py --bot SEED:WALK:N --sram SAVE.
--
-- Environment (check.py sets these):
--   PSIV_ROOT        project folder
--   PSIV_BOT_SPEC    SEED:WALK:N, as psiv_headless --bot
--   PSIV_TRACE_OUT   the trace; the bot's account goes next to it (.bot.txt)
-- It is the C bot line for line, so a log reads the same; the battles differ
-- anyway (BizHawk's beam counter makes its own random numbers).
PSIV_ROOT = (os.getenv("PSIV_ROOT") or "D:/Games/PSIV Project"):gsub("\\", "/")
PSIV_TRACE_OUT = os.getenv("PSIV_TRACE_OUT")

local ok, err = pcall(function()
  local spec = os.getenv("PSIV_BOT_SPEC") or "1:LR:7"
  local seed, walkspec, nb, veh = spec:match("^(%d+):?(%a*):?(%d*):?(%d*)$")
  seed = tonumber(seed) or 1
  local base = string.gsub(PSIV_TRACE_OUT or (PSIV_ROOT .. "/build/bizhawk/bot.trace"), "%.trace$", "")
  local logf = assert(io.open(base .. ".bot.txt", "w"))
  local function log(L, s) logf:write(string.format("bot %d: %s\n", L, s)) logf:flush() end

  -- PSIV_BOT_SHOTS (a folder): screenshots every 3 logical frames for a while
  -- after a gravity technique (Gra, Gigra, Nagra: the raster effects the game
  -- runs from code it writes to RAM), to hold against the port's replay
  local SHOTS = os.getenv("PSIV_BOT_SHOTS")
  local shoot_until = -1

  local function r8(a) return mainmemory.read_u8(a & 0xFFFF) end
  local function r16(a) return mainmemory.read_u16_be(a & 0xFFFF) end

  local GAME_MODE, FIELD_MAP, WINDOW_END = 0xEF00, 0xEC28, 0xEC98
  local BR2, COMD_INPUT, MAIN_OPTION, COMD_INDEX = 0x4108, 0x4106, 0x41A0, 0x41A2
  local TECH_INDEX, SKILL_INDEX, ITEM_INDEX, MACRO_INDEX = 0x41AE, 0x41BA, 0x41C4, 0x41D6
  local TECH_LIST, SKILL_LIST, ITEM_LIST = 0x412E, 0x4142, 0x4152
  local FIGHTERS, MACRO_DATA, CHAR_STATS = 0x4400, 0xF444, 0xF500
  local INVENTORY, VEHICLE = 0xF410, 0xF43C
  local CAMERA_Y, CAMERA_X = 0xEF90, 0xEF94
  local PLANS = { [0] = "attack", "tech", "skill", "item", "macro", "run", "mix" }
  local NPLANS = 7
  local CMDS = { [0] = "attack", "tech", "skill", "item", "defend" }
  local NAMES = { [0] = "Chaz", "Alys", "Hahn", "Rune", "Gryz", "Rika", "Demi", "Wren", "Raja", "Kyra", "Seth" }

  local b = {
    phase = "BOOT", battles = 0, max_battles = tonumber(nb) or NPLANS, plan = 0, round = 0,
    run_tried = false, detours = 0, t0 = 0, since = 0, a = "L", b = "R",
    hold = 0, rest = 0, taps = 0, press = "", st = 0xFFFF, mode = 0xFFFF, map0 = 0,
    cmd = {}, pick = -1, targets = 0, main = 0, macro = 0,
    saw_victory = false, saw_run = false, saw_defeat = false,
    vehicle = tonumber(veh) or 0, tries = 0, slot = -1,
  }
  if b.vehicle < 1 or b.vehicle > 3 then b.vehicle = 0 end
  b.rng = (((seed + 1) * 2654435761) & 0xFFFFFFFF) ~ 0x9E3779B9
  if b.rng == 0 then b.rng = 1 end
  if walkspec == "UD" then b.a, b.b = "U", "D" end
  if walkspec == "RL" then b.a, b.b = "R", "L" end
  if walkspec == "DU" then b.a, b.b = "D", "U" end
  local VEHICLES = { "in the Land Rover", "in the Ice Digger", "in the Hydrofoil" }
  log(0, string.format("seed %d, walk %s, %d battles%s", seed, walkspec ~= "" and walkspec or "LR", b.max_battles,
                       b.vehicle > 0 and (", " .. VEHICLES[b.vehicle]) or ""))

  local function rnd(n)
    local x = b.rng
    x = x ~ ((x << 13) & 0xFFFFFFFF)
    x = x ~ (x >> 17)
    x = x ~ ((x << 5) & 0xFFFFFFFF)
    b.rng = x
    if n == 0 then return 0 end
    return x % n
  end

  local function tap(k)
    b.press, b.hold, b.rest, b.taps = k, 1, 3, b.taps + 1
    return k
  end

  local function stats_of(f) return r16(FIGHTERS + 0x40 * f + 6) end
  local function who(f)
    local s = stats_of(f)
    if f < 0 or f > 4 or s < CHAR_STATS or s >= CHAR_STATS + 11 * 0x80 then return "?" end
    return NAMES[(s - CHAR_STATS) // 0x80]
  end
  local function equipped(f)
    local s, n = stats_of(f), 0
    if s < CHAR_STATS or s >= CHAR_STATS + 11 * 0x80 then return 0 end
    for i = 0, 3 do if r8(s + 0x4C + i) ~= 0 then n = n + 1 end end
    return n
  end

  local function begin_battle(L)
    b.phase = "BATTLE"
    b.battles = b.battles + 1
    b.plan = b.battles <= NPLANS and b.battles - 1 or 6
    b.round, b.run_tried, b.saw_victory, b.saw_run, b.saw_defeat = 0, false, false, false, false
    b.st, b.pick, b.cmd = 0xFFFF, -1, {}
    log(L, string.format("battle %d (%s)", b.battles, PLANS[b.plan]))
  end

  local function end_battle(L)
    local how = b.saw_defeat and "the party fell" or b.saw_victory and "won" or b.saw_run and "ran" or "ended"
    log(L, string.format("battle %d over after %d round%s: %s", b.battles, b.round, b.round == 1 and "" or "s", how))
    b.phase, b.t0 = "AFTER", L
  end

  local function walk(L)
    if L - b.t0 > 6000 then                       -- 100 seconds and no battle
      log(L, "no battle while walking; stopping")
      b.phase = "DONE"
      return ""
    end
    if b.mode ~= 0x0C then return "" end
    if r16(WINDOW_END) > 0x6040 then
      if (L - b.t0) % 20 == 10 then return tap("C") end
      return ""
    end
    -- blocked (a hydrofoil at the shore, a wall): the other axis, then back
    local cam = (r16(CAMERA_X) << 16) | r16(CAMERA_Y)
    if cam ~= b.cam or L - b.t0 < 2 then
      b.cam, b.moved = cam, L
    elseif L - b.moved > 48 then
      local TURN = { L = { "U", "D" }, U = { "R", "L" }, R = { "D", "U" }, D = { "L", "R" } }
      local t = TURN[b.a]
      b.a, b.b = t[1], t[2]
      b.t0, b.moved = L, L
      log(L, "blocked; walking " .. ({ U = "up", R = "right", D = "down", L = "left" })[b.a])
    end
    -- a vehicle covers twice the ground: shorter legs, or it reaches a town
    local leg = r16(VEHICLE) ~= 0 and 12 or 24
    if ((L - b.t0) // leg) % 2 == 1 then return b.b end
    return b.a
  end

  local function board(L)
    local t = L - b.t0
    local walkfor = 32 + 24 * b.tries
    if t < walkfor then
      if b.mode == 0x0C and r16(WINDOW_END) <= 0x6040 then return b.a end
      return ""
    end
    local m, pick = t - walkfor, 110 + 10 * b.slot
    if m > pick + 90 and r16(VEHICLE) ~= 0 then
      log(L, string.format("in the vehicle (%d)", r16(VEHICLE)))
      b.phase, b.t0 = "WALK", L
      return ""
    end
    if m == 10 then return tap("A") end           -- the field menu
    if m == 60 then return tap("C") end           -- ITEM
    if m >= 110 and m < pick and (m - 110) % 10 == 0 then return tap("D") end
    if m == pick + 20 then return tap("C") end    -- the vehicle: USE / LOOK / DSCD
    if m == pick + 80 then return tap("C") end    -- USE
    if m == pick + 240 then log(L, string.format("couldn't board here (try %d)", b.tries + 1)) end
    if m == pick + 240 or m == pick + 280 or m == pick + 320 or m == pick + 360 then return tap("B") end
    if m >= pick + 420 then
      b.t0 = L
      b.tries = b.tries + 1
      if b.tries >= 6 then
        log(L, "never got on; on foot then")
        b.phase = "WALK"
      end
    end
    return ""
  end

  local function save(L)
    local t = L - b.t0
    if t == 0 then return tap("S") end            -- the SYS menu
    if t == 60 then return tap("C") end           -- SAVE
    if t == 120 then return tap("C") end          -- slot 1
    if t == 200 then return tap("C") end          -- "Previous Data will be erased. OK?": YES
    if t == 380 or t == 420 or t == 460 then return tap("B") end
    if t >= 540 then
      b.phase = "DONE"
      log(L, "saved; done")
    end
    return ""
  end

  local function choose()
    if b.plan == 0 then return 0 end
    if b.plan == 1 then return 1 end
    if b.plan == 2 then return 2 end
    if b.plan == 3 then return rnd(2) ~= 0 and 3 or 0 end
    local x = rnd(100)
    if x < 30 then return 0 elseif x < 60 then return 1 elseif x < 80 then return 2 elseif x < 90 then return 3 end
    return 4
  end

  local function macro_pick()
    local c = {}
    for s = 0, 7 do
      local used = false
      for i = 0, 19 do if r8(MACRO_DATA + 20 * s + i) ~= 0 then used = true end end
      if used then c[#c + 1] = s end
    end
    if #c == 0 then return -1 end
    if #c > 1 then return c[2 + rnd(#c - 1)] end
    return c[1]
  end

  local function main_menu(L)
    if b.taps == 0 then
      b.round = b.round + 1
      b.cmd, b.pick, b.main = {}, -1, 0
      if b.plan == 5 and not b.run_tried then
        b.main, b.run_tried = 2, true
      elseif b.plan == 4 and b.round <= 2 then
        b.macro = macro_pick()
        if b.macro >= 0 then b.main = 1 end
      end
      log(L, string.format("round %d: %s", b.round, b.main == 2 and "run" or b.main == 1 and "macro" or "commands"))
    end
    if b.taps > 12 then b.main = 0 end
    local cur = r16(MAIN_OPTION) & 3
    if cur < b.main then return tap("D") end
    if cur > b.main then return tap("U") end
    return tap("C")
  end

  local function vehicle_menu(L)
    if b.taps == 0 then
      b.round = b.round + 1
      if b.plan == 5 and not b.run_tried then
        b.main, b.run_tried = 2, true
      else
        b.main = b.round % 2 == 0 and 1 or 0
      end
      log(L, string.format("round %d: vehicle %s", b.round,
                           b.main == 2 and "option 3" or b.main == 1 and "skill" or "attack"))
    end
    if b.taps > 12 then b.main = 0 end
    local cur = r16(MAIN_OPTION) & 3
    if cur < b.main then return tap("D") end
    if cur > b.main then return tap("U") end
    return tap("C")
  end

  local function command(L, f)
    if f < 0 or f > 4 then return "" end
    if b.cmd[f] == nil then b.cmd[f] = choose() end
    if b.taps > 12 then b.cmd[f] = 0 end
    local cur = r16(COMD_INDEX + 2 * f) % 5
    if cur < b.cmd[f] then return tap("R") end
    if cur > b.cmd[f] then return tap("L") end
    b.pick = -1
    b.targets = rnd(4)
    if b.cmd[f] == 0 or b.cmd[f] == 4 then log(L, string.format("  %s: %s", who(f), CMDS[b.cmd[f]])) end
    return tap("C")
  end

  local function list(L, f, st)
    local kind = st == 0x08 and 1 or st == 0x11 and 2 or 3
    if f < 0 or f > 4 then return "" end
    if b.pick < 0 then
      local c = {}
      if kind == 1 then
        for i = 0, 15 do
          local id = r8(TECH_LIST + i)
          if (id & 0x7F) ~= 0 and (id & 0x80) == 0 then c[#c + 1] = i end
        end
      elseif kind == 2 then
        for i = 0, 7 do
          local id = r8(SKILL_LIST + 2 * i)
          if (id & 0x7F) ~= 0 and (id & 0x80) == 0 and r8(SKILL_LIST + 2 * i + 1) ~= 0 then c[#c + 1] = i end
        end
      else
        for i = equipped(f), 43 do
          if r8(ITEM_LIST + i) ~= 0 then c[#c + 1] = i end
        end
      end
      if #c == 0 then
        log(L, string.format("  %s: no %s to use; attacks instead", who(f), CMDS[kind]))
        b.cmd[f] = 0
        return tap("B")
      end
      b.pick = c[1 + rnd(#c)]
    end
    if b.taps > 40 then
      log(L, string.format("  %s: can't reach %s entry %d; attacks instead", who(f), CMDS[kind], b.pick))
      b.cmd[f], b.pick = 0, -1
      return tap("B")
    end
    local base = kind == 1 and TECH_INDEX or kind == 2 and SKILL_INDEX or ITEM_INDEX
    local cur, t = r16(base + 2 * f), b.pick
    if cur >> 2 ~= t >> 2 then return tap((cur >> 2) < (t >> 2) and "R" or "L") end
    if (cur & 3) ~= (t & 3) then return tap((cur & 3) < (t & 3) and "D" or "U") end
    local id = kind == 1 and r8(TECH_LIST + t) or kind == 2 and r8(SKILL_LIST + 2 * t) or r8(ITEM_LIST + t)
    log(L, string.format("  %s: %s %02X (entry %d)", who(f), CMDS[kind], id, t))
    if kind == 1 and (id & 0x7F) >= 0x0D and (id & 0x7F) <= 0x0F then shoot_until = L + 600 end
    b.targets = rnd(4)
    return tap("C")
  end

  local function macro_menu(L)
    local cur = r16(MACRO_INDEX) & 7
    if b.taps <= 16 then
      if cur < b.macro then return tap("D") end
      if cur > b.macro then return tap("U") end
    end
    log(L, string.format("  macro %d", cur + 1))
    return tap("C")
  end

  local MESSAGES = { [0x1F] = true, [0x20] = true, [0x22] = true, [0x24] = true, [0x26] = true, [0x28] = true,
                     [0x2A] = true, [0x2D] = true, [0x2E] = true, [0x2F] = true }

  local function battle(L)
    if b.mode ~= 0x14 then return "" end
    local r2 = r16(BR2)
    local st = r2 & 0x7FFF
    if r2 ~= b.st then b.st, b.since, b.taps = r2, L, 0 end
    local still = L - b.since
    if still < 3 then return "" end
    local f = r16(COMD_INPUT)
    if st == 0x02 then return main_menu(L) end
    if st == 0x05 then return command(L, f) end
    if st == 0x08 or st == 0x11 or st == 0x17 then return list(L, f, st) end
    if st == 0x0C or st == 0x0D then
      if b.targets > 0 and b.taps < 8 then
        b.targets = b.targets - 1
        return tap("R")
      end
      -- C not taken: the game refuses that one (a Monomate for an android): the next
      if b.taps >= 6 and b.taps % 3 == 0 then return tap("R") end
      return tap("C")
    end
    if st == 0x30 then return macro_menu(L) end
    if st == 0x34 then return vehicle_menu(L) end
    if st == 0x37 then
      if b.taps == 0 then b.targets = rnd(3) end
      if b.targets > 0 and b.taps < 6 then
        b.targets = b.targets - 1
        return tap("D")
      end
      if b.taps < 8 then log(L, string.format("  vehicle skill %d", b.taps + 1)) end
      return tap("C")
    end
    if st == 0x1E or st == 0x1F then b.saw_victory = true end
    if st == 0x23 or st == 0x24 then b.saw_run = true end
    if st == 0x29 or st == 0x2A then b.saw_defeat = true end
    if MESSAGES[st] and still >= 12 then return tap("C") end
    if still >= 1200 and still % 120 == 0 then
      log(L, string.format("battle state %04X unchanged for %d frames", r2, still))
      return tap(still % 240 ~= 0 and "C" or "B")
    end
    return ""
  end

  PSIV_BOT = function(L)
    -- the picture on screen now is logical frame L - 1's (psiv_headless --shot)
    if SHOTS and L <= shoot_until and (L - 1) % 3 == 0 then
      client.screenshot(string.format("%s/%06d.png", SHOTS, L - 1))
    end
    local mode = r16(GAME_MODE)
    if mode ~= b.mode then
      b.mode = mode
      if (mode == 0x10 or mode == 0x14) and b.phase ~= "BATTLE" and b.phase ~= "BOOT" and b.phase ~= "DONE" then
        begin_battle(L)
      elseif b.phase == "BATTLE" and (mode == 0x08 or mode == 0x0C) then
        end_battle(L)
      elseif mode <= 0x04 and b.phase ~= "BOOT" and b.phase ~= "DONE" then
        log(L, "back at the title")
        b.phase = "DONE"
      elseif b.phase == "WALK" and mode == 0x08 then
        log(L, "walked into another map; turning round")
        b.a, b.b, b.t0 = b.b, b.a, L
        b.detours = b.detours + 1
        if b.detours > 3 then b.phase = "DONE" end
      end
    end
    if b.hold > 0 then
      b.hold = b.hold - 1
      return b.press
    end
    if b.rest > 0 then
      b.rest = b.rest - 1
      return ""
    end
    if b.phase == "BOOT" then
      if (L >= 290 and L < 294) or (L >= 460 and L < 464) then return "S" end
      if (L >= 560 and L < 562) or (L >= 620 and L < 622) then return "C" end
      if L >= 760 and mode == 0x0C then
        b.phase, b.t0, b.map0 = "WALK", L, r16(FIELD_MAP)
        log(L, string.format("in the field (map %X)", b.map0))
        if b.vehicle > 0 then
          b.slot = -1
          for i = 0, 39 do
            if b.slot < 0 and r8(INVENTORY + i) == 0x95 + b.vehicle then b.slot = i end
          end
          if b.slot >= 0 then b.phase = "BOARD" else log(L, "the party hasn't that vehicle") end
        end
        if b.phase == "BOARD" then return board(L) end
        return walk(L)
      end
      if L > 3000 then
        log(L, "never reached the field")
        b.phase = "DONE"
      end
      return ""
    end
    if b.phase == "BOARD" then return board(L) end
    if b.phase == "WALK" then return walk(L) end
    if b.phase == "BATTLE" then return battle(L) end
    if b.phase == "AFTER" then
      if mode == 0x0C and r16(WINDOW_END) > 0x6040 then
        if (L - b.t0) % 20 == 10 then return tap("C") end
      elseif mode == 0x0C and L - b.t0 > 60 then
        b.t0 = L
        if b.battles >= b.max_battles then
          b.phase = "SAVE"
          log(L, "saving")
          return save(L)
        end
        b.phase = "WALK"
        return walk(L)
      end
      return ""
    end
    if b.phase == "SAVE" then return save(L) end
    return ""
  end
  PSIV_BOT_DONE = function() return b.phase == "DONE" end
  PSIV_FRAMES = 200000                             -- a safety net: automated, unthrottled
  dofile(PSIV_ROOT .. "/tools/bizhawk/record.lua")
end)
if not ok then
  local f = io.open(PSIV_ROOT .. "/build/bizhawk/error.txt", "w")
  if f then f:write(tostring(err), "\n") f:close() end
  console.log("PSIV script error: " .. tostring(err))
  client.exit()
end
