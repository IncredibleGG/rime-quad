-- 探針用的 rime.lua。librime-lua 的 modules.cc 會 dofile 使用者資料目錄下的
-- 這個檔案 —— 上游是在**模組初始化**就跑，套了 patch 之後改成「第一個 lua
-- 元件被建立時」才跑。這個檔案存在與否、以及它有沒有留下 probe-events.txt，
-- 就是那個差別的證據。
local self = debug.getinfo(1, "S").source:sub(2)
local dir = self:match("^(.*)[/\\][^/\\]*$") or "."
local log = dir .. "/probe-events.txt"

local function ev(s)
  local f = io.open(log, "a")
  if f then f:write(s .. "\n"); f:close() end
end

ev("rime.lua-ran")
ev("os.execute=" .. type(os.execute))
ev("io.popen=" .. type(io.popen))
ev("package.loadlib=" .. type(package.loadlib))
ev("os.remove=" .. type(os.remove))
ev("io.tmpfile=" .. type(io.tmpfile))
ev("debug.getupvalue=" .. type(debug.getupvalue))

local f = io.open("/proc/self/status", "r")
ev("read-outside=" .. (f and "opened" or "denied"))
if f then f:close() end

local g = io.open(dir .. "/from-rime-lua.txt", "w")
ev("write-user=" .. (g and "opened" or "denied"))
if g then g:close() end

local m = rawget(_G, "__rimequad_sandbox")
ev("marker=" .. tostring(m and m.stage) .. "/" .. tostring(m and m.io_confined))
