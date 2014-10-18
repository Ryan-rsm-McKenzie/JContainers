

setmetatable(_G, {
  __newindex = function(t, k, v)
    assert(not t[k], "it's not ok to override global variables")
    rawset(t, k, v)
  end
})


-- JC will supplies these paths
local JCDataPath, JCDllPath = ...

-- Setup globals
do
  package.path = ';' .. JCDataPath .. [[InternalLuaScripts\?.lua;]]

  JConstants = {
    DataPath = JCDataPath,
    HeaderPath = JCDataPath .. [[InternalLuaScripts\api_for_lua.h]],
    DllPath = JCDllPath,
  }

  math.randomseed(os.time())
end

-- that's really stupid that there is no way to pass extra arguments into 'require' funtion
local jc = require('jc')


-- test functionality
jc.testJC()



local function copyLuaTable(table, copy_into)
  local copy = copy_into or {}
  for k,v in pairs(table) do
    copy[k] = v
  end
  
  setmetatable(copy, getmetatable(table))
  return copy
end

------------------------------------

-- SANDBOXING

--[[
Problems? ^^

1.  Prevent dumb declaring of global variables in JValue.eval*.
1.5 Prevent global variables in user's scripts
2.  Prevent from loading native dlls

--]]

-- JValue.evalLua* sandbox
do

  -- all JValue.evalLua* scripts sharing the one, immutable sandbox
  local sandbox = {

    JArray = jc.JArray,
    JFormMap = jc.JFormMap,
    JMap = jc.JMap,
    JValue = jc.JValue,
    jc = jc,

    -- some standard lua modules and functions
    math = math,
    io = io,
    string = string,
    table = table,
        
    --require = nil,
    pairs = pairs,
    ipairs = ipairs,
    next = next,
    error = error,
    tonumber = tonumber,
    tostring = tostring,
    type = type,
    next = next,
  }

  -- cached results of module execution
  local user_modules = {}

  -- an alternative to standard 'require' function
  local function jc_require (s)
    local mod = user_modules[s]
    if not mod then
      local str = string.gsub(s, '.', [[\]])
      local f, message = loadfile (JCDataPath .. [[lua\]] .. str .. '.lua')
      if not f then error(message) end   
      setfenv(f, sandbox)
      user_modules[s] = f()
    end
    return mod
  end

  sandbox.require = jc_require

  setmetatable(sandbox, {
    __newindex = function(_, _, _) error("attempt to modify script's sandbox") end,
  })

  
  -- Any unknown global variable is this sandbox is treated as link to a module - and __index tries find that module
  local sandbox_2 = copyLuaTable(sandbox)
  setmetatable(sandbox_2, {
    __index = function(self, key) return jc_require(key .. '.init') end,
    __newindex = function(_, _, _) error("attempt to modify script's sandbox") end,
  })

  -- Caches compiled JValue.evalLua* string (weak cache)
  local jc_function_cache = {}
  setmetatable(jc_function_cache, {__mode = 'v' })

  local function compileAndCache (luaString)
    local func = jc_function_cache[luaString]
    if not func then
      local f, message = loadstring('local jobject = ...\n' .. luaString)
      if f then
        setfenv(f, sandbox_2)
        jc_function_cache[luaString] = f
      else
        error(message)
      end
    end

    return func
  end

  -- GLOBAL
  function JC_compileAndRun (luaString, handle)
    local func = compileAndCache(luaString)
    if func then return returnJCValue( func(wrapJCHandle(handle)) ) end
  end
end
------------------------------------

print('BUUU')
