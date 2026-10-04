/**
 * @file Ps1GuestHost.h
 * @brief Runs a PS1 game translated by wasm2c (Source/Guest/<name>) inside the engine.
 *
 * Implements the runtime's host interface (Runtime/port/include/port_host.h) on engine
 * and C library services, on every platform the engine builds for: the game runs on a
 * thread of its own and Ps1Player feeds it vblanks and pad bits and takes its frames
 * and audio. One game at a time.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Ps1wModule;

namespace Ps1GuestHost
{
enum class State
{
    Stopped,
    Running,
    Exited,  // main() returned
    Crashed, // trap or fatal error (see the log)
};

// Game options for the next Start, answered to the game's port_debug_values: entries
// "name=v1,v2,..." separated by spaces or ';' (from game.json "options", e.g.
// "holdskip=2"). Integer values only.
void SetOptions(const std::string& options);

// discPath: the disc image; saveDir: folder for memory card files (created as needed).
bool Start(const Ps1wModule* module, const std::string& discPath, const std::string& saveDir);
// Stops the game thread (waits for it) and frees the guest.
void Stop();
State GetState();

// Main thread: one call per elapsed PS1 vblank (60 Hz), and the current pad bits
// (PsyQ PadRead layout).
void AddVblanks(uint32_t count);
void SetPad(uint32_t bits);
// Fast-forward multiplier the game asked for (port_set_speed), 1 = normal.
int GetSpeed();

// Main thread: the newest frame if it changed since lastSerial (RGBA, width x height).
// Returns false when there is nothing new. The pointer stays valid until the next call.
bool GetFrame(uint32_t& lastSerial, const uint8_t*& rgba, int& width, int& height);

// Main thread: queued 44.1 kHz stereo 16-bit audio, without removing it.
uint32_t PeekAudio(const int16_t*& frames, uint32_t maxFrames);
void ConsumeAudio(uint32_t frames);

// Main thread: writes the game's queued log lines to the engine log.
void FlushLog();

// ---- script bridge (Runtime/port/include/port_bridge.h) -----------------------------
// What the running game published with port_bridge_init: named variables (read straight
// from game memory) and named requests (run by the game between two frames).
struct BridgeVar
{
    std::string name;
    uint32_t addr = 0; // guest address of element 0
    int type = 0;      // PB_* (port_bridge.h)
    int count = 0;     // elements (strings, for PB_STR)
    int stride = 0;    // bytes between elements (resolved: never 0 here)
    std::string help;
};
struct BridgeRequestInfo
{
    std::string name;
    std::string help;
};

// Empty until the game publishes (and after it stops).
std::vector<BridgeVar> BridgeVariables();
std::vector<BridgeRequestInfo> BridgeRequests();
// Main thread: element `index` of a numeric variable, or string `index` of a text one.
// False when no game runs, the name is unknown or the index is out of range.
bool BridgeGet(const std::string& name, int index, int64_t& value);
bool BridgeGetString(const std::string& name, int index, std::string& value);
// Main thread: queues a request ("set NAME" writes variable NAME: args value[, index]).
// Returns its id, or 0 when no game runs or the queue is full.
int BridgeRequest(const std::string& name, const std::vector<int>& args);
// Main thread: true once request `id` ran, with what its handler returned
// (PB_RESULT_UNKNOWN for an unknown name). Each result is handed out once.
bool BridgeResult(int id, int& result);
}
