--[[
Test Lua script for hot reload functionality
Version: 1.0
]]

local version = "1.0"

print("[LUA_SCRIPT] Version: " .. version)
print("[LUA_SCRIPT] Arguments: " .. table.concat(arg, ", "))
print("[LUA_SCRIPT] Lua version: " .. _VERSION)

-- Output data
local timestamp = os.time()
local output = {
    result = "success",
    status = "ok",
    timestamp = timestamp
}

print("[LUA_SCRIPT] Output: result=success")
print("[LUA_SCRIPT] Output: status=ok")
print("[LUA_SCRIPT] Output: timestamp=" .. timestamp)

-- Simulate some work
local start = os.clock()
while os.clock() - start < 0.1 do
    -- Wait 100ms
end

print("[LUA_SCRIPT] Execution complete")
