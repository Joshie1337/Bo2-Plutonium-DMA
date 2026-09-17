#pragma once
#include <windows.h>
#include <winternl.h>
#include <string>
#include <vector>
#include "vmmdll.h"

struct Vector3 {
    float x, y, z;
    Vector3() : x(0.0f), y(0.0f), z(0.0f) {}
    Vector3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
};

struct Vector2 {
    float x, y;
    Vector2() : x(0.0f), y(0.0f) {}
    Vector2(float _x, float _y) : x(_x), y(_y) {}
};

class Memory {
public:
    static VMM_HANDLE VmmHandle;
    static DWORD ProcessId;
    static uintptr_t ModuleBase;

    static bool Initialize();
    static bool Attach(const char* processName);
    static void Detach();
    static bool IsValid();
    static uintptr_t GetModuleBase(const char* moduleName);
    static void PrintModules();

    // World to Screen Helper
    static bool WorldToScreen(const Vector3& worldPos, Vector2& screenPos, float* matrix, int screenWidth, int screenHeight);

    template <typename T>
    static T Read(uintptr_t address) {
        T buffer{};
        if (VmmHandle && ProcessId && address) {
            DWORD cbRead = 0;
            VMMDLL_MemReadEx(VmmHandle, ProcessId, address, reinterpret_cast<PBYTE>(&buffer), sizeof(T), &cbRead, VMMDLL_FLAG_NOCACHE);
        }
        return buffer;
    }

    template <typename T>
    static bool Write(uintptr_t address, const T& value) {
        if (!VmmHandle || !ProcessId || !address) return false;
        return VMMDLL_MemWrite(VmmHandle, ProcessId, address, const_cast<PBYTE>(reinterpret_cast<const BYTE*>(&value)), sizeof(T)) != 0;
    }
};
