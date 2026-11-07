#pragma once
#include <windows.h>
#include "../imgui/imgui.h"
#include "../makcu/makcu_wrapper.h"
#include <string>
#include <unordered_map>
#include <cctype>

namespace UI {

struct BindEntry {
    std::string key;
    bool        waiting     = false;
    int         start_frame = -1;
    int         mode        = 3; // 0: disabled, 1: hold, 2: hold off, 3: toggle, 4: always on
};

inline std::unordered_map<std::string, BindEntry> g_binds;

inline void InitBind(const char* id, const char* default_key) {
    if (g_binds.find(id) == g_binds.end()) {
        int default_mode = 3;
        if (default_key) {
            std::string dk = default_key;
            if (dk == "always on") default_mode = 4;
            else if (dk == "disabled") default_mode = 0;
            else if (dk == "hold") default_mode = 1;
            else if (dk == "hold off") default_mode = 2;
        }
        g_binds[id] = { default_key ? default_key : "", false, -1, default_mode };
    }
}

inline bool IsAnyBindWaiting() {
    for (auto& [id, e] : g_binds) if (e.waiting) return true;
    return false;
}

inline bool IsBindWaiting(const char* id) {
    auto it = g_binds.find(id);
    return it != g_binds.end() && it->second.waiting;
}

inline void StartBindWaiting(const char* id) {
    for (auto& [bid, e] : g_binds) e.waiting = false;
    auto it = g_binds.find(id);
    if (it != g_binds.end()) {
        it->second.waiting     = true;
        it->second.start_frame = ImGui::GetFrameCount();
    }
}

inline const char* GetBindDisplay(const char* id) {
    static char buf[48];
    auto it = g_binds.find(id);
    if (it == g_binds.end()) return "";
    
    if (it->second.mode == 0) return "[disabled]";
    if (it->second.mode == 4) return "[always on]";
    
    if (it->second.waiting)  return "press key";
    if (it->second.key.empty()) return "";
    snprintf(buf, sizeof(buf), "[%s]", it->second.key.c_str());
    return buf;
}

inline void UpdateBindCapture() {
    for (auto& [id, e] : g_binds) {
        if (!e.waiting) continue;
        if (ImGui::GetFrameCount() <= e.start_frame + 1) continue;

        ImGuiIO& io = ImGui::GetIO();

        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { e.waiting = false; return; }

        if (io.MouseClicked[0]) { e.key = "M1"; e.waiting = false; return; }
        if (io.MouseClicked[1]) { e.key = "M2"; e.waiting = false; return; }
        if (io.MouseClicked[2]) { e.key = "M3"; e.waiting = false; return; }
        if (io.MouseClicked[3]) { e.key = "M4"; e.waiting = false; return; }
        if (io.MouseClicked[4]) { e.key = "M5"; e.waiting = false; return; }

        for (int k = (int)ImGuiKey_NamedKey_BEGIN; k < (int)ImGuiKey_NamedKey_END; ++k) {
            ImGuiKey key = (ImGuiKey)k;
            if (key == ImGuiKey_Escape) continue;
            if (!ImGui::IsKeyPressed(key)) continue;

            const char* raw = ImGui::GetKeyName(key);
            std::string name = raw;

            if (name == "LeftAlt"   || name == "RightAlt")   name = "ALT";
            else if (name == "LeftCtrl"  || name == "RightCtrl")  name = "CTRL";
            else if (name == "LeftShift" || name == "RightShift") name = "SHIFT";
            else if (name == "LeftSuper" || name == "RightSuper") name = "WIN";
            else if (name == "MouseLeft")   name = "M1";
            else if (name == "MouseRight")  name = "M2";
            else if (name == "MouseMiddle") name = "M3";
            else {
                for (auto& c : name) c = (char)toupper((unsigned char)c);
            }

            e.key     = name;
            e.waiting = false;
            return;
        }
    }
}

inline bool IsKeyActive(const std::string& key, int mode) {
    if (mode == 0) return false;
    if (mode == 4) return true;
    if (key.empty()) return false;

    if (makcu_wrapper::IsConnected()) {
        if (key == "M1") return makcu_wrapper::IsDown(VK_LBUTTON);
        if (key == "M2") return makcu_wrapper::IsDown(VK_RBUTTON);
        if (key == "M3") return makcu_wrapper::IsDown(VK_MBUTTON);
        if (key == "M4") return makcu_wrapper::IsDown(VK_XBUTTON1);
        if (key == "M5") return makcu_wrapper::IsDown(VK_XBUTTON2);
    } else {
        if (key == "M1") return ImGui::IsMouseDown(ImGuiMouseButton_Left);
        if (key == "M2") return ImGui::IsMouseDown(ImGuiMouseButton_Right);
        if (key == "M3") return ImGui::IsMouseDown(ImGuiMouseButton_Middle);
        if (key == "M4") return ImGui::IsMouseDown(ImGuiMouseButton_Middle + 1);
        if (key == "M5") return ImGui::IsMouseDown(ImGuiMouseButton_Middle + 2);
    }

    if (key == "ALT")   return ImGui::IsKeyDown(ImGuiKey_LeftAlt)   || ImGui::IsKeyDown(ImGuiKey_RightAlt);
    if (key == "CTRL")  return ImGui::IsKeyDown(ImGuiKey_LeftCtrl)  || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
    if (key == "SHIFT") return ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);
    if (key == "WIN")   return ImGui::IsKeyDown(ImGuiKey_LeftSuper) || ImGui::IsKeyDown(ImGuiKey_RightSuper);

    for (int k = (int)ImGuiKey_NamedKey_BEGIN; k < (int)ImGuiKey_NamedKey_END; ++k) {
        ImGuiKey k_enum = (ImGuiKey)k;
        const char* name = ImGui::GetKeyName(k_enum);
        if (name) {
            std::string s_name = name;
            for (auto& c : s_name) c = (char)toupper((unsigned char)c);
            if (s_name == key) return ImGui::IsKeyDown(k_enum);
        }
    }
    return false;
}

inline bool IsKeyPressed(const std::string& key) {
    if (key.empty()) return false;

    if (makcu_wrapper::IsConnected()) {
        if (key == "M1") return makcu_wrapper::IsKeyJustPressed(VK_LBUTTON);
        if (key == "M2") return makcu_wrapper::IsKeyJustPressed(VK_RBUTTON);
        if (key == "M3") return makcu_wrapper::IsKeyJustPressed(VK_MBUTTON);
        if (key == "M4") return makcu_wrapper::IsKeyJustPressed(VK_XBUTTON1);
        if (key == "M5") return makcu_wrapper::IsKeyJustPressed(VK_XBUTTON2);
    } else {
        if (key == "M1") return ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        if (key == "M2") return ImGui::IsMouseClicked(ImGuiMouseButton_Right);
        if (key == "M3") return ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
        if (key == "M4") return ImGui::IsMouseClicked(ImGuiMouseButton_Middle + 1);
        if (key == "M5") return ImGui::IsMouseClicked(ImGuiMouseButton_Middle + 2);
    }

    if (key == "ALT")   return ImGui::IsKeyPressed(ImGuiKey_LeftAlt)   || ImGui::IsKeyPressed(ImGuiKey_RightAlt);
    if (key == "CTRL")  return ImGui::IsKeyPressed(ImGuiKey_LeftCtrl)  || ImGui::IsKeyPressed(ImGuiKey_RightCtrl);
    if (key == "SHIFT") return ImGui::IsKeyPressed(ImGuiKey_LeftShift) || ImGui::IsKeyPressed(ImGuiKey_RightShift);
    if (key == "WIN")   return ImGui::IsKeyPressed(ImGuiKey_LeftSuper) || ImGui::IsKeyPressed(ImGuiKey_RightSuper);

    for (int k = (int)ImGuiKey_NamedKey_BEGIN; k < (int)ImGuiKey_NamedKey_END; ++k) {
        ImGuiKey k_enum = (ImGuiKey)k;
        const char* name = ImGui::GetKeyName(k_enum);
        if (name) {
            std::string s_name = name;
            for (auto& c : s_name) c = (char)toupper((unsigned char)c);
            if (s_name == key) return ImGui::IsKeyPressed(k_enum);
        }
    }
    return false;
}

inline void ProcessBindState(const char* id, bool* v) {
    auto it = g_binds.find(id);
    if (it == g_binds.end() || it->second.waiting) return;

    int mode = it->second.mode;
    if (mode == 0) {
        return;
    }
    if (mode == 4) {
        *v = true;
        return;
    }

    if (mode == 1) {
        *v = IsKeyActive(it->second.key, mode);
    } else if (mode == 2) {
        *v = !IsKeyActive(it->second.key, mode);
    } else if (mode == 3) {
        if (IsKeyPressed(it->second.key)) {
            *v = !(*v);
        }
    }
}

}
