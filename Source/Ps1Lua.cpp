/**
 * @file Ps1Lua.cpp
 * @brief Lua access to the running PS1 game's script bridge (Ps1GuestHost::Bridge*).
 *
 * Global table `Ps1`:
 *   Ps1.IsRunning()              true while a game runs in-process
 *   Ps1.Get(name [, index])      a published variable (number, or string for text);
 *                                nil if no game runs / unknown / out of range
 *   Ps1.Set(name, value [, i])   queues a write; returns the request id (nil if refused)
 *   Ps1.Request(name, ...)       queues a game request with integer arguments; id or nil
 *   Ps1.Result(id)               the request's result once the game ran it, else nil
 *   Ps1.Variables()              { {name=, type=, count=, help=}, ... }
 *   Ps1.Requests()               { {name=, help=}, ... }
 *   Ps1.SetInputBlocked(bool)    true: the game gets no gamepad input (while a script's
 *                                own menu is open); false gives it back
 *   Ps1.IsInputBlocked()         true while blocked by a script or an open
 *                                Ps1MenuController UI
 *
 * Requests run on the game thread between two frames, so Result is nil for at least
 * one frame: poll it from a Tick, e.g.
 *
 *     self.req = Ps1.Request("warp", 3, 0)
 *     ...
 *     local r = Ps1.Result(self.req)
 *     if r ~= nil then ... end
 *
 * Every Lua call goes through the engine's Lua_* wrappers (PolyphaseEngineAPI): the
 * addon must not run its own copy of the Lua library on the engine's state (luaL_newlib's
 * version check aborts with "multiple Lua VMs detected", and the copies' internals
 * aren't meant to be mixed).
 */

#include "Ps1Lua.h"

#include "Constants.h"

#if LUA_ENABLED

#include "Ps1GuestHost.h"
#include "Ps1Widgets.h"
#include "Plugins/PolyphaseEngineAPI.h"
#include "../Runtime/port/include/port_bridge.h"

#include <string>
#include <vector>

namespace
{
PolyphaseEngineAPI* sApi = nullptr;

// lua_CFunction / luaL_Reg, as lua.h and lauxlib.h define them (only the layout is used:
// the table goes to the engine's LuaL_setfuncs)
typedef int (*LuaFunction)(lua_State* L);
struct LuaReg
{
    const char* name;
    LuaFunction func;
};

const char* TypeName(int type)
{
    switch (type)
    {
    case PB_U8: return "u8";
    case PB_S8: return "s8";
    case PB_U16: return "u16";
    case PB_S16: return "s16";
    case PB_U32: return "u32";
    case PB_S32: return "s32";
    case PB_STR: return "string";
    default: return "?";
    }
}

int OptInteger(lua_State* L, int arg, int fallback)
{
    if (sApi->Lua_gettop(L) < arg || sApi->Lua_isnil(L, arg))
    {
        return fallback;
    }
    return (int)sApi->LuaL_checkinteger(L, arg);
}

void PushId(lua_State* L, int id)
{
    if (id > 0)
    {
        sApi->Lua_pushinteger(L, id);
    }
    else
    {
        sApi->Lua_pushnil(L);
    }
}

int IsRunning(lua_State* L)
{
    sApi->Lua_pushboolean(L, Ps1GuestHost::GetState() == Ps1GuestHost::State::Running);
    return 1;
}

int Get(lua_State* L)
{
    const std::string name = sApi->LuaL_checkstring(L, 1);
    const int index = OptInteger(L, 2, 0);
    int64_t value = 0;
    std::string text;

    if (Ps1GuestHost::BridgeGet(name, index, value))
    {
        sApi->Lua_pushinteger(L, (long long)value);
    }
    else if (Ps1GuestHost::BridgeGetString(name, index, text))
    {
        sApi->Lua_pushstring(L, text.c_str());
    }
    else
    {
        sApi->Lua_pushnil(L);
    }
    return 1;
}

int Set(lua_State* L)
{
    const std::string name = sApi->LuaL_checkstring(L, 1);
    std::vector<int> args = {(int)sApi->LuaL_checkinteger(L, 2), OptInteger(L, 3, 0)};
    PushId(L, Ps1GuestHost::BridgeRequest("set " + name, args));
    return 1;
}

int Request(lua_State* L)
{
    const std::string name = sApi->LuaL_checkstring(L, 1);
    std::vector<int> args;
    const int top = sApi->Lua_gettop(L);
    for (int i = 2; i <= top; ++i)
    {
        args.push_back((int)sApi->LuaL_checkinteger(L, i));
    }
    PushId(L, Ps1GuestHost::BridgeRequest(name, args));
    return 1;
}

int Result(lua_State* L)
{
    const int id = (int)sApi->LuaL_checkinteger(L, 1);
    int result = 0;
    if (Ps1GuestHost::BridgeResult(id, result))
    {
        sApi->Lua_pushinteger(L, result);
    }
    else
    {
        sApi->Lua_pushnil(L);
    }
    return 1;
}

// list[i] = { field = value, ... } built with Lua_rawset (key below value)
void SetString(lua_State* L, const char* field, const std::string& value)
{
    sApi->Lua_pushstring(L, value.c_str());
    sApi->Lua_setfield(L, -2, field);
}

int SetInputBlocked(lua_State* L)
{
    Ps1Bind::SetInputBlocked(sApi->Lua_toboolean(L, 1) != 0);
    return 0;
}

int IsInputBlocked(lua_State* L)
{
    sApi->Lua_pushboolean(L, Ps1Bind::IsInputBlocked());
    return 1;
}

int Variables(lua_State* L)
{
    const std::vector<Ps1GuestHost::BridgeVar> vars = Ps1GuestHost::BridgeVariables();
    sApi->Lua_createtable(L, (int)vars.size(), 0);
    for (size_t i = 0; i < vars.size(); ++i)
    {
        sApi->Lua_pushinteger(L, (long long)i + 1);
        sApi->Lua_createtable(L, 0, 4);
        SetString(L, "name", vars[i].name);
        SetString(L, "type", TypeName(vars[i].type));
        sApi->Lua_pushinteger(L, vars[i].count);
        sApi->Lua_setfield(L, -2, "count");
        SetString(L, "help", vars[i].help);
        sApi->Lua_rawset(L, -3);
    }
    return 1;
}

int Requests(lua_State* L)
{
    const std::vector<Ps1GuestHost::BridgeRequestInfo> requests = Ps1GuestHost::BridgeRequests();
    sApi->Lua_createtable(L, (int)requests.size(), 0);
    for (size_t i = 0; i < requests.size(); ++i)
    {
        sApi->Lua_pushinteger(L, (long long)i + 1);
        sApi->Lua_createtable(L, 0, 2);
        SetString(L, "name", requests[i].name);
        SetString(L, "help", requests[i].help);
        sApi->Lua_rawset(L, -3);
    }
    return 1;
}
}

void Ps1Lua::Register(lua_State* L, PolyphaseEngineAPI* api)
{
    if (L == nullptr || api == nullptr || api->Lua_createtable == nullptr || api->LuaL_setfuncs == nullptr ||
        api->Lua_setglobal == nullptr)
    {
        return;
    }
    sApi = api;
    static const LuaReg kFuncs[] = {
        {"IsRunning", IsRunning}, {"Get", Get},           {"Set", Set},
        {"Request", Request},     {"Result", Result},     {"Variables", Variables},
        {"Requests", Requests},   {"SetInputBlocked", SetInputBlocked},
        {"IsInputBlocked", IsInputBlocked},   {nullptr, nullptr},
    };
    sApi->Lua_createtable(L, 0, 9);
    sApi->LuaL_setfuncs(L, kFuncs, 0);
    sApi->Lua_setglobal(L, "Ps1");
}

#else

void Ps1Lua::Register(lua_State*, PolyphaseEngineAPI*)
{
}

#endif
