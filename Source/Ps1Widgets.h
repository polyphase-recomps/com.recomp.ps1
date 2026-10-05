/**
 * @file Ps1Widgets.h
 * @brief UI widgets bound to the running PS1 game's script bridge (Ps1GuestHost::Bridge*,
 *        Runtime/port/include/port_bridge.h). Their bindings are plain properties, saved
 *        with the scene, so a UI built from them keeps working as the addon updates.
 *
 *  Ps1Text    Text showing a format with bridge values: "HP {hp}/{hp_max}".
 *  Ps1Toggle  CheckBox showing and setting a variable (0 = off). Mouse only: gamepad
 *             navigation reaches Buttons, so gamepad UIs use a Ps1Button in Toggle mode.
 *  Ps1Button  Button that sends a request ("heal", "add_money 1000"), toggles a variable
 *             or steps it within a range (multiplier "+" / "-"); its label can be a
 *             format too ("Infinite HP: {cheat_infhp?ON|OFF}").
 *  Ps1Bar     ProgressBar showing a variable against a maximum (HP bar).
 *  Ps1MenuController
 *             Put one inside a UI's root. Whenever that root is visible (shown by a
 *             script, the HOME menu or its Toggle Button) and the UI is interactive, it
 *             gets the gamepad: a button is selected for navigation, the game gets no
 *             input, and B hides it.
 *
 * Format tokens (Ps1Text, Ps1Button labels, and Ps1Bind::Format for C++ users):
 *   {name}            a variable (number or text)
 *   {name[3]}         element 3 of an array variable
 *   {name>table}      the variable's value used as an index into another variable
 *                     ({partner_type>digimon_name} is the species name)
 *   {name:02}         numbers zero-padded to 2 digits
 *   {name?yes|no}     "yes" when the variable is non-zero, else "no"
 *   {{ / }}           literal braces
 * A token that can't be resolved yet (the game publishes its variables once it reaches
 * its main loop) shows as "--". When no game runs the widgets leave what the editor
 * shows alone.
 */
#pragma once

#include "Nodes/Widgets/Button.h"
#include "Nodes/Widgets/CheckBox.h"
#include "Nodes/Widgets/ProgressBar.h"
#include "Nodes/Widgets/Text.h"

#include <cstdint>
#include <string>

namespace Ps1Bind
{
// True while a game runs in-process (bridge values are available).
bool IsLive();
// Expands the format tokens above. `missing` is set when a token couldn't be resolved
// (unknown variable, index out of range, -1 lookup index, no game running).
std::string Format(const std::string& format, bool* missing = nullptr);
bool GetNumber(const std::string& name, int index, int64_t& value);
// Keeps the gamepad away from the game while a script's own menu is open (Lua
// Ps1.SetInputBlocked). IsInputBlocked is also true while a Ps1MenuController UI that
// captures input is shown.
void SetInputBlocked(bool blocked);
bool IsInputBlocked();
}

class Ps1Text : public Text
{
public:
    DECLARE_NODE(Ps1Text, Text);

    virtual void Tick(float deltaTime) override;
    virtual void GatherProperties(std::vector<Property>& outProps) override;

    void SetFormat(const std::string& format);
    void SetHideIfMissing(bool hide);

protected:
    std::string mFormat = "{money}";
    bool mHideIfMissing = false; // empty text when a token can't be resolved (unused list rows)
};

class Ps1Toggle : public CheckBox
{
public:
    DECLARE_NODE(Ps1Toggle, CheckBox);

    virtual void Tick(float deltaTime) override;
    virtual void GatherProperties(std::vector<Property>& outProps) override;

    void SetVariable(const std::string& variable, int32_t onValue = 1);

protected:
    std::string mVariable;
    int32_t mOnValue = 1; // written when checked; 0 when unchecked
    int32_t mPending = 0; // request id of the last write, until the game did it
};

class Ps1Button : public Button
{
public:
    DECLARE_NODE(Ps1Button, Button);

    virtual void Activate() override;
    virtual void Tick(float deltaTime) override;
    virtual void GatherProperties(std::vector<Property>& outProps) override;

    // Request mode: sends `request` with comma-separated integer `arguments`.
    void SetRequest(const std::string& request, const std::string& arguments = "");
    // Toggle mode: switches `variable` between 0 and `onValue`.
    void SetToggle(const std::string& variable, int32_t onValue = 1);
    // Step mode: adds `step` to `variable`, kept within [min, max].
    void SetStep(const std::string& variable, int32_t step, int32_t minValue, int32_t maxValue);
    // Label from a format while the game runs (empty: the button's own Text).
    void SetLabelFormat(const std::string& format);

protected:
    std::string mRequest;
    std::string mArguments;
    std::string mToggleVariable;
    int32_t mToggleOnValue = 1;
    std::string mVariable;
    int32_t mStep = 1;
    int32_t mMin = 0;
    int32_t mMax = 10;
    std::string mLabelFormat;
    // drawn around the button while it's the selected one (gamepad focus / mouse hover):
    // the engine's own hovered color is too close to the normal one to see
    glm::vec4 mHighlightColor = {1.0f, 0.8f, 0.2f, 1.0f};
    float mHighlightWidth = 3.0f;
    bool mHighlighted = false;
    int32_t mPending = 0;
};

class Ps1MenuController : public Widget
{
public:
    DECLARE_NODE(Ps1MenuController, Widget);

    virtual void Start() override;
    virtual void Stop() override;
    virtual void Destroy() override;
    virtual void Tick(float deltaTime) override;
    virtual void GatherProperties(std::vector<Property>& outProps) override;

    void Open();
    void Close();
    void Toggle();
    bool IsOpen() const; // = its UI is visible, however it was shown
    bool IsInHomeMenu() const;
    const std::string& GetTitle() const;
    void Setup(const std::string& title, bool startVisible, bool captureInput, Node* firstButton);
    void SetBoundVariable(const std::string& name);
    void SetInHomeMenu(bool inHomeMenu);

    // The controllers of the UIs in the running game (for the HOME menu).
    static const std::vector<Ps1MenuController*>& GetAll();
    // True while an open interactive UI owns the gamepad (the game gets no input).
    static bool IsCapturingInput();

protected:
    Widget* Target();
    bool TargetVisible();
    void SyncBoundVariable();

    std::string mTitle = "Menu";
    bool mStartVisible = true;
    bool mCaptureInput = true;     // interactive: gamepad navigation, game input blocked
    WeakPtr<Button> mFirstButton;  // selected when opened (else the first Button found)
    int32_t mToggleButton = -1;    // GamepadButtonCode that opens/closes it, -1 = none
    bool mInHomeMenu = true;       // listed in the HOME menu (Show / Hide <Title>)
    // A game variable the UI follows (e.g. "paused"): shown while it's not 0, and set
    // to 0 when the UI is closed (B, Start or a script). Empty = not bound.
    std::string mBoundVariable;
    bool mWasVisible = false;      // to notice the UI being shown or hidden
    bool mSelectPending = false;   // select a button once A is released
    bool mBoundOn = false;         // the bound variable's last seen state
    int32_t mBoundPending = 0;     // the "set" request that clears it
};

class Ps1Bar : public ProgressBar
{
public:
    DECLARE_NODE(Ps1Bar, ProgressBar);

    virtual void Tick(float deltaTime) override;
    virtual void GatherProperties(std::vector<Property>& outProps) override;

    void SetVariables(const std::string& variable, const std::string& maxVariable);

protected:
    std::string mVariable = "hp";
    std::string mMaxVariable = "hp_max"; // empty: keep the bar's own Max Value
};
