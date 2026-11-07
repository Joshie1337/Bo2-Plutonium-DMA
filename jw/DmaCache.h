// DmaCache.h — Decoupled DMA read cache for the ESP pipeline
//
// Architecture:
//   DmaWorker thread  →  scatter-reads ALL game state in one round-trip per cycle
//                     →  writes into back-buffer (render thread has zero contention)
//                     →  atomically flips front/back buffer pointer
//
//   Render thread     →  DmaCache::Get() returns a local-RAM pointer (nanoseconds)
//                     →  ZERO DMA calls on the hot render/aimbot path
//
#pragma once
#include <windows.h>
#include <atomic>
#include <thread>
#include <cstring>
#include "Memory.h"

// ── RefDef snapshot (view matrix) ────────────────────────────────────────────
// Captured by worker every round — render thread reads for W2S without DMA
struct RefDefSnapshot {
    int    x, y, width, height;
    float  fovX, fovY, fov;
    float  viewOrigin[3];
    float  viewAxis[3][3]; // [0]=forward [1]=right [2]=up
    bool   valid;
};

// ── Packed per-player snapshot ────────────────────────────────────────────────
struct PlayerSnapshot {
    bool       valid;            // slot is live and visible
    bool       isTeammate;
    int        health;           // from gclient_s
    int        maxHealth;
    Vector3    origin;           // world feet position
    Vector3    velocity;         // trDelta (for prediction)
    char       name[36];         // pre-built display name incl. [clan]
    uint16_t   wValid;
    int        team;
    uint32_t   gentityClientPtr; // gclient_s ptr — cached between rounds
};

static constexpr int MAX_PLAYERS = 18;

// Double-buffered frame cache. Worker writes back, render reads front.
struct PlayerCacheBuffer {
    PlayerSnapshot players[MAX_PLAYERS];
    RefDefSnapshot refdef;
    int            localClientNum;
    DWORD          tickMs;
    bool           dataValid;
};

// ── Public interface ──────────────────────────────────────────────────────────
namespace DmaCache {

// Start the background DMA worker
void Start(uintptr_t cgBase,
           uintptr_t cgEntBase,
           size_t     entStride,
           uintptr_t gEntitiesStatic,
           int        localClientNum_hint);

void Stop();

// Render thread: read-only pointer to the current front buffer.
// Stable for one full frame — never free or write through this pointer.
const PlayerCacheBuffer* Get();

// Update cg_t base (call on map change without restarting the thread)
void SetCgBase(uintptr_t cgBase, uintptr_t cgEntBase);

// Worker fetch interval in microseconds. Default 1000 us = 1000 Hz cap.
extern std::atomic<int> g_intervalUs;

} // namespace DmaCache
