/**
 * @file Ps1Player.h
 * @brief Node that runs a natively compiled PS1 game (com.recomp.ps1 runtime) and shows
 *        its frames on a Quad.
 *
 * The game is chosen by package id (Game property, e.g. "com.recomp.digimonworld");
 * that package's game.json gives the executable, disc image and save folder, and the
 * explicit properties override them when set.
 *
 * When the game was translated into this addon (Source/Guest/<name>, from the game
 * package's Native/build.ps1 -Guest wasm), it runs in-process on any platform
 * (Ps1GuestHost). Otherwise, on Windows, the game package's 32-bit executable runs as
 * a child process and talks to the editor through shared memory: frames come back as
 * RGBA, pad bits go out, and the child plays its own audio.
 */
#pragma once

#include "AssetRef.h"
#include "Nodes/3D/Node3D.h"

#include <string>
#include <vector>

struct PolyphaseEngineAPI;
struct PortShm;

class Ps1Player : public Node3D
{
public:
    DECLARE_NODE(Ps1Player, Node3D);

    Ps1Player();
    virtual ~Ps1Player();

    virtual void Create() override;
    virtual void Destroy() override;
    virtual void Tick(float deltaTime) override;
    virtual void GatherProperties(std::vector<Property>& outProps) override;

    virtual void SaveStream(Stream& stream, Platform platform) override;
    virtual void LoadStream(Stream& stream, Platform platform, uint32_t version) override;

    static void SetEngineAPI(PolyphaseEngineAPI* api);
    // Stops every running game process; called when the addon unloads.
    static void ShutdownAll();
    // Starts the game of every player of a package again on its next tick (the launcher's
    // Play: the disc the player chose)
    static void RestartGame(const std::string& package);

private:
    bool StartGame();
    void StopGame();
    // in-process game (Ps1GuestHost)
    bool StartGuest(const struct Ps1wModule* module);
    void TickGuest(float deltaTime);
    void StopGuest();
    // child process (Windows)
    bool StartProcess();
    void StopProcess();
    bool HasProcessExited() const;
    unsigned int ReadPad() const;
    void EnsureDisplayQuad();
    void UpdateDisplayTexture(const uint8_t* pixels, unsigned int width, unsigned int height);
    std::string ResolvePath(const std::string& path) const;
    // Fills the empty path properties from Packages/<mGame>/game.json.
    void ResolveGameDefaults(std::string& exe, std::string& disc, std::string& saves) const;

    // HOME menu: Wii Remote / Classic HOME, GameCube Z+Start, keyboard Home. While it
    // is open the game is paused (no vblanks, no pad). Items depend on the platform,
    // plus Show / Hide for every UI with a Ps1MenuController in the running game.
    enum class HomeAction
    {
        Resume,
        Reset,
        Exit,     // back to the loader (Homebrew Channel / Swiss) or quit the game
        WiiMenu,  // Wii System Menu
        Panel,    // show / hide a UI (Ps1MenuController)
    };
    struct HomeEntry
    {
        HomeAction action;
        WeakPtr<class Ps1MenuController> panel;
        WeakPtr<class RecompMenuController> recompPanel; // com.recomp.mod.base menus (mod settings)
    };
    // Returns true while the menu is open.
    bool UpdateHomeMenu();
    void BuildHomeMenu();
    void LayoutHomeMenu();
    void ShowHomeMenu(bool show);
    void RefreshHomeMenu();
    void RunHomeAction(const HomeEntry& entry);
    uint32_t ReadMenuButtons() const;

    static PolyphaseEngineAPI* sAPI;

    class Quad* mDisplayQuad = nullptr;
    WeakPtr<class Quad> mBoundQuad;
    // Transient texture owned by the AssetManager; this ref keeps it alive.
    AssetRef mFrameTexture;

    std::string mGame;
    std::string mExePath;
    std::string mDiscPath;
    std::string mSaveDir;
    std::string mLogPath;
    // game.json "options" (filled by ResolveGameDefaults)
    mutable std::string mGameOptions;

    void* mProcess = nullptr;   // HANDLE
    void* mJob = nullptr;       // HANDLE, kills the game with the editor
    void* mMapping = nullptr;   // HANDLE
    PortShm* mShm = nullptr;
    unsigned int mLastSerial = 0;
    bool mInProcess = false;
    float mVblankTime = 0.0f;
    uint32_t mAudioStream = 0;
    bool mStartAttempted = false;
    bool mReportedExit = false;

    // HOME menu widgets (children, built on first open; weak refs as for the display)
    WeakPtr<class Quad> mHomeBackdrop;
    WeakPtr<class Text> mHomeTitle;
    WeakPtr<class Text> mHomeHint;
    std::vector<WeakPtr<class Text>> mHomeItems; // a pool; the first mHomeEntries.size() are shown
    std::vector<HomeEntry> mHomeEntries;
    std::string mGameTitle;
    bool mHomeOpen = false;
    int mHomeSelection = 0;
    uint32_t mMenuButtonsPrev = 0;
    bool mHoldPadUntilRelease = false; // keep the closing press away from the game
};
