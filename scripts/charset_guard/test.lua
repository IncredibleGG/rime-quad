-- test.lua — 字集守門的純函式測試
--
-- 由 scripts/verify_charset_guard.sh 用**真的 Lua 5.4.8**（librime-lua 出貨的
-- 那一份原始碼）跑。測的是 core/data/lua/luminakey_charset.lua 裡的純函式
-- （passes / pick / run / choose_set / puller）、filter 的進入點 M.func，
-- 以及兩份產生出來的字集資料。不需要 librime、不需要裝置。
--
-- 為什麼要抽成純函式才測得到：真正的 filter 需要 engine、session、詞庫，
-- 也就是只有真機跑得動，而真機不在 CI 上。M.func 本身靠假的 input / env
-- 與自己接管的全域 yield 測 —— 它的錯誤處理正是這一層出過事的地方。
--
-- 用法：lua test.lua <core/data/lua 的路徑>

local DATA = arg[1] or "core/data/lua"
package.path = DATA .. "/?.lua;" .. package.path

local M = require("luminakey_charset")

local pass, fail = 0, 0
local function ok(cond, name)
  if cond then
    pass = pass + 1
  else
    fail = fail + 1
    io.stderr:write("  [FAIL] " .. name .. "\n")
  end
end
local function eq(got, want, name)
  if got ~= want then
    io.stderr:write(string.format("  [FAIL] %s（得到 %s，預期 %s）\n",
      name, tostring(got), tostring(want)))
    fail = fail + 1
  else
    pass = pass + 1
  end
end

-- ── 小工具：把一份字串當字集 ──────────────────────────────────────────────
local function setof(s)
  local t = {}
  for _, cp in utf8.codes(s) do t[cp] = true end
  return t
end
local function cands(...)
  local out = {}
  for _, s in ipairs({ ... }) do out[#out + 1] = { text = s } end
  return out
end
-- 會數「被拉了幾次」的迭代器，用來驗惰性。
local function puller(list)
  local i, n = 0, 0
  return function()
    i = i + 1
    n = n + 1
    return list[i]
  end, function() return n end
end
local function collect()
  local out = {}
  return function(c) out[#out + 1] = c.text end, out
end
local function join(t) return table.concat(t, " ") end

-- ═══════════════════════════════════════════════════════════════════════
--  1. passes：這串字裡的漢字是不是全都在字集裡
-- ═══════════════════════════════════════════════════════════════════════
local S = setof("你好我它是")

ok(M.passes("你好", S), "全部在字集裡就通過")
ok(not M.passes("妳好", S), "有一個字不在字集裡就不通過")
ok(not M.passes("妳", S), "單字也一樣")
ok(M.passes("，。！", S), "標點不是漢字，一律放行")
ok(M.passes("abc123", S), "拉丁字母與數字一律放行")
ok(M.passes("ㄅㄆㄇ", S), "注音符號不是漢字，放行")
ok(M.passes("", S), "空字串放行")
ok(M.passes(nil, S), "nil 放行")
ok(M.passes("妳好", nil), "沒有字集時一律放行（讀不到資料 = 這一層不存在）")
ok(M.passes("\xff\xfe 壞掉的位元組", S), "壞的 UTF-8 一律放行，不可以吃掉候選")

-- ═══════════════════════════════════════════════════════════════════════
--  2. pick：現在該用哪一個字集
-- ═══════════════════════════════════════════════════════════════════════
local function getter(t) return function(k) return t[k] == true end end

eq(M.pick(getter({ zh_hans = true })), "hans", "zh_hans 用簡體字集")
eq(M.pick(getter({ zh_hant_tw = true })), "hant", "zh_hant_tw 用繁體字集")
eq(M.pick(getter({ zh_hant_hk = true })), "hant", "zh_hant_hk 用繁體字集")
eq(M.pick(getter({ zh_hant = true })), "hant", "zh_hant 用繁體字集")
eq(M.pick(getter({ simplification = true })), "hans", "別的方案用 simplification 這個名字")
eq(M.pick(getter({})), nil, "一支都沒開就不過濾")
eq(M.pick(getter({ zh_hans = true, luminakey_charset_off = true })), nil,
  "使用者關掉守門時，什麼都不過濾")
eq(M.pick(getter({ zh_hant_tw = true, luminakey_charset_off = true })), nil,
  "關掉守門對繁體也一樣")
-- 互斥組被弄髒（兩支同時為真）時，opencc 實際輸出的是簡體，判斷要跟著引擎走。
eq(M.pick(getter({ zh_hans = true, zh_hant_tw = true })), "hans",
  "同組兩支都開時，跟著引擎真正在輸出的那一種")

-- ═══════════════════════════════════════════════════════════════════════
--  3. run：走一遍候選，決定吐哪些
-- ═══════════════════════════════════════════════════════════════════════
do
  local p = puller(cands("你好", "我", "它"))
  local e, out = collect()
  M.run(p, e, S)
  eq(join(out), "你好 我 它", "全部合格時一個都不少")
end

do
  local p = puller(cands("你好", "妳好", "我", "祂"))
  local e, out = collect()
  M.run(p, e, S)
  eq(join(out), "你好 我", "不合格的被拿掉")
end

do
  local p = puller(cands("妳好", "你好"))
  local e, out = collect()
  M.run(p, e, S)
  eq(join(out), "你好", "排在前面的不合格候選也會被拿掉")
end

do  -- 硬規則：整段一個都沒過 → 退回不過濾
  local p = puller(cands("妳", "祂", "牠"))
  local e, out = collect()
  M.run(p, e, S)
  eq(join(out), "妳 祂 牠", "整段都不合格時整段退回，順序不變")
end

do
  local p = puller(cands("妳"))
  local e, out = collect()
  M.run(p, e, S)
  eq(join(out), "妳", "只有一個候選而且不合格時，也要退回（不可以留空）")
end

do
  local p = puller(cands())
  local e, out = collect()
  M.run(p, e, S)
  eq(#out, 0, "本來就沒有候選時不無中生有")
end

do  -- 沒有字集 = 這一層不存在
  local p = puller(cands("妳", "祂"))
  local e, out = collect()
  M.run(p, e, nil)
  eq(join(out), "妳 祂", "沒有字集時整段原樣通過")
end

do  -- 惰性：第一個就合格時，只可以拉一次
  local list = cands("你", "妳", "我")
  local p, pulled = puller(list)
  local emitted = 0
  local n = 0
  -- 只吐一個就停（模擬 Menu::Prepare 只要一個候選）
  local co = coroutine.create(function()
    M.run(p, function() emitted = emitted + 1; coroutine.yield() end, S)
  end)
  coroutine.resume(co)
  n = pulled()
  eq(emitted, 1, "只要一個候選時只吐一個")
  eq(n, 1, "第一個就合格時只拉一次 —— 過濾器必須是惰性的")
end

do  -- 惰性：前面幾個不合格時，只緩衝到第一個合格為止
  local list = cands("妳", "祂", "你", "我", "它")
  local p, pulled = puller(list)
  local emitted = 0
  local co = coroutine.create(function()
    M.run(p, function() emitted = emitted + 1; coroutine.yield() end, S)
  end)
  coroutine.resume(co)
  eq(emitted, 1, "第一個合格的被吐出來")
  eq(pulled(), 3, "只拉到第一個合格的為止，不會把整段跑完")
end

do  -- 緩衝上限：前 64 個全不合格就放棄過濾這一段
  local list = {}
  for i = 1, 64 do list[#list + 1] = { text = "妳" } end
  list[#list + 1] = { text = "你" }
  list[#list + 1] = { text = "祂" }
  local p = puller(list)
  local e, out = collect()
  M.run(p, e, S)
  eq(#out, 66, "撞到緩衝上限之後整段放行，一個都不濾")
  eq(out[1], "妳", "倒出來的順序就是原順序")
  eq(out[67], nil, "沒有多吐")
end

do  -- 63 個不合格、第 64 個合格 → 還在上限內，正常過濾
  local list = {}
  for i = 1, 63 do list[#list + 1] = { text = "妳" } end
  list[#list + 1] = { text = "你" }
  local p = puller(list)
  local e, out = collect()
  M.run(p, e, S)
  eq(join(out), "你", "還沒撞到上限就正常過濾")
end

-- ═══════════════════════════════════════════════════════════════════════
--  4. 兩份產生出來的字集資料
-- ═══════════════════════════════════════════════════════════════════════
local hans_raw = require("luminakey_charset_hans")
local hant_raw = require("luminakey_charset_hant")
local HANS, HANT = setof(hans_raw), setof(hant_raw)
local function has(set, ch) return set[utf8.codepoint(ch)] == true end
local function count(set)
  local n = 0
  for _ in pairs(set) do n = n + 1 end
  return n
end

ok(count(HANS) > 8000 and count(HANS) < 8400, "簡體字集的字數在合理範圍（8,181）")
ok(count(HANT) > 12800 and count(HANT) < 13300, "繁體字集的字數在合理範圍（13,063）")

ok(has(HANS, "你") and not has(HANS, "妳"), "簡體字集有『你』沒有『妳』——使用者回報的那一條")
ok(has(HANS, "国") and not has(HANS, "國"), "簡體字集有『国』沒有『國』")
ok(has(HANT, "國") and not has(HANT, "国"), "繁體字集有『國』沒有『国』")
ok(has(HANT, "妳"), "繁體字集有『妳』——它在台港是正字，繁體模式不該濾掉")
ok(not has(HANS, "楽") and not has(HANT, "楽"), "日本新字体兩邊都不收")
ok(not has(HANS, "嘅") and not has(HANT, "嘅"), "粵語專用字兩邊都不收")
-- 兩份字集不可以是同一份（複製貼上會過所有上面的單點測試）
local both, only_s, only_t = 0, 0, 0
for cp in pairs(HANS) do
  if HANT[cp] then both = both + 1 else only_s = only_s + 1 end
end
for cp in pairs(HANT) do
  if not HANS[cp] then only_t = only_t + 1 end
end
ok(only_s > 1000 and only_t > 4000,
  string.format("兩份字集確實不同（只在簡體 %d、只在繁體 %d、共有 %d）", only_s, only_t, both))

-- ═══════════════════════════════════════════════════════════════════════
--  5. 硬規則：**任何**錯誤都不可以讓一段候選變成 0
-- ═══════════════════════════════════════════════════════════════════════
--
-- 為什麼要有這一節：這一層被撤回過一次，原因就是「候選變成 0」，而且真正的
-- 破口不在過濾邏輯裡 —— 是模組載入與錯誤處理。上面 1–3 節全綠的同時，
-- 使用者打 nihao 得到的是 n-i-h-a-o。所以這裡測的不是「濾得對不對」，
-- 是「壞掉的時候會不會把候選吃掉」。
--
-- ⚠ M.func 用的是**全域** yield（librime-lua 註冊的 C 函式）。測試自己接管它。

-- input:iter() 回三件套；raise_at 指定第幾次拉的時候壞掉。
local function fake_input(list, raise_at)
  local i = 0
  return {
    iter = function()
      return function()
        i = i + 1
        if raise_at and i == raise_at then error("pull 壞了") end
        return list[i]
      end, nil, nil
    end,
  }
end

local function fake_env(opts)
  return { engine = { context = {
    get_option = function(_, k) return opts[k] == true end,
  } } }
end

-- 連 env.engine.context 都拿不到的環境（引擎狀態不對時真的會這樣）。
local broken_env = { engine = setmetatable({}, {
  __index = function() error("engine 壞了") end,
}) }

local function with_yield(fn)
  local out = {}
  local saved = rawget(_G, "yield")
  rawset(_G, "yield", function(c) out[#out + 1] = c.text end)
  local okc, err = pcall(fn)
  rawset(_G, "yield", saved)
  if not okc then error(err, 0) end   -- 錯誤漏到呼叫端本身就是缺陷
  return out
end

do  -- 正控：M.func 真的會過濾（否則底下每一條都是「什麼都沒做」的假綠）
  local out = with_yield(function()
    M.func(fake_input(cands("你好", "妳好", "我")), fake_env({ zh_hans = true }))
  end)
  eq(join(out), "你好 我", "M.func 走得通，而且簡體模式真的濾掉了「妳好」")
end

do  -- 決定字集那一步出錯 → 這一層不做事
  local out = with_yield(function()
    M.func(fake_input(cands("妳好", "你好", "祂")), broken_env)
  end)
  eq(join(out), "妳好 你好 祂", "問不到開關時整段原樣放行，不是變成 0")
end

do  -- 連迭代器都拿不到 → 不可以把錯誤丟給 librime（那才會讓整段變空）
  local out = with_yield(function()
    M.func({ iter = function() error("iter 壞了") end }, fake_env({ zh_hans = true }))
  end)
  eq(#out, 0, "取不到迭代器時安靜收工，不把錯誤丟出去")
end

do  -- 拉候選拉到一半壞掉：緩衝裡的必須倒出來
  local out = with_yield(function()
    M.func(fake_input(cands("妳好", "妳", "你"), 3), fake_env({ zh_hans = true }))
  end)
  eq(join(out), "妳好 妳", "拉到一半壞掉時，緩衝裡的候選不可以憑空消失")
end

do  -- 已經吐過候選之後才壞掉：吐出去的留著，錯誤不外漏
  local out = with_yield(function()
    M.func(fake_input(cands("你", "我", "它"), 3), fake_env({ zh_hans = true }))
  end)
  eq(join(out), "你 我", "壞掉之前吐出去的候選留著")
end

do  -- M.run 這一層的保證：先 emit 緩衝，再把錯誤往外丟
  local list = cands("妳", "祂")
  local i = 0
  local p = function()
    i = i + 1
    if i == 3 then error("pull 壞了") end
    return list[i]
  end
  local e, out = collect()
  local okr = pcall(M.run, p, e, S)
  eq(okr, false, "M.run 把錯誤往外丟 —— 上層要知道出過事")
  eq(join(out), "妳 祂", "但緩衝裡的候選在丟出去之前一定先 emit")
end

do  -- 沒有字集時中途壞掉：已經放行的留著
  local list = cands("妳", "祂")
  local i = 0
  local p = function()
    i = i + 1
    if i == 3 then error("pull 壞了") end
    return list[i]
  end
  local e, out = collect()
  pcall(M.run, p, e, nil)
  eq(join(out), "妳 祂", "不過濾模式下壞掉，已經放行的也留著")
end

-- ═══════════════════════════════════════════════════════════════════════
io.write(string.format("字集守門：%d 過、%d 失敗\n", pass, fail))
if fail > 0 then os.exit(1) end
