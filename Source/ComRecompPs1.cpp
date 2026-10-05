/**
 * @file ComRecompPs1.cpp
 * @brief Native addon: com.recomp.ps1
 *
 * Shared runtime for PS1 games compiled natively from their decompilations (see
 * ../Runtime). Exposes the Ps1Player node; each game package (e.g.
 * com.recomp.digimonworld) only carries its build config, patches and game.json.
 * In the editor it also pre-processes the game packages (Ps1Dependencies): from
 * Tools > Recomp > <game> > Pre Process Rom (also opened from the build profile's Target
 * Options), and before every packaging.
 */

#include "Plugins/PolyphasePluginAPI.h"
#include "Plugins/PolyphaseEngineAPI.h"
#if EDITOR
#include "Plugins/EditorUIHooks.h"
#include "Plugins/PolyphaseBuildTargetAPI.h"
#endif

#include "Ps1Dependencies.h"
#include "Ps1GameUIs.h"
#include "Ps1Lua.h"
#include "Ps1Player.h"
#include "Ps1Provider.h"
#include "Ps1Widgets.h"

#include <cstdio>

static PolyphaseEngineAPI* sEngineAPI = nullptr;

static int OnLoad(PolyphaseEngineAPI* api)
{
    sEngineAPI = api;
    Ps1Player::SetEngineAPI(api);
    FORCE_LINK_CALL(Ps1Player);
    // widgets bound to the script bridge (Ps1Widgets.h)
    FORCE_LINK_CALL(Ps1Text);
    FORCE_LINK_CALL(Ps1Toggle);
    FORCE_LINK_CALL(Ps1Button);
    FORCE_LINK_CALL(Ps1Bar);
    FORCE_LINK_CALL(Ps1MenuController);
    // com.recomp.mod.base (mod settings, Recomp / Mods Lua, Mods windows) sees the PS1 game
    Recomp_RegisterProvider(&Ps1Provider::Get());
    if (api && api->LogDebug)
    {
        api->LogDebug("com.recomp.ps1 loaded!");
    }
    return 0;
}

static void OnUnload()
{
    // Game processes are driven by this module's node instances: stop them first.
    Ps1Player::ShutdownAll();
    Recomp_UnregisterProvider(&Ps1Provider::Get());
    // a background pre-process thread runs this module's code: stop it (its builds are killed)
    Ps1Dependencies::Shutdown();
    Ps1Player::SetEngineAPI(nullptr);
    if (sEngineAPI && sEngineAPI->LogDebug)
    {
        sEngineAPI->LogDebug("com.recomp.ps1 unloaded.");
    }
    sEngineAPI = nullptr;
}

static void RegisterTypes(void* nodeFactory)
{
    // Register custom node types here
    // Example: REGISTER_NODE(MyCustomNode);
}

static void RegisterScriptFuncs(lua_State* L)
{
    // Ps1.*: the running game's script bridge (Ps1Lua.cpp)
    Ps1Lua::Register(L, sEngineAPI);
}

#if EDITOR
static EditorUIHooks* sHooks = nullptr;

// Pre-processes the game packages before packaging unless the profile turned it off
// (Target Options); a failure cancels the build.
static bool OnPreBuild(int32_t platform, void* userData)
{
    (void)platform;
    (void)userData;
    char value[8] = "";
    if (sHooks != nullptr && sHooks->GetBuildSetting != nullptr &&
        sHooks->GetBuildSetting(Ps1Dependencies::kSetupOption, value, sizeof(value)) && value[0] == '0')
    {
        return true;
    }
    if (!Ps1Dependencies::SetupAll())
    {
        if (sEngineAPI && sEngineAPI->LogError)
        {
            sEngineAPI->LogError("[ps1] Pre-processing failed, packaging cancelled (see the log; "
                                 "Tools > Recomp > <game> > Pre Process Rom sets the ROM)");
        }
        return false;
    }
    return true;
}

static void RegisterEditorUI(EditorUIHooks* hooks, uint64_t hookId)
{
    sHooks = hooks;
    if (hooks->AddTargetOptions != nullptr)
    {
        hooks->AddTargetOptions(hookId, "PS1 Recomp",
            [](const PolyphaseBuildContext* ctx, void*) { Ps1Dependencies::DrawTargetOptions(ctx); }, nullptr);
    }
    else
    {
        // engines without Target Options sections for every target
        hooks->AddMenuItem(hookId, "Developer", "PS1/Pre Process All Games",
            [](void*) { Ps1Dependencies::SetupAllAsync(); }, nullptr, nullptr);
    }
    hooks->RegisterOnPreBuild(hookId, OnPreBuild, nullptr);
    // Tools > Recomp > <Game> > Pre Process Rom (the ROM picker modal)
    Ps1Dependencies::RegisterMenus(hooks, hookId);
    // Tools > Recomp > <Game> > Create ... UI
    Ps1GameUIs::RegisterMenus(hooks, hookId);
    Ps1Dependencies::CheckReady();
}

static void TickEditor(float deltaTime)
{
    (void)deltaTime;
    Ps1Dependencies::Tick();
}
#endif

static int FillDesc(PolyphasePluginDesc* desc)
{
    desc->apiVersion = OCTAVE_PLUGIN_API_VERSION;
    desc->pluginName = "com.recomp.ps1";
    desc->pluginVersion = "1.0.0";
    desc->OnLoad = OnLoad;
    desc->OnUnload = OnUnload;
    desc->RegisterTypes = RegisterTypes;
    desc->RegisterScriptFuncs = RegisterScriptFuncs;
#if EDITOR
    desc->RegisterEditorUI = RegisterEditorUI;
    desc->TickEditor = TickEditor;
#else
    desc->RegisterEditorUI = nullptr;
    desc->TickEditor = nullptr;
#endif
    desc->OnEditorPreInit = nullptr;
    desc->OnEditorReady = nullptr;
    return 0;
}

#if EDITOR
extern "C" OCTAVE_PLUGIN_API int PolyphasePlugin_GetDesc(PolyphasePluginDesc* desc)
{
    return FillDesc(desc);
}
#else
extern "C" int PolyphasePlugin_GetDesc_com_recomp_ps1(PolyphasePluginDesc* desc)
{
    return FillDesc(desc);
}
#endif
