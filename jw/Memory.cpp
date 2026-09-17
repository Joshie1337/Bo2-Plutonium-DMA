#include "Memory.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <sys/stat.h>

VMM_HANDLE Memory::VmmHandle = nullptr;
DWORD Memory::ProcessId = 0;
uintptr_t Memory::ModuleBase = 0;

bool MmapExists(const std::string& name) {
    struct _stat buffer;
    return (_stat(name.c_str(), &buffer) == 0);
}

bool Memory::Initialize() {
    if (VmmHandle) return true;

    std::string mmapPath = "mmap.txt";

    if (MmapExists(mmapPath)) {
        char* args[] = { (char*)"", (char*)"-device", (char*)"fpga", (char*)"-memmap", (char*)mmapPath.c_str() };
        DWORD argc = 5;
        VmmHandle = VMMDLL_Initialize(argc, args);
        if (VmmHandle) {
            return true;
        }
    }

    // Fallback: generate mmap.txt
    char* args[] = { (char*)"", (char*)"-device", (char*)"fpga", (char*)"-v" };
    DWORD argc = 4;

    VmmHandle = VMMDLL_Initialize(argc, args);
    if (VmmHandle) {
        PVMMDLL_MAP_PHYSMEM pMPhys = NULL;
        if (VMMDLL_Map_GetPhysMem(VmmHandle, &pMPhys)) {
            if (pMPhys->dwVersion == VMMDLL_MAP_PHYSMEM_VERSION) {
                std::stringstream sb;
                for (DWORD i = 0; i < pMPhys->cMap; i++) {
                    sb << std::setfill('0') << std::setw(4) << i << "  " 
                       << std::hex << pMPhys->pMap[i].pa << "  -  " 
                       << (pMPhys->pMap[i].pa + pMPhys->pMap[i].cb - 1) << "  ->  " 
                       << pMPhys->pMap[i].pa << std::endl;
                }
                std::ofstream nFile("mmap.txt");
                nFile << sb.str();
                nFile.close();
                VMMDLL_MemFree(pMPhys);

                // Re-initialize with the newly generated mmap.txt
                VMMDLL_Close(VmmHandle);
                char* args_mmap[] = { (char*)"", (char*)"-device", (char*)"fpga", (char*)"-memmap", (char*)mmapPath.c_str() };
                VmmHandle = VMMDLL_Initialize(5, args_mmap);
                return VmmHandle != nullptr;
            }
            VMMDLL_MemFree(pMPhys);
        }
    }

    return false;
}

bool Memory::Attach(const char* processName) {
    if (!Initialize()) {
        return false;
    }

    if (IsValid()) {
        return true;
    }

    Detach();

    DWORD pid = 0;
    if (!VMMDLL_PidGetFromName(VmmHandle, const_cast<LPSTR>(processName), &pid)) {
        return false;
    }

    ProcessId = pid;
    ModuleBase = GetModuleBase(processName);

    if (!ModuleBase) {
        // Fallback: Query process base from PEB
        VMMDLL_PROCESS_INFORMATION procInfo;
        SIZE_T procInfoSz = sizeof(procInfo);
        procInfo.magic = VMMDLL_PROCESS_INFORMATION_MAGIC;
        procInfo.wVersion = VMMDLL_PROCESS_INFORMATION_VERSION;
        if (VMMDLL_ProcessGetInformation(VmmHandle, ProcessId, &procInfo, &procInfoSz)) {
            // Read base address from process PEB structure
            struct TempPEB {
                BYTE InheritedAddressSpace;
                BYTE ReadImageFileExecOptions;
                BYTE BeingDebugged;
                BYTE BitField;
                char padding[4];
                ULONG64 Mutant;
                ULONG64 ImageBaseAddress;
            };
            TempPEB peb = Read<TempPEB>(procInfo.win.vaPEB);
            if (peb.ImageBaseAddress) {
                ModuleBase = peb.ImageBaseAddress;
            }
        }
    }

    if (!ModuleBase) {
        Detach();
        return false;
    }

    return true;
}

void Memory::Detach() {
    ProcessId = 0;
    ModuleBase = 0;
}

bool Memory::IsValid() {
    if (!VmmHandle || !ProcessId) return false;

    VMMDLL_PROCESS_INFORMATION procInfo;
    SIZE_T procInfoSz = sizeof(procInfo);
    procInfo.magic = VMMDLL_PROCESS_INFORMATION_MAGIC;
    procInfo.wVersion = VMMDLL_PROCESS_INFORMATION_VERSION;

    if (VMMDLL_ProcessGetInformation(VmmHandle, ProcessId, &procInfo, &procInfoSz)) {
        if (procInfo.dwState == 0) {
            return true;
        }
    }

    Detach();
    return false;
}

uintptr_t Memory::GetModuleBase(const char* moduleName) {
    if (!VmmHandle || !ProcessId) return 0;

    PVMMDLL_MAP_MODULEENTRY pEntry = nullptr;
    if (!VMMDLL_Map_GetModuleFromNameU(VmmHandle, ProcessId, const_cast<LPSTR>(moduleName), &pEntry, NULL)) {
        return 0;
    }

    uintptr_t base = static_cast<uintptr_t>(pEntry->vaBase);
    VMMDLL_MemFree(pEntry);
    return base;
}

bool Memory::WorldToScreen(const Vector3& worldPos, Vector2& screenPos, float* matrix, int screenWidth, int screenHeight) {
    if (!matrix) return false;

    float clipX = worldPos.x * matrix[0] + worldPos.y * matrix[1] + worldPos.z * matrix[2] + matrix[3];
    float clipY = worldPos.x * matrix[4] + worldPos.y * matrix[5] + worldPos.z * matrix[6] + matrix[7];
    float clipZ = worldPos.x * matrix[8] + worldPos.y * matrix[9] + worldPos.z * matrix[10] + matrix[11];
    float clipW = worldPos.x * matrix[12] + worldPos.y * matrix[13] + worldPos.z * matrix[14] + matrix[15];

    if (clipW < 0.1f) return false;

    float ndcX = clipX / clipW;
    float ndcY = clipY / clipW;

    screenPos.x = (screenWidth / 2.0f) + (ndcX * (screenWidth / 2.0f));
    screenPos.y = (screenHeight / 2.0f) - (ndcY * (screenHeight / 2.0f));

    return true;
}

void Memory::PrintModules() {
    if (!VmmHandle || !ProcessId) return;

    PVMMDLL_MAP_MODULE pMMap = nullptr;
    if (VMMDLL_Map_GetModuleU(VmmHandle, ProcessId, &pMMap, 0)) {
        printf("[*] Enumerating Loaded Modules for PID %u:\n", ProcessId);
        for (DWORD i = 0; i < pMMap->cMap; i++) {
            printf("  - %s: 0x%I64X (Size: 0x%X)\n", pMMap->pMap[i].uszText, pMMap->pMap[i].vaBase, pMMap->pMap[i].cbImageSize);
        }
        VMMDLL_MemFree(pMMap);
    } else {
        printf("[-] Failed to retrieve modules map for PID %u.\n", ProcessId);
    }
}
