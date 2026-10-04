/**
 * @file Ps1GameUIs.h
 * @brief Editor tools that build ready-made UIs for PS1 games (Tools > Recomp > <Game>):
 *        a Canvas root with Quad/Widget containers and the bound widgets of
 *        Ps1Widgets.h, in the open scene, to save as a Scene and instance anywhere.
 *
 * Running a tool again updates its UI without destroying anything: nodes are matched by
 * name, missing ones are added with their defaults, and existing ones (moved, restyled,
 * re-bound) are left exactly as they are. Delete a node to get the default one back.
 */
#pragma once

#if EDITOR

#include <cstdint>

struct EditorUIHooks;

namespace Ps1GameUIs
{
void RegisterMenus(EditorUIHooks* hooks, uint64_t hookId);
}

#endif
