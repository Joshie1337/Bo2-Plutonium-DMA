#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <tchar.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <chrono>

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_dx11.h"
#include "menu.h"
#include "elements/elements.h"
#include "fonts.h"
#include "Memory.h"
#include "OverlayDX11.h"
#include "trace.h"
#include "makcu/makcu_wrapper.h"
#include "DmaCache.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwmapi.lib")

#include <vector>
#include <chrono>

struct MonitorInfo {
    RECT rect;
};
std::vector<MonitorInfo> g_monitors;

static BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    MonitorInfo info;
    info.rect = *lprcMonitor;
    g_monitors.push_back(info);
    return TRUE;
}

void UpdateMonitorsList() {
    g_monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, 0);
}

static ID3D11Device*            g_pd3dDevice           = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext     = nullptr;
UINT                            g_ResizeWidth           = 0;
UINT                            g_ResizeHeight          = 0;
static ID3D11ShaderResourceView* g_pBackgroundTexture   = nullptr;
ImFont* g_logo_font = nullptr;
HWND g_hwnd = nullptr;
float g_menu_anim = 0.0f;
OverlayDX11 g_overlay;

static bool LoadTextureFromFile(const char* filename, ID3D11ShaderResourceView** out_srv, int* out_width, int* out_height) {
    IWICImagingFactory* pFactory = nullptr;
    IWICBitmapDecoder* pDecoder = nullptr;
    IWICBitmapFrameDecode* pFrame = nullptr;
    IWICFormatConverter* pConverter = nullptr;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory));
    if (FAILED(hr)) return false;

    wchar_t wfilename[512];
    MultiByteToWideChar(CP_ACP, 0, filename, -1, wfilename, 512);

    hr = pFactory->CreateDecoderFromFilename(wfilename, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &pDecoder);
    if (FAILED(hr)) { pFactory->Release(); return false; }

    hr = pDecoder->GetFrame(0, &pFrame);
    if (FAILED(hr)) { pDecoder->Release(); pFactory->Release(); return false; }

    hr = pFactory->CreateFormatConverter(&pConverter);
    if (FAILED(hr)) { pFrame->Release(); pDecoder->Release(); pFactory->Release(); return false; }

    hr = pConverter->Initialize(pFrame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) { pConverter->Release(); pFrame->Release(); pDecoder->Release(); pFactory->Release(); return false; }

    UINT width, height;
    pConverter->GetSize(&width, &height);

    BYTE* pPixels = new BYTE[width * height * 4];
    pConverter->CopyPixels(nullptr, width * 4, width * height * 4, pPixels);

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA subResource;
    subResource.pSysMem = pPixels;
    subResource.SysMemPitch = width * 4;
    subResource.SysMemSlicePitch = 0;

    ID3D11Texture2D* pTexture = nullptr;
    hr = g_pd3dDevice->CreateTexture2D(&desc, &subResource, &pTexture);
    delete[] pPixels;

    if (FAILED(hr)) { pConverter->Release(); pFrame->Release(); pDecoder->Release(); pFactory->Release(); return false; }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    hr = g_pd3dDevice->CreateShaderResourceView(pTexture, &srvDesc, out_srv);
    pTexture->Release();

    if (out_width) *out_width = width;
    if (out_height) *out_height = height;

    pConverter->Release();
    pFrame->Release();
    pDecoder->Release();
    pFactory->Release();

    return SUCCEEDED(hr);
}

struct sTrajectory {
    int trType;
    int trTime;
    int trDuration;
    Vector3 trBase;
    Vector3 trDelta;
};

// ══════════════════════════════════════════════════════════════════════════════
// DObj Bone System v3 — Dynamic offset discovery for Plutonium T6 MP
// ══════════════════════════════════════════════════════════════════════════════
// Key UC offsets: GetDObj=0x4DA190, CG=0x113F18C
//
// The old code hardcoded sDObj with pBoneMatrix at +0x04 — WRONG.
// IW engine DObj (CoD4x ref) has bone matrix inside DSkel at ~+0x48.
// Rather than guessing the T6 layout, we brute-force scan DObj memory
// for a pointer to valid DObjAnimMat data (quaternion magnitude ≈ 1.0).
//
// DObjAnimMat layout (confirmed from CoD4x + UC):
//   +0x00: float quat[4]    — rotation quaternion
//   +0x10: float trans[3]   — bone position (world or model space)
//   +0x1C: float transWeight
// ══════════════════════════════════════════════════════════════════════════════

struct sDObjAnimMat {
    float orientation[4];  // +0x00  quaternion (x,y,z,w)
    float translation[3];  // +0x10  bone position
    float transWeight;     // +0x1C  blend weight
}; // sizeof = 0x20

// BO2 T6 bone indices — corrected mapping from bone dump analysis.
// Based on actual bone dump: bone[0]=root, bone[1]=pelvis, bones 2/6/8/14 are duplicates.
// The game stores bones in hierarchy order. We need to find the correct indices.
// For now, we use a FLEXIBLE bone system that maps by position heuristics.
enum BO2BoneIdx {
    BONE_MAINROOT   = 0,   // j_mainroot (feet, dZ≈0)
    BONE_PELVIS     = 1,   // pelvis center (dZ≈31)
    BONE_L_HIP      = 2,   // left hip  
    BONE_R_HIP      = 3,   // right hip
    BONE_L_CLAV     = 4,   // left clavicle
    BONE_R_CLAV     = 5,   // right clavicle
    BONE_CHEST      = 6,   // chest/spine4
    BONE_SPINE      = 7,   // spine mid
    BONE_L_ELBOW    = 8,   // left elbow
    BONE_R_ELBOW    = 9,   // right elbow
    BONE_NECK       = 10,  // neck
    BONE_L_WRIST    = 11,  // left wrist
    BONE_R_WRIST    = 12,  // right wrist
    BONE_L_KNEE     = 13,  // left knee
    BONE_R_KNEE     = 14,  // right knee
    BONE_HEAD       = 15,  // j_head
    BONE_L_ANKLE    = 16,  // left ankle
    BONE_R_ANKLE    = 17,  // right ankle
    BONE_COUNT      = 18   // We'll map these dynamically
};

// Bone connection hierarchy for drawing (parent -> child)
struct BoneConnection {
    int parentIdx;
    int childIdx;
    const char* name;
};

// Bone connections to draw
static const BoneConnection g_boneConnections[] = {
    {BONE_MAINROOT, BONE_PELVIS, "root->pelvis"},
    {BONE_PELVIS, BONE_L_HIP, "pelvis->l_hip"},
    {BONE_PELVIS, BONE_R_HIP, "pelvis->r_hip"},
    {BONE_PELVIS, BONE_CHEST, "pelvis->chest"},  // spine chain
    {BONE_CHEST, BONE_NECK, "chest->neck"},
    {BONE_NECK, BONE_HEAD, "neck->head"},
    {BONE_L_HIP, BONE_L_KNEE, "l_hip->l_knee"},
    {BONE_L_KNEE, BONE_L_ANKLE, "l_knee->l_ankle"},
    {BONE_R_HIP, BONE_R_KNEE, "r_hip->r_knee"},
    {BONE_R_KNEE, BONE_R_ANKLE, "r_knee->r_ankle"},
    {BONE_L_CLAV, BONE_L_ELBOW, "l_clav->l_elbow"},
    {BONE_L_ELBOW, BONE_L_WRIST, "l_elbow->l_wrist"},
    {BONE_R_CLAV, BONE_R_ELBOW, "r_clav->r_elbow"},
    {BONE_R_ELBOW, BONE_R_WRIST, "r_elbow->r_wrist"},
    {BONE_CHEST, BONE_L_CLAV, "chest->l_clav"},
    {BONE_CHEST, BONE_R_CLAV, "chest->r_clav"},
};

const int g_numConnections = sizeof(g_boneConnections) / sizeof(g_boneConnections[0]);

// ── Cached bone system state ─────────────────────────────────────────────────
static bool      g_boneInitDone   = false;  // One-time init completed?
static uintptr_t g_handleTable    = 0;      // DObj handle table address
static uintptr_t g_dobjBase       = 0;      // DObj clients base address
static size_t    g_dobjSize       = 0x7C;   // Size of each DObj entry
static int       g_boneMatOffset  = -1;     // Cached offset in DObj for bone mat ptr
static bool      g_boneWorldSpace = true;   // Are bones in world-space or model-space?
static bool      g_boneScanDone   = false;  // Have we printed the first scan diagnostic?
static bool      g_boneEverFound  = false;  // Have bones EVER been successfully read?
static bool      g_boneDumpDone   = false;  // One-time full bone dump printed?

// ── DEBUG: raw bone visualization ──────────────────────────────────────────
static bool      g_debugBoneDots  = false;  // Draw all raw bones as dots (set true to debug)
static bool      g_debugDumpOnce  = false;  // One-time dump flag
constexpr int    DEBUG_MAX_BONES  = 160;
static Vector3   g_debugRawBones[DEBUG_MAX_BONES]; // Copy of last entity's raw bones
static bool      g_debugBonesValid = false;
static Vector3   g_debugEntityOrigin;        // Entity origin for distance filtering

// Helper struct for batch DMA reads
struct RawBlock256 { uint8_t data[256]; };

// ── Extract DObj addresses from GetDObj function (0x4DA190) ──────────────────
static void InitBoneSystem() {
    if (g_boneInitDone) return;
    g_boneInitDone = true;

    printf("\n");
    printf("================================================================\n");
    printf("  BONE SYSTEM v3 - Dynamic Offset Discovery\n");
    printf("================================================================\n");

    // Read GetDObj function bytes in one batch (not byte-by-byte)
    printf("\n[BONE] Reading GetDObj (0x4DA190) function bytes...\n");
    RawBlock256 funcRaw = Memory::Read<RawBlock256>(0x4DA190);

    // Hex dump
    for (int row = 0; row < 8; row++) {
        printf("  0x%08X: ", 0x4DA190 + row * 16);
        for (int col = 0; col < 16; col++)
            printf("%02X ", funcRaw.data[row * 16 + col]);
        printf("\n");
    }

    // Extract addresses using x86 instruction pattern matching.
    // From the hex dump above, GetDObj follows this pattern:
    //   movzx eax, word [eax*2 + HANDLE_TABLE]  →  0F B7 04 45 [4-byte addr]
    //   imul  eax, eax, DOBJ_SIZE               →  6B C0 [1-byte size]
    //   add   eax, DOBJ_BASE                    →  05 [4-byte addr]
    printf("\n[BONE] Parsing x86 instructions for DObj addresses...\n");

    // Pattern 1: movzx eax, word ptr [eax*2 + IMM32] → handle table
    for (int i = 0; i < 120; i++) {
        if (funcRaw.data[i] == 0x0F && funcRaw.data[i+1] == 0xB7 &&
            funcRaw.data[i+2] == 0x04 && funcRaw.data[i+3] == 0x45) {
            g_handleTable = *(uint32_t*)(funcRaw.data + i + 4);
            printf("  [+0x%02X] movzx eax,[eax*2+0x%08X] → HANDLE TABLE\n",
                   i, (unsigned)g_handleTable);
            break;
        }
        // Also check movzx with other SIB variants (0x4D = ecx*2, etc)
        if (funcRaw.data[i] == 0x0F && funcRaw.data[i+1] == 0xB7 &&
            funcRaw.data[i+2] == 0x04) {
            uint8_t sib = funcRaw.data[i+3];
            // SIB scale=1 (*2) check: bits 7:6 = 01, bits 2:0 = 101 (disp32, no base)
            if ((sib & 0xC7) == 0x45) {
                g_handleTable = *(uint32_t*)(funcRaw.data + i + 4);
                printf("  [+0x%02X] movzx eax,[reg*2+0x%08X] → HANDLE TABLE\n",
                       i, (unsigned)g_handleTable);
                break;
            }
        }
    }

    // Pattern 2: imul eax, eax, IMM8 → DObj entry size
    for (int i = 0; i < 126; i++) {
        if (funcRaw.data[i] == 0x6B && funcRaw.data[i+1] == 0xC0) {
            g_dobjSize = funcRaw.data[i + 2];
            printf("  [+0x%02X] imul eax,eax,0x%X → DOBJ SIZE = %u bytes\n",
                   i, (unsigned)g_dobjSize, (unsigned)g_dobjSize);
            break;
        }
        // Also check imul eax, eax, IMM32
        if (funcRaw.data[i] == 0x69 && funcRaw.data[i+1] == 0xC0) {
            uint32_t sz = *(uint32_t*)(funcRaw.data + i + 2);
            if (sz >= 0x40 && sz <= 0x200) {
                g_dobjSize = sz;
                printf("  [+0x%02X] imul eax,eax,0x%X → DOBJ SIZE = %u bytes\n",
                       i, (unsigned)g_dobjSize, (unsigned)g_dobjSize);
                break;
            }
        }
    }

    // Pattern 3: add eax, IMM32 → DObj clients base
    for (int i = 0; i < 124; i++) {
        if (funcRaw.data[i] == 0x05) {
            uint32_t val = *(uint32_t*)(funcRaw.data + i + 1);
            if (val > 0x01000000 && val < 0x10000000) {
                g_dobjBase = val;
                printf("  [+0x%02X] add eax,0x%08X → DOBJ BASE\n",
                       i, (unsigned)g_dobjBase);
                break;
            }
        }
        // Also check lea eax, [eax + IMM32]: 8D 80 [4-byte addr]
        if (funcRaw.data[i] == 0x8D && funcRaw.data[i+1] == 0x80) {
            uint32_t val = *(uint32_t*)(funcRaw.data + i + 2);
            if (val > 0x01000000 && val < 0x10000000) {
                g_dobjBase = val;
                printf("  [+0x%02X] lea eax,[eax+0x%08X] → DOBJ BASE\n",
                       i, (unsigned)g_dobjBase);
                break;
            }
        }
    }

    if (g_handleTable && g_dobjBase) {
        printf("[BONE] Extracted: handleTable=0x%08X dobjBase=0x%08X dobjSize=0x%X\n",
               (unsigned)g_handleTable, (unsigned)g_dobjBase, (unsigned)g_dobjSize);
    }

    // Fallback: vanilla T6 addresses (these returned handle=40 in previous diagnostic)
    if (g_handleTable == 0) {
        g_handleTable = 0x0262D1C0;
        g_dobjBase    = 0x025EF1B8;
        g_dobjSize    = 0x7C;
        printf("[BONE] Using known fallback: handleTable=0x%08X dobjBase=0x%08X\n",
               (unsigned)g_handleTable, (unsigned)g_dobjBase);
    }

    printf("[BONE] Init complete. Will scan for bone matrix on first valid entity.\n\n");
}

// ── Read bone positions for an entity ────────────────────────────────────────
// Uses HEURISTIC bone mapping based on relative positions to pelvis/origin
inline bool ReadBonePositions(int localClientNum, int entityIdx,
                              const Vector3& entityOrigin, Vector3 bonePos[BONE_COUNT])
{
    if (!Memory::IsValid()) return false;

    // One-time init: extract DObj addresses from GetDObj function
    if (!g_boneInitDone) InitBoneSystem();
    if (g_handleTable == 0) return false;

    // ── Resolve entity → DObj pointer via handle table ──
    // T6/BO2: The DObj handle table is indexed directly by entity number.
    // The BO6 formula (entityIdx + localClientNum*0x701) is wrong for T6.
    uint16_t handle = Memory::Read<uint16_t>(g_handleTable + (entityIdx * 2));
    uintptr_t dobjPtr = 0;
    if (handle != 0) {
        dobjPtr = g_dobjBase + ((uintptr_t)handle * g_dobjSize);
    }

    // Debug: print handle/dobjPtr once per entity to verify
    static DWORD s_lastHandleDbg = 0;
    if (GetTickCount() - s_lastHandleDbg > 5000) {
        s_lastHandleDbg = GetTickCount();
        printf("[DOBJ] entity=%d handle=0x%04X dobjPtr=0x%08X\n",
               entityIdx, (unsigned)handle, (unsigned)dobjPtr);
    }

    // ── DObj path: only when we have a valid handle ──
    if (dobjPtr != 0) {
        // Raw DObj struct dump - consumed by the offset-scan below. This read was
        // previously missing, leaving dobjRaw undefined in that path (compile error).
        RawBlock256 dobjRaw = Memory::Read<RawBlock256>(dobjPtr);

    // ── Fast path: use cached bone matrix offset ──
    if (g_boneMatOffset >= 0 && dobjPtr != 0) {
        uint32_t boneMatPtr = Memory::Read<uint32_t>(dobjPtr + g_boneMatOffset);
        if (boneMatPtr > 0x10000 && boneMatPtr < 0x7FFFFFFF) {
            // *** BODY-ONLY SCAN LIMIT ***
            // The DObjAnimMat array contains bones from ALL sub-models:
            //   indices 0-~65:  player body skeleton
            //   indices 70+:    weapon model, attachments, accessories
            // Weapon bones are at world positions near the player (attached to hands)
            // so they pass the distance filter, but they're NOT body bones.
            // Diagnostic data confirmed: stable body bones use indices 1-65,
            // all bad upper-body mappings (HEAD[140], CHEST[151], L_CLAV[158])
            // came from weapon/attachment bones at indices 100+.
            constexpr int BODY_BONE_LIMIT = 70;

            // *** BATCH READ: all bones in ONE DMA read ***
            constexpr int MAX_BONES_TO_READ = 160;
            struct BoneBlock { sDObjAnimMat bones[MAX_BONES_TO_READ]; };
            BoneBlock block = Memory::Read<BoneBlock>(boneMatPtr);

            // Only scan body bones, never weapon/attachment bones
            int scanLimit = BODY_BONE_LIMIT;

            // First pass: read all bone positions into temp array
            Vector3 rawBones[MAX_BONES_TO_READ];
            bool boneQuatValid[MAX_BONES_TO_READ]; // quaternion validity per bone
            bool anyValid = false;
            for (int b = 0; b < MAX_BONES_TO_READ; b++) {
                float bx, by, bz;
                if (g_boneWorldSpace) {
                    bx = block.bones[b].translation[0];
                    by = block.bones[b].translation[1];
                    bz = block.bones[b].translation[2];
                } else {
                    bx = entityOrigin.x + block.bones[b].translation[0];
                    by = entityOrigin.y + block.bones[b].translation[1];
                    bz = entityOrigin.z + block.bones[b].translation[2];
                }
                rawBones[b] = Vector3(bx, by, bz);

                // Validate quaternion: must be unit length (|q| ≈ 1.0)
                float* q = block.bones[b].orientation;
                float qMag = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
                boneQuatValid[b] = (qMag > 0.5f && qMag < 1.5f);

                // Validate: bone must be within 200 units of entity origin
                float dx = bx - entityOrigin.x;
                float dy = by - entityOrigin.y;
                float dz = bz - entityOrigin.z;
                float distSq = dx*dx + dy*dy + dz*dz;
                if (bx != 0.0f && distSq < 200.f * 200.f && b < scanLimit)
                    anyValid = true;
            }

            if (anyValid) {
                // ═══ DEBUG: store raw bones for dot visualization ═══
                if (g_debugBoneDots && !g_debugBonesValid) {
                    memcpy(g_debugRawBones, rawBones, sizeof(Vector3) * MAX_BONES_TO_READ);
                    g_debugEntityOrigin = entityOrigin;
                    g_debugBonesValid = true;

                    // One-time console dump of all valid bones
                    if (!g_debugDumpOnce) {
                        g_debugDumpOnce = true;
                        printf("\n");
                        printf("══════════════════════════════════════════════════════════════\n");
                        printf("  RAW BONE DUMP — Entity %d  (origin: %.1f, %.1f, %.1f)\n",
                               entityIdx, entityOrigin.x, entityOrigin.y, entityOrigin.z);
                        printf("  CoD is Z-UP: higher Z = higher on body\n");
                        printf("══════════════════════════════════════════════════════════════\n");
                        for (int b = 0; b < MAX_BONES_TO_READ; b++) {
                            if (rawBones[b].x == 0 && rawBones[b].y == 0 && rawBones[b].z == 0) continue;
                            float dx = rawBones[b].x - entityOrigin.x;
                            float dy = rawBones[b].y - entityOrigin.y;
                            float dz = rawBones[b].z - entityOrigin.z;
                            float dist = sqrtf(dx*dx + dy*dy + dz*dz);
                            if (dist > 200.f) continue;
                            printf("  [%3d] pos=(%.1f, %.1f, %.1f)  dZ=%.1f  lat=%.1f  dist=%.1f\n",
                                   b, rawBones[b].x, rawBones[b].y, rawBones[b].z,
                                   dz, sqrtf(dx*dx + dy*dy), dist);
                        }
                        printf("══════════════════════════════════════════════════════════════\n\n");
                    }
                }

                // ═══════════════════════════════════════════════════════════
                //  T6 PLAYER SKELETON — FIXED BONE INDEX TABLE
                //  Every bone dump confirms T6 MP models use identical skeleton
                //  indices. No heuristic position mapper needed.
                // ═══════════════════════════════════════════════════════════
                
                // Validated indices from multiple bone dumps across entities:
                //   bone[0]  = j_mainroot    (always at entity origin)
                //   bone[1]  = j_hip         (pelvis, dZ≈31-34)
                //   bone[7]  = j_spinelower  (lower chest, dZ≈36-39)
                //   bone[9]  = j_hip_le      (left thigh top, dZ≈28-31)
                //   bone[10] = j_hip_ri      (right thigh top, dZ≈28-30)
                //   bone[11] = j_knee_le     (left knee, dZ≈16-17)
                //   bone[12] = j_knee_ri     (right knee, dZ≈14-18)
                //   bone[13] = j_spineupper  (chest, dZ≈41-44)
                //   bone[15] = j_ankle_le    (left ankle, dZ≈3-5)
                //   bone[16] = j_ankle_ri    (right ankle area)
                //   bone[22] = j_neck        (neck, dZ≈47-51)
                //   bone[24] = j_head        (head center, dZ≈50-54)
                //   bone[42] = j_clavicle_le (left shoulder, lat≈18-23)
                //   bone[43] = j_clavicle_ri (right shoulder, lat≈18-23)
                //   bone[66] = j_elbow_le    (left elbow, lat≈23-26)
                //   bone[67] = j_elbow_ri    (right elbow, lat≈20-24)
                //   bone[68] = j_wrist_le    (left wrist/forearm)
                //   bone[69] = j_wrist_ri    (right wrist/forearm)

                // Helper: read a bone by index with distance validation
                auto safeReadBone = [&](int idx, const Vector3& fallback) -> Vector3 {
                    if (idx < 0 || idx >= scanLimit) return fallback;
                    Vector3& b = rawBones[idx];
                    if (b.x == 0.f && b.y == 0.f && b.z == 0.f) return fallback;
                    float dx = b.x - entityOrigin.x;
                    float dy = b.y - entityOrigin.y;
                    float dz = b.z - entityOrigin.z;
                    if (dx*dx + dy*dy + dz*dz > 120.f * 120.f) return fallback;
                    return b;
                };

                bonePos[BONE_MAINROOT] = entityOrigin;
                bonePos[BONE_PELVIS]   = safeReadBone(1, entityOrigin);
                bonePos[BONE_CHEST]    = safeReadBone(13, entityOrigin);
                bonePos[BONE_NECK]     = safeReadBone(22, entityOrigin);
                bonePos[BONE_HEAD]     = safeReadBone(24, entityOrigin);
                bonePos[BONE_L_HIP]    = safeReadBone(9,  bonePos[BONE_PELVIS]);
                bonePos[BONE_R_HIP]    = safeReadBone(10, bonePos[BONE_PELVIS]);
                bonePos[BONE_L_KNEE]   = safeReadBone(11, entityOrigin);
                bonePos[BONE_R_KNEE]   = safeReadBone(12, entityOrigin);
                bonePos[BONE_L_ANKLE]  = safeReadBone(15, entityOrigin);
                bonePos[BONE_R_ANKLE]  = safeReadBone(16, entityOrigin);
                bonePos[BONE_L_CLAV]   = safeReadBone(42, bonePos[BONE_NECK]);
                bonePos[BONE_R_CLAV]   = safeReadBone(43, bonePos[BONE_NECK]);
                bonePos[BONE_L_ELBOW]  = safeReadBone(66, bonePos[BONE_L_CLAV]);
                bonePos[BONE_R_ELBOW]  = safeReadBone(67, bonePos[BONE_R_CLAV]);
                bonePos[BONE_L_WRIST]  = safeReadBone(68, bonePos[BONE_L_ELBOW]);
                bonePos[BONE_R_WRIST]  = safeReadBone(69, bonePos[BONE_R_ELBOW]);

                // Minimal sanity: pelvis must have been read successfully
                if (bonePos[BONE_PELVIS].x == entityOrigin.x &&
                    bonePos[BONE_PELVIS].y == entityOrigin.y &&
                    bonePos[BONE_PELVIS].z == entityOrigin.z)
                    return false;

                return true;



            }
        }
        // Cached offset no longer valid for THIS entity — try slow path
        // but DON'T reset g_boneMatOffset, it may still work for other entities.
    }

    // Scan every 4-byte aligned offset in the DObj for a valid bone matrix ptr
    for (int off = 0; off < 256; off += 4) {
        uint32_t ptr = *(uint32_t*)(dobjRaw.data + off);
        if (ptr < 0x10000 || ptr > 0x7FFFFFFF) continue;

        // Read first two DObjAnimMat entries from the candidate pointer
        sDObjAnimMat mat0 = Memory::Read<sDObjAnimMat>(ptr);
        sDObjAnimMat mat1 = Memory::Read<sDObjAnimMat>(ptr + sizeof(sDObjAnimMat));

        // Validate quaternion: magnitude must be ≈ 1.0 (unit quaternion)
        float qMag0 = mat0.orientation[0]*mat0.orientation[0] +
                      mat0.orientation[1]*mat0.orientation[1] +
                      mat0.orientation[2]*mat0.orientation[2] +
                      mat0.orientation[3]*mat0.orientation[3];
        float qMag1 = mat1.orientation[0]*mat1.orientation[0] +
                      mat1.orientation[1]*mat1.orientation[1] +
                      mat1.orientation[2]*mat1.orientation[2] +
                      mat1.orientation[3]*mat1.orientation[3];
        if (qMag0 < 0.5f || qMag0 > 1.5f) continue;
        if (qMag1 < 0.5f || qMag1 > 1.5f) continue;

        // NaN check
        if (mat0.translation[0] != mat0.translation[0]) continue; // NaN
        if (mat0.translation[0] == 0.f && mat0.translation[1] == 0.f &&
            mat0.translation[2] == 0.f) continue;

        // Two valid quaternions is enough to confirm this is a bone matrix.
        // Position validation happens per-entity in the fast path.
        // Check if world-space (near ANY reasonable world coords) vs model-space
        bool worldSpace = (fabsf(mat0.translation[0]) > 10.f ||
                           fabsf(mat0.translation[1]) > 10.f ||
                           fabsf(mat0.translation[2]) > 10.f);

        // ── HIT: cache this offset ──
        g_boneMatOffset  = off;
        g_boneWorldSpace = worldSpace;

        // Only log the first discovery, not every frame
        static int foundCount = 0;
        if (foundCount < 5) {
            foundCount++;
            printf("[BONE] !! FOUND bone matrix at DObj+0x%02X => ptr=0x%08X (%s)\n",
                   off, ptr, worldSpace ? "WORLD-SPACE" : "MODEL-SPACE");
        }
        if (!g_boneEverFound) {
            printf("[BONE]    bone[0] quat=(%.3f,%.3f,%.3f,%.3f) |q|=%.3f\n",
                   mat0.orientation[0], mat0.orientation[1],
                   mat0.orientation[2], mat0.orientation[3], sqrtf(qMag0));
            printf("[BONE]    bone[0] trans=(%.1f, %.1f, %.1f)\n",
                   mat0.translation[0], mat0.translation[1], mat0.translation[2]);
            printf("[BONE]    bone[1] trans=(%.1f, %.1f, %.1f)\n",
                   mat1.translation[0], mat1.translation[1], mat1.translation[2]);
            if (!worldSpace)
                printf("[BONE]    (will add entity origin to get world pos)\n");
        }

        // *** BATCH READ: all bones in ONE DMA read ***
        // Read the full pose so the diagnostic dump shows arms/wrists at higher indices.
        constexpr int MAX_BONES_TO_READ = 160;
        struct BoneBlock { sDObjAnimMat bones[MAX_BONES_TO_READ]; };
        BoneBlock block = Memory::Read<BoneBlock>(ptr);

        // Body bone limit: indices 0-69 are body, 70+ are weapon/attachments
        constexpr int BODY_BONE_LIMIT = 70;

        // Read all bones + diagnostic dump
        bool dumpBones = !g_boneDumpDone;
        g_boneDumpDone = true;
        g_boneEverFound = true;
        if (dumpBones) {
            printf("\n[BONE] === Full Bone Dump (all %d bones, BODY limit=%d) ===\n",
                   MAX_BONES_TO_READ, BODY_BONE_LIMIT);
            printf("[BONE] Entity origin=(%.1f, %.1f, %.1f)\n",
                   entityOrigin.x, entityOrigin.y, entityOrigin.z);
        }

        int validCount = 0;
        for (int b = 0; b < MAX_BONES_TO_READ; b++) {
            float bx, by, bz;
            if (worldSpace) {
                bx = block.bones[b].translation[0];
                by = block.bones[b].translation[1];
                bz = block.bones[b].translation[2];
            } else {
                bx = entityOrigin.x + block.bones[b].translation[0];
                by = entityOrigin.y + block.bones[b].translation[1];
                bz = entityOrigin.z + block.bones[b].translation[2];
            }

            // Validate distance from entity origin
            float dx = bx - entityOrigin.x;
            float dy = by - entityOrigin.y;
            float dz = bz - entityOrigin.z;
            float distSq = dx*dx + dy*dy + dz*dz;
            float* q = block.bones[b].orientation;
            float qMag = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
            bool valid = (bx != 0.0f && distSq < 200.f * 200.f && qMag > 0.5f && qMag < 1.5f);

            if (valid) {
                validCount++;
            }

            if (dumpBones) {
                const char* cat = (b < BODY_BONE_LIMIT) ? "BODY" : "WPN/ATT";
                printf("  bone[%3d]: pos=(%.1f, %.1f, %.1f)  dZ=%.1f  dist=%.0f  |q|=%.2f  %s  [%s]\n",
                       b, bx, by, bz, bz - entityOrigin.z,
                       sqrtf(distSq), sqrtf(qMag),
                       valid ? "OK" : "INVALID", cat);
            }
        }
        if (dumpBones)
            printf("[BONE] === End Bone Dump (%d/%d valid) ===\n\n",
                   validCount, MAX_BONES_TO_READ);

        // Use heuristic mapping (handled in fast path above)
        // Fill bonePos via the same logic - call ReadBonePositions recursively
        // For now, mark success and let fast path handle mapping
        return (validCount >= 3);  // Need at least 3 valid bones
    }

    } // end if (dobjPtr != 0)

    // ── Fallback: scan entity's ClientTagCache pointer (IDA: centity+0x300) ──
    // ClientTagCache stores pre-computed bone/tag world positions.
    // Read it directly from the entity struct instead of going through DObj.
    {
        uintptr_t cg_ent_base = (uintptr_t)Memory::Read<uint32_t>(0x1140878);
        if (!cg_ent_base) cg_ent_base = 0x1140878;
        uintptr_t entityBase = cg_ent_base + (entityIdx * 0x380);

        // Try ClientTagCache at centity + 0x300
        uint32_t tagCachePtr = Memory::Read<uint32_t>(entityBase + 0x300);
        // Also try XAnimTree at centity + 0x2EC (follow one level of indirection)
        uint32_t treePtr = Memory::Read<uint32_t>(entityBase + 0x2EC);

        // Scan ClientTagCache for DObjAnimMat-like data or plain vec3_t near origin
        uint32_t ptrsToTry[] = { tagCachePtr, treePtr };
        const char* ptrNames[] = { "ClientTagCache(+0x300)", "XAnimTree(+0x2EC)" };

        for (int pi = 0; pi < 2; pi++) {
            uint32_t basePtr = ptrsToTry[pi];
            if (basePtr < 0x10000 || basePtr > 0x7FFFFFFF) continue;

            // Read a large block and scan for bone-like data
            struct RawBlock512 { uint8_t data[512]; };
            RawBlock512 cacheRaw = Memory::Read<RawBlock512>(basePtr);

            // Scan for DObjAnimMat entries (quaternion + translation)
            for (int off = 0; off < 480; off += 4) {
                float* fdata = (float*)(cacheRaw.data + off);

                // Try as DObjAnimMat: quat[4] then trans[3]
                if (off + 32 <= 512) {
                    float qMag = fdata[0]*fdata[0] + fdata[1]*fdata[1] +
                                 fdata[2]*fdata[2] + fdata[3]*fdata[3];
                    if (qMag > 0.5f && qMag < 1.5f) {
                        float tx = fdata[4], ty = fdata[5], tz = fdata[6];
                        if (tx != tx) continue; // NaN
                        if (tx == 0.f && ty == 0.f && tz == 0.f) continue;

                        float dx = tx - entityOrigin.x;
                        float dy = ty - entityOrigin.y;
                        float dz = tz - entityOrigin.z;
                        float distSq = dx*dx + dy*dy + dz*dz;

                        bool ws = (distSq < 500.f * 500.f);
                        bool ms = (!ws && fabsf(tx) < 300.f && fabsf(ty) < 300.f && fabsf(tz) < 300.f);

                        if (ws || ms) {
                            // Verify next entry too
                            if (off + 64 <= 512) {
                                float* f2 = (float*)(cacheRaw.data + off + 32);
                                float q2 = f2[0]*f2[0]+f2[1]*f2[1]+f2[2]*f2[2]+f2[3]*f2[3];
                                if (q2 < 0.5f || q2 > 1.5f) continue;
                            }

                            g_boneMatOffset = -2; // flag: using entity cache, not DObj
                            g_boneWorldSpace = ws;
                            printf("[BONE] !! FOUND bones via %s+0x%X (ptr=0x%08X, %s)\n",
                                   ptrNames[pi], off, basePtr + off,
                                   ws ? "WORLD" : "MODEL");
                            printf("[BONE]    quat=(%.3f,%.3f,%.3f,%.3f) trans=(%.1f,%.1f,%.1f)\n",
                                   fdata[0], fdata[1], fdata[2], fdata[3],
                                   fdata[4], fdata[5], fdata[6]);

                            // Read bones from this location
                            for (int b = 0; b < BONE_COUNT; b++) {
                                sDObjAnimMat m = Memory::Read<sDObjAnimMat>(
                                    basePtr + off + b * sizeof(sDObjAnimMat));
                                if (ws) {
                                    bonePos[b] = Vector3(m.translation[0],
                                                         m.translation[1],
                                                         m.translation[2]);
                                } else {
                                    bonePos[b] = Vector3(
                                        entityOrigin.x + m.translation[0],
                                        entityOrigin.y + m.translation[1],
                                        entityOrigin.z + m.translation[2]);
                                }
                            }
                            return true;
                        }
                    }
                }

                // Also try as plain vec3_t (12 bytes)
                if (off + 12 <= 512) {
                    float vx = fdata[0], vy = fdata[1], vz = fdata[2];
                    if (vx != vx) continue;
                    if (vx == 0.f && vy == 0.f && vz == 0.f) continue;
                    float dx2 = vx - entityOrigin.x;
                    float dy2 = vy - entityOrigin.y;
                    float dz2 = vz - entityOrigin.z;
                    float d2 = dx2*dx2 + dy2*dy2 + dz2*dz2;
                    if (d2 < 200.f * 200.f && d2 > 1.f) {
                        // Found a vec3 near entity origin — check if next vec3 is too
                        if (off + 24 <= 512) {
                            float* v2 = (float*)(cacheRaw.data + off + 12);
                            float ddx = v2[0] - entityOrigin.x;
                            float ddy = v2[1] - entityOrigin.y;
                            float ddz = v2[2] - entityOrigin.z;
                            float d22 = ddx*ddx + ddy*ddy + ddz*ddz;
                            if (d22 < 200.f * 200.f && d22 > 1.f) {
                                static bool s_vec3Found = false;
                                if (!s_vec3Found) {
                                    s_vec3Found = true;
                                    printf("[BONE] !! FOUND vec3 cluster via %s+0x%X\n",
                                           ptrNames[pi], off);
                                    printf("[BONE]    v0=(%.1f,%.1f,%.1f) v1=(%.1f,%.1f,%.1f)\n",
                                           vx, vy, vz, v2[0], v2[1], v2[2]);
                                }

                                // Read as vec3_t array (stride 12 bytes).
                                // Values are LOCAL bone offsets from entityOrigin — add origin to get world pos.
                                for (int b = 0; b < BONE_COUNT; b++) {
                                    float bv[3];
                                    if (off + b*12 + 12 > 512) {
                                        struct V3 { float v[3]; };
                                        V3 rv = Memory::Read<V3>(basePtr + off + b * 12);
                                        bonePos[b] = Vector3(
                                            entityOrigin.x + rv.v[0],
                                            entityOrigin.y + rv.v[1],
                                            entityOrigin.z + rv.v[2]);
                                    } else {
                                        bv[0] = *(float*)(cacheRaw.data + off + b*12 + 0);
                                        bv[1] = *(float*)(cacheRaw.data + off + b*12 + 4);
                                        bv[2] = *(float*)(cacheRaw.data + off + b*12 + 8);
                                        bonePos[b] = Vector3(
                                            entityOrigin.x + bv[0],
                                            entityOrigin.y + bv[1],
                                            entityOrigin.z + bv[2]);
                                    }
                                }
                                g_boneMatOffset = -3; // flag: vec3 cluster mode
                                return true;
                            }
                        }
                    }
                }
            }

            // Follow one level of pointer indirection from XAnimTree
            if (pi == 1 && treePtr > 0x10000) {
                // Read first few pointers from XAnimTree struct
                for (int poff = 0; poff < 32; poff += 4) {
                    uint32_t subPtr = Memory::Read<uint32_t>(treePtr + poff);
                    if (subPtr < 0x10000 || subPtr > 0x7FFFFFFF) continue;
                    if (subPtr == treePtr) continue; // self-reference

                    // Quick check: read first DObjAnimMat from subPtr
                    sDObjAnimMat testMat = Memory::Read<sDObjAnimMat>(subPtr);
                    float qTest = testMat.orientation[0]*testMat.orientation[0] +
                                  testMat.orientation[1]*testMat.orientation[1] +
                                  testMat.orientation[2]*testMat.orientation[2] +
                                  testMat.orientation[3]*testMat.orientation[3];
                    if (qTest < 0.5f || qTest > 1.5f) continue;
                    if (testMat.translation[0] == 0.f) continue;

                    float tdx = testMat.translation[0] - entityOrigin.x;
                    float tdy = testMat.translation[1] - entityOrigin.y;
                    float tdz = testMat.translation[2] - entityOrigin.z;
                    if (tdx*tdx + tdy*tdy + tdz*tdz < 500.f*500.f) {
                        printf("[BONE] !! FOUND via XAnimTree->+0x%X->0x%08X\n",
                               poff, subPtr);
                        for (int b = 0; b < BONE_COUNT; b++) {
                            sDObjAnimMat m = Memory::Read<sDObjAnimMat>(
                                subPtr + b * sizeof(sDObjAnimMat));
                            bonePos[b] = Vector3(m.translation[0],
                                                 m.translation[1],
                                                 m.translation[2]);
                        }
                        g_boneMatOffset = -4; // flag: XAnimTree indirect
                        return true;
                    }
                }
            }
        }
    }

    // ── Try swapped handleTable/dobjBase ONLY if bones were NEVER found ──
    // Don't corrupt working addresses just because one entity is dead/invalid.
    if (!g_boneEverFound) {
        static bool triedSwap = false;
        if (!triedSwap && g_boneMatOffset < 0) {
            triedSwap = true;
            printf("[BONE] No bones found, trying swapped handleTable/dobjBase...\n");
            std::swap(g_handleTable, g_dobjBase);
        }

        static int sizeAttempt = 0;
        static const size_t altSizes[] = { 0x7C, 0x80, 0x68, 0x70, 0x84, 0x88, 0x60 };
        static const int numAltSizes = sizeof(altSizes) / sizeof(altSizes[0]);
        if (g_boneMatOffset < 0 && sizeAttempt < numAltSizes) {
            g_dobjSize = altSizes[sizeAttempt++];
            if (sizeAttempt <= 3)
                printf("[BONE] Trying DObj size=0x%X\n", (unsigned)g_dobjSize);
        }
    }

    return false;
}

struct sLerpEntityState {
    int eFlags1;
    int eFlags2;
    sTrajectory PositionTrajectory;
    sTrajectory AngleTrajectory;
    char _0x50[0xC];
    int iWeaponID1;
    int iWeaponID2;
    char _0x64[0x18];
};

struct sEntityState {
    int iEntityNum;
    sLerpEntityState LerpEntityState;
    char _0x80[0x58];
    short wEntityType;
    short wGroundEntityNum;
    char _0xDA[0x2];
    short wOtherEntityNum;
    short wAttackerEntityNum;
    char _0xE2[0x2];
    int iWeaponID;
    char _0xE8[0x10];
};

struct sCEntity {
    char _0x0[0x2];
    short wValid;
    short wUsedForPlayerMesh;
    char _0x6[0x26];
    Vector3 vOrigin;
    Vector3 vViewAngles;
    char _0x44[0x11C];
    sLerpEntityState CurrentEntityState;
    sEntityState NextEntityState;
    char _0x2D4[0x18];         // padding to +0x2EC
    uint32_t pXAnimTree;       // +0x2EC  XAnimTree_s* (IDA: centity_t.tree)
    char _0x2F0[0x10];         // padding to +0x300
    uint32_t pClientTagCache;  // +0x300  ClientTagCache* (IDA: cached bone positions)
    uint32_t pAimTargetInfo;   // +0x304  AimTargetCache*
    char _0x308[0x70];         // padding to +0x378
    int iAlive;
    char _0x37C[0x4];
};

struct sRefDef {
    int iX;
    int iY;
    int iWidth;
    int iHeight;
    char _0x10[0x10];
    float flFovX;
    float flFovY;
    char _0x28[0x8];
    float flFov;
    Vector3 vViewOrigin;
    char _0x40[0x4];
    Vector3 vViewAxis[3];
};

struct sClientInfo {
    int iInfoValid;
    int iNextValid;
    int iClientNum;
    char szName[32];
    int iTeam1;
    int iTeam2;
    int iFFATeam;
    char _0x38[0x28];
    int iRank;
    char _0x64[0x14];
    unsigned __int64 qwXuid;
    char szClan[8];
    char _0x88[0x7];
    bool bDead;
    char _0x90[0x4];
    int iScore;
    char _0x98[0x58];
    char szPlayerModel[32];
};

inline float dot_product(const Vector3& a, const Vector3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline bool WorldToScreenRefDef(const Vector3& worldPos, Vector2& screenPos, const sRefDef& refdef) {
    Vector3 local(
        worldPos.x - refdef.vViewOrigin.x,
        worldPos.y - refdef.vViewOrigin.y,
        worldPos.z - refdef.vViewOrigin.z
    );

    Vector3 transform;
    transform.x = dot_product(local, refdef.vViewAxis[1]); // Right
    transform.y = dot_product(local, refdef.vViewAxis[2]); // Up
    transform.z = dot_product(local, refdef.vViewAxis[0]); // Forward

    if (transform.z < 0.1f) return false;

    float x = (refdef.iWidth / 2.0f) * (1.0f - (transform.x / refdef.flFovX / transform.z));
    float y = (refdef.iHeight / 2.0f) * (1.0f - (transform.y / refdef.flFovY / transform.z));

    screenPos.x = refdef.iX + x;
    screenPos.y = refdef.iY + y;
    return true;
}

void DrawESP() {
    if (!Memory::IsValid()) return;

    // Read cg_t structure base address as a 32-bit pointer (since BO2 is 32-bit)
    uint32_t cg_t_ptr_val = Memory::Read<uint32_t>(Memory::ModuleBase + 0x113F18C);
    uint32_t cg_t_ptr_absolute_val = Memory::Read<uint32_t>(0x113F18C);
    
    // Fallback: Check Zombies cg_t offset if Multiplayer offset reads 0
    if (!cg_t_ptr_val && !cg_t_ptr_absolute_val) {
        cg_t_ptr_val = Memory::Read<uint32_t>(Memory::ModuleBase + 0x1113F9C);
        cg_t_ptr_absolute_val = Memory::Read<uint32_t>(0x1113F9C);
    }

    uintptr_t cg_t_ptr = cg_t_ptr_val;
    uintptr_t cg_t_ptr_absolute = cg_t_ptr_absolute_val;

    static DWORD last_print = 0;
    DWORD now = GetTickCount();
    if (now - last_print > 3000) {
        last_print = now;
        printf("[*] cg_t_ptr (Relative: Base + Offset): 0x%I64X\n", cg_t_ptr);
        printf("[*] cg_t_ptr (Absolute: Offset): 0x%I64X\n", cg_t_ptr_absolute);
        
        uintptr_t active_ptr = cg_t_ptr ? cg_t_ptr : cg_t_ptr_absolute;
        if (active_ptr) {
            sRefDef refdef_debug = Memory::Read<sRefDef>(active_ptr + 0x4D890);
            printf("[*] RefDef Viewport: %dx%d | Camera Pos: (%.1f, %.1f, %.1f) | FOV: %.1f\n", 
                refdef_debug.iWidth, refdef_debug.iHeight, 
                refdef_debug.vViewOrigin.x, refdef_debug.vViewOrigin.y, refdef_debug.vViewOrigin.z,
                refdef_debug.flFov);
        }
    }

    // Determine the working cg_t pointer and module base offset configuration
    uintptr_t working_cg_base = 0;
    if (cg_t_ptr) {
        working_cg_base = cg_t_ptr;
    } else if (cg_t_ptr_absolute) {
        working_cg_base = cg_t_ptr_absolute;
    }

    if (!working_cg_base) return;

    // ── Initialize clipMap for BSP raycasting (once per map, retries on failure) ──
    {
        static uintptr_t lastCgBase       = 0;   // detect map changes
        static int       clipMapAttempts  = 0;   // retry counter
        static DWORD     lastAttemptTime  = 0;

        // Detect map reload: cg_t base changed → new map loaded
        if (lastCgBase != working_cg_base && working_cg_base != 0) {
            lastCgBase      = working_cg_base;
            clipMapAttempts = 0;
            if (g_clipMap.initialized) {
                printf("[TRACE] Map changed (new cg_base=0x%I64X), resetting clipMap cache.\n",
                       working_cg_base);
                g_clipMap.Reset();
            }
        }

        // Detect in-session map change: node[0]'s plane pointer changes when map reloads.
        // Check every 3 seconds to avoid DMA overhead.
        {
            static DWORD  s_lastMapCheck  = 0;
            static uint32_t s_lastNodePlane = 0;
            DWORD nowMs = GetTickCount();
            if (nowMs - s_lastMapCheck > 3000) {
                s_lastMapCheck = nowMs;
                // Read G_NODES ptr and dereference node[0].planePtr
                uint32_t pNodes = Memory::Read<uint32_t>(0x02531CEC);
                uint32_t curPlane = (pNodes > 0x10000) ? Memory::Read<uint32_t>(pNodes) : 0;
                if (curPlane != 0 && curPlane != s_lastNodePlane) {
                    if (s_lastNodePlane != 0 && g_clipMap.initialized) {
                        printf("[TRACE] Map geometry changed (node[0].plane 0x%X→0x%X), reloading clipMap.\n",
                               s_lastNodePlane, curPlane);
                        g_clipMap.Reset();
                        clipMapAttempts = 0;
                        lastAttemptTime = 0;
                    }
                    s_lastNodePlane = curPlane;
                }
            }
        }

        // Attempt the scan if not yet initialized and haven't exhausted retries
        if (!g_clipMap.initialized && clipMapAttempts < 4) {
            DWORD now = GetTickCount();
            // Wait 5 seconds between attempts (give the map time to fully load)
            if (clipMapAttempts == 0 || (now - lastAttemptTime) > 5000) {
                lastAttemptTime = now;
                clipMapAttempts++;
                printf("[TRACE] clipMap search attempt %d/4...\n", clipMapAttempts);
                g_clipMap.FindAndCache(0);
                if (!g_clipMap.initialized && clipMapAttempts >= 4)
                    printf("[TRACE] All attempts exhausted. Vis-check will default to visible.\n");
            }
        }
    }


    // ── Permanent VSAT: write to cg_t + 0x48554 (found via CE scan) ──
    if (s.vsat) {
        Memory::Write<int>(working_cg_base + 0x48554, 1);
    }

    // ── FOV Changer: write to cg_fov dvar current value at +0x18 ──
    // T6 dvar struct: +0x10=type, +0x14=modified, +0x18=current, +0x28=latched, +0x38=default
    if (s.w_fov_changer) {
        uintptr_t fov_dvar_ptr = (uintptr_t)Memory::Read<uint32_t>(0x0114227C);
        if (fov_dvar_ptr > 0x10000 && fov_dvar_ptr < 0x7FFFFFFF) {
            Memory::Write<float>(fov_dvar_ptr + 0x18, s.w_fov_value);  // current value only
        }
    }

    // ── Third Person: always write current toggle state ──
    {
        uintptr_t tp_dvar_ptr = (uintptr_t)Memory::Read<uint32_t>(0x010AA224);
        if (tp_dvar_ptr > 0x10000 && tp_dvar_ptr < 0x7FFFFFFF) {
            Memory::Write<int>(tp_dvar_ptr + 0x18, s.w_third_person ? 1 : 0);
        }
    }

    // ── Force Laser / Show FPS: runtime dvar scan ──
    // We need to find these dvar pointers at runtime since we don't have their
    // global storage addresses from Ghidra. Scan the dvar pool near known dvars.
    {
        static uintptr_t gun_dvar_ptr = 0;
        static uintptr_t fps_dvar_ptr = 0;
        static bool dvar_scan_done = false;

        if (!dvar_scan_done) {
            for (uintptr_t addr = 0x01080000; addr < 0x01160000; addr += 4) {
                uintptr_t maybe_dvar = (uintptr_t)Memory::Read<uint32_t>(addr);
                if (maybe_dvar < 0x10000 || maybe_dvar > 0x7FFFFFFF) continue;

                uintptr_t name_ptr = (uintptr_t)Memory::Read<uint32_t>(maybe_dvar);
                if (name_ptr < 0x00400000 || name_ptr > 0x01500000) continue;

                char name_buf[20] = {};
                for (int i = 0; i < 19; i++) {
                    name_buf[i] = Memory::Read<char>(name_ptr + i);
                    if (name_buf[i] == 0) break;
                }

                if (strcmp(name_buf, "cg_drawGun") == 0) {
                    gun_dvar_ptr = maybe_dvar;
                    printf("[DVAR-FIND] cg_drawGun dvar @ 0x%X (global @ 0x%X)\n", 
                           (uint32_t)maybe_dvar, (uint32_t)addr);
                }
                if (strcmp(name_buf, "cg_drawFPS") == 0) {
                    fps_dvar_ptr = maybe_dvar;
                    printf("[DVAR-FIND] cg_drawFPS dvar @ 0x%X (global @ 0x%X)\n",
                           (uint32_t)maybe_dvar, (uint32_t)addr);
                }

                if (gun_dvar_ptr && fps_dvar_ptr) break;
            }
            dvar_scan_done = true;
            if (!gun_dvar_ptr) printf("[DVAR-FIND] cg_drawGun NOT FOUND\n");
            if (!fps_dvar_ptr) printf("[DVAR-FIND] cg_drawFPS NOT FOUND\n");
        }

        // cg_drawGun: 1 = show (default), 0 = hidden
        if (gun_dvar_ptr) {
            Memory::Write<int>(gun_dvar_ptr + 0x18, s.w_hide_gun ? 0 : 1);
        }
        if (fps_dvar_ptr) {
            Memory::Write<int>(fps_dvar_ptr + 0x18, s.w_show_fps ? 1 : 0);
        }
    }

    // ── Start DMA cache worker once ──────────────────────────────────────────
    static bool g_cacheStarted = false;
    static uintptr_t g_lastCgBase = 0;
    {
        uintptr_t cgEntBase = (uintptr_t)Memory::Read<uint32_t>(0x1140878);
        if (!cgEntBase) cgEntBase = 0x1140878;
        if (!g_cacheStarted) {
            DmaCache::Start(working_cg_base, cgEntBase, 0x380, 0, 0);
            g_cacheStarted = true;
            g_lastCgBase   = working_cg_base;
        } else if (working_cg_base != g_lastCgBase) {
            DmaCache::SetCgBase(working_cg_base, cgEntBase);
            g_lastCgBase = working_cg_base;
        }
    }

    // ── All game state from cache — zero DMA on render thread ────────────────
    const PlayerCacheBuffer* cache = DmaCache::Get();
    if (!cache->dataValid || !cache->refdef.valid) return;

    const RefDefSnapshot& rds = cache->refdef;
    int localClientNum = cache->localClientNum;

    // Build a lightweight sRefDef-compatible view for WorldToScreenRefDef
    // (avoids changing the W2S function signature — just fill from cache)
    sRefDef refdef = {};
    refdef.iX       = rds.x;
    refdef.iY       = rds.y;
    refdef.iWidth   = rds.width;
    refdef.iHeight  = rds.height;
    refdef.flFovX   = rds.fovX;
    refdef.flFovY   = rds.fovY;
    refdef.flFov    = rds.fov;
    refdef.vViewOrigin = Vector3(rds.viewOrigin[0], rds.viewOrigin[1], rds.viewOrigin[2]);
    for (int ax = 0; ax < 3; ax++)
        refdef.vViewAxis[ax] = Vector3(rds.viewAxis[ax][0], rds.viewAxis[ax][1], rds.viewAxis[ax][2]);

    if (refdef.iWidth <= 0 || refdef.iHeight <= 0) return;

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    g_debugBonesValid = false;

    // ── Pre-compute W2S scalars once per frame (same for all entities/bones) ──
    // WorldToScreenRefDef uses: right=(vViewAxis[1] dot local), up=(vViewAxis[2] dot local)
    // The view axis vectors are constant for the whole frame — cache them.
    const Vector3& vFwd   = refdef.vViewAxis[0];
    const Vector3& vRight = refdef.vViewAxis[1];
    const Vector3& vUp    = refdef.vViewAxis[2];
    const float    halfW  = refdef.iWidth  / 2.0f;
    const float    halfH  = refdef.iHeight / 2.0f;

    float bestDistSq = 1e12f;           // squared distance — no sqrtf in hot loop
    float bestTargetX = 0.0f;
    float bestTargetY = 0.0f;
    int bestTargetIdx = -1;
    int bestTargetHealth = 99999;
    Vector3 bestTargetOrigin = {};
    Vector3 bestTargetVel    = {};
    float cx = halfW + refdef.iX;
    float cy = halfH + refdef.iY;
    float gameFov   = rds.fov > 0.0f ? rds.fov : 65.0f;
    float fovPixels = (s.fov / gameFov) * halfW;
    float fovPixelsSq = fovPixels * fovPixels;  // compare squared, skip sqrt
    bool triggerbot_found_target = false;


    // Iterate through clients — all data comes from local cache, zero DMA
    for (int i = 0; i < 18; ++i) {
        if (i == localClientNum) continue;

        const PlayerSnapshot& snap = cache->players[i];
        if (!snap.valid) continue;

        bool isTeammate   = snap.isTeammate;
        int  playerHealth    = snap.health;
        int  playerMaxHealth = snap.maxHealth;

        // Build a minimal sCEntity-like surface for legacy code below
        // (bone reading, w2s, etc. still need vOrigin and velocity)
        // We synthesize it from the cache — no DMA needed.
        struct { Vector3 vOrigin; uint16_t wValid; int iAlive; } ent;
        ent.vOrigin = snap.origin;
        ent.wValid  = snap.wValid;
        ent.iAlive  = snap.valid ? 0x2 : 0;

        bool isAlive = snap.valid; // cache already filtered dead players

        // Select ESP settings
        bool use_box      = isTeammate ? s.f_box      : s.box;
        bool use_skeleton  = isTeammate ? s.f_skeleton  : s.skeleton;
        bool use_name      = isTeammate ? s.f_name      : s.name;
        bool use_health    = isTeammate ? s.f_health    : s.health;
        bool use_weapon    = isTeammate ? s.f_weapon    : s.weapon;
        bool use_ammo      = isTeammate ? s.f_ammo      : s.ammo;
        ImVec4 use_box_color      = isTeammate ? s.f_box_color      : s.box_color;
        ImVec4 use_skeleton_color  = isTeammate ? s.f_skeleton_color  : s.skeleton_color;
        ImVec4 use_name_color      = isTeammate ? s.f_name_color      : s.name_color;
        ImVec4 use_health_color    = isTeammate ? s.f_health_color    : s.health_color;
        if (!use_box && !use_skeleton && !use_name && !use_health && !use_weapon && !use_ammo
            && !s.aimbot && !s.triggerbot) continue;

        // Name comes pre-built from the cache worker
        const char* displayNameBuf = snap.name;

        // Project foot position onto screen
        Vector2 screenFoot;
        if (!WorldToScreenRefDef(ent.vOrigin, screenFoot, refdef)) continue;

        // BO2 player capsule: feet are at vOrigin, head is ~54 units above (standing)
        Vector3 headPos = ent.vOrigin;
        headPos.z += 54.0f;
        Vector2 screenHead;
        if (!WorldToScreenRefDef(headPos, screenHead, refdef)) continue;

        float boxHeight = screenFoot.y - screenHead.y;
        if (boxHeight < 3.0f) continue; // Too small / behind camera artifact

        float boxWidth = boxHeight / 2.2f;
        float centerX  = screenHead.x;

        // Color: use team-appropriate menu colors
        ImU32 boxColor  = ImGui::ColorConvertFloat4ToU32(use_box_color);
        ImU32 nameColor = ImGui::ColorConvertFloat4ToU32(use_name_color);

        // Bones: only read when skeleton ESP is enabled (avoids N×DMA reads per frame).
        // For aimbot, we select the target cheaply first, then read its bones once after the loop.
        Vector3 bonePos[BONE_COUNT] = {};
        bool boneDataValid = false;
        if (use_skeleton) {
            boneDataValid = ReadBonePositions(localClientNum, i, ent.vOrigin, bonePos);
        }



        // Render bounding box
        if (use_box) {
            drawList->AddRect(
                ImVec2(centerX - boxWidth / 2.0f, screenHead.y),
                ImVec2(centerX + boxWidth / 2.0f, screenFoot.y),
                boxColor,
                0.0f,
                0,
                1.5f
            );
        }

        // Render health bar (left side of box)
        if (use_health && playerHealth > 0 && playerMaxHealth > 0) {
            float healthPct = (float)playerHealth / (float)playerMaxHealth;
            if (healthPct > 1.0f) healthPct = 1.0f;
            
            float barWidth = 3.0f;
            float barGap   = 3.0f;
            float barLeft  = centerX - boxWidth / 2.0f - barGap - barWidth;
            float barTop   = screenHead.y;
            float barBot   = screenFoot.y;
            float barFillTop = barBot - (barBot - barTop) * healthPct;
            
            // Background (dark outline)
            drawList->AddRectFilled(
                ImVec2(barLeft - 1.0f, barTop - 1.0f),
                ImVec2(barLeft + barWidth + 1.0f, barBot + 1.0f),
                IM_COL32(0, 0, 0, 180)
            );
            
            // Health fill: red→yellow→green gradient based on health %
            int r = (int)(255 * (1.0f - healthPct));
            int g = (int)(255 * healthPct);
            ImU32 fillColor = IM_COL32(r, g, 0, 255);
            
            drawList->AddRectFilled(
                ImVec2(barLeft, barFillTop),
                ImVec2(barLeft + barWidth, barBot),
                fillColor
            );
        }

        // Render skeleton
        if (use_skeleton) {
            ImU32 skelColor = ImGui::ColorConvertFloat4ToU32(use_skeleton_color);
            constexpr float skelThick = 1.5f;

            // Bones already read above

            // Debug: print bone status periodically for any visible entity
            {
                static DWORD last_bone_print = 0;
                DWORD tnow = GetTickCount();
                if (tnow - last_bone_print > 3000) {
                    last_bone_print = tnow;
                    printf("[BONE] entity=%d | matOffset=0x%02X | valid=%s",
                        i, g_boneMatOffset, boneDataValid ? "YES" : "NO");
                    if (boneDataValid) {
                        printf(" | HEAD=(%.1f,%.1f,%.1f) PELVIS=(%.1f,%.1f,%.1f)",
                            bonePos[BONE_HEAD].x, bonePos[BONE_HEAD].y, bonePos[BONE_HEAD].z,
                            bonePos[BONE_PELVIS].x, bonePos[BONE_PELVIS].y, bonePos[BONE_PELVIS].z);
                    }
                    printf("\n");
                }
            }

            // Helper: project a bone world pos → 2D screen point
            // Also validates the bone is within 100 units of entity origin
            // (rejects stale data from the shared bone buffer)
            auto ProjectBone = [&](int boneIdx) -> std::pair<ImVec2, bool> {
                if (!boneDataValid) return { ImVec2(0,0), false };
                Vector3& b = bonePos[boneIdx];
                if (b.x == 0.0f && b.y == 0.0f && b.z == 0.0f) return { ImVec2(0,0), false };

                // Distance check: reject bones from other entities' stale buffer data
                float dx = b.x - ent.vOrigin.x;
                float dy = b.y - ent.vOrigin.y;
                float dz = b.z - ent.vOrigin.z;
                float distSq = dx*dx + dy*dy + dz*dz;
                if (distSq > 100.f * 100.f) return { ImVec2(0,0), false };

                Vector2 sp;
                bool ok = WorldToScreenRefDef(b, sp, refdef);
                return { ImVec2(sp.x, sp.y), ok };
            };

            // Helper: draw line between two bones only if both project successfully
            auto DrawBoneLine = [&](int a, int b) {
                auto [pa, oka] = ProjectBone(a);
                auto [pb, okb] = ProjectBone(b);
                if (oka && okb)
                    drawList->AddLine(pa, pb, skelColor, skelThick);
            };

            if (boneDataValid) {
                // ── SKELETON: 100% real bones - tracks character animation ──────
                // Every line uses actual bone positions from the game.
                // Arms will track the character's real arm movements.

                // Spine: pelvis → chest → neck → head
                DrawBoneLine(BONE_PELVIS,   BONE_CHEST);
                DrawBoneLine(BONE_CHEST,    BONE_NECK);
                DrawBoneLine(BONE_NECK,     BONE_HEAD);

                // Left arm: neck → clavicle → elbow → wrist (real bones)
                DrawBoneLine(BONE_NECK,     BONE_L_CLAV);
                DrawBoneLine(BONE_L_CLAV,   BONE_L_ELBOW);
                DrawBoneLine(BONE_L_ELBOW,  BONE_L_WRIST);

                // Right arm: neck → clavicle → elbow → wrist (real bones)
                DrawBoneLine(BONE_NECK,     BONE_R_CLAV);
                DrawBoneLine(BONE_R_CLAV,   BONE_R_ELBOW);
                DrawBoneLine(BONE_R_ELBOW,  BONE_R_WRIST);

                // Left leg: pelvis → hip → knee → ankle (real bones)
                DrawBoneLine(BONE_PELVIS,   BONE_L_HIP);
                DrawBoneLine(BONE_L_HIP,    BONE_L_KNEE);
                DrawBoneLine(BONE_L_KNEE,   BONE_L_ANKLE);

                // Right leg: pelvis → hip → knee → ankle (real bones)
                DrawBoneLine(BONE_PELVIS,   BONE_R_HIP);
                DrawBoneLine(BONE_R_HIP,    BONE_R_KNEE);
                DrawBoneLine(BONE_R_KNEE,   BONE_R_ANKLE);
            } else {
                // ── Fallback: screen-space approximation (box-relative) ─────────
                ImVec2 hd(centerX, screenHead.y);
                ImVec2 nk(centerX, screenHead.y + boxHeight * 0.12f);
                ImVec2 sp(centerX, screenHead.y + boxHeight * 0.30f);
                ImVec2 pv(centerX, screenHead.y + boxHeight * 0.55f);
                ImVec2 lSh(centerX - boxWidth * 0.28f, screenHead.y + boxHeight * 0.20f);
                ImVec2 rSh(centerX + boxWidth * 0.28f, screenHead.y + boxHeight * 0.20f);
                ImVec2 lEl(centerX - boxWidth * 0.38f, screenHead.y + boxHeight * 0.37f);
                ImVec2 rEl(centerX + boxWidth * 0.38f, screenHead.y + boxHeight * 0.37f);
                ImVec2 lHd(centerX - boxWidth * 0.42f, screenHead.y + boxHeight * 0.52f);
                ImVec2 rHd(centerX + boxWidth * 0.42f, screenHead.y + boxHeight * 0.52f);
                ImVec2 lTh(centerX - boxWidth * 0.20f, screenHead.y + boxHeight * 0.60f);
                ImVec2 rTh(centerX + boxWidth * 0.20f, screenHead.y + boxHeight * 0.60f);
                ImVec2 lKn(centerX - boxWidth * 0.24f, screenHead.y + boxHeight * 0.80f);
                ImVec2 rKn(centerX + boxWidth * 0.24f, screenHead.y + boxHeight * 0.80f);
                ImVec2 lFt(centerX - boxWidth * 0.27f, screenFoot.y);
                ImVec2 rFt(centerX + boxWidth * 0.27f, screenFoot.y);

                // Spine
                drawList->AddLine(hd, nk, skelColor, skelThick);
                drawList->AddLine(nk, sp, skelColor, skelThick);
                drawList->AddLine(sp, pv, skelColor, skelThick);
                // Left arm
                drawList->AddLine(nk, lSh, skelColor, skelThick);
                drawList->AddLine(lSh, lEl, skelColor, skelThick);
                drawList->AddLine(lEl, lHd, skelColor, skelThick);
                // Right arm
                drawList->AddLine(nk, rSh, skelColor, skelThick);
                drawList->AddLine(rSh, rEl, skelColor, skelThick);
                drawList->AddLine(rEl, rHd, skelColor, skelThick);
                // Left leg
                drawList->AddLine(pv, lTh, skelColor, skelThick);
                drawList->AddLine(lTh, lKn, skelColor, skelThick);
                drawList->AddLine(lKn, lFt, skelColor, skelThick);
                // Right leg
                drawList->AddLine(pv, rTh, skelColor, skelThick);
                drawList->AddLine(rTh, rKn, skelColor, skelThick);
                drawList->AddLine(rKn, rFt, skelColor, skelThick);
            }
        }

        // Render name tag using real name from +0x7E8
        if (use_name) {
            ImVec2 textSize = ImGui::CalcTextSize(displayNameBuf);
            drawList->AddText(
                ImVec2(centerX - textSize.x / 2.0f, screenHead.y - textSize.y - 2.0f),
                nameColor,
                displayNameBuf
            );
        }

        // Aimbot target acquisition — cheap static-offset pass (no DMA bone read here).
        // We select the best target by FOV proximity. After the entity loop we do a single
        // bone read for just that target to get the exact hitbox world position.
        if (s.aimbot && makcu_wrapper::IsConnected()) {
            static const float kHitboxHeights[] = { 54.0f, 48.0f, 38.0f, 28.0f, 14.0f };
            for (int b = 0; b < 5; b++) {
                if (!s.hitboxes[b]) continue;

                Vector3 aimPos = ent.vOrigin;
                aimPos.z += kHitboxHeights[b];

                // Velocity prediction offset
                if (s.aim_pred) {
                    const Vector3& vel = snap.velocity;
                    float dtSec = s.aim_pred_ms * 0.001f;
                    aimPos.x += vel.x * dtSec;
                    aimPos.y += vel.y * dtSec;
                    aimPos.z += vel.z * dtSec;
                }

                Vector2 screenBone;
                if (WorldToScreenRefDef(aimPos, screenBone, refdef)) {
                    float dx = screenBone.x - cx;
                    float dy = screenBone.y - cy;
                    float distSq = dx * dx + dy * dy;

                    if (distSq < fovPixelsSq) {
                        bool isBetter = false;
                        if (s.aimbot_filter == 1) {
                            if (playerHealth < bestTargetHealth ||
                                (playerHealth == bestTargetHealth && distSq < bestDistSq)) {
                                isBetter = true;
                            }
                        } else {
                            if (distSq < bestDistSq) isBetter = true;
                        }

                        if (isBetter) {
                            bestDistSq          = distSq;
                            bestTargetX         = screenBone.x;
                            bestTargetY         = screenBone.y;
                            bestTargetIdx       = i;
                            bestTargetHealth    = playerHealth;
                            bestTargetOrigin    = ent.vOrigin;
                            bestTargetVel       = snap.velocity;
                        }
                    }
                }
            }
        }

        // Triggerbot target acquisition (basic check: target near crosshair center)
        if (s.triggerbot && makcu_wrapper::IsConnected()) {
            static const float kHitboxHeights[] = { 54.0f, 46.0f, 38.0f, 28.0f, 18.0f };
            for (int b = 0; b < 5; b++) {
                if (!s.hitboxes[b]) continue;

                Vector3 bonePos = ent.vOrigin;
                bonePos.z += kHitboxHeights[b];

                Vector2 screenBone;
                if (WorldToScreenRefDef(bonePos, screenBone, refdef)) {
                    float dx = screenBone.x - cx;
                    float dy = screenBone.y - cy;
                    float dist = sqrtf(dx * dx + dy * dy);

                    if (dist <= 6.0f) {
                        triggerbot_found_target = true;
                    }
                }
            }
        }
    }

    // ═══ DEBUG: Draw all raw bone dots with index numbers ═══
    if (g_debugBoneDots && g_debugBonesValid) {
        for (int b = 0; b < DEBUG_MAX_BONES; b++) {
            Vector3& bone = g_debugRawBones[b];
            if (bone.x == 0 && bone.y == 0 && bone.z == 0) continue;
            float dx = bone.x - g_debugEntityOrigin.x;
            float dy = bone.y - g_debugEntityOrigin.y;
            float dz = bone.z - g_debugEntityOrigin.z;
            float dist = sqrtf(dx*dx + dy*dy + dz*dz);
            if (dist > 120.f) continue;

            Vector2 sp;
            if (WorldToScreenRefDef(bone, sp, refdef)) {
                // Color by height: red=head, yellow=chest, green=pelvis, cyan=legs
                float heightFrac = (dz + 5.f) / 65.f; // 0=feet, 1=head
                if (heightFrac < 0) heightFrac = 0;
                if (heightFrac > 1) heightFrac = 1;
                ImU32 dotColor;
                if (heightFrac > 0.7f) dotColor = IM_COL32(255, 50, 50, 220);   // head area = RED
                else if (heightFrac > 0.45f) dotColor = IM_COL32(255, 200, 0, 220); // chest = YELLOW
                else if (heightFrac > 0.25f) dotColor = IM_COL32(50, 255, 50, 220); // pelvis = GREEN
                else dotColor = IM_COL32(50, 200, 255, 220); // legs = CYAN

                drawList->AddCircleFilled(ImVec2(sp.x, sp.y), 4.0f, dotColor);
                // Draw index number next to dot
                char label[8];
                snprintf(label, sizeof(label), "%d", b);
                drawList->AddText(ImVec2(sp.x + 6, sp.y - 4), IM_COL32(255, 255, 255, 200), label);
            }
        }
    }

    // ── Post-loop: single bone read for best aimbot target ────────────────────
    // Now that we know which entity to aim at, do ONE bone read for that target.
    // This replaces all 18 per-entity bone reads with a single targeted read.
    if (s.aimbot && makcu_wrapper::IsConnected() && bestTargetIdx >= 0) {
        static const int kHitboxToBone[] = {
            BONE_HEAD, BONE_NECK, BONE_CHEST, BONE_PELVIS, BONE_PELVIS
        };
        static const float kHitboxHeights[] = { 54.0f, 48.0f, 38.0f, 28.0f, 14.0f };

        Vector3 refinedBones[BONE_COUNT] = {};
        bool bonesOk = ReadBonePositions(localClientNum, bestTargetIdx, bestTargetOrigin, refinedBones);

        if (bonesOk) {
            // Find which hitbox slot is selected and project its bone
            for (int b = 0; b < 5; b++) {
                if (!s.hitboxes[b]) continue;
                Vector3 aimPos = refinedBones[kHitboxToBone[b]];
                // Zero-check: fall back to static if bone is invalid
                if (aimPos.x == 0.0f && aimPos.y == 0.0f && aimPos.z == 0.0f) {
                    aimPos = bestTargetOrigin;
                    aimPos.z += kHitboxHeights[b];
                }
                // Velocity prediction
                if (s.aim_pred) {
                    float dtSec = s.aim_pred_ms * 0.001f;
                    aimPos.x += bestTargetVel.x * dtSec;
                    aimPos.y += bestTargetVel.y * dtSec;
                    aimPos.z += bestTargetVel.z * dtSec;
                }
                Vector2 refined2D;
                if (WorldToScreenRefDef(aimPos, refined2D, refdef)) {
                    bestTargetX = refined2D.x;
                    bestTargetY = refined2D.y;
                }
                break; // only need the first enabled hitbox
            }
        }
    }

    // ─── Aimbot mouse dispatch ─────────────────────────────────────────────────
    // System: exponential ease-out + sub-pixel accumulation
    //
    //  • Ease-out: move (dx * aim_speed) each frame — fast when far, slow when
    //    close. Mimics natural muscle-memory: hard flick → smooth settle.
    //  • Sub-pixel accumulator: fractional pixel remainders carry forward every
    //    frame so zero precision is lost. Over 10 frames of aim_speed=0.35,
    //    all fractional pixels are delivered — no drift.
    //  • Deadzone: inside aim_deadzone radius, don't move at all. Prevents the
    //    constant micro-twitch that makes bots obvious on replays.
    //  • Target-switch: reset accumulator so stale carry-over doesn't snap aim.

    if (s.aimbot && makcu_wrapper::IsConnected() && bestTargetIdx >= 0) {
        bool is_aim_key_pressed = false;
        auto it = UI::g_binds.find("aimbot");
        if (it != UI::g_binds.end()) {
            if (it->second.mode == 3 || it->second.mode == 4) {
                is_aim_key_pressed = true;
            } else {
                is_aim_key_pressed = UI::IsKeyActive(it->second.key, 1);
            }
        } else {
            is_aim_key_pressed = makcu_wrapper::IsDown(VK_RBUTTON);
        }

        if (is_aim_key_pressed) {
            // Sub-pixel accumulator state
            static float  acc_x        = 0.0f;
            static float  acc_y        = 0.0f;
            static int    lastTarget   = -1;

            // Reset accumulator when we switch targets (no stale carry-over)
            if (bestTargetIdx != lastTarget) {
                acc_x = acc_y = 0.0f;
                lastTarget = bestTargetIdx;
            }

            float dx   = bestTargetX - cx;
            float dy   = bestTargetY - cy;
            float dist = sqrtf(dx * dx + dy * dy);

            // Deadzone — don't move inside this radius (stops micro-twitching)
            if (dist > s.aim_deadzone) {
                // Exponential ease-out: fraction of remaining distance per frame.
                // aim_speed=0.35 → ~35% of gap closed per frame.
                // When dist=200px: moves 70px. When dist=5px: moves 1.75px.
                // This is indistinguishable from a skilled player's tracking motion.
                float frac = s.aim_speed;

                // Taper fraction slightly when very close for extra smoothness
                // (avoids the small oscillation a pure exponential can produce)
                if (dist < 15.0f) frac *= (dist / 15.0f);

                acc_x += dx * frac;
                acc_y += dy * frac;

                // Extract integer pixels to send, keep remainder for next frame
                int mx = (int)acc_x;
                int my = (int)acc_y;
                acc_x -= (float)mx;
                acc_y -= (float)my;

                if (mx != 0 || my != 0) {
                    makcu_wrapper::move(mx, my);
                }
            } else {
                // On target — drain accumulator so we don't suddenly lurch
                acc_x = acc_y = 0.0f;
            }
        } else {
            // Key released — keep accumulator so next press resumes smoothly
        }
    }

    // Triggerbot: auto-fire when crosshair is on a target
    if (s.triggerbot && makcu_wrapper::IsConnected() && triggerbot_found_target) {
        bool is_trigger_key_pressed = false;
        auto it = UI::g_binds.find("triggerbot");
        if (it != UI::g_binds.end()) {
            if (it->second.mode == 3 || it->second.mode == 4) {
                is_trigger_key_pressed = true;
            } else {
                is_trigger_key_pressed = UI::IsKeyActive(it->second.key, 1);
            }
        } else {
            is_trigger_key_pressed = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        }

        if (is_trigger_key_pressed) {
            static auto lastShot = std::chrono::steady_clock::time_point{};
            auto now = std::chrono::steady_clock::now();
            int sinceLastShot = (int)std::chrono::duration_cast<std::chrono::milliseconds>(now - lastShot).count();
            int effDelay = (int)s.tb_delay;

            if (sinceLastShot >= effDelay) {
                makcu_wrapper::left_click();
                Sleep(10);
                makcu_wrapper::left_click_release();
                lastShot = now;
            }
        }
    }

    // Draw FOV circle
    if (s.draw_fov) {
        drawList->AddCircle(ImVec2(cx, cy), fovPixels, ImGui::ColorConvertFloat4ToU32(s.fov_color), 64, 1.0f);
    }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static const float MENU_W = 660.0f;
static const float MENU_H = 446.0f;

ImGuiContext* g_espContext = nullptr;
ImGuiContext* g_menuContext = nullptr;

HWND g_menuHwnd = nullptr;
IDXGISwapChain* g_menuSwapChain = nullptr;
ID3D11RenderTargetView* g_menuRTV = nullptr;

LRESULT CALLBACK MenuWndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, w, l))
        return TRUE;
    switch (msg) {
    case WM_NCHITTEST: {
        // Allow dragging the borderless window ONLY from the top-left logo area (JWare.cc logo zone)
        // This ensures the tabs, search bar, and settings button remain fully clickable.
        LRESULT hit = DefWindowProcW(hwnd, msg, w, l);
        if (hit == HTCLIENT) {
            POINT pt;
            pt.x = (short)LOWORD(l);
            pt.y = (short)HIWORD(l);
            ScreenToClient(hwnd, &pt);
            if (pt.x < 115 && pt.y < 45) { // Logo boundary box
                return HTCAPTION;
            }
        }
        return hit;
    }
    case WM_SYSCOMMAND:
        if ((w & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    UpdateMonitorsList();
    if (g_monitors.empty()) {
        MonitorInfo fallback;
        fallback.rect.left = 0;
        fallback.rect.top = 0;
        fallback.rect.right = GetSystemMetrics(SM_CXSCREEN);
        fallback.rect.bottom = GetSystemMetrics(SM_CYSCREEN);
        g_monitors.push_back(fallback);
    }

    int w = g_monitors[0].rect.right - g_monitors[0].rect.left;
    int h = g_monitors[0].rect.bottom - g_monitors[0].rect.top;
    if (!g_overlay.Init(w, h)) {
        return 1;
    }

    g_hwnd = g_overlay.GetHwnd();
    g_pd3dDevice = g_overlay.GetDevice();
    g_pd3dDeviceContext = g_overlay.GetContext();

    // Spawn a standard Win32 debug console window
    AllocConsole();
    FILE* fp = nullptr;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONIN$", "r", stdin);
    SetConsoleTitleW(L"Black Ops 2 DMA Debug Terminal");
    printf("[+] Debug Terminal Allocated successfully!\n");
    printf("[+] Searching for target game processes...\n");

    // Create detached menu window
    WNDCLASSEXW wcm = {};
    wcm.cbSize = sizeof(wcm);
    wcm.lpfnWndProc = MenuWndProc;
    wcm.hInstance = hInstance;
    wcm.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wcm.lpszClassName = L"BO2MenuWindowClass";
    RegisterClassExW(&wcm);

    int menuW = (int)(MENU_W); 
    int menuH = (int)(MENU_H);
    int menuX = g_monitors[0].rect.left + (w - menuW) / 2;
    int menuY = g_monitors[0].rect.top + (h - menuH) / 2;

    g_menuHwnd = CreateWindowExW(
        WS_EX_TOPMOST,
        L"BO2MenuWindowClass",
        L"Black Ops 2 DMA Trainer Menu",
        WS_POPUP,
        menuX, menuY, menuW, menuH,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!g_menuHwnd) return 1;

    // Create swap chain for the menu window using DXGI
    IDXGIDevice* dxgiDev = nullptr;
    IDXGIAdapter* dxgiAdpt = nullptr;
    IDXGIFactory2* dxgiFact = nullptr;
    if (SUCCEEDED(g_pd3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev))) {
        dxgiDev->GetAdapter(&dxgiAdpt);
        dxgiAdpt->GetParent(__uuidof(IDXGIFactory2), (void**)&dxgiFact);
        
        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferDesc.Width = menuW;
        sd.BufferDesc.Height = menuH;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.OutputWindow = g_menuHwnd;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

        dxgiFact->CreateSwapChain(g_pd3dDevice, &sd, &g_menuSwapChain);

        dxgiFact->Release();
        dxgiAdpt->Release();
        dxgiDev->Release();
    }

    if (!g_menuSwapChain) return 1;

    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED(g_menuSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) {
        g_pd3dDevice->CreateRenderTargetView(bb, nullptr, &g_menuRTV);
        bb->Release();
    }

    if (!g_menuRTV) return 1;

    // Initialize ImGui contexts
    IMGUI_CHECKVERSION();
    
    // 1. ESP context
    g_espContext = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_espContext);
    ImGuiIO& ioEsp = ImGui::GetIO();
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // 2. Menu context
    g_menuContext = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_menuContext);
    ImGuiIO& ioMenu = ImGui::GetIO();
    ioMenu.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding   = 4.0f;
    style.FrameRounding    = 2.0f;
    style.PopupRounding    = 3.0f;
    style.ScrollbarRounding= 3.0f;
    style.GrabRounding     = 2.0f;
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize  = 0.0f;

    ImGui_ImplWin32_Init(g_menuHwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // Load fonts into both contexts
    ImFontConfig font_config;
    font_config.OversampleH = 1;
    font_config.OversampleV = 1;
    font_config.PixelSnapH = true;
    font_config.FontDataOwnedByAtlas = false;
 
    // Load Exo2-Regular & solid FA icons into ESP context
    ImGui::SetCurrentContext(g_espContext);
    ioEsp.Fonts->AddFontFromMemoryTTF((void*)font_exo2_regular, font_exo2_regular_size, 14.0f, &font_config);
    static const ImWchar icons_ranges[] = { 0xE000, 0xF8FF, 0 };
    ImFontConfig icons_config;
    icons_config.MergeMode = true;
    icons_config.PixelSnapH = true;
    icons_config.OversampleH = 1;
    icons_config.OversampleV = 1;
    icons_config.FontDataOwnedByAtlas = false;
    ioEsp.Fonts->AddFontFromMemoryTTF((void*)font_fa_solid, font_fa_solid_size, 13.0f, &icons_config, icons_ranges);
 
    // Load Exo2-Regular, Exo2-Bold, & solid FA icons into Menu context
    ImGui::SetCurrentContext(g_menuContext);
    ioMenu.Fonts->AddFontFromMemoryTTF((void*)font_exo2_regular, font_exo2_regular_size, 14.0f, &font_config);
    ioMenu.Fonts->AddFontFromMemoryTTF((void*)font_fa_solid, font_fa_solid_size, 13.0f, &icons_config, icons_ranges);
    g_logo_font = ioMenu.Fonts->AddFontFromMemoryTTF((void*)font_exo2_bold, font_exo2_bold_size, 18.0f, &font_config);

    int bg_width = 0, bg_height = 0;
    if (!LoadTextureFromFile("background.png", &g_pBackgroundTexture, &bg_width, &bg_height)) {
        bg_width = 256;
        bg_height = 256;
        BYTE* pPixels = new BYTE[bg_width * bg_height * 4];
        for (int y = 0; y < bg_height; y++) {
            for (int x = 0; x < bg_width; x++) {
                int idx = (y * bg_width + x) * 4;
                BYTE r = (BYTE)(20 + (x * 10 / bg_width));
                BYTE g = (BYTE)(15 + (y * 15 / bg_height));
                BYTE b = (BYTE)(30 + ((x + y) * 10 / (bg_width + bg_height)));
                if (x % 32 == 0 || y % 32 == 0) {
                    r = (BYTE)(r * 1.5f);
                    g = (BYTE)(g * 1.5f);
                    b = (BYTE)(b * 1.5f);
                }
                pPixels[idx + 0] = r;
                pPixels[idx + 1] = g;
                pPixels[idx + 2] = b;
                pPixels[idx + 3] = 255;
            }
        }
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = bg_width;
        desc.Height = bg_height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA subResource;
        subResource.pSysMem = pPixels;
        subResource.SysMemPitch = bg_width * 4;
        subResource.SysMemSlicePitch = 0;

        ID3D11Texture2D* pTexture = nullptr;
        HRESULT hr = g_pd3dDevice->CreateTexture2D(&desc, &subResource, &pTexture);
        delete[] pPixels;

        if (SUCCEEDED(hr)) {
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;

            g_pd3dDevice->CreateShaderResourceView(pTexture, &srvDesc, &g_pBackgroundTexture);
            pTexture->Release();
        }
    }

    bool done = false;
    bool last_toggle_menu_state = false;

    // Overlay is always transparent and click-through
    LONG_PTR exStyle = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    exStyle |= (WS_EX_TRANSPARENT | WS_EX_LAYERED);
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, exStyle);
    SetWindowPos(g_hwnd, HWND_TOPMOST, g_monitors[0].rect.left, g_monitors[0].rect.top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);

    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        // Update key state history from the MAKCU serial interface
        if (makcu_wrapper::IsConnected()) {
            makcu_wrapper::UpdateKeyHistory();
        }

        // Try to attach to the game process using DMA
        static DWORD last_attach_tick = 0;
        DWORD current_tick = GetTickCount();
        if (current_tick - last_attach_tick > 2000) {
            last_attach_tick = current_tick;
            if (!Memory::IsValid()) {
                printf("[*] Attempting process attachment...\n");
                if (Memory::Attach("plutonium-bootstrapper-win32.exe")) {
                    // Under Plutonium, the game offsets are absolute addresses (no module base offset needs to be added).
                    // We set ModuleBase to 0 to prevent shifting of the offsets.
                    Memory::ModuleBase = 0;
                    printf("[+] Attached to plutonium-bootstrapper-win32.exe. PID: %u, Base: 0x%I64X\n", Memory::ProcessId, Memory::ModuleBase);
                    Memory::PrintModules();
                } else if (Memory::Attach("t6mp.exe")) {
                    printf("[+] Attached to t6mp.exe. PID: %u, Base: 0x%I64X\n", Memory::ProcessId, Memory::ModuleBase);
                    Memory::PrintModules();
                } else if (Memory::Attach("t6zm.exe")) {
                    printf("[+] Attached to t6zm.exe. PID: %u, Base: 0x%I64X\n", Memory::ProcessId, Memory::ModuleBase);
                    Memory::PrintModules();
                } else if (Memory::Attach("t6sp.exe")) {
                    printf("[+] Attached to t6sp.exe. PID: %u, Base: 0x%I64X\n", Memory::ProcessId, Memory::ModuleBase);
                    Memory::PrintModules();
                } else {
                    printf("[-] Failed to find target processes. Retrying...\n");
                }
            }
        }

        // Toggle menu visibility with INSERT key
        static bool insert_pressed = false;
        if (GetAsyncKeyState(VK_INSERT) & 0x8000) {
            if (!insert_pressed) {
                s.toggle_menu = !s.toggle_menu;
                insert_pressed = true;
            }
        } else {
            insert_pressed = false;
        }

        static auto last_time = std::chrono::high_resolution_clock::now();
        auto current_time = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(current_time - last_time).count();
        last_time = current_time;
        if (dt > 0.1f) dt = 0.1f;

        // Smoothly animate menu animation factor
        float target_anim = s.toggle_menu ? 1.0f : 0.0f;
        g_menu_anim += (target_anim - g_menu_anim) * dt * 12.0f;
        if (fabsf(g_menu_anim - target_anim) < 0.001f) g_menu_anim = target_anim;

        // Monitor Selection Tracking for overlay
        static int last_selected_monitor = -1;
        if (s.selected_monitor != last_selected_monitor) {
            last_selected_monitor = s.selected_monitor;
            UpdateMonitorsList();
            if (!g_monitors.empty()) {
                int idx = s.selected_monitor;
                if (idx < 0 || idx >= (int)g_monitors.size()) idx = 0;
                RECT m_rect = g_monitors[idx].rect;
                int mw = m_rect.right - m_rect.left;
                int mh = m_rect.bottom - m_rect.top;
                SetWindowPos(g_hwnd, HWND_TOPMOST, m_rect.left, m_rect.top, mw, mh, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
                g_overlay.Resize(mw, mh);
            }
        }
        
        bool is_menu_active = s.toggle_menu || (g_menu_anim > 0.005f);

        if (is_menu_active != last_toggle_menu_state) {
            last_toggle_menu_state = is_menu_active;
            if (is_menu_active) {
                ShowWindow(g_menuHwnd, SW_SHOW);
                SetWindowPos(g_menuHwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                SetForegroundWindow(g_menuHwnd);
            } else {
                ShowWindow(g_menuHwnd, SW_HIDE);
            }
        }

        // Keep the menu window above the overlay window (which is also TOPMOST)
        if (is_menu_active) {
            SetWindowPos(g_menuHwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }

        if (g_ResizeWidth && g_ResizeHeight) {
            g_overlay.Resize(g_ResizeWidth, g_ResizeHeight);
            g_ResizeWidth = g_ResizeHeight = 0;
        }

        // Render pass 1: transparent full-screen overlay for ESP
        // For HDMI Hardware Fusers, we clear the frame with solid black (RGB 0,0,0, Alpha 1.0) 
        // so the hardware fuser keys out the black background.
        g_overlay.BeginFrame(true);

        ImGui::SetCurrentContext(g_espContext);
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // ── Initialize keybinds globally (not just when tab is visible) ──
        static bool binds_initialized = false;
        if (!binds_initialized) {
            binds_initialized = true;
            UI::InitBind("aimbot", "M1");
            UI::InitBind("triggerbot", "ALT");
        }

        // ── Process aimbot/triggerbot keybinds every frame ──
        // This ensures they work even when the menu is closed or on a different tab
        UI::ProcessBindState("aimbot", &s.aimbot);
        UI::ProcessBindState("triggerbot", &s.triggerbot);

        // Render the bot ESP overlays using DMA card reads
        DrawESP();

        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_overlay.EndFrame();

        // Render pass 2: detached menu window
        if (is_menu_active) {
            ImGui::SetCurrentContext(g_menuContext);
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            Menu::Render();
            Menu::RenderWidgets();

            ImGui::Render();

            float clearColor[4] = { 0.11f, 0.10f, 0.13f, 1.0f };
            g_pd3dDeviceContext->OMSetRenderTargets(1, &g_menuRTV, nullptr);
            g_pd3dDeviceContext->ClearRenderTargetView(g_menuRTV, clearColor);

            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            g_menuSwapChain->Present(1, 0); // VSync enabled
        }
    }

    ImGui::SetCurrentContext(g_espContext);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();

    ImGui::SetCurrentContext(g_menuContext);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();

    ImGui::DestroyContext(g_espContext);
    ImGui::DestroyContext(g_menuContext);

    if (g_menuRTV) g_menuRTV->Release();
    if (g_menuSwapChain) g_menuSwapChain->Release();
    if (g_pBackgroundTexture) g_pBackgroundTexture->Release();
    return 0;
}
