/**
 * @file Ps1Launcher.h
 * @brief The PS1 games in this addon as com.recomp.mod.base launchers (ModBaseLauncher.h):
 *        a launcher scene (RecompLauncher node, @launcher buttons, Recomp.* Lua) lets the
 *        player point a game at their own disc image and start it.
 *
 * One launcher per game module in the addon (Decomp or Recomp builds alike), registered when
 * the addon loads. The disc is checked by what its SYSTEM.CNF boots: the executable the game
 * package names (Assets/game.json "rom" "id") and, when the package ships recompiler data
 * (Assets/Recomp/Live/game.json "rom" "sha1"), that executable's SHA-1 - the symbols describe
 * one revision. The chosen disc is remembered in Saves/<package>.disc.txt, and Ps1Player
 * plays it (after its own Disc Image property, before the disc extracted into the package).
 * StartGame (Play) starts the game in every Ps1Player of the package; a game scene loaded
 * after it starts its own.
 */
#pragma once

#include <string>

namespace Ps1Launcher
{
// Registers one launcher per game module of the addon (OnLoad) / removes them (OnUnload).
void RegisterAll();
void UnregisterAll();

// The disc the player chose for a game package ("" = none): a .bin/.iso image.
std::string ChosenDisc(const std::string& package);

// Checks a disc image (.bin, .cue, .iso, or an extracted folder) for a game package. `resolved`
// gets the image to play (a .cue's .bin). False, with message saying why, when it won't do.
bool CheckDisc(const std::string& package, const std::string& path, std::string& message, std::string& resolved);
}
