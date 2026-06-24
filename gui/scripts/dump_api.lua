-- 枚举当前 Lua 引擎注册的所有全局 API
local function dumpAPI()
    local skip = {
        _G = true, _VERSION = true, arg = true,
        coroutine = true, debug = true, io = true,
        math = true, os = true, package = true,
        string = true, table = true,
    }

    local globals = {}
    for k, v in pairs(_G) do
        if not skip[k] then
            globals[#globals + 1] = k
        end
    end
    table.sort(globals)

    for _, name in ipairs(globals) do
        local val = _G[name]
        local t = type(val)

        if t == "function" then
            log("[function] " .. name .. "()")
        elseif t == "table" then
            log("[table] " .. name)
            local keys = {}
            for k in pairs(val) do
                keys[#keys + 1] = k
            end
            table.sort(keys)
            for _, k in ipairs(keys) do
                local ft = type(val[k])
                if ft == "function" then
                    log("    ." .. k .. "()")
                else
                    log("    ." .. k .. " = " .. tostring(val[k]) .. "  [" .. ft .. "]")
                end
            end
        else
            log("[" .. t .. "] " .. name .. " = " .. tostring(val))
        end
    end
end

dumpAPI()
