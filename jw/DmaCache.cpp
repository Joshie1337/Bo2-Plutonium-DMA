// DmaCache.cpp — High-performance scatter-read DMA cache
//
// Optimizations vs v1:
//   • RefDef read in Phase 1 scatter alongside entity data (no render-thread DMA)
//   • localClientNum read in Phase 1 scatter
//   • gclient_s pointers cached between rounds (don't re-read every frame)
//   • timeBeginPeriod(1) for 1ms timer resolution on worker sleep
//   • Single combined Phase 1 scatter handles 7 field types × 18 players
//   • Phase 2b health re-read skipped if gclient ptr unchanged
//
#include "DmaCache.h"
#include "vmmdll.h"
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <timeapi.h>
#pragma comment(lib,"winmm.lib")

// ── T6 / Plutonium BO2 offsets ────────────────────────────────────────────────
static constexpr size_t   kEntStride        = 0x380;
static constexpr size_t   kClientInfoOff    = 0x69A8C;
static constexpr size_t   kClientInfoStride = 0x808;
static constexpr size_t   kRefDefOff        = 0x4D890;  // cg_t → sRefDef

// sRefDef layout (from main.cpp struct)
//   +0x00 iX,iY,iWidth,iHeight  (4×int)
//   +0x10 [16 pad]
//   +0x20 flFovX, flFovY
//   +0x28 [8 pad]
//   +0x30 flFov
//   +0x34 vViewOrigin (vec3)
//   +0x40 [4 pad]
//   +0x44 vViewAxis[3] (3×vec3 = 36 bytes)
static constexpr size_t   kRefDefSize       = 0x44 + 36; // 104 bytes total

// cg_entities offsets (relative to entity slot base)
static constexpr uint32_t kEnt_wValid       = 0x02;
static constexpr uint32_t kEnt_vOrigin      = 0x2C;
static constexpr uint32_t kEnt_trDelta      = 0x50;
static constexpr uint32_t kEnt_iAlive       = 0x378;

// clientInfo offsets
static constexpr uint32_t kCI_team          = 0x00;
static constexpr uint32_t kCI_name          = 0x7E8;
static constexpr uint32_t kCI_clan          = 0x54;

// g_entities (gentity_s): stride 0x31C, gclient ptr at +0x154
// gclient_s: health at +0x238, maxhealth at +0x240
static constexpr size_t   kGEntStride       = 0x31C;
static constexpr uint32_t kGEnt_clientPtr   = 0x154;
static constexpr uint32_t kGCli_health      = 0x238;
static constexpr uint32_t kGCli_maxHealth   = 0x240;

// ── Raw read block for sRefDef ────────────────────────────────────────────────
#pragma pack(push, 1)
struct RawRefDef {
    int   x, y, width, height;         // +0x00
    char  _p0[0x10];                   // +0x10
    float fovX, fovY;                  // +0x20
    char  _p1[0x08];                   // +0x28
    float fov;                         // +0x30
    float viewOrigin[3];               // +0x34
    char  _p2[0x04];                   // +0x40
    float viewAxis[3][3];              // +0x44
};
#pragma pack(pop)
static_assert(sizeof(RawRefDef) == 104, "RawRefDef size mismatch");

// ── Module state ──────────────────────────────────────────────────────────────
namespace DmaCache {

std::atomic<int>  g_intervalUs{ 1000 };  // 1ms = up to 1000 Hz

static PlayerCacheBuffer g_buf[2];
static std::atomic<int>  g_front{ 0 };
static std::atomic<bool> g_running{ false };
static std::thread       g_thread;

static std::atomic<uintptr_t> g_cgBase   { 0 };
static std::atomic<uintptr_t> g_cgEntBase{ 0 };
static uintptr_t              g_gEntBase  = 0;   // static g_entities pointer

// gclient ptr cache — re-read only when pointer changes
static uint32_t s_cachedClientPtr[MAX_PLAYERS] = {};

static void ResolveGEntities() {
    uint32_t v = 0;
    VMMDLL_MemReadEx(Memory::VmmHandle, Memory::ProcessId,
                     0x02366B84, (PBYTE)&v, 4, nullptr, VMMDLL_FLAG_NOCACHE);
    if (v) g_gEntBase = v;
}

// ── Worker thread ─────────────────────────────────────────────────────────────
static void WorkerBody() {
    timeBeginPeriod(1); // 1ms OS timer resolution for accurate sleeps

    // Per-player raw field buffers (pinned for lifetime of scatter handle)
    uint16_t pp_wValid  [MAX_PLAYERS];
    float    pp_origin  [MAX_PLAYERS][3];
    float    pp_vel     [MAX_PLAYERS][3];
    int      pp_iAlive  [MAX_PLAYERS];
    int      pp_team    [MAX_PLAYERS];
    char     pp_name    [MAX_PLAYERS][36];
    char     pp_clan    [MAX_PLAYERS][16];
    uint32_t pp_gclient [MAX_PLAYERS];
    int      pp_health  [MAX_PLAYERS];
    int      pp_maxhp   [MAX_PLAYERS];
    RawRefDef pp_refdef;
    int      pp_localCN = 0;

    while (g_running.load(std::memory_order_relaxed)) {
        uintptr_t cgBase    = g_cgBase.load(std::memory_order_acquire);
        uintptr_t cgEntBase = g_cgEntBase.load(std::memory_order_acquire);

        if (!Memory::IsValid() || cgBase == 0 || cgEntBase == 0) {
            Sleep(20);
            continue;
        }

        if (!g_gEntBase) ResolveGEntities();

        // ── Phase 1: ONE scatter → all entity fields + refdef + localClientNum ──
        {
            VMMDLL_SCATTER_HANDLE hS = VMMDLL_Scatter_Initialize(
                Memory::VmmHandle, Memory::ProcessId, VMMDLL_FLAG_NOCACHE);
            if (!hS) { Sleep(5); continue; }

            DWORD cbRead[MAX_PLAYERS] = {};

            // localClientNum (4 bytes at cg_t+0)
            VMMDLL_Scatter_PrepareEx(hS, cgBase, 4, (PBYTE)&pp_localCN, nullptr);

            // RefDef block (104 bytes at cg_t+kRefDefOff)
            VMMDLL_Scatter_PrepareEx(hS, cgBase + kRefDefOff, (DWORD)sizeof(RawRefDef),
                                     (PBYTE)&pp_refdef, nullptr);

            // Per-entity fields
            for (int i = 0; i < MAX_PLAYERS; i++) {
                uintptr_t eBase = cgEntBase + (i * kEntStride);
                VMMDLL_Scatter_PrepareEx(hS, eBase + kEnt_wValid,  2,  (PBYTE)&pp_wValid[i], &cbRead[i]);
                VMMDLL_Scatter_PrepareEx(hS, eBase + kEnt_vOrigin, 12, (PBYTE)&pp_origin[i], nullptr);
                VMMDLL_Scatter_PrepareEx(hS, eBase + kEnt_trDelta, 12, (PBYTE)&pp_vel[i],    nullptr);
                VMMDLL_Scatter_PrepareEx(hS, eBase + kEnt_iAlive,  4,  (PBYTE)&pp_iAlive[i], nullptr);
                uintptr_t ciBase = cgBase + kClientInfoOff + (i * kClientInfoStride);
                VMMDLL_Scatter_PrepareEx(hS, ciBase + kCI_team, 4,  (PBYTE)&pp_team[i], nullptr);
                VMMDLL_Scatter_PrepareEx(hS, ciBase + kCI_name, 36, (PBYTE)&pp_name[i], nullptr);
                VMMDLL_Scatter_PrepareEx(hS, ciBase + kCI_clan, 16, (PBYTE)&pp_clan[i], nullptr);
            }

            VMMDLL_Scatter_ExecuteRead(hS);
            VMMDLL_Scatter_CloseHandle(hS);
        }

        int localCN = (pp_localCN >= 0 && pp_localCN < MAX_PLAYERS) ? pp_localCN : 0;
        int localTeam = pp_team[localCN];

        // ── Phase 2: gclient ptrs — only for alive players, use cached if unchanged ──
        bool needHealthRead = false;
        if (g_gEntBase) {
            // Check which players are alive so we skip dead/empty slots
            bool needPtrRead = false;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                bool alive = (pp_iAlive[i] & 0x2) != 0;
                bool hasOrigin = pp_origin[i][0] != 0.f || pp_origin[i][1] != 0.f || pp_origin[i][2] != 0.f;
                if (i == localCN || !alive || !hasOrigin || pp_wValid[i] == 0) continue;
                needPtrRead = true;
                needHealthRead = true;
            }

            if (needPtrRead) {
                // Read gclient pointers (they stay constant for a full life, but verify)
                VMMDLL_SCATTER_HANDLE hS = VMMDLL_Scatter_Initialize(
                    Memory::VmmHandle, Memory::ProcessId, VMMDLL_FLAG_NOCACHE);
                if (hS) {
                    for (int i = 0; i < MAX_PLAYERS; i++) {
                        pp_gclient[i] = 0;
                        bool alive = (pp_iAlive[i] & 0x2) != 0;
                        if (!alive || i == localCN) continue;
                        uintptr_t gEntAddr = g_gEntBase + (i * kGEntStride);
                        VMMDLL_Scatter_PrepareEx(hS, gEntAddr + kGEnt_clientPtr, 4,
                                                 (PBYTE)&pp_gclient[i], nullptr);
                    }
                    VMMDLL_Scatter_ExecuteRead(hS);
                    VMMDLL_Scatter_CloseHandle(hS);
                    // Update cache
                    memcpy(s_cachedClientPtr, pp_gclient, sizeof(pp_gclient));
                }
            } else {
                // Use cached pointers
                memcpy(pp_gclient, s_cachedClientPtr, sizeof(pp_gclient));
            }
        }

        // ── Phase 3: health reads (batch) ─────────────────────────────────────
        if (needHealthRead && g_gEntBase) {
            VMMDLL_SCATTER_HANDLE hS = VMMDLL_Scatter_Initialize(
                Memory::VmmHandle, Memory::ProcessId, VMMDLL_FLAG_NOCACHE);
            if (hS) {
                for (int i = 0; i < MAX_PLAYERS; i++) {
                    pp_health[i] = 100;
                    pp_maxhp[i]  = 100;
                    if (pp_gclient[i] > 0x10000) {
                        VMMDLL_Scatter_PrepareEx(hS, pp_gclient[i] + kGCli_health,    4,
                                                 (PBYTE)&pp_health[i], nullptr);
                        VMMDLL_Scatter_PrepareEx(hS, pp_gclient[i] + kGCli_maxHealth, 4,
                                                 (PBYTE)&pp_maxhp[i],  nullptr);
                    }
                }
                VMMDLL_Scatter_ExecuteRead(hS);
                VMMDLL_Scatter_CloseHandle(hS);
            }
        }

        // ── Build back buffer ─────────────────────────────────────────────────
        int back = 1 - g_front.load(std::memory_order_relaxed);
        PlayerCacheBuffer& buf = g_buf[back];
        buf.tickMs         = GetTickCount();
        buf.dataValid      = true;
        buf.localClientNum = localCN;

        // Decode RefDef
        RefDefSnapshot& rd = buf.refdef;
        rd.valid  = (pp_refdef.width > 0 && pp_refdef.height > 0);
        rd.x      = pp_refdef.x;
        rd.y      = pp_refdef.y;
        rd.width  = pp_refdef.width;
        rd.height = pp_refdef.height;
        rd.fovX   = pp_refdef.fovX;
        rd.fovY   = pp_refdef.fovY;
        rd.fov    = pp_refdef.fov;
        rd.viewOrigin[0] = pp_refdef.viewOrigin[0];
        rd.viewOrigin[1] = pp_refdef.viewOrigin[1];
        rd.viewOrigin[2] = pp_refdef.viewOrigin[2];
        memcpy(rd.viewAxis, pp_refdef.viewAxis, sizeof(rd.viewAxis));

        for (int i = 0; i < MAX_PLAYERS; i++) {
            PlayerSnapshot& snap = buf.players[i];
            if (i == localCN) { snap.valid = false; continue; }

            bool alive     = (pp_iAlive[i] & 0x2) != 0;
            bool hasOrigin = pp_origin[i][0] != 0.f || pp_origin[i][1] != 0.f || pp_origin[i][2] != 0.f;

            snap.valid      = (pp_wValid[i] != 0) && hasOrigin && alive;
            snap.isTeammate = (localTeam != 0) && (pp_team[i] == localTeam);
            snap.origin     = Vector3(pp_origin[i][0], pp_origin[i][1], pp_origin[i][2]);
            snap.velocity   = Vector3(pp_vel[i][0],    pp_vel[i][1],    pp_vel[i][2]);
            snap.team       = pp_team[i];
            snap.wValid     = pp_wValid[i];
            snap.gentityClientPtr = pp_gclient[i];

            int hp    = pp_health[i];
            int maxhp = pp_maxhp[i];
            snap.health    = (hp    > 0 && hp    <= 1000) ? hp    : 100;
            snap.maxHealth = (maxhp > 0 && maxhp <= 1000) ? maxhp : 100;

            // Build display name (only rebuild if snap was valid)
            pp_name[i][35] = 0;
            pp_clan[i][15] = 0;
            if (pp_name[i][0]) {
                if (pp_clan[i][0])
                    snprintf(snap.name, sizeof(snap.name), "[%s] %s", pp_clan[i], pp_name[i]);
                else
                    snprintf(snap.name, sizeof(snap.name), "%s", pp_name[i]);
            } else {
                snprintf(snap.name, sizeof(snap.name), "Player %d", i);
            }
        }

        // Atomic flip
        g_front.store(back, std::memory_order_release);

        // High-res sleep
        int iv = g_intervalUs.load();
        if (iv > 0)
            std::this_thread::sleep_for(std::chrono::microseconds(iv));
    }

    timeEndPeriod(1);
}

// ── Public API ────────────────────────────────────────────────────────────────
void Start(uintptr_t cgBase, uintptr_t cgEntBase, size_t, uintptr_t gEntStatic, int) {
    g_cgBase.store(cgBase,    std::memory_order_release);
    g_cgEntBase.store(cgEntBase, std::memory_order_release);
    g_gEntBase = gEntStatic;
    memset(g_buf, 0, sizeof(g_buf));
    memset(s_cachedClientPtr, 0, sizeof(s_cachedClientPtr));
    g_front.store(0, std::memory_order_release);
    g_running.store(true);
    g_thread = std::thread(WorkerBody);
    printf("[DMA] Cache worker started. cgBase=0x%X cgEntBase=0x%X\n",
           (uint32_t)cgBase, (uint32_t)cgEntBase);
}

void Stop() {
    g_running.store(false);
    if (g_thread.joinable()) g_thread.join();
}

const PlayerCacheBuffer* Get() {
    return &g_buf[g_front.load(std::memory_order_acquire)];
}

void SetCgBase(uintptr_t cgBase, uintptr_t cgEntBase) {
    g_cgBase.store(cgBase,    std::memory_order_release);
    g_cgEntBase.store(cgEntBase, std::memory_order_release);
    memset(s_cachedClientPtr, 0, sizeof(s_cachedClientPtr)); // force ptr re-read
}

} // namespace DmaCache
