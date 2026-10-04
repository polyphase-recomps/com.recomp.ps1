/**
 * @file Ps1Widgets.h
 * @brief UI widgets bound to the running PS1 game's script bridge (Ps1GuestHost::Bridge*,
 *        Runtime/port/include/port_bridge.h). Their bindings are plain properties, saved
 *        with the scene, so a UI built from them keeps working as the addon updates.
 *
 *  Ps1Text    Text showing a format with bridge values: "HP {hp}/{hp_max}".
 *  Ps1Toggle  CheckBox showing and setting a variable (0 = off).
 *  Ps1Button  Button that sends a request ("heal", "add_money 1000") or steps a
 *             variable within a range (multiplier "+" / "-" buttons).
 *  Ps1Bar     ProgressBar showing a variable against a maximum (HP bar).
 *
 * Format tokens (Ps1Text, and Ps1Bind::Format for C++ users):
 *   {name}            a variable (number or text)
 *   {name[3]}         element 3 of an array variable
 *   {name>table}      the variable's value used as an index into another variable
 *                     ({partner_type>digimon_name} is the species name)
 *   {name:02}         numbers zero-padded to 2 digits
 *   {{ / }}           literal braces
 * When the game isn't running the widgets leave what the editor shows alone.
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
    // Step mode: adds `step` to `variable`, kept within [min, max].
    void SetStep(const std::string& variable, int32_t step, int32_t minValue, int32_t maxValue);

protected:
    std::string mRequest;
    std::string mArguments;
    std::string mVariable;
    int32_t mStep = 1;
    int32_t mMin = 0;
    int32_t mMax = 10;
    int32_t mPending = 0;
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
