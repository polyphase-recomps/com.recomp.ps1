/**
 * @file Ps1Dependencies.h
 * @brief Editor side of the PS1 game packages: pre-processing a game runs its
 *        Native/build.ps1, which translates the game into this addon
 *        (Source/Guest/<name>) and extracts its disc into the package's Assets/Disc.
 *
 * Game packages are Packages/<id>/Native folders whose CMakeLists.txt uses
 * Ps1Game.cmake. Each gets Tools > Recomp > <menu> > Pre Process Rom (game.json "menu",
 * else its "title"): a modal to
 * point at the user's own disc image (and the decomp it builds from), showing what is
 * already processed, with a Pre Process button. The paths go to Native/local.cmake,
 * the variables named by the "rom" block of the package's Assets/game.json.
 *
 * The setup also runs before packaging (pre-build hook; a failure cancels the build)
 * unless the build profile turns it off in its Target Options, which show each game's
 * state and open the modal.
 *
 * Build mode (Target Options, kModeOption): Decomp is the above. Recomp and Recomp (live)
 * build a game package's Recomp/ folder instead (Windows x64): the game recompiled from the
 * disc by N64Recomp, or LiveRecomp recompiling it when it boots, published as a library and
 * Source/Guest/<name>_recomp (Runtime/tools/recomp/build_recomp.ps1). A package with only
 * Recomp/ (no decomp) is a game package too.
 */
#pragma once

#include <cstdint>

struct PolyphaseBuildContext;
struct EditorUIHooks;

namespace Ps1Dependencies
{
// Build profile option (Target Options): "1" (default) runs the setup before packaging.
constexpr const char* kSetupOption = "ps1.setupDependencies";
// Build profile option (Target Options): "auto" (default), "decomp", "recomp" or "live".
constexpr const char* kModeOption = "ps1.buildMode";

// The Build mode the next setup uses (the pre-build hook passes the profile's).
void SetBuildMode(const char* mode);

// Sets up every game package now, logging the output. False if one failed.
bool SetupAll();
// Same on a background thread; output and the result show in the log.
void SetupAllAsync();
bool IsRunning();
// Stops a background run (its build processes are killed).
void Cancel();
// Module unload: stops a background run and waits for its thread, whose code is this module.
void Shutdown();
// Editor tick: writes queued output of a background setup to the log.
void Tick();
// Logs which game packages still need pre-processing.
void CheckReady();
// Tools > Recomp > <menu> > Pre Process Rom for every game package in the project.
void RegisterMenus(EditorUIHooks* hooks, uint64_t hookId);
// Opens the Pre Process Rom modal of a game package (package id).
void OpenPreprocess(const char* gameId);
// The "PS1 Recomp" section of the Packaging window's Target Options.
void DrawTargetOptions(const PolyphaseBuildContext* ctx);
}
