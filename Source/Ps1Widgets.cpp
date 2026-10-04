/**
 * @file Ps1Widgets.cpp
 * @brief Widgets bound to the PS1 script bridge (see Ps1Widgets.h).
 */

#include "Ps1Widgets.h"

#include "Ps1GuestHost.h"

#include "Log.h"

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
// One token: name[index]>table:width
struct Token
{
    std::string name;
    int index = 0;
    std::string table;
    int width = 0;
    bool zeroPad = false;
};

Token ParseToken(const std::string& text)
{
    Token t;
    size_t end = text.find_first_of("[>:");
    t.name = text.substr(0, end);
    while (end != std::string::npos && end < text.size())
    {
        const char c = text[end];
        size_t next = text.find_first_of("[>:", end + 1);
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
            out += "?";
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
void Ps1Text::Tick(float deltaTime)
{
    Text::Tick(deltaTime);
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
    outProps.push_back(Property(DatumType::String, "Request", this, &mRequest));
    outProps.push_back(Property(DatumType::String, "Arguments", this, &mArguments));
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
