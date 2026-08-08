-- probe.lua — Lua 沙盒探針
--
-- 每一條都是「真的去做那件事」，不是「看某個名字在不在」。os.execute 要真的
-- 跑出一個外部行程、io.popen 要真的讀到子行程的輸出、io.open 要真的拿到
-- 檔案控制代碼 —— 沙盒之前那三件事本來就都做得到，這份探針最早的用途就是
-- 證明它們做得到。
--
-- 每一條在三個階段各有一個**期望值**：
--   stage 0 = luaL_openlibs 之後什麼都不做（上游行為）
--   stage 1 = 套上 src/lib/lua.cc 的第一層
--   stage 2 = 再套上 src/modules.cc 的第二層（路徑收斂）
--
-- stage 0 的期望值就是這份探針的**反向測試**：它要求那些事情在沒有沙盒時
-- 真的會成功。萬一沙盒字串抽取失敗變成空字串，stage 1/2 的結果會和 stage 0
-- 一樣，整批期望值立刻對不上 —— 探針不會靜靜地印一片綠。

local U = RIMEQUAD_USER
local S = RIMEQUAD_SHARED
local O = RIMEQUAD_OUTSIDE
local STAGE = RIMEQUAD_STAGE

local function opened(path, mode)
  local f, err = io.open(path, mode)
  if f then f:close() return "opened" end
  return "denied", err
end

local function is_fn(t, k) return type(t) == "table" and type(t[k]) == "function" end

local cases = {}
local function case(id, expect0, expect1, expect2, fn)
  cases[#cases + 1] = { id = id, expect = { [0] = expect0, [1] = expect1, [2] = expect2 }, fn = fn }
end

-- ═════════════════ 第一層：跳出行程 / 載入原生碼 ═════════════════

case("os.execute", "executed", "gone", "gone", function()
  if os.execute == nil then return "gone" end
  local ok, how, code = os.execute("exit 7")
  if code == 7 then return "executed" end
  return "present-but-odd(" .. tostring(how) .. "/" .. tostring(code) .. ")"
end)

case("io.popen", "executed", "gone", "gone", function()
  if io.popen == nil then return "gone" end
  local p = io.popen("printf rimequad-probe")
  if not p then return "present-but-failed" end
  local out = p:read("a")
  p:close()
  if out == "rimequad-probe" then return "executed" end
  return "present-but-odd(" .. tostring(out) .. ")"
end)

case("package.loadlib", "loaded", "gone", "gone", function()
  if package.loadlib == nil then return "gone" end
  for _, so in ipairs({ "libc.so.6", "/lib/x86_64-linux-gnu/libc.so.6",
                        "/usr/lib/x86_64-linux-gnu/libc.so.6", "libm.so.6" }) do
    local ok = package.loadlib(so, "*")
    if ok then return "loaded" end
  end
  return "present-but-failed"
end)

case("package.searchers[3][4]", "present", "gone", "gone", function()
  local a, b = package.searchers[3], package.searchers[4]
  if a == nil and b == nil then return "gone" end
  if a ~= nil and b ~= nil then return "present" end
  return "half"
end)

case("package.cpath", "set", "empty", "empty", function()
  if package.cpath == "" then return "empty" end
  return "set"
end)

case("os.remove/rename/tmpname/exit/setlocale", "present", "gone", "gone", function()
  local names = { "remove", "rename", "tmpname", "exit", "setlocale" }
  local n = 0
  for _, k in ipairs(names) do if is_fn(os, k) then n = n + 1 end end
  if n == 0 then return "gone" end
  if n == #names then return "present" end
  return "mixed(" .. n .. ")"
end)

case("io.tmpfile", "present", "gone", "gone", function()
  if is_fn(io, "tmpfile") then return "present" end
  return "gone"
end)

case("debug.getupvalue/getlocal/setmetatable", "present", "gone", "gone", function()
  local names = { "getupvalue", "getlocal", "setmetatable", "getregistry",
                  "sethook", "setupvalue", "upvalueid" }
  local n = 0
  for _, k in ipairs(names) do if is_fn(debug, k) then n = n + 1 end end
  if n == 0 then return "gone" end
  if n == #names then return "present" end
  return "mixed(" .. n .. ")"
end)

case("load(bytecode)", "loaded", "rejected", "rejected", function()
  local dumped = string.dump(function() return 1 end)
  if load(dumped, "c", "b") then return "loaded" end
  if load(dumped) then return "loaded" end            -- 不傳 mode 也要擋
  if load(dumped, "c", "bt") then return "loaded" end -- 明著要 b 也要擋
  return "rejected"
end)

-- 第一層留下來的東西 —— 雾凇/萬象真的在用，拿掉會打壞它們。
case("compat os.date/time/clock/getenv", "present", "present", "present", function()
  for _, k in ipairs({ "date", "time", "clock", "getenv", "difftime" }) do
    if not is_fn(os, k) then return "missing:" .. k end
  end
  return "present"
end)

case("compat collectgarbage/setmetatable/utf8", "present", "present", "present", function()
  if type(collectgarbage) ~= "function" then return "missing:collectgarbage" end
  if type(setmetatable) ~= "function" then return "missing:setmetatable" end
  if type(utf8) ~= "table" then return "missing:utf8" end
  return "present"
end)

case("compat debug.getinfo/traceback", "present", "present", "present", function()
  if not is_fn(debug, "getinfo") then return "missing:getinfo" end
  if not is_fn(debug, "traceback") then return "missing:traceback" end
  return "present"
end)

-- ═════════════════ 第二層：檔案系統邊界 ═════════════════

case("io.open 包裝過了", "C", "C", "Lua", function()
  local info = debug.getinfo(io.open)
  return info and info.what or "?"
end)

case("debug.getupvalue 挖 io.open", "no-escape", "gone", "gone", function()
  if not is_fn(debug, "getupvalue") then return "gone" end
  local i = 1
  while true do
    local n, v = debug.getupvalue(io.open, i)
    if n == nil then break end
    if type(v) == "function" then
      local ok, f = pcall(v, O .. "/secret.txt", "r")
      if ok and f then f:close() return "escaped" end
    end
    i = i + 1
  end
  return "no-escape"
end)

case("讀 外部檔案", "opened", "opened", "denied", function()
  return (opened(O .. "/secret.txt", "r"))
end)

case("讀 使用者目錄", "opened", "opened", "opened", function()
  return (opened(U .. "/lua/mymod.lua", "r"))
end)

case("寫 使用者目錄", "opened", "opened", "opened", function()
  return (opened(U .. "/probe-write.txt", "w"))
end)

case("讀 共用目錄", "opened", "opened", "opened", function()
  return (opened(S .. "/lua/sharedmod.lua", "r"))
end)

case("寫 共用目錄", "opened", "opened", "denied", function()
  return (opened(S .. "/probe-write.txt", "w"))
end)

case("讀 使用者目錄 r+（算寫）", "opened", "opened", "opened", function()
  return (opened(U .. "/lua/mymod.lua", "r+"))
end)

case("寫 共用目錄 a（附加）", "opened", "opened", "denied", function()
  return (opened(S .. "/probe-append.txt", "a"))
end)

case("路徑穿越 ../..", "opened", "opened", "denied", function()
  return (opened(U .. "/../../outside/secret.txt", "r"))
end)

case("路徑穿越 內嵌 ..", "opened", "opened", "denied", function()
  return (opened(U .. "/lua/../../../outside/secret.txt", "r"))
end)

case("io.lines 外部檔案", "allowed", "allowed", "denied", function()
  local ok = pcall(io.lines, O .. "/secret.txt")
  if ok then return "allowed" end
  return "denied"
end)

case("io.output 外部檔案", "allowed", "allowed", "denied", function()
  local ok = pcall(io.output, O .. "/probe-out.txt")
  pcall(io.output, io.stdout)
  if ok then return "allowed" end
  return "denied"
end)

case("io.input 外部檔案", "allowed", "allowed", "denied", function()
  local ok = pcall(io.input, O .. "/secret.txt")
  pcall(io.input, io.stdin)
  if ok then return "allowed" end
  return "denied"
end)

case("loadfile 外部檔案", "loaded", "loaded", "denied", function()
  local f = loadfile(O .. "/evil.lua")
  if f then return "loaded" end
  return "denied"
end)

case("dofile 外部檔案", "ran", "ran", "denied", function()
  local ok, v = pcall(dofile, O .. "/evil.lua")
  if ok and v == "evil-ran" then return "ran" end
  return "denied"
end)

-- 第一層只是把 package.path 清空,沒有換掉搜尋器 —— 方案自己把 package.path
-- 設回去就又能從任意位置載入程式碼。這一條的 stage 1 期望值刻意是 "loaded":
-- 它記錄的是「第一層擋不住這個」,第二層才擋得住。
case("改 package.path 再 require", "loaded", "loaded", "denied", function()
  local save = package.path
  package.path = O .. "/?.lua"
  package.loaded["evil"] = nil
  local ok, m = pcall(require, "evil")
  package.path = save
  package.loaded["evil"] = nil
  if ok and m == "evil-ran" then return "loaded" end
  return "denied"
end)

case("require 使用者目錄的模組", "denied", "denied", "loaded", function()
  package.loaded["mymod"] = nil
  local ok, m = pcall(require, "mymod")
  if ok and m == "mymod-ok" then return "loaded" end
  return "denied"
end)

case("require 帶點的子模組", "denied", "denied", "loaded", function()
  package.loaded["pkg.sub"] = nil
  local ok, m = pcall(require, "pkg.sub")
  if ok and m == "sub-ok" then return "loaded" end
  return "denied"
end)

case("require 共用目錄的模組", "denied", "denied", "loaded", function()
  package.loaded["sharedmod"] = nil
  local ok, m = pcall(require, "sharedmod")
  if ok and m == "sharedmod-ok" then return "loaded" end
  return "denied"
end)

case("require 模組名帶斜線", "denied", "denied", "denied", function()
  package.loaded["../../outside/evil"] = nil
  local ok, m = pcall(require, "../../outside/evil")
  if ok and m == "evil-ran" then return "loaded" end
  return "denied"
end)

case("沙盒標記", "none", "1", "2+confined", function()
  local m = rawget(_G, "__rimequad_sandbox")
  if type(m) ~= "table" then return "none" end
  if m.stage == 2 and m.io_confined and m.user_dir == U then return "2+confined" end
  return tostring(m.stage)
end)

case("package.path", "other", "empty", "confined", function()
  local p = package.path
  if p == "" then return "empty" end
  if p:find(U, 1, true) and p:find(S, 1, true) and not p:find("./?.lua", 1, true) then
    return "confined"
  end
  return "other"
end)

-- ═════════════════ 相容性：市集裡真的存在的 4 處 io.open ═════════════════

case("萬象 moran.lua 讀 zrmdb.txt", "opened", "opened", "opened", function()
  return (opened(U .. "/lua/zrmdb.txt"))     -- 不傳 mode = 唯讀
end)

case("萬象 桌面回退路徑 /usr/share", "denied", "denied", "denied", function()
  -- 這條在 Android 上本來就不存在（上游對 nil 有處理），第二層把它變成
  -- 明確拒絕。列在這裡是因為它是唯一一處「行為真的變了」的相容性差異。
  return (opened("/usr/share/rime-data/lua/zrmdb.txt"))
end)

case("雾凇 cold_word_drop 寫紀錄", "opened", "opened", "opened", function()
  return (opened(U .. "/lua/cold_word_drop/runLog.txt", "a"))
end)

case("雾凇 cold_word_drop 寫詞表", "opened", "opened", "opened", function()
  return (opened(U .. "/lua/cold_word_drop/drop_words.lua", "w"))
end)

-- ═════════════════ 跑 ═════════════════

local fails = 0
local ran = 0
print(("── stage %d ─────────────────────────────────────────"):format(STAGE))
for _, c in ipairs(cases) do
  local ok, got = pcall(c.fn)
  if not ok then got = "ERROR: " .. tostring(got) end
  local want = c.expect[STAGE]
  ran = ran + 1
  if got == want then
    print(("  [PASS] %-38s %s"):format(c.id, got))
  else
    fails = fails + 1
    print(("  [FAIL] %-38s 期望 %s，實際 %s"):format(c.id, tostring(want), tostring(got)))
  end
end
print(("  stage %d：%d 條，失敗 %d 條"):format(STAGE, ran, fails))

-- 條數不對就當成失敗。這一條防的是「探針被改瘦了卻沒有人發現」。
local EXPECTED_CASES = 39
if ran ~= EXPECTED_CASES then
  print(("  [FAIL] 探針條數 %d，預期 %d —— 有人加減了案例卻沒改這個數字")
        :format(ran, EXPECTED_CASES))
  fails = fails + 1
end

RIMEQUAD_FAILURES = fails
