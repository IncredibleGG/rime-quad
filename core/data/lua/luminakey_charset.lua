-- luminakey_charset.lua — 字集守門（librime 的 lua_filter）
--
-- ═══════════════════════════════════════════════════════════════════════════
--  這是什麼、為什麼在這一層
-- ═══════════════════════════════════════════════════════════════════════════
--
-- 使用者選了「簡體」就不該在候選裡看到繁體字，選了「繁體」也不該看到簡體字。
-- RIME 的簡繁開關做不到這件事：它是 opencc 的**字形轉換**，不是**字集篩選**。
-- 詞庫（luna_pinyin）收了整個 CJK 基本區 + 擴展 A/B，裡面有粵語字、日本
-- 新字体、部首、異體字 —— 那些字沒有簡繁對應，轉換再多次也還在。
--
-- 所以在轉換之後再加一層：**把不屬於當前字集的候選拿掉**。
--
-- ── 為什麼放 librime 的 filter 層，不放各端的 UI ──────────────────────────
-- engine.cc 依 schema 的順序把 filters 掛進 Menu；menu.cc 的 Menu::Prepare()
-- 是 `while (candidates_.size() < requested && !result_->exhausted())`。
-- 惰性過濾器因此會**自動補滿一頁**，翻頁與 page_size 都不會亂。
-- 放在 UI 那一側的話，引擎以為給了 5 個、畫面上只剩 2 個，翻頁還會跳號。
-- 而且四端各做一次就會做出四種結果。
--
-- ── 硬規則：一個 segment 被濾到 0 就整段退回，不過濾 ──────────────────────
-- Android 的 RimeInputMethodService 在「還在組字、但候選是空的」時會直接把
-- preedit 上屏（allowAutoCommit）。也就是說濾到 0 的下場不是「畫面上少一格」，
-- 是**使用者打 fong 得到 f-o-n-g 四個字母**。所以這裡寧可放行也不留空。
--
-- 實測（emulator-5558，luna_pinyin_tw，全音節單字掃描 841 組）：
-- 簡體有 4 組（fong / den / din / wong）、繁體有 4 組（den / din / cei / wong）
-- 會整頁被濾光。那幾組原本就只有一兩個罕用字候選（甮 / 㩐 / 𨈖 / 𥥈）。
--
-- ── 為什麼緩衝有上限 ──────────────────────────────────────────────────────
-- 「濾到 0 才退回」要知道結果是不是 0，就得把整段走完。單音節的候選可以有
-- 好幾千個，全部先跑完再吐第一個字 = 使用者按下最後一個鍵之後畫面停住。
-- 所以只在**還沒吐出任何一個**的時候緩衝，而且緩衝到 MAX_HELD 就放棄過濾
-- 這一段（前 64 個全部不合格的段落，本來就不是這一層該處理的東西）。
--
-- ── 開關 ──────────────────────────────────────────────────────────────────
-- 預設**開**。使用者在設定裡關掉時，各端送 `luminakey_charset_off = true`。
-- 用否定式命名是因為 librime 的 option 沒有「宣告過但預設為真」這種東西：
-- 沒宣告的 option 一律讀成 false，所以「預設開」只能寫成「預設沒有關」。
--
-- ⚠ 這一層**做不到絕對純度**，不要在任何地方宣稱它做得到：
--    · 字集之外還有字集。8105 收不下 蚵 砦 疋 糸，Big5 收不下 酶 礴 珏。
--      那些字會被濾掉，而它們不是「另一種字」。
--    · 濾到 0 會整段退回，那一刻使用者看到的就是不合字集的候選。
--    · 詞庫本身沒有字集約束，這一層是補救，不是解法。真正的解法是換一本
--      有字集約束的詞庫（task #27 的 rime-ice）。

local M = {}

-- 與 scripts/gen_charset_data.py 的 HAN_RANGES **必須一致**。
-- 兩邊各一份是因為一邊 Lua 一邊 Python，沒有共用的地方；
-- scripts/verify_charset_guard.sh 會比對這兩份清單，改了一邊會紅。
local HAN_RANGES = {
  { 0x3400,  0x4DBF  },   -- 擴展 A
  { 0x4E00,  0x9FFF  },   -- 基本區
  { 0xF900,  0xFAFF  },   -- 相容漢字
  { 0x20000, 0x3134F },   -- 擴展 B 以上
}

local function is_han(cp)
  for i = 1, #HAN_RANGES do
    local r = HAN_RANGES[i]
    if cp >= r[1] and cp <= r[2] then return true end
  end
  return false
end

-- 只在還沒吐出任何候選時才緩衝，上限見檔頭。
local MAX_HELD = 64

-- ── 字集載入 ──────────────────────────────────────────────────────────────
-- 資料模組是產生出來的，內容是一個長字串。這裡只把**漢字碼位**收進集合，
-- 所以資料檔裡的換行、註解外的空白都不影響結果。
local function load_set(module_name)
  local ok, data = pcall(require, module_name)
  if not ok or type(data) ~= "string" then
    -- 讀不到字集就等於沒有這一層。回 nil，呼叫端會整段放行 ——
    -- 絕對不可以在這裡回一個空集合，那會把每一個候選都濾掉。
    return nil
  end
  local set = {}
  local n = 0
  for _, cp in utf8.codes(data) do
    if is_han(cp) then
      set[cp] = true
      n = n + 1
    end
  end
  if n == 0 then return nil end
  return set
end

local CHARSETS = {}
local function charset(name)
  if CHARSETS[name] == nil then
    CHARSETS[name] = load_set("luminakey_charset_" .. name) or false
  end
  return CHARSETS[name] or nil
end

-- ── 純函式：這個字串裡的漢字是不是全都在字集裡 ────────────────────────────
-- 非漢字（標點、拉丁字母、emoji、注音符號）一律放行 —— 它們不是這一層的事。
-- utf8.codes 遇到壞的 UTF-8 會 raise，那時一律放行（寧可不濾也不要吃掉候選）。
function M.passes(text, set)
  if not set or text == nil or text == "" then return true end
  local ok, bad = pcall(function()
    for _, cp in utf8.codes(text) do
      if is_han(cp) and not set[cp] then return true end
    end
    return false
  end)
  if not ok then return true end
  return not bad
end

-- ── 純函式：現在該用哪一個字集 ────────────────────────────────────────────
-- get 是 `function(option_name) -> boolean`，測試餵一張表進來就好。
-- 順序有意義：`zh_hans` 先問，因為互斥組被弄髒時（兩支同時為真）
-- opencc 實際的輸出就是簡體 —— 判斷要跟著引擎真正在做的事，不是跟著名字。
function M.pick(get)
  if get("luminakey_charset_off") then return nil end
  if get("zh_hans") then return "hans" end
  if get("zh_hant_tw") or get("zh_hant_hk") or get("zh_hant") then return "hant" end
  -- 別的方案用 simplifier 的預設 option 名。
  if get("simplification") then return "hans" end
  return nil
end

-- ── 純函式：把一段候選走一遍，決定吐哪些出去 ──────────────────────────────
-- 抽成純函式是為了測得到：真正的 func 需要 engine、session、詞庫，
-- 也就是只有真機跑得動，而真機不在 CI 上。
--
-- pull() 每次回傳下一個候選（沒有了就回 nil），emit(c) 吐一個出去。
-- 回傳值是「這一段有沒有被過濾」，只給測試看。
function M.run(pull, emit, set)
  if not set then
    for c in pull do emit(c) end
    return false
  end
  local held = {}
  local emitted = false
  local bailed = false
  for c in pull do
    if bailed or emitted then
      if bailed or M.passes(c.text, set) then emit(c) end
    elseif M.passes(c.text, set) then
      emitted = true
      held = nil
      emit(c)
    else
      held[#held + 1] = c
      if #held >= MAX_HELD then
        -- 前 MAX_HELD 個全部不合格：放棄過濾這一段，把緩衝倒出去，
        -- 剩下的原樣放行。這是「不留空」那條規則的延伸。
        bailed = true
        for i = 1, #held do emit(held[i]) end
        held = nil
      end
    end
  end
  if not emitted and not bailed and held then
    -- 整段一個都沒過：退回不過濾（見檔頭的硬規則）。
    for i = 1, #held do emit(held[i]) end
    return false
  end
  return emitted or bailed
end

-- ── librime 的 filter 介面 ────────────────────────────────────────────────
function M.func(input, env)
  local ctx = env.engine.context
  local name = M.pick(function(opt) return ctx:get_option(opt) end)
  local set = name and charset(name) or nil
  -- ⚠ `input:iter()` 回的是**三件套**(f, s, var)，不是一個無狀態的函式。
  --   只接第一個回傳值就會拿到一個少了 self 的 f，一呼叫就是
  --   `bad argument #2 (LuaType<Translation&> expected)`，而 librime 對
  --   filter 出錯的處置是**整段候選變成空的** —— 使用者看到的是打了字沒有候選。
  --   （在 emulator-5558 上真的發生過，四個輸入全部 0 個候選。）
  local f, s, var = input:iter()
  local pull = function()
    local c = f(s, var)
    var = c
    return c
  end
  M.run(pull, function(c) yield(c) end, set)
end

return M
