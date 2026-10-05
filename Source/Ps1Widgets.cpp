/**
 * @file Ps1Widgets.cpp
 * @brief Widgets bound to the PS1 script bridge (see Ps1Widgets.h).
 */

#include "Ps1Widgets.h"

#include "Ps1GuestHost.h"
#include "ModBaseProvider.h"

#include "Log.h"
#include "Nodes/Widgets/Quad.h"
#include "Input/Input.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

FORCE_LINK_DEF(Ps1Text);
DEFINE_NODE(Ps1Text, Text);
FORCE_LINK_DEF(Ps1Toggle);
DEFINE_NODE(Ps1Toggle, CheckBox);
FORCE_LINK_DEF(Ps1Button);
DEFINE_NODE(Ps1Button, Button);
FORCE_LINK_DEF(Ps1Bar);
DEFINE_NODE(Ps1Bar, ProgressBar);

// ---- Ps1Bind ---------------------------------------------------------------------------
bool Ps1Bind::IsLive()
{
    return Ps1GuestHost::GetState() == Ps1GuestHost::State::Running;
}

bool Ps1Bind::GetNumber(const std::string& name, int index, int64_t& value)
{
    return Ps1GuestHost::BridgeGet(name, index, value);
}

namespace
{
// One token: name[index]>table:width or name[index]?yes|no
struct Token
{
    std::string name;
    int index = 0;
    std::string table;
    int width = 0;
    bool zeroPad = false;
    bool conditional = false;
    std::string yes, no;
};

Token ParseToken(const std::string& text)
{
    Token t;
    size_t end = text.find_first_of("[>:?");
    t.name = text.substr(0, end);
    while (end != std::string::npos && end < text.size())
    {
        const char c = text[end];
        if (c == '?')
        {
            // the rest is "yes|no" (either may be empty)
            const std::string rest = text.substr(end + 1);
            const size_t bar = rest.find('|');
            t.conditional = true;
            t.yes = rest.substr(0, bar);
            t.no = bar == std::string::npos ? std::string() : rest.substr(bar + 1);
            break;
        }
        size_t next = text.find_first_of("[>:?", end + 1);
        std::string part = text.substr(end + 1, next == std::string::npos ? std::string::npos : next - end - 1);
        if (c == '[')
        {
            t.index = atoi(part.c_str());
        }
        else if (c == '>')
        {
            t.table = part;
        }
        else if (c == ':')
        {
            t.zeroPad = !part.empty() && part[0] == '0';
            t.width = atoi(part.c_str());
        }
        end = next;
    }
    return t;
}

// A variable element as text; false when it can't be resolved.
bool ValueText(const std::string& name, int index, const Token& t, std::string& out)
{
    int64_t number = 0;
    if (Ps1GuestHost::BridgeGet(name, index, number))
    {
        char buf[32];
        if (t.width > 0)
        {
            snprintf(buf, sizeof(buf), t.zeroPad ? "%0*lld" : "%*lld", t.width, (long long)number);
        }
        else
        {
            snprintf(buf, sizeof(buf), "%lld", (long long)number);
        }
        out = buf;
        return true;
    }
    return Ps1GuestHost::BridgeGetString(name, index, out);
}

bool Resolve(const Token& t, std::string& out)
{
    if (t.conditional)
    {
        int64_t value = 0;
        if (!Ps1GuestHost::BridgeGet(t.name, t.index, value))
        {
            return false;
        }
        out = value != 0 ? t.yes : t.no;
        return true;
    }
    if (t.table.empty())
    {
        return ValueText(t.name, t.index, t, out);
    }
    int64_t lookup = 0;
    if (!Ps1GuestHost::BridgeGet(t.name, t.index, lookup) || lookup < 0)
    {
        return false;
    }
    return ValueText(t.table, (int)lookup, t, out);
}
}

std::string Ps1Bind::Format(const std::string& format, bool* missing)
{
    std::string out;
    bool anyMissing = false;

    for (size_t i = 0; i < format.size(); ++i)
    {
        const char c = format[i];
        if ((c == '{' || c == '}') && i + 1 < format.size() && format[i + 1] == c)
        {
            out.push_back(c); // {{ or }}
            ++i;
            continue;
        }
        if (c != '{')
        {
            out.push_back(c);
            continue;
        }
        const size_t close = format.find('}', i + 1);
        if (close == std::string::npos)
        {
            out.append(format, i, std::string::npos);
            break;
        }
        std::string value;
        if (Resolve(ParseToken(format.substr(i + 1, close - i - 1)), value))
        {
            out += value;
        }
        else
        {
            out += "--"; // not published yet (title screen, before the game's main loop)
            anyMissing = true;
        }
        i = close;
    }
    if (missing)
    {
        *missing = anyMissing;
    }
    return out;
}

// ---- Ps1Text ---------------------------------------------------------------------------
// Once per session, what the bound widgets see (to tell "no game" from "not published").
static void ReportBridgeState()
{
    static bool reported = false, reportedLive = false;
    if (reportedLive)
    {
        return;
    }
    const bool live = Ps1Bind::IsLive();
    const size_t vars = live ? Ps1GuestHost::BridgeVariables().size() : 0;

    if (!reported)
    {
        reported = true;
        if (!live)
        {
            LogWarning("PS1 UI: no PS1 game is running in this process (add a Ps1Player whose game is "
                       "translated into com.recomp.ps1: Tools > Recomp > <game> > Pre Process Rom). Bound widgets keep their "
                       "editor text until one runs.");
        }
    }
    if (live && vars > 0 && !reportedLive)
    {
        reportedLive = true;
        LogDebug("PS1 UI: the game published %d variables; bound widgets are live", (int)vars);
    }
}

void Ps1Text::Tick(float deltaTime)
{
    Text::Tick(deltaTime);
    ReportBridgeState();
    if (!Ps1Bind::IsLive())
    {
        return; // keep the text the editor shows (the format itself, by default)
    }
    bool missing = false;
    const std::string text = Ps1Bind::Format(mFormat, &missing);
    SetText((missing && mHideIfMissing) ? std::string() : text);
}

void Ps1Text::GatherProperties(std::vector<Property>& outProps)
{
    Text::GatherProperties(outProps);
    SCOPED_CATEGORY("PS1 Bridge");
    outProps.push_back(Property(DatumType::String, "Format", this, &mFormat));
    outProps.push_back(Property(DatumType::Bool, "Hide If Missing", this, &mHideIfMissing));
}

void Ps1Text::SetFormat(const std::string& format)
{
    mFormat = format;
    SetText(format);
}

void Ps1Text::SetHideIfMissing(bool hide)
{
    mHideIfMissing = hide;
}

// ---- Ps1Toggle -------------------------------------------------------------------------
void Ps1Toggle::Tick(float deltaTime)
{
    const bool before = IsChecked();
    CheckBox::Tick(deltaTime);
    if (mVariable.empty() || !Ps1Bind::IsLive())
    {
        return;
    }

    if (IsChecked() != before)
    {
        // the player clicked it: write the variable (done by the game next frame)
        mPending = Ps1GuestHost::BridgeRequest("set " + mVariable, {IsChecked() ? mOnValue : 0});
    }
    int32_t result = 0;
    if (mPending != 0 && Ps1GuestHost::BridgeResult(mPending, result))
    {
        mPending = 0;
    }
    if (mPending == 0)
    {
        // show the game's value (it may change from scripts or game.json options)
        int64_t value = 0;
        if (Ps1GuestHost::BridgeGet(mVariable, 0, value) && (value != 0) != IsChecked())
        {
            SetChecked(value != 0);
        }
    }
}

void Ps1Toggle::GatherProperties(std::vector<Property>& outProps)
{
    CheckBox::GatherProperties(outProps);
    SCOPED_CATEGORY("PS1 Bridge");
    outProps.push_back(Property(DatumType::String, "Variable", this, &mVariable));
    outProps.push_back(Property(DatumType::Integer, "On Value", this, &mOnValue));
}

void Ps1Toggle::SetVariable(const std::string& variable, int32_t onValue)
{
    mVariable = variable;
    mOnValue = onValue;
}

// ---- Ps1Button -------------------------------------------------------------------------
void Ps1Button::Activate()
{
    Button::Activate();
    if (!Ps1Bind::IsLive())
    {
        return;
    }

    if (!mRequest.empty())
    {
        std::vector<int> args;
        const char* p = mArguments.c_str();
        while (*p)
        {
            char* end = nullptr;
            const long v = strtol(p, &end, 0);
            if (end == p)
            {
                ++p; // skip separators
                continue;
            }
            args.push_back((int)v);
            p = end;
        }
        mPending = Ps1GuestHost::BridgeRequest(mRequest, args);
    }
    else if (!mToggleVariable.empty())
    {
        int64_t value = 0;
        if (Ps1GuestHost::BridgeGet(mToggleVariable, 0, value))
        {
            mPending = Ps1GuestHost::BridgeRequest("set " + mToggleVariable, {value != 0 ? 0 : mToggleOnValue});
        }
    }
    else if (!mVariable.empty())
    {
        int64_t value = 0;
        if (Ps1GuestHost::BridgeGet(mVariable, 0, value))
        {
            int64_t next = value + mStep;
            next = next < mMin ? mMin : (next > mMax ? mMax : next);
            mPending = Ps1GuestHost::BridgeRequest("set " + mVariable, {(int)next});
        }
    }
}

void Ps1Button::Tick(float deltaTime)
{
    Button::Tick(deltaTime);
    const bool selected = Button::GetSelectedButton() == this;
    if (selected != mHighlighted)
    {
        mHighlighted = selected;
        if (Quad* quad = GetQuad())
        {
            quad->SetBorderColor(mHighlightColor);
            quad->SetBorderWidth(selected ? mHighlightWidth : 0.0f);
        }
    }
    if (!mLabelFormat.empty() && Ps1Bind::IsLive())
    {
        const std::string label = Ps1Bind::Format(mLabelFormat);
        if (label != GetTextString())
        {
            SetTextString(label);
        }
    }
    int32_t result = 0;
    if (mPending != 0 && Ps1GuestHost::BridgeResult(mPending, result))
    {
        if (result < 0)
        {
            LogWarning("%s: %s answered %d (busy or not possible right now)", GetName().c_str(),
                       mRequest.empty() ? mVariable.c_str() : mRequest.c_str(), result);
        }
        mPending = 0;
    }
}

void Ps1Button::GatherProperties(std::vector<Property>& outProps)
{
    Button::GatherProperties(outProps);
    SCOPED_CATEGORY("PS1 Bridge");
    outProps.push_back(Property(DatumType::String, "Label Format", this, &mLabelFormat));
    outProps.push_back(Property(DatumType::Color, "Highlight Color", this, &mHighlightColor));
    outProps.push_back(Property(DatumType::Float, "Highlight Width", this, &mHighlightWidth));
    outProps.push_back(Property(DatumType::String, "Request", this, &mRequest));
    outProps.push_back(Property(DatumType::String, "Arguments", this, &mArguments));
    outProps.push_back(Property(DatumType::String, "Toggle Variable", this, &mToggleVariable));
    outProps.push_back(Property(DatumType::Integer, "Toggle On Value", this, &mToggleOnValue));
    outProps.push_back(Property(DatumType::String, "Step Variable", this, &mVariable));
    outProps.push_back(Property(DatumType::Integer, "Step", this, &mStep));
    outProps.push_back(Property(DatumType::Integer, "Step Min", this, &mMin));
    outProps.push_back(Property(DatumType::Integer, "Step Max", this, &mMax));
}

void Ps1Button::SetRequest(const std::string& request, const std::string& arguments)
{
    mRequest = request;
    mArguments = arguments;
}

void Ps1Button::SetToggle(const std::string& variable, int32_t onValue)
{
    mToggleVariable = variable;
    mToggleOnValue = onValue;
}

void Ps1Button::SetLabelFormat(const std::string& format)
{
    mLabelFormat = format;
}

void Ps1Button::SetStep(const std::string& variable, int32_t step, int32_t minValue, int32_t maxValue)
{
    mVariable = variable;
    mStep = step;
    mMin = minValue;
    mMax = maxValue;
}

// ---- Ps1Bar ----------------------------------------------------------------------------
void Ps1Bar::Tick(float deltaTime)
{
    ProgressBar::Tick(deltaTime);
    if (!Ps1Bind::IsLive())
    {
        return;
    }
    int64_t value = 0, maxValue = 0;
    if (!mMaxVariable.empty() && Ps1GuestHost::BridgeGet(mMaxVariable, 0, maxValue) && maxValue > 0)
    {
        SetMaxValue((float)maxValue);
    }
    if (Ps1GuestHost::BridgeGet(mVariable, 0, value))
    {
        SetValue((float)value);
    }
}

void Ps1Bar::GatherProperties(std::vector<Property>& outProps)
{
    ProgressBar::GatherProperties(outProps);
    SCOPED_CATEGORY("PS1 Bridge");
    outProps.push_back(Property(DatumType::String, "Variable", this, &mVariable));
    outProps.push_back(Property(DatumType::String, "Max Variable", this, &mMaxVariable));
}

void Ps1Bar::SetVariables(const std::string& variable, const std::string& maxVariable)
{
    mVariable = variable;
    mMaxVariable = maxVariable;
}

// ---- Ps1MenuController -----------------------------------------------------------------
FORCE_LINK_DEF(Ps1MenuController);
DEFINE_NODE(Ps1MenuController, Widget);

namespace
{
std::vector<Ps1MenuController*> sControllers; // in the running game

bool IsInside(Node* node, Node* ancestor)
{
    for (Node* n = node; n != nullptr; n = n->GetParent())
    {
        if (n == ancestor)
        {
            return true;
        }
    }
    return false;
}

Button* FirstButtonIn(Node* node)
{
    if (node == nullptr)
    {
        return nullptr;
    }
    if (Button* b = node->As<Button>())
    {
        return b;
    }
    for (uint32_t i = 0; i < node->GetNumChildren(); ++i)
    {
        if (Button* b = FirstButtonIn(node->GetChild((int32_t)i)))
        {
            return b;
        }
    }
    return nullptr;
}
}

Widget* Ps1MenuController::Target()
{
    Node* parent = GetParent();
    return parent ? parent->As<Widget>() : nullptr;
}

bool Ps1MenuController::TargetVisible()
{
    Widget* target = Target();
    return target != nullptr && target->IsVisible();
}

void Ps1MenuController::Start()
{
    Widget::Start();
    if (std::find(sControllers.begin(), sControllers.end(), this) == sControllers.end())
    {
        sControllers.push_back(this);
    }
    mWasVisible = false;
    if (Widget* target = Target())
    {
        target->SetVisible(mStartVisible);
    }
}

void Ps1MenuController::Stop()
{
    sControllers.erase(std::remove(sControllers.begin(), sControllers.end(), this), sControllers.end());
    Widget::Stop();
}

void Ps1MenuController::Destroy()
{
    sControllers.erase(std::remove(sControllers.begin(), sControllers.end(), this), sControllers.end());
    Widget::Destroy();
}

// Opening and closing are just showing and hiding the UI: a script calling SetVisible on
// it does the same (Tick notices the change).
void Ps1MenuController::Open()
{
    if (Widget* target = Target())
    {
        target->SetVisible(true);
    }
}

void Ps1MenuController::Close()
{
    if (Widget* target = Target())
    {
        target->SetVisible(false);
    }
}

void Ps1MenuController::Toggle()
{
    if (IsOpen())
    {
        Close();
    }
    else
    {
        Open();
    }
}

bool Ps1MenuController::IsOpen() const
{
    return const_cast<Ps1MenuController*>(this)->TargetVisible();
}

bool Ps1MenuController::IsInHomeMenu() const
{
    return mInHomeMenu;
}

const std::string& Ps1MenuController::GetTitle() const
{
    return mTitle;
}

void Ps1MenuController::Setup(const std::string& title, bool startVisible, bool captureInput, Node* firstButton)
{
    mTitle = title;
    mStartVisible = startVisible;
    mCaptureInput = captureInput;
    mFirstButton = ResolveWeakPtr<Button>(firstButton);
}

const std::vector<Ps1MenuController*>& Ps1MenuController::GetAll()
{
    return sControllers;
}

bool Ps1MenuController::IsCapturingInput()
{
    for (Ps1MenuController* c : sControllers)
    {
        if (c->mCaptureInput && c->TargetVisible())
        {
            return true;
        }
    }
    return false;
}

void Ps1MenuController::Tick(float deltaTime)
{
    Widget::Tick(deltaTime);
    if (mToggleButton >= 0 && INP_IsGamepadButtonJustDown(mToggleButton, 0))
    {
        Toggle();
    }

    Widget* target = Target();
    if (!mBoundVariable.empty())
    {
        SyncBoundVariable();
    }
    const bool visible = TargetVisible();
    if (visible && !mWasVisible)
    {
        // just shown (by this controller, the HOME menu or a script): select a button
        // once A is up, so the press that opened it doesn't also press its first button
        mSelectPending = mCaptureInput;
    }
    else if (!visible && mWasVisible)
    {
        mSelectPending = false;
        Button* selected = Button::GetSelectedButton();
        if (selected != nullptr && target != nullptr && IsInside(selected, target))
        {
            Button::SetSelectedButton(nullptr);
        }
    }
    mWasVisible = visible;

    if (!visible || !mCaptureInput || target == nullptr)
    {
        return;
    }
    if (INP_IsGamepadButtonJustDown(GAMEPAD_B, 0) ||
        (!mBoundVariable.empty() && INP_IsGamepadButtonJustDown(GAMEPAD_START, 0)))
    {
        Close(); // back to the game (or to a script's own menu); Start too for a pause menu
        return;
    }
    if (mSelectPending && INP_IsGamepadButtonDown(GAMEPAD_A, 0))
    {
        return;
    }
    mSelectPending = false;
    // keep a button of this UI selected so the gamepad always has something to move from
    Button* selected = Button::GetSelectedButton();
    if (selected == nullptr || !IsInside(selected, target))
    {
        Button* first = mFirstButton.Get();
        Button::SetSelectedButton(first != nullptr ? first : FirstButtonIn(target));
    }
}

// The UI follows the game: shown when the variable turns on, hidden when it turns off.
// Closing the UI while it's on asks the game to turn it off (a pause menu's resume).
void Ps1MenuController::SyncBoundVariable()
{
    int32_t result = 0;
    if (mBoundPending != 0 && Ps1GuestHost::BridgeResult(mBoundPending, result))
    {
        mBoundPending = 0;
    }
    int64_t value = 0;
    if (!Ps1GuestHost::BridgeGet(mBoundVariable, 0, value))
    {
        mBoundOn = false; // no game (or it doesn't publish the variable): leave the UI alone
        return;
    }
    const bool on = value != 0;
    if (on != mBoundOn)
    {
        mBoundOn = on;
        if (on)
        {
            Open();
        }
        else
        {
            Close();
        }
    }
    else if (on && mWasVisible && !TargetVisible() && mBoundPending == 0)
    {
        mBoundPending = Ps1GuestHost::BridgeRequest("set " + mBoundVariable, {0});
    }
}

void Ps1MenuController::GatherProperties(std::vector<Property>& outProps)
{
    Widget::GatherProperties(outProps);
    SCOPED_CATEGORY("PS1 Menu");
    outProps.push_back(Property(DatumType::String, "Title", this, &mTitle));
    outProps.push_back(Property(DatumType::Bool, "Start Visible", this, &mStartVisible));
    outProps.push_back(Property(DatumType::Bool, "Capture Input", this, &mCaptureInput));
    outProps.push_back(Property(DatumType::Node, "First Button", this, &mFirstButton));
    outProps.push_back(Property(DatumType::Integer, "Toggle Button", this, &mToggleButton));
    outProps.push_back(Property(DatumType::Bool, "In HOME Menu", this, &mInHomeMenu));
    outProps.push_back(Property(DatumType::String, "Bound Variable", this, &mBoundVariable));
}

void Ps1MenuController::SetBoundVariable(const std::string& name)
{
    mBoundVariable = name;
}

void Ps1MenuController::SetInHomeMenu(bool inHomeMenu)
{
    mInHomeMenu = inHomeMenu;
}

// ---- input blocking for scripts (Lua Ps1.SetInputBlocked) ---------------------------------
static bool sInputBlocked = false;

void Ps1Bind::SetInputBlocked(bool blocked)
{
    sInputBlocked = blocked;
}

bool Ps1Bind::IsInputBlocked()
{
    // also the com.recomp.mod.base menus (generated mod settings) and Recomp.SetInputBlocked
    return sInputBlocked || Ps1MenuController::IsCapturingInput() || Recomp_IsInputCaptured();
}
