/**
 * @file Ps1Dependencies.h
 * @brief Editor side of the PS1 game packages: "Setup Dependencies" runs each game's
 *        Native/build.ps1, which translates the game into this addon
 *        (Source/Guest/<name>) and extracts its disc into the package's Assets/Disc.
 *
 * Game packages are Packages/<id>/Native folders whose CMakeLists.txt uses
 * Ps1Game.cmake. The setup runs before packaging (pre-build hook; a failure cancels the
 * build) unless the build profile turns it off, and on demand from the profile's
 * Target Options in the Packaging window.
 */
#pragma once

struct PolyphaseBuildContext;

namespace Ps1Dependencies
{
// Build profile option (Target Options): "1" (default) runs the setup before packaging.
constexpr const char* kSetupOption = "ps1.setupDependencies";

// Sets up every game package now, logging the output. False if one failed.
bool SetupAll();
// Same on a background thread; output and the result show in the log.
void SetupAllAsync();
bool IsRunning();
// Editor tick: writes queued output of a background setup to the log.
void Tick();
// Logs which game packages still need the setup (no translated game in this addon, or
// no extracted disc).
void CheckReady();
// The "PS1 Recomp" section of the Packaging window's Target Options.
void DrawTargetOptions(const PolyphaseBuildContext* ctx);
}
