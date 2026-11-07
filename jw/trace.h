// trace.h — DMA BSP Trace for T6 (Black Ops 2) Visibility Check
//
// Globals confirmed via GhidraMCP (CG_LocationalTrace call chain):
//   0x02531D14 = cBrushSide_t* (stride 12: {planePtrOrNamePtr, sflags, cflags})
//   0x02531D3C = leafBrush index array (uint16_t*)
//   0x02531D60 = cNode_t* (stride 8: {planePtr u32, childFront s16, childBack s16})
//   0x02531D68 = leaf node array (stride 44: {s16 firstLeafBrush, s16 numLeafBrushes, ...})
//               *** BSP traversal uses THIS for leaves, NOT 0x02531D84! ***
//               Game: DAT_02531d68 + (childIdx+1)*(-0x2c) [FUN_00886010]
//   0x02531D84 = cLeaf_t* (stride 12) — different leaf format, used for other purposes
//   0x02531D8C = cBrush_t* (stride 32)
//

// CRITICAL LAYOUT NOTE (from FUN_0050ba80 decompilation):
//   The brush at stride 0x20 (32 bytes) stores:
//     +0x00: center.x    (float) — NOT mins.x!
//     +0x04: center.y    (float)
//     +0x08: center.z    (float)
//     +0x0C: firstSideIdx (int16) — low  16 bits of u32
//     +0x0E: numSides     (int16) — high 16 bits of u32
//     +0x10: halfExtent.x (float) — half-width in X
//     +0x14: halfExtent.y (float)
//     +0x18: halfExtent.z (float)
//     +0x1C: subBrushIdx  (u32)   — compound brush child index
//   actualMins = center - halfExtent
//   actualMaxs = center + halfExtent
//
// ALL plane data is preloaded during FindAndCache.
// BSP_Trace and RayHitsBrush do ZERO DMA reads — pure CPU.
//
#pragma once
#include <cstdint>
#include <cmath>
#include <vector>
#include <unordered_map>
#include <cstdio>
#include <algorithm>
#include "Memory.h"

// Avoid Windows min/max macro pollution
#ifndef NOMINMAX
#define NOMINMAX
#endif
#undef min
#undef max
template<typename T> static inline T cmMin(T a, T b) { return a < b ? a : b; }
template<typename T> static inline T cmMax(T a, T b) { return a > b ? a : b; }


namespace T6CM {
    constexpr uint32_t G_BRUSHSIDES = 0x02531D14u;
    constexpr uint32_t G_LEAFBRUSH  = 0x02531D3Cu;
    constexpr uint32_t G_NODES      = 0x02531D60u;
    constexpr uint32_t G_LEAFS      = 0x02531D84u;
    constexpr uint32_t G_BRUSHES    = 0x02531D8Cu;
    constexpr uint32_t PTR_MIN      = 0x00400000u;
    constexpr uint32_t PTR_MAX      = 0x5FFFFFFFu;
    inline bool IsPtr(uint32_t p) { return p >= PTR_MIN && p <= PTR_MAX; }
}

// ── Raw game structs (DMA layout) ─────────────────────────────────────────────

struct cplane_t {
    float   normal[3];
    float   dist;
    uint8_t type;      // 0=X-axial, 1=Y-axial, 2=Z-axial, 3+=non-axial
    uint8_t signbits;
    uint8_t pad[2];
};
static_assert(sizeof(cplane_t) == 20, "cplane_t");

struct cNode_t {          // stride 8 at G_NODES
    uint32_t planePtr;    // game address of cplane_t
    int16_t  childFront;  // >=0: node idx, <0: leaf = (-1-child)
    int16_t  childBack;
};
static_assert(sizeof(cNode_t) == 8, "cNode_t");

struct cLeaf_t {          // stride 12 at G_LEAFS
    uint16_t firstCollAabb;
    uint16_t numCollAabb;
    int32_t  contents;
    uint16_t firstLeafBrush;
    uint16_t numLeafBrushes;
};
static_assert(sizeof(cLeaf_t) == 12, "cLeaf_t");

struct cBrushSide_t {     // stride 12 at G_BRUSHSIDES
    uint32_t planePtr;    // game address of cplane_t
    uint32_t sflags;
    uint32_t cflags;
};
static_assert(sizeof(cBrushSide_t) == 12, "cBrushSide_t");

#pragma pack(push, 1)
struct cBrush_t {         // stride 32 at G_BRUSHES
    float    center[3];       // +0x00: world-space center (NOT mins!)
    int16_t  firstSideIdx;    // +0x0C: index into brushSides array
    int16_t  numSides;        // +0x0E: count of brush sides
    float    halfExtent[3];   // +0x10: half-widths (NOT maxs!)
    uint32_t subBrushIdx;     // +0x1C: compound-brush child (unused for trace)
};
#pragma pack(pop)
static_assert(sizeof(cBrush_t) == 32, "cBrush_t");

// ── Pre-baked local data (zero DMA in hot path) ───────────────────────────────

struct LocalNode {
    cplane_t plane;      // pre-read from node.planePtr
    int16_t  childFront;
    int16_t  childBack;
};

struct LocalBrushSide {
    cplane_t plane;      // pre-read from brushSide.planePtr
    uint32_t cflags;     // content flags (SOLID = bit 0)
};

struct LocalBrush {
    float    mins[3];        // center - halfExtent
    float    maxs[3];        // center + halfExtent
    int32_t  firstSide;      // index into localBrushSides
    int32_t  numSides;
};

// ── ClipMapCache ──────────────────────────────────────────────────────────────

class ClipMapCache {
public:
    bool initialized = false;

    std::vector<LocalNode>      nodes;
    std::vector<LocalBrushSide> brushSides;
    std::vector<LocalBrush>     brushes;
    std::vector<cLeaf_t>        leafs;
    std::vector<uint16_t>       leafBrushes;  // per-leaf brush index list

    // ── FindAndCache ──
    // Reads all BSP data once. Resolves all plane ptrs to local structs.
    // After this returns true, all traces are pure CPU work (no DMA).
    bool FindAndCache(uint32_t /*unused*/) {
        if (initialized) return true;

        // Read the six globals
        uint32_t pBrushSides = Memory::Read<uint32_t>(T6CM::G_BRUSHSIDES);
        uint32_t pLeafBrush  = Memory::Read<uint32_t>(T6CM::G_LEAFBRUSH);
        uint32_t pNodes      = Memory::Read<uint32_t>(T6CM::G_NODES);
        uint32_t pLeafs      = Memory::Read<uint32_t>(T6CM::G_LEAFS);
        uint32_t pBrushes    = Memory::Read<uint32_t>(T6CM::G_BRUSHES);

        printf("[TRACE] CM ptrs: nodes=0x%X leafs=0x%X brushSides=0x%X brushes=0x%X\n",
               pNodes, pLeafs, pBrushSides, pBrushes);

        if (!T6CM::IsPtr(pNodes) || !T6CM::IsPtr(pLeafs) ||
            !T6CM::IsPtr(pBrushSides) || !T6CM::IsPtr(pBrushes)) {
            printf("[TRACE] CM pointers not ready (map not loaded?)\n");
            return false;
        }

        // ── Verify node[0] ──
        cNode_t n0 = Memory::Read<cNode_t>(pNodes);
        if (!T6CM::IsPtr(n0.planePtr)) {
            printf("[TRACE] node[0].planePtr 0x%X invalid\n", n0.planePtr);
            return false;
        }
        cplane_t p0 = Memory::Read<cplane_t>(n0.planePtr);
        float nl = sqrtf(p0.normal[0]*p0.normal[0] + p0.normal[1]*p0.normal[1] + p0.normal[2]*p0.normal[2]);
        printf("[TRACE] node[0]: planePtr=0x%X  plane: n=(%.3f,%.3f,%.3f) d=%.1f |n|=%.3f\n",
               n0.planePtr, p0.normal[0], p0.normal[1], p0.normal[2], p0.dist, nl);
        if (nl < 0.85f || nl > 1.15f) {
            printf("[TRACE] plane[0] not unit-normal — BSP not ready\n");
            return false;
        }

        // ── Read counts (stored 4 bytes BEFORE each pointer) ──
        uint32_t numNodes     = Memory::Read<uint32_t>(T6CM::G_NODES     - 4);
        uint32_t numLeafs     = Memory::Read<uint32_t>(T6CM::G_LEAFS     - 4);
        uint32_t numBrushSides= Memory::Read<uint32_t>(T6CM::G_BRUSHSIDES- 4);
        uint32_t numBrushes   = Memory::Read<uint32_t>(T6CM::G_BRUSHES   - 4);

        printf("[TRACE] CM raw counts: nodes=%u leafs=%u brushSides=%u brushes=%u\n",
               numNodes, numLeafs, numBrushSides, numBrushes);

        // Validate counts
        bool ok = numNodes > 10 && numNodes < 300000 &&
                  numLeafs > 10 && numLeafs < 300000 &&
                  numBrushSides > 0 && numBrushSides < 1000000 &&
                  numBrushes > 10 && numBrushes < 200000;
        if (!ok) {
            // Try +4 offset (alternate cm layout)
            numNodes      = Memory::Read<uint32_t>(T6CM::G_NODES      + 4);
            numLeafs      = Memory::Read<uint32_t>(T6CM::G_LEAFS      + 4);
            numBrushSides = Memory::Read<uint32_t>(T6CM::G_BRUSHSIDES + 4);
            numBrushes    = Memory::Read<uint32_t>(T6CM::G_BRUSHES    + 4);
            printf("[TRACE] CM alt counts: nodes=%u leafs=%u brushSides=%u brushes=%u\n",
                   numNodes, numLeafs, numBrushSides, numBrushes);
            ok = numNodes > 10 && numNodes < 300000 &&
                 numLeafs > 10 && numLeafs < 300000 &&
                 numBrushSides > 0 && numBrushSides < 1000000 &&
                 numBrushes > 10 && numBrushes < 200000;
        }

        // If still bad, scan node count by probing valid plane ptrs
        if (!ok || numNodes > 100000) {
            printf("[TRACE] Scanning node count...\n");
            numNodes = 0;
            for (uint32_t i = 0; i < 50000; i++) {
                cNode_t n = Memory::Read<cNode_t>(pNodes + i * 8);
                if (!T6CM::IsPtr(n.planePtr)) { numNodes = i; break; }
            }
            if (numNodes == 0) numNodes = 5000;
            printf("[TRACE] Scanned numNodes=%u\n", numNodes);
        }

        // Cap counts to reasonable maxes
        numNodes      = cmMin(numNodes,      100000u);
        numLeafs      = ok ? cmMin(numLeafs, 100000u) : 20000u;
        numBrushSides = ok ? cmMin(numBrushSides, 500000u) : 5000u;
        numBrushes    = ok ? cmMin(numBrushes, 100000u) : 20000u;


        // ── Build plane lookup: gameAddr → local plane ──
        // We collect all unique plane ptrs from nodes and brushSides.
        std::unordered_map<uint32_t, cplane_t> planeCache;
        auto GetPlane = [&](uint32_t ptr) -> cplane_t {
            auto it = planeCache.find(ptr);
            if (it != planeCache.end()) return it->second;
            cplane_t pl = Memory::Read<cplane_t>(ptr);
            planeCache[ptr] = pl;
            return pl;
        };

        // ── Load nodes with inline planes ──
        printf("[TRACE] Loading %u nodes...\n", numNodes);
        nodes.reserve(numNodes);
        for (uint32_t i = 0; i < numNodes; i++) {
            cNode_t raw = Memory::Read<cNode_t>(pNodes + i * 8);
            LocalNode ln;
            ln.childFront = raw.childFront;
            ln.childBack  = raw.childBack;
            if (T6CM::IsPtr(raw.planePtr))
                ln.plane = GetPlane(raw.planePtr);
            else
                memset(&ln.plane, 0, sizeof(ln.plane));
            nodes.push_back(ln);
        }

        // ── Load brushes first — needed to compute actual max brushSide index ──
        printf("[TRACE] Loading %u brushes...\n", numBrushes);
        brushes.reserve(numBrushes);
        uint32_t maxBrushSideRef = 0;
        for (uint32_t i = 0; i < numBrushes; i++) {
            cBrush_t raw = Memory::Read<cBrush_t>(pBrushes + i * 32);
            LocalBrush lb;
            for (int a = 0; a < 3; a++) {
                lb.mins[a] = raw.center[a] - raw.halfExtent[a];
                lb.maxs[a] = raw.center[a] + raw.halfExtent[a];
            }
            lb.firstSide = (int32_t)(uint16_t)raw.firstSideIdx;
            lb.numSides  = (int32_t)(uint16_t)raw.numSides;
            brushes.push_back(lb);
            // Track highest side index actually referenced
            if (lb.numSides > 0) {
                uint32_t lastIdx = (uint32_t)lb.firstSide + (uint32_t)lb.numSides;
                if (lastIdx > maxBrushSideRef) maxBrushSideRef = lastIdx;
            }
        }

        // Override numBrushSides with the actual count needed (what brushes actually reference)
        // The global count (704) was from a different field and is wrong.
        if (maxBrushSideRef > numBrushSides) {
            printf("[TRACE] BrushSide count override: global=%u → actual_max_ref=%u\n",
                   numBrushSides, maxBrushSideRef);
            numBrushSides = maxBrushSideRef;
        }
        numBrushSides = cmMin(numBrushSides, 500000u);

        // ── Load brush sides with inline planes ──
        printf("[TRACE] Loading %u brushSides...\n", numBrushSides);
        brushSides.reserve(numBrushSides);
        for (uint32_t i = 0; i < numBrushSides; i++) {
            cBrushSide_t raw = Memory::Read<cBrushSide_t>(pBrushSides + i * 12);
            LocalBrushSide ls;
            ls.cflags = raw.cflags;
            if (T6CM::IsPtr(raw.planePtr))
                ls.plane = GetPlane(raw.planePtr);
            else
                memset(&ls.plane, 0, sizeof(ls.plane));
            brushSides.push_back(ls);
        }

        // ── Load leafs ──
        printf("[TRACE] Loading %u leafs...\n", numLeafs);
        leafs.resize(numLeafs);
        for (uint32_t i = 0; i < numLeafs; i++)
            leafs[i] = Memory::Read<cLeaf_t>(pLeafs + i * 12);

        // ── Load leaf-brush index array ──
        uint32_t maxLeafBrush = 0;
        for (auto& lf : leafs) {
            uint32_t end = lf.firstLeafBrush + lf.numLeafBrushes;
            if (end > maxLeafBrush) maxLeafBrush = end;
        }
        maxLeafBrush = cmMin(maxLeafBrush, 200000u);

        leafBrushes.resize(maxLeafBrush);
        for (uint32_t i = 0; i < maxLeafBrush; i++)
            leafBrushes[i] = Memory::Read<uint16_t>(pLeafBrush + i * 2);


        // ── Sanity report ──
        uint32_t validBrushes = 0, totalSolid = 0;
        for (auto& b : brushes) {
            bool geomOk = (b.maxs[0] > b.mins[0]) && (b.maxs[1] > b.mins[1]) && (b.maxs[2] > b.mins[2]);
            if (geomOk) validBrushes++;
            if (geomOk && b.numSides > 0) totalSolid++;
        }

        printf("[TRACE] *** clipMap READY ***\n");
        printf("[TRACE]   nodes=%zu  brushSides=%zu  brushes=%zu  leafs=%zu  leafBrushes=%zu\n",
               nodes.size(), brushSides.size(), brushes.size(), leafs.size(), leafBrushes.size());
        printf("[TRACE]   geometrically-valid brushes: %u  (numSides>0: %u)\n", validBrushes, totalSolid);
        printf("[TRACE]   cached %zu unique planes\n", planeCache.size());

        if (!brushes.empty()) {
            auto& b0 = brushes[0];
            printf("[TRACE]   brush[0]: mins=(%.1f,%.1f,%.1f) maxs=(%.1f,%.1f,%.1f) sides=%d\n",
                   b0.mins[0], b0.mins[1], b0.mins[2],
                   b0.maxs[0], b0.maxs[1], b0.maxs[2], b0.numSides);
        }

        initialized = true;
        return true;
    }

    // ── Plane-ray intersection helper ──────────────────────────────────────────
    // Returns signed distance from point to plane.
    static float PlaneDist(const cplane_t& pl, const float p[3]) {
        if (pl.type < 3)
            return p[pl.type] - pl.dist;
        return pl.normal[0]*p[0] + pl.normal[1]*p[1] + pl.normal[2]*p[2] - pl.dist;
    }

    // ── AABB slab ray-segment intersection ────────────────────────────────────
    // Returns true if segment [start → end] passes through the brush AABB.
    bool RayHitsBrush(const float start[3], const float end[3], const LocalBrush& brush) const {
        if (brush.numSides <= 0) return false;

        // Compute extents and validate
        float size[3];
        for (int a = 0; a < 3; a++) {
            size[a] = brush.maxs[a] - brush.mins[a];
            if (size[a] <= 0.0f) return false;
        }

        // Skip map-boundary / skybox brushes (any axis > 1500 units = not a wall)
        // Hijacked walls are typically 8-600 units thick. Skybox brushes span thousands.
        if (size[0] > 1500.0f && size[1] > 1500.0f) return false;  // huge XY = skybox floor/ceiling
        if (size[0] > 1500.0f && size[2] > 1500.0f) return false;  // huge XZ = side boundary
        if (size[1] > 1500.0f && size[2] > 1500.0f) return false;  // huge YZ = side boundary

        // Require minimum thickness on at least one axis (skip zero-thickness triggers)
        if (size[0] < 2.0f && size[1] < 2.0f && size[2] < 2.0f) return false;

        // If camera is inside this brush, it can't block us
        bool insideBrush = true;
        for (int a = 0; a < 3; a++) {
            if (start[a] < brush.mins[a] || start[a] > brush.maxs[a]) {
                insideBrush = false; break;
            }
        }
        if (insideBrush) return false;

        // Slab method: parametric t range where ray is inside AABB.
        // tMin starts at 0.06 to skip brush hits very close to the camera —
        // the camera sits inside/adjacent to structural brushes (house floors,
        // ceilings, walls) that would otherwise falsely block rays to nearby
        // visible enemies. 6% = ~30-60 units for typical combat distances.
        float tMin = 0.06f, tMax = 1.0f;
        for (int a = 0; a < 3; a++) {
            float d = end[a] - start[a];
            if (fabsf(d) < 1e-6f) {
                if (start[a] < brush.mins[a] || start[a] > brush.maxs[a]) return false;
            } else {
                float ood = 1.0f / d;
                float t1 = (brush.mins[a] - start[a]) * ood;
                float t2 = (brush.maxs[a] - start[a]) * ood;
                if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
                tMin = t1 > tMin ? t1 : tMin;
                tMax = t2 < tMax ? t2 : tMax;
                if (tMin > tMax) return false;
            }
        }
        return true;
    }



    // ── BSP recursive traversal ────────────────────────────────────────────────
    // Returns true if ANY solid brush blocks the segment.
    // All data is local — zero DMA reads.
    bool BSP_Trace(int32_t nodeIdx, const float start[3], const float end[3]) const {
        // Leaf
        if (nodeIdx < 0) {
            int32_t leafIdx = -1 - nodeIdx;
            if (leafIdx < 0 || leafIdx >= (int32_t)leafs.size()) return false;
            const cLeaf_t& leaf = leafs[leafIdx];
            for (int li = 0; li < leaf.numLeafBrushes; li++) {
                uint32_t lbIdx = leaf.firstLeafBrush + li;
                if (lbIdx >= leafBrushes.size()) break;
                uint32_t bIdx = leafBrushes[lbIdx];
                if (bIdx >= brushes.size()) continue;
                if (RayHitsBrush(start, end, brushes[bIdx])) return true;
            }
            return false;
        }

        if (nodeIdx >= (int32_t)nodes.size()) return false;
        const LocalNode& node = nodes[nodeIdx];
        const cplane_t& pl = node.plane;

        float d1 = PlaneDist(pl, start);
        float d2 = PlaneDist(pl, end);

        // Both on same side — only visit that child
        if (d1 >= 0.0f && d2 >= 0.0f)
            return BSP_Trace(node.childFront, start, end);
        if (d1 < 0.0f && d2 < 0.0f)
            return BSP_Trace(node.childBack,  start, end);

        // Straddling the plane — visit near side first, then far
        float frac = d1 / (d1 - d2);
        float mid[3] = {
            start[0] + frac * (end[0] - start[0]),
            start[1] + frac * (end[1] - start[1]),
            start[2] + frac * (end[2] - start[2])
        };

        int32_t nearChild = (d1 >= 0.0f) ? node.childFront : node.childBack;
        int32_t farChild  = (d1 >= 0.0f) ? node.childBack  : node.childFront;

        if (BSP_Trace(nearChild, start, mid)) return true;
        return BSP_Trace(farChild, mid, end);
    }

    // ── High-level visibility check ────────────────────────────────────────────
    // Returns true if there is clear line-of-sight from (ex,ey,ez) to (tx,ty,tz).
    bool IsVisible(float ex, float ey, float ez,
                   float tx, float ty, float tz) const {
        if (!initialized) return true;

        const float start[3] = { ex, ey, ez };
        const float end[3]   = { tx, ty, tz };

        for (const auto& b : brushes) {
            if (b.numSides <= 0) continue;
            if (RayHitsBrush(start, end, b)) return false;  // occluded
        }
        return true;  // visible
    }


    void Reset() {
        initialized = false;
        nodes.clear(); brushSides.clear(); brushes.clear();
        leafs.clear(); leafBrushes.clear();
    }
};

static ClipMapCache g_clipMap;
