/**
 * @file Ps1Provider.cpp
 * @brief The PS1 runtime's RecompProvider (see Ps1Provider.h).
 */

#include "Ps1Provider.h"

#include "Ps1GuestHost.h"
#include "Wasm/ps1w_bridge.h"
#include "../Runtime/port/include/port_bridge.h"

#include <cmath>
#include <cstring>

namespace
{
RecompType ToRecompType(int type)
{
    switch (type)
    {
    case PB_U8: return RecompType::U8;
    case PB_S8: return RecompType::S8;
    case PB_U16: return RecompType::U16;
    case PB_S16: return RecompType::S16;
    case PB_U32: return RecompType::U32;
    case PB_STR: return RecompType::Str;
    default: return RecompType::S32;
    }
}
}

Ps1Provider& Ps1Provider::Get()
{
    static Ps1Provider sProvider;
    return sProvider;
}

void Ps1Provider::SetGame(const std::string& package)
{
    mGame = package;
}

void Ps1Provider::SetFrame(int width, int height)
{
    mWidth = width;
    mHeight = height;
}

bool Ps1Provider::IsLive() const
{
    return Ps1GuestHost::GetState() == Ps1GuestHost::State::Running;
}

void Ps1Provider::Variables(std::vector<RecompVarInfo>& out) const
{
    for (const Ps1GuestHost::BridgeVar& v : Ps1GuestHost::BridgeVariables())
    {
        RecompVarInfo info;
        info.name = v.name;
        info.help = v.help;
        info.type = ToRecompType(v.type);
        info.count = v.count;
        info.writable = v.type != PB_STR;
        out.push_back(info);
    }
}

void Ps1Provider::Requests(std::vector<RecompRequestInfo>& out) const
{
    for (const Ps1GuestHost::BridgeRequestInfo& r : Ps1GuestHost::BridgeRequests())
    {
        out.push_back({r.name, r.help});
    }
}

bool Ps1Provider::Get(const std::string& name, int index, RecompValue& out)
{
    int64_t number = 0;
    if (Ps1GuestHost::BridgeGet(name, index, number))
    {
        out = RecompValue::Number((double)number);
        return true;
    }
    std::string text;
    if (Ps1GuestHost::BridgeGetString(name, index, text))
    {
        out = RecompValue::Text(text);
        return true;
    }
    return false;
}

bool Ps1Provider::Set(const std::string& name, int index, const RecompValue& value)
{
    if (value.isText)
    {
        return false; // the bridge's "set" takes numbers
    }
    // the game writes it between two frames (port_bridge_pump's built-in "set")
    return Ps1GuestHost::BridgeRequest("set " + name, {(int)std::lround(value.number), index}) > 0;
}

int Ps1Provider::Request(const std::string& name, const std::vector<int>& args)
{
    return Ps1GuestHost::BridgeRequest(name, args);
}

bool Ps1Provider::Result(int id, int& result)
{
    return Ps1GuestHost::BridgeResult(id, result);
}

// Guest memory is little-endian on every host (the wasm guest's byte order).
bool Ps1Provider::ReadAddress(uint64_t address, RecompType type, RecompValue& out)
{
    if (!IsLive())
    {
        return false;
    }
    if (type == RecompType::Str)
    {
        const uint8_t* p = (const uint8_t*)ps1w_guest_ptr((unsigned)address, 64);
        if (p == nullptr) return false;
        std::string text;
        for (int i = 0; i < 64 && p[i] != 0; ++i) text += (char)p[i];
        out = RecompValue::Text(text);
        return true;
    }
    const int size = Recomp_TypeSize(type);
    const uint8_t* p = (const uint8_t*)ps1w_guest_ptr((unsigned)address, (unsigned)size);
    if (p == nullptr)
    {
        return false;
    }
    uint32_t raw = 0;
    for (int i = size - 1; i >= 0; --i)
    {
        raw = (raw << 8) | p[i];
    }
    double v = 0;
    switch (type)
    {
    case RecompType::U8: v = (uint8_t)raw; break;
    case RecompType::S8: v = (int8_t)raw; break;
    case RecompType::U16: v = (uint16_t)raw; break;
    case RecompType::S16: v = (int16_t)raw; break;
    case RecompType::U32: v = raw; break;
    case RecompType::F32:
    {
        float f;
        memcpy(&f, &raw, 4);
        v = f;
        break;
    }
    default: v = (int32_t)raw; break;
    }
    out = RecompValue::Number(v);
    return true;
}

bool Ps1Provider::WriteAddress(uint64_t address, RecompType type, const RecompValue& value)
{
    if (!IsLive() || type == RecompType::Str || value.isText)
    {
        return false;
    }
    const int size = Recomp_TypeSize(type);
    uint8_t* p = (uint8_t*)ps1w_guest_ptr((unsigned)address, (unsigned)size);
    if (p == nullptr)
    {
        return false;
    }
    uint32_t raw;
    if (type == RecompType::F32)
    {
        const float f = (float)value.number;
        memcpy(&raw, &f, 4);
    }
    else
    {
        raw = (uint32_t)(int64_t)std::llround(value.number);
    }
    for (int i = 0; i < size; ++i)
    {
        p[i] = (uint8_t)(raw >> (8 * i));
    }
    return true;
}

RecompFrameInfo Ps1Provider::FrameInfo() const
{
    RecompFrameInfo info;
    info.width = mWidth;
    info.height = mHeight;
    info.displayAspect = 4.0f / 3.0f;
    return info;
}
