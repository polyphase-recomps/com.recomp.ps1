/**
 * @file Ps1Provider.h
 * @brief The PS1 runtime's RecompProvider (com.recomp.mod.base): mod settings, the
 *        Recomp/Mods Lua tables, Recomp* widgets and the editor's Mods windows work on
 *        the PS1 game running in-process (Ps1GuestHost) through it.
 *
 * Variables and requests are the game's script bridge (port_bridge.h); raw addresses
 * are PS1 addresses (0x80xxxxxx) read from the guest's memory. A game running as a child
 * process (Windows, no translated game in the addon) has no bridge: IsLive is false.
 */
#pragma once

#include "ModBaseProvider.h"

#include <string>

class Ps1Provider : public RecompProvider
{
public:
    static Ps1Provider& Get();

    // Set by Ps1Player when it starts a game, and per frame.
    void SetGame(const std::string& package);
    void SetFrame(int width, int height);

    const char* RuntimeId() const override { return "ps1"; }
    std::string GamePackage() const override { return mGame; }
    bool IsLive() const override;
    void Variables(std::vector<RecompVarInfo>& out) const override;
    void Requests(std::vector<RecompRequestInfo>& out) const override;
    bool Get(const std::string& name, int index, RecompValue& out) override;
    bool Set(const std::string& name, int index, const RecompValue& value) override;
    int Request(const std::string& name, const std::vector<int>& args) override;
    bool Result(int id, int& result) override;
    bool ReadAddress(uint64_t address, RecompType type, RecompValue& out) override;
    bool WriteAddress(uint64_t address, RecompType type, const RecompValue& value) override;
    RecompFrameInfo FrameInfo() const override;

private:
    std::string mGame;
    int mWidth = 0;
    int mHeight = 0;
};
