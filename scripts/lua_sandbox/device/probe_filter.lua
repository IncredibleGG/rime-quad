-- 一個什麼都不改的 lua_filter。它存在的唯一目的是讓某個 schema 真的宣告
-- 一個 lua 元件 —— 「有 lua 元件的方案被選到」正是延後初始化的觸發條件。
local self = debug.getinfo(1, "S").source:sub(2)
local dir = self:match("^(.*)[/\\]lua[/\\][^/\\]*$") or "."
local f = io.open(dir .. "/probe-events.txt", "a")
if f then f:write("probe_filter-required\n"); f:close() end

return function(input, env)
  for cand in input:iter() do
    yield(cand)
  end
end
