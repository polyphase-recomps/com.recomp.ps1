/**
 * @file Ps1Lua.h
 * @brief Registers the global Lua table `Ps1` (script bridge to the running game; see
 *        Ps1Lua.cpp for the functions), through the engine's Lua wrappers.
 */
#pragma once

struct lua_State;
struct PolyphaseEngineAPI;

namespace Ps1Lua
{
void Register(lua_State* L, PolyphaseEngineAPI* api);
}
