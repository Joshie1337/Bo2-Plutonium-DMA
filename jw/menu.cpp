#include "menu.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "elements/elements.h"
#include "Memory.h"
#include "makcu/makcu_wrapper.h"
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <vector>
#include <string>
#include <thread>
#include <cmath>
 
extern ImFont* g_logo_font;
extern HWND g_hwnd;
extern float g_menu_anim;

static const float MENU_W  = 660.0f;
static const float MENU_H  = 446.0f;
static const float HEADER_H = 44.0f;
static const float PADDING = 10.0f;
static const float L_W     = 300.0f;
static const float GAP     = 14.0f;
static const float R_X     = PADDING + L_W + GAP;
static const float R_W     = MENU_W - R_X - PADDING;
static const float BOX_PAD = 6.0f;

State s;

static const char* k_hitboxes[]  = { "head", "neck", "chest", "stomach", "pelvis" };
static const char* k_target_types[] = { "NPCs", "Players" };
static const char* k_hitgroups[] = { "head, chest...", "head only", "all", "chest, arms" };
static const char* k_weapons[]   = { "shared", "pistols", "rifles", "snipers", "smg" };
static const char* k_tabs[]      = { "aim", "visuals", "skins", "misc", "config", "lua" };
static const char* k_visuals_subtabs[] = { "enemy", "friendly", "world", "extra" };
static const char* k_chat_types[]      = { "edgebug", "killfeed", "both" };
static const char* k_sound_types[]     = { "hit", "kill", "hurt" };

static std::vector<std::string> s_cfg_list;
static int                      s_cfg_sel   = -1;
static char                     s_cfg_input[64] = {};
static bool                     s_cfg_dirty = true;

static const char* k_cfg_ext = ".JW";

static std::string JwareCfgDir() {
    char local[MAX_PATH] = {};
    std::string base = "C:\\jware.cc";
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local)))
        base = std::string(local) + "\\JWare";
    base += "\\Bo2-Plutonium-DMA";
    std::string dir = base + "\\config\\";
    CreateDirectoryA(base.c_str(), nullptr);
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

static const std::string k_cfg_dir = JwareCfgDir();

static void CfgEnsureDir() {
    CreateDirectoryA(k_cfg_dir.c_str(), nullptr);
}

static void CfgScan() {
    std::string prev;
    if (s_cfg_sel >= 0 && s_cfg_sel < (int)s_cfg_list.size())
        prev = s_cfg_list[s_cfg_sel];
    s_cfg_list.clear();
    WIN32_FIND_DATAA fd;
    std::string pattern = std::string(k_cfg_dir) + "*" + k_cfg_ext;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::string name = fd.cFileName;
            size_t ext_pos = name.rfind(k_cfg_ext);
            if (ext_pos != std::string::npos) name = name.substr(0, ext_pos);
            s_cfg_list.push_back(name);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    s_cfg_sel   = -1;
    s_cfg_dirty = false;
    if (!prev.empty()) {
        for (int i = 0; i < (int)s_cfg_list.size(); i++) {
            if (s_cfg_list[i] == prev) { s_cfg_sel = i; break; }
        }
    }
}

static void CfgCreate() {
    if (s_cfg_input[0] == '\0') return;
    CfgEnsureDir();
    std::string name = s_cfg_input;
    std::string path = std::string(k_cfg_dir) + name + k_cfg_ext;
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    CloseHandle(h);
    CfgScan();
    for (int i = 0; i < (int)s_cfg_list.size(); i++) {
        if (s_cfg_list[i] == name) { s_cfg_sel = i; break; }
    }
    s_cfg_input[0] = '\0';
}

static const uint32_t CFG_MAGIC = 0x3242574A; // 'JWB2'
static const uint32_t CFG_BIND_MAGIC = 0x42494E44; // 'BIND'

static void CfgSave() {
    if (s_cfg_sel < 0 || s_cfg_sel >= (int)s_cfg_list.size()) return;
    CfgEnsureDir();
    std::string path = std::string(k_cfg_dir) + s_cfg_list[s_cfg_sel] + k_cfg_ext;
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD bw;
        uint32_t magic = CFG_MAGIC;
        uint32_t sz = (uint32_t)sizeof(State);
        WriteFile(h, &magic, 4, &bw, nullptr);
        WriteFile(h, &sz, 4, &bw, nullptr);
        WriteFile(h, &s, sizeof(State), &bw, nullptr);

        // Keybind section: magic + count + (idLen,id,keyLen,key,mode) per bind
        uint32_t bmagic = CFG_BIND_MAGIC;
        uint32_t n = (uint32_t)UI::g_binds.size();
        WriteFile(h, &bmagic, 4, &bw, nullptr);
        WriteFile(h, &n, 4, &bw, nullptr);
        for (const auto& [id, e] : UI::g_binds) {
            uint32_t idLen  = (uint32_t)id.size();
            uint32_t keyLen = (uint32_t)e.key.size();
            uint8_t  mode   = (uint8_t)e.mode;
            WriteFile(h, &idLen, 4, &bw, nullptr);
            if (idLen) WriteFile(h, id.data(), idLen, &bw, nullptr);
            WriteFile(h, &keyLen, 4, &bw, nullptr);
            if (keyLen) WriteFile(h, e.key.data(), keyLen, &bw, nullptr);
            WriteFile(h, &mode, 1, &bw, nullptr);
        }
        CloseHandle(h);
    }
}

static void CfgLoad() {
    if (s_cfg_sel < 0 || s_cfg_sel >= (int)s_cfg_list.size()) return;
    std::string path = std::string(k_cfg_dir) + s_cfg_list[s_cfg_sel] + k_cfg_ext;
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD br;
        uint32_t magic = 0, sz = 0;
        ReadFile(h, &magic, 4, &br, nullptr);
        ReadFile(h, &sz, 4, &br, nullptr);
        if (magic == CFG_MAGIC && sz == sizeof(State)) {
            State temp;
            if (ReadFile(h, &temp, sizeof(State), &br, nullptr) && br == sizeof(State)) {
                s = temp;
                s.toggle_menu = true;
                s.search_active = false;
                s.search_query[0] = '\0';
                s.settings_open = false;
            }

            // Optional keybind section
            uint32_t bmagic = 0, bcount = 0;
            DWORD br2 = 0;
            if (ReadFile(h, &bmagic, 4, &br2, nullptr) && br2 == 4 && bmagic == CFG_BIND_MAGIC &&
                ReadFile(h, &bcount, 4, &br2, nullptr) && br2 == 4 && bcount < 512) {
                for (uint32_t i = 0; i < bcount; i++) {
                    uint32_t idLen = 0, keyLen = 0;
                    std::string id, key;
                    uint8_t mode = 0;
                    if (!ReadFile(h, &idLen, 4, &br2, nullptr) || br2 != 4 || idLen > 64) break;
                    id.resize(idLen);
                    if (idLen && (!ReadFile(h, &id[0], idLen, &br2, nullptr) || br2 != idLen)) break;
                    if (!ReadFile(h, &keyLen, 4, &br2, nullptr) || br2 != 4 || keyLen > 32) break;
                    key.resize(keyLen);
                    if (keyLen && (!ReadFile(h, &key[0], keyLen, &br2, nullptr) || br2 != keyLen)) break;
                    if (!ReadFile(h, &mode, 1, &br2, nullptr) || br2 != 1) break;
                    if (mode > 4) mode = 3;
                    UI::g_binds[id] = { key, false, -1, (int)mode };
                }
            }
        }
        CloseHandle(h);
    }
}

static void CfgOpenDir() {
    CfgEnsureDir();
    ShellExecuteA(nullptr, "open", k_cfg_dir.c_str(), nullptr, nullptr, SW_SHOW);
}

static void DrawBox(ImDrawList* dl, ImVec2 wpos,
                    float lx, float box_top, float box_bottom, float w,
                    const char* label, ImU32 label_col) {
    float scale = s.menu_scale;
    auto S = [&](float x, float y) { return ImVec2(wpos.x + x * scale, wpos.y + y * scale); };

    float lh   = ImGui::GetTextLineHeight();
    float gap  = 5.0f * scale;

    ImVec2 ts  = ImGui::CalcTextSize(label);
    float  tx  = lx * scale + (w * scale - ts.x) * 0.5f;
    float  ty  = box_top * scale - lh * 0.5f;

    dl->AddLine(S(lx, box_top), ImVec2(wpos.x + tx - gap, wpos.y + box_top * scale), Colors::SectionBorder, 1.0f);
    dl->AddLine(ImVec2(wpos.x + tx + ts.x + gap, wpos.y + box_top * scale), S(lx + w, box_top), Colors::SectionBorder, 1.0f);

    dl->AddLine(S(lx, box_top), S(lx, box_bottom), Colors::SectionBorder, 1.0f);
    dl->AddLine(S(lx + w, box_top), S(lx + w, box_bottom), Colors::SectionBorder, 1.0f);
    dl->AddLine(S(lx, box_bottom), S(lx + w, box_bottom), Colors::SectionBorder, 1.0f);

    dl->AddText(ImVec2(wpos.x + tx, wpos.y + ty), label_col, label);
}

static void DrawInnerSep(ImDrawList* dl, ImVec2 wpos,
                         float lx, float ly, float w, const char* label) {
    float scale = s.menu_scale;
    auto S = [&](float x, float y) { return ImVec2(wpos.x + x * scale, wpos.y + y * scale); };
    float  lh  = ImGui::GetTextLineHeight();
    float  gap = 5.0f * scale;
    ImVec2 ts  = ImGui::CalcTextSize(label);
    float  tx  = lx * scale + (w * scale - ts.x) * 0.5f;
    float  ty  = ly * scale - lh * 0.5f;

    dl->AddLine(S(lx, ly), ImVec2(wpos.x + tx - gap, wpos.y + ly * scale), Colors::SectionBorder, 1.0f);
    dl->AddLine(ImVec2(wpos.x + tx + ts.x + gap, wpos.y + ly * scale), S(lx + w, ly), Colors::SectionBorder, 1.0f);

    dl->AddText(ImVec2(wpos.x + tx, wpos.y + ty), Colors::Section, label);
}

static void RenderChildPanel(const std::string& name, float col_x, float col_y, float col_w, float col_h, ImDrawList* dl, ImVec2 wpos, ImGuiIO& io) {
    float scale = s.menu_scale;
    float lbox_top = col_y;
    float lbox_bottom = col_y + col_h;
    
    DrawBox(dl, wpos, col_x, lbox_top, lbox_bottom, col_w, name.c_str(), Colors::ColHdr);
    
    float ly = (lbox_top + 16.0f) * scale;
    
    if (name == "aimbot") {
        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        UI::Checkbox("aimbot", &s.aimbot, "aimbot", "M1", (col_w - BOX_PAD * 2) * scale);
        ly = ImGui::GetCursorPos().y + 1.0f * scale;

        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD + 18.0f) * scale, ly));
        ImGui::PushStyleColor(ImGuiCol_Text, Colors::TextDim);
        ImGui::Text("hitboxes");
        ImGui::PopStyleColor();
        ly = ImGui::GetCursorPos().y - 1.0f * scale;

        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD + 18.0f) * scale, ly));
        UI::MultiDropdown("##hitboxes_dd", s.hitboxes, k_hitboxes, 5, (col_w - BOX_PAD * 2 - 18.0f) * scale);
        ly = ImGui::GetCursorPos().y + 1.0f * scale;


        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        {
            float current_y = ImGui::GetCursorScreenPos().y;
            UI::Checkbox("draw fov circle", &s.draw_fov, nullptr, nullptr, (col_w - BOX_PAD * 2) * scale);
            float next_y = ImGui::GetCursorScreenPos().y;
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, current_y + (20.0f * scale - 12.0f * scale) * 0.5f));
            UI::ColorPicker("##fov_color_cp", &s.fov_color, (col_x + col_w - BOX_PAD) * scale);
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, next_y));
        }
        ly = ImGui::GetCursorPos().y;
    }
    else if (name == "settings") {
        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        UI::SliderFloat("field of view", &s.fov, 0.0f, 180.0f, "", "%.0f", (col_w - BOX_PAD * 2) * scale);
        ly = ImGui::GetCursorPos().y + 1.0f * scale;

        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        UI::SliderFloat("aim speed", &s.aim_speed, 0.01f, 1.0f, "", "%.2f", (col_w - BOX_PAD * 2) * scale);
        ly = ImGui::GetCursorPos().y + 1.0f * scale;

        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        UI::SliderFloat("deadzone", &s.aim_deadzone, 0.0f, 8.0f, "px", "%.1f", (col_w - BOX_PAD * 2) * scale);
        ly = ImGui::GetCursorPos().y + 1.0f * scale;

        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        UI::Checkbox("prediction", &s.aim_pred, nullptr, nullptr, (col_w - BOX_PAD * 2) * scale);
        ly = ImGui::GetCursorPos().y + 1.0f * scale;

        if (s.aim_pred) {
            ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD + 18.0f) * scale, ly));
            UI::SliderFloat("lookahead", &s.aim_pred_ms, 10.0f, 200.0f, "ms", "%.0f", (col_w - BOX_PAD * 2 - 18.0f) * scale);
            ly = ImGui::GetCursorPos().y + 1.0f * scale;
        }

        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        static const char* k_filters[] = { "Closest", "Lowest HP" };
        UI::Dropdown("target filter", &s.aimbot_filter, k_filters, 2, (col_w - BOX_PAD * 2) * scale);
        ly = ImGui::GetCursorPos().y + 1.0f * scale;

    }

    else if (name == "triggerbot") {
        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        UI::Checkbox("triggerbot", &s.triggerbot, "triggerbot", "ALT", (col_w - BOX_PAD * 2) * scale);
        ly = ImGui::GetCursorPos().y + 1.0f * scale;

        if (s.triggerbot) {
            ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD + 18.0f) * scale, ly));
            UI::SliderFloat("delay", &s.tb_delay, 0.0f, 500.0f, "ms", "%.0f", (col_w - BOX_PAD * 2 - 18.0f) * scale);
            ly = ImGui::GetCursorPos().y + 1.0f * scale;

            ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD + 18.0f) * scale, ly));
            UI::SliderFloat("burst", &s.tb_burst, 0.0f, 500.0f, "ms", "%.0f", (col_w - BOX_PAD * 2 - 18.0f) * scale);
            ly = ImGui::GetCursorPos().y + 1.0f * scale;

            ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD + 18.0f) * scale, ly));
            ImGui::PushStyleColor(ImGuiCol_Text, Colors::TextDim);
            ImGui::Text("target type");
            ImGui::PopStyleColor();
            ly = ImGui::GetCursorPos().y - 1.0f * scale;

            ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD + 18.0f) * scale, ly));
            UI::MultiDropdown("##tb_target_type_dd", s.target_types, k_target_types, 2, (col_w - BOX_PAD * 2 - 18.0f) * scale);
            ly = ImGui::GetCursorPos().y + 1.0f * scale;

            ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD + 18.0f) * scale, ly));
            UI::Checkbox("require aim key", &s.tb_require_aim, nullptr, nullptr, (col_w - BOX_PAD * 2 - 18.0f) * scale);
            ly = ImGui::GetCursorPos().y + 1.0f * scale;
        }
    }
    else if (name == "write: game") {
        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        UI::Checkbox("permanent vsat", &s.vsat, nullptr, nullptr, (col_w - BOX_PAD * 2) * scale);
        ly = ImGui::GetCursorPos().y + 1.0f * scale;

        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        UI::Checkbox("fov changer", &s.w_fov_changer, nullptr, nullptr, (col_w - BOX_PAD * 2) * scale);
        ly = ImGui::GetCursorPos().y + 1.0f * scale;

        if (s.w_fov_changer) {
            ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD + 18.0f) * scale, ly));
            UI::SliderFloat("fov value", &s.w_fov_value, 30.0f, 160.0f, "", "%.0f", (col_w - BOX_PAD * 2 - 18.0f) * scale);
            ly = ImGui::GetCursorPos().y + 1.0f * scale;
        }

        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        UI::Checkbox("third person", &s.w_third_person, nullptr, nullptr, (col_w - BOX_PAD * 2) * scale);
        ly = ImGui::GetCursorPos().y + 1.0f * scale;
    }
    else if (name == "write: player") {
        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        UI::Checkbox("hide gun model", &s.w_hide_gun, nullptr, nullptr, (col_w - BOX_PAD * 2) * scale);
        ly = ImGui::GetCursorPos().y + 1.0f * scale;

        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        UI::Checkbox("show fps", &s.w_show_fps, nullptr, nullptr, (col_w - BOX_PAD * 2) * scale);
        ly = ImGui::GetCursorPos().y + 1.0f * scale;
    }
    else if (name == "enemy esp") {
        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        UI::Checkbox("bounding box", &s.box, nullptr, nullptr, (col_w - BOX_PAD * 2) * scale);
        UI::ColorPicker("##box_cp_enemy", &s.box_color, (col_x + col_w - BOX_PAD) * scale);
        ly = ImGui::GetCursorPos().y + 1.0f * scale;

        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        UI::Checkbox("skeleton", &s.skeleton, nullptr, nullptr, (col_w - BOX_PAD * 2) * scale);
        UI::ColorPicker("##skeleton_cp_enemy", &s.skeleton_color, (col_x + col_w - BOX_PAD) * scale);
        ly = ImGui::GetCursorPos().y + 1.0f * scale;

        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        UI::Checkbox("name tags", &s.name, nullptr, nullptr, (col_w - BOX_PAD * 2) * scale);
        UI::ColorPicker("##name_cp_enemy", &s.name_color, (col_x + col_w - BOX_PAD) * scale);
        ly = ImGui::GetCursorPos().y + 1.0f * scale;

        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        UI::Checkbox("health bar", &s.health, nullptr, nullptr, (col_w - BOX_PAD * 2) * scale);
        UI::ColorPicker("##health_cp_enemy", &s.health_color, (col_x + col_w - BOX_PAD) * scale);
        ly = ImGui::GetCursorPos().y + 1.0f * scale;

        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        UI::Checkbox("weapon name", &s.weapon, nullptr, nullptr, (col_w - BOX_PAD * 2) * scale);
        UI::ColorPicker("##weapon_cp_enemy", &s.weapon_color, (col_x + col_w - BOX_PAD) * scale);
        ly = ImGui::GetCursorPos().y + 1.0f * scale;

        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        UI::Checkbox("ammo count", &s.ammo, nullptr, nullptr, (col_w - BOX_PAD * 2) * scale);
        UI::ColorPicker("##ammo_cp_enemy", &s.ammo_color, (col_x + col_w - BOX_PAD) * scale);
        ly = ImGui::GetCursorPos().y + 1.0f * scale;

    }
    else if (name == "friendly esp") {
        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        UI::Checkbox("bounding box", &s.f_box, nullptr, nullptr, (col_w - BOX_PAD * 2) * scale);
        UI::ColorPicker("##box_cp_friendly", &s.f_box_color, (col_x + col_w - BOX_PAD) * scale);
        ly = ImGui::GetCursorPos().y + 1.0f * scale;

        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        UI::Checkbox("skeleton", &s.f_skeleton, nullptr, nullptr, (col_w - BOX_PAD * 2) * scale);
        UI::ColorPicker("##skeleton_cp_friendly", &s.f_skeleton_color, (col_x + col_w - BOX_PAD) * scale);
        ly = ImGui::GetCursorPos().y + 1.0f * scale;

        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        UI::Checkbox("name tags", &s.f_name, nullptr, nullptr, (col_w - BOX_PAD * 2) * scale);
        UI::ColorPicker("##name_cp_friendly", &s.f_name_color, (col_x + col_w - BOX_PAD) * scale);
        ly = ImGui::GetCursorPos().y + 1.0f * scale;

        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        UI::Checkbox("health bar", &s.f_health, nullptr, nullptr, (col_w - BOX_PAD * 2) * scale);
        UI::ColorPicker("##health_cp_friendly", &s.f_health_color, (col_x + col_w - BOX_PAD) * scale);
        ly = ImGui::GetCursorPos().y + 1.0f * scale;

        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        UI::Checkbox("weapon name", &s.f_weapon, nullptr, nullptr, (col_w - BOX_PAD * 2) * scale);
        UI::ColorPicker("##weapon_cp_friendly", &s.f_weapon_color, (col_x + col_w - BOX_PAD) * scale);
        ly = ImGui::GetCursorPos().y + 1.0f * scale;

        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        UI::Checkbox("ammo count", &s.f_ammo, nullptr, nullptr, (col_w - BOX_PAD * 2) * scale);
        UI::ColorPicker("##ammo_cp_friendly", &s.f_ammo_color, (col_x + col_w - BOX_PAD) * scale);
    }
    else if (name == "indicators") {
        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        UI::Checkbox("keybind list", &s.ind_keybinds, nullptr, nullptr, (col_w - BOX_PAD * 2) * scale);
        ly = ImGui::GetCursorPos().y + 1.0f * scale;

        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        UI::Checkbox("spectator list", &s.ind_spectators, nullptr, nullptr, (col_w - BOX_PAD * 2) * scale);
    }
    else if (name == "configs") {
        if (s_cfg_dirty) CfgScan();

        const float item_h   = 17.0f * scale;
        const float btn_h    = 18.0f * scale;
        const float input_h  = 17.0f * scale;
        const float below    = (4.0f + 17.0f + 4.0f
                             + 18.0f + 3.0f + 18.0f + 3.0f
                             + 18.0f + 3.0f + 18.0f
                             + BOX_PAD + 14.0f) * scale;
        float list_h = col_h * scale - 16.0f * scale - below;

        float  list_w   = (col_w - BOX_PAD * 2) * scale;
        ImVec2 list_pos = ImVec2(wpos.x + (col_x + BOX_PAD) * scale, wpos.y + ly);
        ImVec2 list_end = ImVec2(list_pos.x + list_w, list_pos.y + list_h);

        dl->AddRectFilled(list_pos, list_end, IM_COL32(15, 15, 15, (int)(s.bg_opacity * 255.f)), 3.0f * scale);
        dl->AddRect(list_pos, list_end, Colors::SectionBorder, 3.0f * scale, 0, 1.0f);

        ImVec2 mouse   = ImGui::GetIO().MousePos;
        int    hov_idx = -1;
        int    max_vis = (int)(list_h / item_h);
        float  lh      = ImGui::GetTextLineHeight();

        for (int i = 0; i < (int)s_cfg_list.size() && i < max_vis; i++) {
            ImVec2 ip0 = ImVec2(list_pos.x + 1, list_pos.y + 1 + i * item_h);
            ImVec2 ip1 = ImVec2(list_end.x - 1, ip0.y + item_h);

            bool hov_i = (mouse.x >= ip0.x && mouse.x <= ip1.x &&
                          mouse.y >= ip0.y && mouse.y <= ip1.y);
            bool sel_i = (s_cfg_sel == i);

            if (hov_i) hov_idx = i;

            if (sel_i)      dl->AddRectFilled(ip0, ip1, IM_COL32(64, 10, 10, (int)(s.bg_opacity * 255.f)), 2.0f * scale);
            else if (hov_i) dl->AddRectFilled(ip0, ip1, IM_COL32(25, 25, 25, (int)(s.bg_opacity * 255.f)), 2.0f * scale);

            float ty2 = ip0.y + (item_h - lh) * 0.5f;
            ImU32 tc  = sel_i ? Colors::Accent : (hov_i ? Colors::TextBright : Colors::Text);
            dl->AddText(ImVec2(ip0.x + 6.0f * scale, ty2), tc, s_cfg_list[i].c_str());
        }

        ImGui::SetCursorScreenPos(list_pos);
        ImGui::InvisibleButton("##cfg_list", ImVec2(list_w, list_h));
        if (ImGui::IsItemClicked() && hov_idx >= 0)
            s_cfg_sel = hov_idx;

        ly += list_h + 4.0f * scale;

        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        UI::TextInput("##cfg_name", s_cfg_input, sizeof(s_cfg_input), list_w, 17.0f * scale);
        ly = ImGui::GetCursorPos().y + 4.0f * scale;

        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        if (UI::Button("create", list_w, 18.0f * scale)) CfgCreate();
        ly = ImGui::GetCursorPos().y + 3.0f * scale;

        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        if (UI::Button("save", list_w, 18.0f * scale)) CfgSave();
        ly = ImGui::GetCursorPos().y + 3.0f * scale;

        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        if (UI::Button("load", list_w, 18.0f * scale)) CfgLoad();
        ly = ImGui::GetCursorPos().y + 3.0f * scale;

        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        if (UI::Button("open directory", list_w, 18.0f * scale)) CfgOpenDir();
    }
    else if (name == "miscellaneous") {
        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Colors::TextBright), "Diagnostics Terminal");
        ly = ImGui::GetCursorPos().y + 4.0f * scale;

        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        ImGui::TextWrapped("Diagnostic logging details have been relocated to the dedicated debug terminal console window.");
        ly = ImGui::GetCursorPos().y + 4.0f * scale;

        // Draw Separator for MAKCU
        ly += 10.0f * scale;
        DrawInnerSep(dl, wpos, col_x, ly / scale, col_w, "makcu mouse");
        ly += 12.0f * scale;

        // Show status: MAKCU: Connected / Disconnected
        bool mc = makcu_wrapper::IsConnected();
        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        ImGui::TextColored(mc ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f) : ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
            mc ? "status: connected" : "status: disconnected");
        ly = ImGui::GetCursorPos().y + 4.0f * scale;

        // Static parameters for MAKCU
        static std::vector<std::string> availablePorts;
        static int selectedPort = -1;
        static std::string lastStatus;
        static bool g_makcuFirstRender = true;
        static std::string g_makcuPort;
        static bool g_makcuHighSpeed = false;

        if (g_makcuFirstRender) {
            g_makcuFirstRender = false;
            availablePorts = makcu_wrapper::GetAvailablePorts();
            selectedPort = availablePorts.empty() ? -1 : 0;
            lastStatus = makcu_wrapper::GetConnectionStatus();
        }

        // Port Selection Dropdown
        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        ImGui::PushStyleColor(ImGuiCol_Text, Colors::TextDim);
        ImGui::Text("port selection");
        ImGui::PopStyleColor();
        ly = ImGui::GetCursorPos().y;

        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        std::vector<const char*> portItems;
        for (auto& p : availablePorts) portItems.push_back(p.c_str());
        
        UI::Dropdown("##makcuPort", &selectedPort, portItems.data(), (int)portItems.size(), (col_w - BOX_PAD * 2) * scale);
        ly = ImGui::GetCursorPos().y + 4.0f * scale;

        // "4M Baud" Checkbox
        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        UI::Checkbox("4M Baud Rate", &g_makcuHighSpeed, nullptr, nullptr, (col_w - BOX_PAD * 2) * scale);
        ly = ImGui::GetCursorPos().y + 6.0f * scale;

        // Control Buttons: Connect & Scan Ports
        float btn_w = (col_w - BOX_PAD * 2 - 6.0f) * 0.5f;

        // Connect button
        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        if (UI::Button(mc ? "reconnect" : "connect", btn_w * scale, 18.0f * scale)) {
            if (selectedPort >= 0 && selectedPort < (int)availablePorts.size()) {
                g_makcuPort = availablePorts[selectedPort];
                makcu_wrapper::MakcuInitialize(g_makcuPort, g_makcuHighSpeed);
                lastStatus = makcu_wrapper::GetConnectionStatus();
            } else {
                lastStatus = "select a port";
            }
        }

        // Scan button
        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD + btn_w + 6.0f) * scale, ly));
        if (UI::Button("scan ports", btn_w * scale, 18.0f * scale)) {
            availablePorts = makcu_wrapper::GetAvailablePorts();
            selectedPort = availablePorts.empty() ? -1 : 0;
            lastStatus = availablePorts.empty() ? "no ports found"
                : "found " + std::to_string(availablePorts.size()) + " port(s)";
        }
        ly = ImGui::GetCursorPos().y + 4.0f * scale;

        // Auto-Detect & Test Device buttons
        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        if (UI::Button("auto-detect", btn_w * scale, 18.0f * scale)) {
            if (selectedPort >= 0 && selectedPort < (int)availablePorts.size()) {
                g_makcuPort = availablePorts[selectedPort];
                makcu_wrapper::MakcuAutoDetect(g_makcuPort);
                lastStatus = makcu_wrapper::GetConnectionStatus();
            } else {
                lastStatus = "select a port";
            }
        }

        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD + btn_w + 6.0f) * scale, ly));
        if (UI::Button("test device", btn_w * scale, 18.0f * scale)) {
            if (makcu_wrapper::IsConnected()) {
                std::thread([]() {
                    const int steps = 100;
                    const float radius = 120.f;
                    const float PI = 3.14159265f;
                    
                    float prev_x = radius;
                    float prev_y = 0.f;
                    
                    for (int i = 1; i <= steps; i++) {
                        float theta = (2.0f * PI * i) / steps;
                        float cur_x = radius * cosf(theta);
                        float cur_y = radius * sinf(theta);
                        
                        int dx = (int)roundf(cur_x - prev_x);
                        int dy = (int)roundf(cur_y - prev_y);
                        
                        makcu_wrapper::move(dx, dy);
                        
                        prev_x = cur_x;
                        prev_y = cur_y;
                        
                        std::this_thread::sleep_for(std::chrono::milliseconds(8));
                    }
                }).detach();
                lastStatus = "running circular test movement";
            } else {
                lastStatus = "device not connected";
            }
        }
        ly = ImGui::GetCursorPos().y + 4.0f * scale;

        // Status text
        ImGui::SetCursorPos(ImVec2((col_x + BOX_PAD) * scale, ly));
        ImGui::PushStyleColor(ImGuiCol_Text, Colors::TextDim);
        ImGui::TextWrapped("%s", lastStatus.c_str());
        ImGui::PopStyleColor();
    }
}

void Menu::Render() {
    if (g_menu_anim <= 0.005f) return;

    // Global click-away consumption
    static int last_frame_checked = -1;
    if (ImGui::GetFrameCount() != last_frame_checked) {
        last_frame_checked = ImGui::GetFrameCount();
        
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            ImGuiID dd_open = UI::_dd::open_id;
            ImGuiID mdd_open = UI::_mdd::open_id;
            ImGuiID cp_open = UI::_cp::open_id;
            
            if (dd_open != 0 || mdd_open != 0 || cp_open != 0) {
                ImVec2 mouse = ImGui::GetIO().MousePos;
                bool inside = false;
                
                if (dd_open != 0) {
                    ImVec2 p0 = UI::_dd::open_pos;
                    ImVec2 p1 = ImVec2(p0.x + UI::_dd::open_w, p0.y + (float)UI::_dd::count * 16.0f + 4.0f);
                    if (mouse.x >= p0.x && mouse.x <= p1.x && mouse.y >= p0.y && mouse.y <= p1.y) {
                        inside = true;
                    }
                }
                
                if (mdd_open != 0) {
                    ImVec2 p0 = UI::_mdd::open_pos;
                    ImVec2 p1 = ImVec2(p0.x + UI::_mdd::open_w, p0.y + (float)UI::_mdd::count * 16.0f + 4.0f);
                    if (mouse.x >= p0.x && mouse.x <= p1.x && mouse.y >= p0.y && mouse.y <= p1.y) {
                        inside = true;
                    }
                }
                
                if (cp_open != 0) {
                    ImVec2 p0 = UI::_cp::open_pos;
                    ImVec2 p1 = ImVec2(p0.x + 104.0f, p0.y + 116.0f);
                    if (mouse.x >= p0.x && mouse.x <= p1.x && mouse.y >= p0.y && mouse.y <= p1.y) {
                        inside = true;
                    }
                }
                
                if (!inside) {
                    UI::_dd::open_id = 0;
                    UI::_mdd::open_id = 0;
                    UI::_cp::open_id = 0;
                    UI::_blocked_click_frame = ImGui::GetFrameCount();
                }
            }
        }
    }

    UI::UpdateBindCapture();

    {
        ImVec4 mc = s.menu_color;
        Colors::Accent      = ImGui::ColorConvertFloat4ToU32(mc);
        ImVec4 hov(ImMin(mc.x * 1.09f, 1.0f), ImMin(mc.y * 1.09f, 1.0f),
                   ImMin(mc.z * 1.09f, 1.0f), mc.w);
        Colors::AccentHover  = ImGui::ColorConvertFloat4ToU32(hov);
        ImVec4 dark(mc.x * 0.32f, mc.y * 0.32f, mc.z * 0.32f, mc.w);
        Colors::AccentDark   = ImGui::ColorConvertFloat4ToU32(dark);
        ImVec4 bh(mc.x * 0.82f, mc.y * 0.82f, mc.z * 0.82f, mc.w);
        Colors::CbBorderHov  = ImGui::ColorConvertFloat4ToU32(bh);
    }

    ImGuiIO& io = ImGui::GetIO();
    float original_font_scale = io.FontGlobalScale;
    float scale = s.menu_scale * (0.95f + 0.05f * g_menu_anim);
    io.FontGlobalScale = scale;

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(MENU_W * scale, MENU_H * scale));

    ImGuiWindowFlags wf =
        ImGuiWindowFlags_NoTitleBar        |
        ImGuiWindowFlags_NoResize          |
        ImGuiWindowFlags_NoScrollbar       |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBackground      |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoMove;

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_menu_anim);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(0, 0));
    ImGui::Begin("##jware.cc", nullptr, wf);
    ImGui::PopStyleVar(2);
    ImDrawList* dl   = ImGui::GetWindowDrawList();
    ImVec2      wpos = ImGui::GetWindowPos();
    auto S = [&](float x, float y) { return ImVec2(wpos.x + x * scale, wpos.y + y * scale); };

    float total_h = MENU_H * scale;
 
    ImU32 dynamic_bg = IM_COL32(11, 10, 13, (int)(s.bg_opacity * 255.f));
    dl->AddRectFilled(wpos, ImVec2(wpos.x + MENU_W * scale, wpos.y + total_h), dynamic_bg, 4.0f * scale);
 
    static float sidebar_anim_t = s.navigation_style == 1 ? 1.0f : 0.0f;
    sidebar_anim_t += (((s.navigation_style == 1) ? 1.0f : 0.0f) - sidebar_anim_t) * io.DeltaTime * 8.0f;
    if (sidebar_anim_t < 0.0f) sidebar_anim_t = 0.0f;
    if (sidebar_anim_t > 1.0f) sidebar_anim_t = 1.0f;

    float current_sidebar_w = 120.0f * sidebar_anim_t;
    float current_header_h = HEADER_H * (1.0f - sidebar_anim_t);

    // Draw integrated header/sidebar background
    ImU32 dynamic_hdr = IM_COL32(18, 18, 18, (int)(s.bg_opacity * 255.f));
    if (sidebar_anim_t > 0.005f) {
        ImU32 side_col = IM_COL32(18, 18, 18, (int)(s.bg_opacity * 255.f * sidebar_anim_t));
        dl->AddRectFilled(wpos, ImVec2(wpos.x + current_sidebar_w * scale, wpos.y + total_h), side_col, 4.0f * scale, ImDrawFlags_RoundCornersLeft);
        dl->AddLine(ImVec2(wpos.x + current_sidebar_w * scale, wpos.y), ImVec2(wpos.x + current_sidebar_w * scale, wpos.y + total_h), Colors::SectionBorder, 1.0f);
    }
    if (sidebar_anim_t < 0.995f) {
        ImU32 top_col = IM_COL32(18, 18, 18, (int)(s.bg_opacity * 255.f * (1.0f - sidebar_anim_t)));
        dl->AddRectFilled(wpos, ImVec2(wpos.x + MENU_W * scale, wpos.y + current_header_h * scale), top_col, 4.0f * scale, ImDrawFlags_RoundCornersTop);
        dl->AddLine(ImVec2(wpos.x, wpos.y + current_header_h * scale), ImVec2(wpos.x + MENU_W * scale, wpos.y + current_header_h * scale), Colors::SectionBorder, 1.0f);
    }

    #define ICON_FA_CROSSHAIR "\xef\x81\x9b"
    #define ICON_FA_USERS     "\xef\x83\x80"
    #define ICON_FA_EYE       "\xef\x81\xae"
    #define ICON_FA_SLIDERS   "\xef\x87\x9e"
    #define ICON_FA_FOLDER    "\xef\x81\xbb"
    #define ICON_FA_COG       "\xef\x80\x93"
    #define ICON_FA_SEARCH    "\xef\x80\x82"
    #define ICON_FA_WARNING   "\xef\x81\xb1"

    const char* tab_names[] = { "Aimbot", "Visuals", "Misc", "Configs", "Write" };
    const char* tab_icons[] = { ICON_FA_CROSSHAIR, ICON_FA_EYE, ICON_FA_SLIDERS, ICON_FA_FOLDER, ICON_FA_WARNING };
    static std::unordered_map<std::string, float> hover_anims;
    ImVec2 mouse = io.MousePos;

    // --- SIDEBAR NAVIGATION LAYOUT ---
    if (sidebar_anim_t > 0.005f) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_menu_anim * sidebar_anim_t);
        ImGui::PushClipRect(wpos, ImVec2(wpos.x + current_sidebar_w * scale, wpos.y + total_h), true);
        
        // 1. Draw Logo inside Sidebar
        {
            ImVec2 logo_pos = S(15.f, 15.f);
            float logo_w = 90.f * scale;
            float logo_h = 28.f * scale;
            dl->AddRectFilled(logo_pos, ImVec2(logo_pos.x + logo_w, logo_pos.y + logo_h), IM_COL32(28, 28, 28, (int)(255.f * sidebar_anim_t)), 4.0f * scale);
            
            if (g_logo_font) ImGui::PushFont(g_logo_font);
            ImVec2 jw_size = ImGui::CalcTextSize("JW");
            ImVec2 are_size = ImGui::CalcTextSize("are");
            ImVec2 cc_size = ImGui::CalcTextSize(".cc");
            float total_w = jw_size.x + are_size.x + cc_size.x;
            float start_x = logo_pos.x + (logo_w - total_w) * 0.5f;
            float text_y = logo_pos.y + (logo_h - jw_size.y) * 0.5f - 1.f * scale;
            
            dl->AddText(ImVec2(start_x, text_y), Colors::TextBright, "JW");
            ImVec2 are_pos = ImVec2(start_x + jw_size.x, text_y);
            dl->AddText(ImVec2(are_pos.x - 1.f * scale, are_pos.y), IM_COL32(0, 0, 0, (int)(255.f * sidebar_anim_t)), "are");
            dl->AddText(ImVec2(are_pos.x + 1.f * scale, are_pos.y), IM_COL32(0, 0, 0, (int)(255.f * sidebar_anim_t)), "are");
            dl->AddText(ImVec2(are_pos.x, are_pos.y - 1.f * scale), IM_COL32(0, 0, 0, (int)(255.f * sidebar_anim_t)), "are");
            dl->AddText(ImVec2(are_pos.x, are_pos.y + 1.f * scale), IM_COL32(0, 0, 0, (int)(255.f * sidebar_anim_t)), "are");
            dl->AddText(are_pos, Colors::Accent, "are");
            dl->AddText(ImVec2(start_x + jw_size.x + are_size.x, text_y), Colors::TextBright, ".cc");
            if (g_logo_font) ImGui::PopFont();
        }

        // 2. Draw Sidebar Tabs
        {
            float cur_y = 60.f * scale;
            for (int i = 0; i < 5; i++) {
                ImVec2 tp0 = ImVec2(wpos.x + 10.f * scale, wpos.y + cur_y);
                ImVec2 tp1 = ImVec2(wpos.x + 110.f * scale, wpos.y + cur_y + 36.f * scale);
                
                bool hov    = (mouse.x >= tp0.x && mouse.x <= tp1.x &&
                               mouse.y >= tp0.y && mouse.y <= tp1.y);
                bool active = (i == s.active_tab);
                
                if (sidebar_anim_t > 0.5f && hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    s.active_tab = i;
                    s.search_active = false;
                    s.search_query[0] = '\0';
                    if (UI::_ti::focused_id == ImGui::GetID("##search_bar_btn")) {
                        UI::_ti::focused_id = 0;
                    }
                    if (i == 1 && (s.visuals_subtab < 0 || s.visuals_subtab > 3)) {
                        s.visuals_subtab = 0;
                    }
                }
                
                float& ha = hover_anims[tab_names[i]];
                ha += ((hov ? 1.0f : 0.0f) - ha) * io.DeltaTime * 12.0f;
                
                if (active) {
                    dl->AddRectFilled(tp0, tp1, IM_COL32(28, 28, 28, (int)(255.f * g_menu_anim * sidebar_anim_t)), 4.0f * scale);
                    dl->AddRectFilled(tp0, ImVec2(tp0.x + 3.f * scale, tp1.y), Colors::Accent, 4.0f * scale, ImDrawFlags_RoundCornersLeft);
                }
                
                ImU32 tc;
                if (active) {
                    tc = Colors::Accent;
                } else {
                    ImVec4 dim = ImGui::ColorConvertU32ToFloat4(Colors::TextDim);
                    ImVec4 lit = ImGui::ColorConvertU32ToFloat4(Colors::Text);
                    tc = ImGui::ColorConvertFloat4ToU32(ImLerp(dim, lit, ha));
                }
                
                ImVec2 icon_size = ImGui::CalcTextSize(tab_icons[i]);
                dl->AddText(ImVec2(tp0.x + 10.f * scale, tp0.y + (36.f * scale - icon_size.y) * 0.5f), tc, tab_icons[i]);
                
                ImVec2 text_size = ImGui::CalcTextSize(tab_names[i]);
                dl->AddText(ImVec2(tp0.x + 10.f * scale + icon_size.x + 8.f * scale, tp0.y + (36.f * scale - text_size.y) * 0.5f), active ? Colors::TextBright : tc, tab_names[i]);
                
                cur_y += 40.f * scale;
            }
        }

        // 3. Draw Search Bar at Sidebar Bottom
        {
            ImVec2 search_pos = S(5.f, MENU_H - 78.f);
            if (sidebar_anim_t > 0.5f) {
                ImGui::SetCursorPos(ImVec2(5.f * scale, (MENU_H - 78.f) * scale));
                ImGui::InvisibleButton("##search_bar_btn", ImVec2(110.f * scale, 28.f * scale));
            }
            
            static ImGuiID search_bar_id = ImGui::GetID("##search_bar_btn");
            bool search_hov = (sidebar_anim_t > 0.5f) && ImGui::IsItemHovered();
            bool search_clicked = (sidebar_anim_t > 0.5f) && ImGui::IsItemClicked();
            
            if (search_clicked) {
                s.search_active = true;
                UI::_ti::focused_id = search_bar_id;
            }
            
            bool search_focused = (UI::_ti::focused_id == search_bar_id);
            if (search_focused && sidebar_anim_t > 0.5f) {
                bool changed = false;
                int len = (int)strlen(s.search_query);
                if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && len > 0) {
                    s.search_query[--len] = '\0';
                    changed = true;
                }
                for (int i = 0; i < io.InputQueueCharacters.Size; i++) {
                    ImWchar c = io.InputQueueCharacters[i];
                    if (c < 32 || len >= sizeof(s.search_query) - 1) continue;
                    s.search_query[len++] = (char)c;
                    s.search_query[len] = '\0';
                    changed = true;
                }
                if (changed) io.InputQueueCharacters.resize(0);
            }
            
            ImU32 search_bg = search_focused ? IM_COL32(20, 20, 20, (int)(255.f * sidebar_anim_t)) : IM_COL32(28, 28, 28, (int)(255.f * sidebar_anim_t));
            ImU32 search_border = search_focused ? Colors::Accent : Colors::SectionBorder;
            
            dl->AddRectFilled(search_pos, ImVec2(search_pos.x + 110.f * scale, search_pos.y + 28.f * scale), search_bg, 4.0f * scale);
            dl->AddRect(search_pos, ImVec2(search_pos.x + 110.f * scale, search_pos.y + 28.f * scale), search_border, 4.0f * scale);
            
            ImVec2 search_icon_sz = ImGui::CalcTextSize(ICON_FA_SEARCH);
            dl->AddText(ImVec2(search_pos.x + 6.f * scale, search_pos.y + (28.f * scale - search_icon_sz.y) * 0.5f), search_focused ? Colors::Accent : Colors::TextDim, ICON_FA_SEARCH);
            
            float search_text_y = search_pos.y + (28.f * scale - ImGui::GetTextLineHeight()) * 0.5f;
            if (s.search_query[0] != '\0') {
                std::string clip_query = s.search_query;
                if (clip_query.length() > 9) clip_query = clip_query.substr(0, 7) + "..";
                dl->AddText(ImVec2(search_pos.x + (6.f * scale) + search_icon_sz.x + 4.f * scale, search_text_y), Colors::TextBright, clip_query.c_str());
            } else if (!search_focused) {
                dl->AddText(ImVec2(search_pos.x + (6.f * scale) + search_icon_sz.x + 4.f * scale, search_text_y), Colors::TextDim, "Search..");
            }
            
            if (search_focused && (int)(ImGui::GetTime() * 2.0) % 2 == 0) {
                ImVec2 ts = ImGui::CalcTextSize(s.search_query);
                float cx = search_pos.x + (6.f * scale) + search_icon_sz.x + 4.f * scale + ts.x + 1.f * scale;
                if (cx < search_pos.x + 104.f * scale) {
                    dl->AddLine(ImVec2(cx, search_text_y), ImVec2(cx, search_text_y + ImGui::GetTextLineHeight()), Colors::TextBright, 1.0f * scale);
                }
            }
        }

        // 4. Draw Settings Button at Sidebar Bottom (Styled button matching search bar)
        {
            ImVec2 settings_pos = S(5.f, MENU_H - 42.f);
            if (sidebar_anim_t > 0.5f) {
                ImGui::SetCursorPos(ImVec2(5.f * scale, (MENU_H - 42.f) * scale));
                ImGui::InvisibleButton("##settings_gear", ImVec2(110.f * scale, 28.f * scale));
            }
            bool set_hov = (sidebar_anim_t > 0.5f) && ImGui::IsItemHovered();
            bool set_clicked = (sidebar_anim_t > 0.5f) && ImGui::IsItemClicked();
            if (set_clicked) {
                s.settings_open = !s.settings_open;
            }
            
            static float set_ha = 0.0f;
            set_ha += (((set_hov || s.settings_open) ? 1.0f : 0.0f) - set_ha) * io.DeltaTime * 12.0f;
            
            ImU32 btn_bg = ImGui::ColorConvertFloat4ToU32(ImLerp(
                ImGui::ColorConvertU32ToFloat4(IM_COL32(28, 28, 28, (int)(s.bg_opacity * 255.f))),
                ImGui::ColorConvertU32ToFloat4(IM_COL32(20, 20, 20, (int)(s.bg_opacity * 255.f))),
                set_ha
            ));
            ImU32 btn_border = ImGui::ColorConvertFloat4ToU32(ImLerp(
                ImGui::ColorConvertU32ToFloat4(Colors::SectionBorder),
                ImGui::ColorConvertU32ToFloat4(Colors::Accent),
                set_ha
            ));
            ImU32 text_color = ImGui::ColorConvertFloat4ToU32(ImLerp(
                ImGui::ColorConvertU32ToFloat4(Colors::TextDim),
                ImGui::ColorConvertU32ToFloat4(Colors::TextBright),
                set_ha
            ));
            
            dl->AddRectFilled(settings_pos, ImVec2(settings_pos.x + 110.f * scale, settings_pos.y + 28.f * scale), btn_bg, 4.0f * scale);
            dl->AddRect(settings_pos, ImVec2(settings_pos.x + 110.f * scale, settings_pos.y + 28.f * scale), btn_border, 4.0f * scale);
            
            ImVec2 gear_sz = ImGui::CalcTextSize(ICON_FA_COG);
            dl->AddText(ImVec2(settings_pos.x + 10.f * scale, settings_pos.y + (28.f * scale - gear_sz.y) * 0.5f), (set_hov || s.settings_open) ? Colors::Accent : text_color, ICON_FA_COG);
            
            ImVec2 txt_sz = ImGui::CalcTextSize("Settings");
            dl->AddText(ImVec2(settings_pos.x + 10.f * scale + gear_sz.x + 8.f * scale, settings_pos.y + (28.f * scale - txt_sz.y) * 0.5f), text_color, "Settings");
        }
        
        ImGui::PopClipRect();
        ImGui::PopStyleVar();
    }

    // --- TOP HEADER NAVIGATION LAYOUT ---
    if (sidebar_anim_t < 0.995f) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_menu_anim * (1.0f - sidebar_anim_t));
        ImGui::PushClipRect(wpos, ImVec2(wpos.x + MENU_W * scale, wpos.y + current_header_h * scale), true);
        
        // 1. Draw Logo
        {
            ImVec2 logo_pos = S(12.f, 8.f);
            float logo_w = 90.f * scale;
            float logo_h = 28.f * scale;
            dl->AddRectFilled(logo_pos, ImVec2(logo_pos.x + logo_w, logo_pos.y + logo_h), IM_COL32(28, 28, 28, (int)(255.f * (1.0f - sidebar_anim_t))), 4.0f * scale);
            
            if (g_logo_font) ImGui::PushFont(g_logo_font);
            ImVec2 jw_size = ImGui::CalcTextSize("JW");
            ImVec2 are_size = ImGui::CalcTextSize("are");
            ImVec2 cc_size = ImGui::CalcTextSize(".cc");
            float total_w = jw_size.x + are_size.x + cc_size.x;
            float start_x = logo_pos.x + (logo_w - total_w) * 0.5f;
            float text_y = logo_pos.y + (logo_h - jw_size.y) * 0.5f - 1.f * scale;
            
            dl->AddText(ImVec2(start_x, text_y), Colors::TextBright, "JW");
            ImVec2 are_pos = ImVec2(start_x + jw_size.x, text_y);
            dl->AddText(ImVec2(are_pos.x - 1.f * scale, are_pos.y), IM_COL32(0, 0, 0, (int)(255.f * (1.0f - sidebar_anim_t))), "are");
            dl->AddText(ImVec2(are_pos.x + 1.f * scale, are_pos.y), IM_COL32(0, 0, 0, (int)(255.f * (1.0f - sidebar_anim_t))), "are");
            dl->AddText(ImVec2(are_pos.x, are_pos.y - 1.f * scale), IM_COL32(0, 0, 0, (int)(255.f * (1.0f - sidebar_anim_t))), "are");
            dl->AddText(ImVec2(are_pos.x, are_pos.y + 1.f * scale), IM_COL32(0, 0, 0, (int)(255.f * (1.0f - sidebar_anim_t))), "are");
            dl->AddText(are_pos, Colors::Accent, "are");
            dl->AddText(ImVec2(start_x + jw_size.x + are_size.x, text_y), Colors::TextBright, ".cc");
            if (g_logo_font) ImGui::PopFont();
        }

        // 2. Draw Tabs
        {
            float cur_x = 115.f * scale;
            static float anim_line_x = -1.f;
            static float anim_line_w = 0.f;
            static float anim_line_alpha = 0.f;

            float target_line_x = -1.f;
            float target_line_w = 0.f;
            float target_alpha = s.search_active ? 0.f : 1.f;

            for (int i = 0; i < 5; i++) {
                ImVec2 text_size = ImGui::CalcTextSize(tab_names[i]);
                ImVec2 icon_size = ImGui::CalcTextSize(tab_icons[i]);
                float tab_width = icon_size.x + 6.f * scale + text_size.x;
                
                ImVec2 tp0 = ImVec2(wpos.x + cur_x, wpos.y);
                ImVec2 tp1 = ImVec2(wpos.x + cur_x + tab_width, wpos.y + HEADER_H * scale);

                bool hov    = (mouse.x >= tp0.x && mouse.x <= tp1.x &&
                               mouse.y >= tp0.y && mouse.y <= tp1.y);
                bool active = (i == s.active_tab) && !s.search_active;

                if (sidebar_anim_t < 0.5f && hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    s.active_tab = i;
                    s.search_active = false;
                    s.search_query[0] = '\0';
                    if (UI::_ti::focused_id == ImGui::GetID("##search_bar_btn")) {
                        UI::_ti::focused_id = 0;
                    }
                    if (i == 1 && (s.visuals_subtab < 0 || s.visuals_subtab > 3)) {
                        s.visuals_subtab = 0;
                    }
                }

                if (active) {
                    target_line_x = wpos.x + cur_x - 4.f * scale;
                    target_line_w = tab_width + 8.f * scale;
                }

                float& ha = hover_anims[tab_names[i]];
                ha += ((hov ? 1.0f : 0.0f) - ha) * io.DeltaTime * 12.0f;

                ImU32 tc;
                if (active) {
                    tc = Colors::Accent;
                } else {
                    ImVec4 dim = ImGui::ColorConvertU32ToFloat4(Colors::TextDim);
                    ImVec4 lit = ImGui::ColorConvertU32ToFloat4(Colors::Text);
                    tc = ImGui::ColorConvertFloat4ToU32(ImLerp(dim, lit, ha));
                }

                dl->AddText(ImVec2(wpos.x + cur_x, wpos.y + (HEADER_H * scale - icon_size.y) * 0.5f), tc, tab_icons[i]);
                dl->AddText(ImVec2(wpos.x + cur_x + icon_size.x + 6.f * scale, wpos.y + (HEADER_H * scale - text_size.y) * 0.5f), active ? Colors::TextBright : tc, tab_names[i]);

                cur_x += tab_width + 22.f * scale;
            }

            if (target_line_x != -1.f) {
                if (anim_line_x == -1.f) {
                    anim_line_x = target_line_x;
                    anim_line_w = target_line_w;
                } else {
                    anim_line_x += (target_line_x - anim_line_x) * io.DeltaTime * 12.0f;
                    anim_line_w += (target_line_w - anim_line_w) * io.DeltaTime * 12.0f;
                }
            }
            anim_line_alpha += (target_alpha - anim_line_alpha) * io.DeltaTime * 12.0f;

            if (anim_line_alpha > 0.005f && anim_line_x != -1.f) {
                ImVec4 acc_col = ImGui::ColorConvertU32ToFloat4(Colors::Accent);
                acc_col.w *= anim_line_alpha;
                dl->AddRectFilled(
                    ImVec2(anim_line_x, wpos.y + (HEADER_H - 2.f) * scale),
                    ImVec2(anim_line_x + anim_line_w, wpos.y + HEADER_H * scale),
                    ImGui::ColorConvertFloat4ToU32(acc_col)
                );
            }
        }

        // 3. Draw Search Bar
        {
            ImVec2 search_pos = S(500.f, 8.f);
            if (sidebar_anim_t < 0.5f) {
                ImGui::SetCursorPos(ImVec2(500.f * scale, 8.f * scale));
                ImGui::InvisibleButton("##search_bar_btn", ImVec2(115.f * scale, 28.f * scale));
            }
            
            static ImGuiID search_bar_id = ImGui::GetID("##search_bar_btn");
            bool search_hov = (sidebar_anim_t < 0.5f) && ImGui::IsItemHovered();
            bool search_clicked = (sidebar_anim_t < 0.5f) && ImGui::IsItemClicked();
            
            if (search_clicked) {
                s.search_active = true;
                UI::_ti::focused_id = search_bar_id;
            }
            
            bool search_focused = (UI::_ti::focused_id == search_bar_id);
            if (search_focused && sidebar_anim_t < 0.5f) {
                ImGuiIO& io = ImGui::GetIO();
                io.WantCaptureKeyboard = true;
                int len = (int)strlen(s.search_query);
                bool changed = false;
                
                if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && len > 0) {
                    s.search_query[--len] = '\0';
                    changed = true;
                }
                for (int i = 0; i < io.InputQueueCharacters.Size; i++) {
                    ImWchar c = io.InputQueueCharacters[i];
                    if (c < 32 || len >= sizeof(s.search_query) - 1) continue;
                    s.search_query[len++] = (char)c;
                    s.search_query[len] = '\0';
                    changed = true;
                }
                if (changed) io.InputQueueCharacters.resize(0);
            }
            
            ImU32 search_bg = search_focused ? IM_COL32(20, 20, 20, (int)(255.f * (1.0f - sidebar_anim_t))) : IM_COL32(28, 28, 28, (int)(255.f * (1.0f - sidebar_anim_t)));
            ImU32 search_border = search_focused ? Colors::Accent : Colors::SectionBorder;
            
            dl->AddRectFilled(search_pos, ImVec2(search_pos.x + 115.f * scale, search_pos.y + 28.f * scale), search_bg, 4.0f * scale);
            dl->AddRect(search_pos, ImVec2(search_pos.x + 115.f * scale, search_pos.y + 28.f * scale), search_border, 4.0f * scale);
            
            ImVec2 search_icon_sz = ImGui::CalcTextSize(ICON_FA_SEARCH);
            dl->AddText(ImVec2(search_pos.x + 8.f * scale, search_pos.y + (28.f * scale - search_icon_sz.y) * 0.5f), search_focused ? Colors::Accent : Colors::TextDim, ICON_FA_SEARCH);
            
            float search_text_y = search_pos.y + (28.f * scale - ImGui::GetTextLineHeight()) * 0.5f;
            if (s.search_query[0] != '\0') {
                dl->AddText(ImVec2(search_pos.x + (8.f * scale) + search_icon_sz.x + 6.f * scale, search_text_y), Colors::TextBright, s.search_query);
            } else if (!search_focused) {
                dl->AddText(ImVec2(search_pos.x + (8.f * scale) + search_icon_sz.x + 6.f * scale, search_text_y), Colors::TextDim, "Search...");
            }
            
            if (search_focused && (int)(ImGui::GetTime() * 2.0) % 2 == 0) {
                ImVec2 ts = ImGui::CalcTextSize(s.search_query);
                float cx = search_pos.x + (8.f * scale) + search_icon_sz.x + 6.f * scale + ts.x + 1.f * scale;
                dl->AddLine(ImVec2(cx, search_text_y), ImVec2(cx, search_text_y + ImGui::GetTextLineHeight()), Colors::TextBright, 1.0f * scale);
            }
        }

        // 4. Draw Settings Gear
        {
            ImVec2 settings_pos = S(632.f, 14.f);
            if (sidebar_anim_t < 0.5f) {
                ImGui::SetCursorPos(ImVec2(626.f * scale, 8.f * scale));
                ImGui::InvisibleButton("##settings_gear", ImVec2(24.f * scale, 24.f * scale));
            }
            bool set_hov = (sidebar_anim_t < 0.5f) && ImGui::IsItemHovered();
            bool set_clicked = (sidebar_anim_t < 0.5f) && ImGui::IsItemClicked();
            
            if (set_clicked) {
                s.settings_open = !s.settings_open;
            }
            
            dl->AddText(settings_pos, (set_hov || s.settings_open) ? Colors::Accent : Colors::TextDim, ICON_FA_COG);
        }
        
        ImGui::PopClipRect();
        ImGui::PopStyleVar();
    }

    // 5. Tab switching animation
    static int last_tab = -1;
    static float transition_t = 1.0f;
    if (s.active_tab != last_tab) {
        last_tab = s.active_tab;
        transition_t = 0.0f;
    }
    transition_t += io.DeltaTime * 6.0f;
    if (transition_t > 1.0f) transition_t = 1.0f;

    float eased_t = 1.0f - powf(1.0f - transition_t, 3.0f);
    float anim_y_offset = (1.0f - eased_t) * 10.0f;

    wpos.y += anim_y_offset;
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_menu_anim * eased_t);

    const float cy   = ImLerp(HEADER_H + 10.0f + anim_y_offset, 10.0f + anim_y_offset, sidebar_anim_t);
    const float lh   = ImGui::GetTextLineHeight();
    if (s.search_active) {
        struct PanelInfo {
            std::string name;
            std::vector<std::string> keywords;
        };
        
        std::vector<PanelInfo> panels_info = {
            { "aimbot", { "aimbot", "hitboxes", "draw fov circle", "triggerbot", "target type" } },
            { "settings", { "settings", "field of view", "fov" } },
            { "enemy esp", { "enemy esp", "esp", "skeleton" } },
            { "enemy chams", { "enemy chams", "chams" } },
            { "friendly esp", { "friendly esp", "esp", "skeleton" } },
            { "friendly chams", { "friendly chams", "chams" } },
            { "world", { "world" } },
            { "extra", { "extra" } },
            { "miscellaneous", { "miscellaneous", "misc" } },
            { "indicators", { "indicators" } },
            { "menu", { "menu" } },
            { "configs", { "configs", "config", "create", "save", "load" } }
        };
        
        std::vector<std::string> matches;
        std::string query = s.search_query;
        for (auto& c : query) c = tolower(c);
        
        if (!query.empty()) {
            for (const auto& info : panels_info) {
                bool matched = false;
                std::string lower_name = info.name;
                for (auto& c : lower_name) c = tolower(c);
                if (lower_name.find(query) != std::string::npos) {
                    matched = true;
                }
                if (!matched) {
                    for (const auto& keyword : info.keywords) {
                        std::string lower_kw = keyword;
                        for (auto& c : lower_kw) c = tolower(c);
                        if (lower_kw.find(query) != std::string::npos) {
                            matched = true;
                            break;
                        }
                    }
                }
                if (matched) {
                    matches.push_back(info.name);
                }
            }
        }
        
        float content_y = cy;
        ImGui::SetCursorPos(ImVec2(0, content_y));
        
        float PADDING = ImLerp(10.f, 130.f, sidebar_anim_t);
        float L_W     = ImLerp(310.f, 250.f, sidebar_anim_t);
        float R_X     = ImLerp(336.f, 390.f, sidebar_anim_t);
        float R_W     = ImLerp(314.f, 250.f, sidebar_anim_t);

        if (matches.size() >= 1) {
            RenderChildPanel(matches[0], PADDING, content_y, L_W, MENU_H - 10.0f - content_y, dl, wpos, io);
        }
        if (matches.size() >= 2) {
            RenderChildPanel(matches[1], R_X, content_y, R_W, MENU_H - 10.0f - content_y, dl, wpos, io);
        }
    }
    else {
        if (s.active_tab == 1) {
            float sub_box_x0 = ImLerp(10.f, 130.f, sidebar_anim_t);
            float sub_box_x1 = ImLerp(130.f, 230.f, sidebar_anim_t);
            float PADDING = ImLerp(140.f, 240.f, sidebar_anim_t);
            float L_W     = ImLerp(245.f, 195.f, sidebar_anim_t);
            float R_X     = ImLerp(397.f, 445.f, sidebar_anim_t);
            float R_W     = ImLerp(253.f, 205.f, sidebar_anim_t);
            float BOX_PAD = 6.0f;
      
            // Render vertical subtabs on the left
            const char* sub_labels[] = { "Enemy", "Friendly", "World", "Extra" };
            
            // Draw vertical subtabs
            {
                float box_x0 = sub_box_x0;
                float box_y0 = cy;
                float box_x1 = sub_box_x1;
                float box_y1 = MENU_H - 10.f;
    
                // Draw panel background and border
                dl->AddRectFilled(S(box_x0, box_y0), S(box_x1, box_y1), IM_COL32(16, 16, 16, (int)(s.bg_opacity * 255.f)), 5.0f * scale);
                dl->AddRect(S(box_x0, box_y0), S(box_x1, box_y1), Colors::SectionBorder, 5.0f * scale);
    
                float subtab_w = (box_x1 - box_x0 - 8.f);
                float subtab_x = box_x0 + 4.f;
                float item_h = 32.f;
                float cur_y = box_y0 + 8.f;
                ImVec2 mouse = io.MousePos;
     
                for (int i = 0; i < 4; i++) {
                    ImVec2 tp0 = S(subtab_x, cur_y);
                    ImVec2 tp1 = S(subtab_x + subtab_w, cur_y + item_h);
     
                    bool hov = (mouse.x >= tp0.x && mouse.x <= tp1.x &&
                                mouse.y >= tp0.y && mouse.y <= tp1.y);
                    bool active = (i == s.visuals_subtab);
     
                    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        s.visuals_subtab = i;
                    }
     
                    if (active) {
                        dl->AddRectFilled(tp0, tp1, IM_COL32(28, 28, 28, (int)(s.bg_opacity * 255.f)), 4.0f * scale);
                        dl->AddRectFilled(tp0, ImVec2(tp0.x + 3.f * scale, tp1.y), Colors::Accent, 4.0f * scale, ImDrawFlags_RoundCornersLeft);
                    }
     
                    ImVec2 ts = ImGui::CalcTextSize(sub_labels[i]);
                    ImVec2 ttp = ImVec2(tp0.x + 12.f * scale, tp0.y + (item_h * scale - ts.y) * 0.5f);
                    dl->AddText(ttp, active ? Colors::TextBright : (hov ? Colors::Text : Colors::TextDim), sub_labels[i]);
     
                    cur_y += item_h + 4.f;
                }
            }
     
            float content_y = cy;
            ImGui::SetCursorPos(ImVec2(0, content_y));
     
            if (s.visuals_subtab == 0) {
                RenderChildPanel("enemy esp", PADDING, content_y, L_W, MENU_H - 10.0f - content_y, dl, wpos, io);
                RenderChildPanel("enemy chams", R_X, content_y, R_W, MENU_H - 10.0f - content_y, dl, wpos, io);
            }
            else if (s.visuals_subtab == 1) {
                RenderChildPanel("friendly esp", PADDING, content_y, L_W, MENU_H - 10.0f - content_y, dl, wpos, io);
                RenderChildPanel("friendly chams", R_X, content_y, R_W, MENU_H - 10.0f - content_y, dl, wpos, io);
            }
            else if (s.visuals_subtab == 2) {
                float L_W_full = ImLerp(510.f, 410.f, sidebar_anim_t);
                RenderChildPanel("world", PADDING, content_y, L_W_full, MENU_H - 10.0f - content_y, dl, wpos, io);
            }
            else if (s.visuals_subtab == 3) {
                float L_W_full = ImLerp(510.f, 410.f, sidebar_anim_t);
                RenderChildPanel("extra", PADDING, content_y, L_W_full, MENU_H - 10.0f - content_y, dl, wpos, io);
            }
        }
        else if (s.active_tab == 2) {
            float PADDING = ImLerp(10.f, 130.f, sidebar_anim_t);
            float L_W     = ImLerp(310.f, 250.f, sidebar_anim_t);
            float R_X     = ImLerp(336.f, 390.f, sidebar_anim_t);
            float R_W     = ImLerp(314.f, 250.f, sidebar_anim_t);
            float content_y = cy;
            RenderChildPanel("miscellaneous", PADDING, content_y, L_W, MENU_H - 10.0f - content_y, dl, wpos, io);
            RenderChildPanel("indicators", R_X, content_y, R_W, MENU_H - 10.0f - content_y, dl, wpos, io);
        }
        else if (s.active_tab == 3) {
            float PADDING = ImLerp(10.f, 130.f, sidebar_anim_t);
            float L_W     = ImLerp(310.f, 250.f, sidebar_anim_t);
            float R_X     = ImLerp(336.f, 390.f, sidebar_anim_t);
            float R_W     = ImLerp(314.f, 250.f, sidebar_anim_t);
            float content_y = cy;
            RenderChildPanel("menu", PADDING, content_y, L_W, MENU_H - 10.0f - content_y, dl, wpos, io);
            RenderChildPanel("configs", R_X, content_y, R_W, MENU_H - 8.0f - content_y, dl, wpos, io);
            
            ImGui::SetCursorPos(ImVec2(PADDING, MENU_H - 8.0f + 1.0f));
            ImGui::Dummy(ImVec2(1.0f, 1.0f));
        }
        else if (s.active_tab == 0) {
            float PADDING = ImLerp(10.f, 130.f, sidebar_anim_t);
            float L_W     = ImLerp(310.f, 250.f, sidebar_anim_t);
            float R_X     = ImLerp(336.f, 390.f, sidebar_anim_t);
            float R_W     = ImLerp(314.f, 250.f, sidebar_anim_t);
            float content_y = cy;
            float right_h = MENU_H - 10.0f - content_y;
            float settings_h = right_h * 0.48f;  // top half for settings
            float tb_y = content_y + settings_h + 4.0f;  // bottom half for triggerbot
            float tb_h = right_h - settings_h - 4.0f;
            RenderChildPanel("aimbot", PADDING, content_y, L_W, MENU_H - 10.0f - content_y, dl, wpos, io);
            RenderChildPanel("settings", R_X, content_y, R_W, settings_h, dl, wpos, io);
            RenderChildPanel("triggerbot", R_X, tb_y, R_W, tb_h, dl, wpos, io);
        }
        else if (s.active_tab == 4) {
            float PADDING = ImLerp(10.f, 130.f, sidebar_anim_t);
            float L_W_full = ImLerp(640.f, 510.f, sidebar_anim_t);
            float content_y = cy;

            // If not yet accepted, show confirmation popup
            if (!s.w_tab_accepted) {
                static bool popup_opened = false;
                if (!popup_opened) {
                    ImGui::OpenPopup("##write_warning_popup");
                    popup_opened = true;
                }

                // Center the popup
                ImVec2 center(wpos.x + (MENU_W * scale) * 0.5f, wpos.y + (MENU_H * scale) * 0.5f);
                ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
                ImGui::SetNextWindowSize(ImVec2(380.0f * scale, 0.0f));

                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f * scale, 16.0f * scale));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * scale);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f * scale);
                ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(22, 22, 22, 245));
                ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(200, 50, 50, 200));
                ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(22, 22, 22, 245));

                if (ImGui::BeginPopupModal("##write_warning_popup", nullptr,
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize)) {

                    // Warning icon + title
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 80, 80, 255));
                    ImGui::Text(ICON_FA_WARNING);
                    ImGui::SameLine();
                    ImGui::Text("  Warning");
                    ImGui::PopStyleColor();

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Spacing();

                    // Warning text
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(210, 200, 200, 255));
                    ImGui::TextWrapped(
                        "These features write directly to game memory "
                        "and may trigger anti-cheat detections, resulting "
                        "in a ban.");
                    ImGui::Spacing();
                    ImGui::TextWrapped(
                        "Only use these on private/offline matches. "
                        "Do you wish to continue?");
                    ImGui::PopStyleColor();

                    ImGui::Spacing();
                    ImGui::Spacing();

                    // Buttons
                    float btn_w = 160.0f * scale;
                    float total_w = btn_w * 2.0f + 10.0f * scale;
                    float start_x = (ImGui::GetWindowWidth() - total_w) * 0.5f;

                    ImGui::SetCursorPosX(start_x);

                    // "Yes" button — red accent
                    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(160, 30, 30, 255));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(200, 50, 50, 255));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(130, 20, 20, 255));
                    if (ImGui::Button("Yes, I understand", ImVec2(btn_w, 28.0f * scale))) {
                        s.w_tab_accepted = true;
                        popup_opened = false;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::PopStyleColor(3);

                    ImGui::SameLine(0, 10.0f * scale);

                    // "No" button — neutral
                    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(50, 50, 50, 255));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(70, 70, 70, 255));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(35, 35, 35, 255));
                    if (ImGui::Button("No, go back", ImVec2(btn_w, 28.0f * scale))) {
                        s.active_tab = 0;  // go back to Aimbot
                        popup_opened = false;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::PopStyleColor(3);

                    ImGui::Spacing();
                    ImGui::EndPopup();
                }

                ImGui::PopStyleColor(3);
                ImGui::PopStyleVar(3);
            }
            else {
                // User accepted — show the write features
                float panel_h = MENU_H - 10.0f - content_y;
                float half_w = (L_W_full - 14.0f) / 2.0f;
                float r_x = PADDING + half_w + 14.0f;
                RenderChildPanel("write: game", PADDING, content_y, half_w, panel_h, dl, wpos, io);
                RenderChildPanel("write: player", r_x, content_y, half_w, panel_h, dl, wpos, io);
            }
        }
    }

    // Dragging handled by Win32 caption window now

    if (s.settings_open) {
        float settings_x = ImLerp(MENU_W - 240.f - 10.f, current_sidebar_w + 10.f, sidebar_anim_t);
        float settings_y = ImLerp(HEADER_H + 8.f, 10.f, sidebar_anim_t);
        ImGui::SetNextWindowPos(ImVec2(wpos.x + settings_x * scale, wpos.y + settings_y * scale), ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(240.f * scale, 380.f * scale));
        
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.f * scale, 10.f * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 5.f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f * scale);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Colors::Bg);
        ImGui::PushStyleColor(ImGuiCol_Border, Colors::SectionBorder);
        
        ImGui::Begin("##settings_popup", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar);
        
        // Settings Title
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Colors::TextBright), "Settings");
        
        ImGui::SameLine(ImGui::GetWindowWidth() - 25.f * scale);
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(40, 40, 40, 100));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(60, 60, 60, 150));
        if (ImGui::Button("x", ImVec2(16.f * scale, 16.f * scale))) {
            s.settings_open = false;
        }
        ImGui::PopStyleColor(3);
        
        ImGui::Separator();
        ImGui::Spacing();
        
        // 1. Menu Scaling
        float scale_val = s.menu_scale;
        if (UI::SliderFloat("Menu Scaling", &scale_val, 1.0f, 2.0f, "", "%.1f", 216.f * scale)) {
            s.menu_scale = scale_val;
        }
        ImGui::Spacing();
        
        // 2. Navigation Style
        static const char* nav_styles[] = { "Top Header", "Left Sidebar" };
        UI::Dropdown("Navigation Style", &s.navigation_style, nav_styles, 2, 216.f * scale);
        ImGui::Spacing();
        
        // 3. Theme
        static const char* theme_names[] = { "Default", "Blue", "Purple", "Green", "Red" };
        UI::Dropdown("Theme", &s.selected_theme, theme_names, 5, 216.f * scale);
        
        static int last_theme = -1;
        if (s.selected_theme != last_theme) {
            last_theme = s.selected_theme;
            if (s.selected_theme == 0) s.menu_color = ImVec4(0.85f, 0.1f, 0.1f, 1.0f);
            else if (s.selected_theme == 1) s.menu_color = ImVec4(0.1f, 0.5f, 0.85f, 1.0f);
            else if (s.selected_theme == 2) s.menu_color = ImVec4(0.5f, 0.1f, 0.85f, 1.0f);
            else if (s.selected_theme == 3) s.menu_color = ImVec4(0.1f, 0.85f, 0.5f, 1.0f);
            else if (s.selected_theme == 4) s.menu_color = ImVec4(0.85f, 0.1f, 0.1f, 1.0f);
        }
        ImGui::Spacing();
        
        // 4. Background Opacity
        float opacity_pct = s.bg_opacity * 100.f;
        if (UI::SliderFloat("Background Opacity", &opacity_pct, 0.f, 100.f, "%", "%.0f", 216.f * scale)) {
            s.bg_opacity = opacity_pct / 100.f;
        }
        ImGui::Spacing();

        
        // 6. Menu Color
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Colors::TextDim), "Menu Color");
        ImGui::SameLine();
        UI::ColorPicker("##menu_color_picker", &s.menu_color, ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 10.f * scale);
        
        ImGui::Dummy(ImVec2(0.f, 10.f * scale));
        
        // 7. Select Monitor
        static const char* monitor_names[] = { "Monitor 1", "Monitor 2", "Monitor 3", "Monitor 4" };
        UI::Dropdown("Select Monitor", &s.selected_monitor, monitor_names, 4, 216.f * scale);
        ImGui::Spacing();
        
        // 8. Auto Load Config
        UI::Checkbox("Auto Load Config", &s.auto_load_config, nullptr, nullptr, 216.f * scale);
        
        ImGui::Dummy(ImVec2(1.f * scale, 1.f * scale));
        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
    }

    // Ensure window boundaries are grown to match the menu size to satisfy ImGui assertions
    ImGui::SetCursorPos(ImVec2(MENU_W * scale, MENU_H * scale));
    ImGui::Dummy(ImVec2(0.f, 0.f));
 
    ImGui::PopStyleVar(2);
    ImGui::End();

    UI::RenderOpenDropdown();
    UI::RenderOpenMultiDropdown();
    UI::RenderOpenColorPicker();

    io.FontGlobalScale = original_font_scale;
}

void Menu::RenderWidgets() {
    float scale = s.menu_scale;

    // 1. Keybinds List Widget
    if (s.ind_keybinds) {
        ImGui::SetNextWindowSize(ImVec2(180.0f * scale, 0.0f)); // auto height
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * scale, 8.0f * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Colors::Bg);
        
        ImGui::Begin("##keybinds_widget", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);
        
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wpos = ImGui::GetWindowPos();
        ImVec2 wsize = ImGui::GetWindowSize();
        
        // Centered lowercase title
        float text_width = ImGui::CalcTextSize("keybinds").x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - text_width) * 0.5f);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Colors::TextBright), "keybinds");
        
        // Separator line underneath
        ImGui::Dummy(ImVec2(0.0f, 3.0f * scale));
        ImVec2 cursor_pos = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(
            ImVec2(wpos.x, cursor_pos.y),
            ImVec2(wpos.x + wsize.x, cursor_pos.y + 1.5f * scale),
            Colors::Accent
        );
        ImGui::Dummy(ImVec2(0.0f, 6.0f * scale));
        
        int active_count = 0;
        for (auto& [id, entry] : UI::g_binds) {
            bool is_active = false;
            if (entry.mode == 4) is_active = true;
            else if (entry.mode == 1) is_active = UI::IsKeyActive(entry.key, entry.mode);
            else if (entry.mode == 2) is_active = !UI::IsKeyActive(entry.key, entry.mode);
            else if (entry.mode == 3) {
                if (id == "aimbot") is_active = s.aimbot;
                else if (id == "triggerbot") is_active = s.triggerbot;
            }
            
            if (is_active && entry.mode != 0) {
                const char* mode_str = "always";
                if (entry.mode == 1) mode_str = "hold";
                else if (entry.mode == 2) mode_str = "hold off";
                else if (entry.mode == 3) mode_str = "toggle";
                
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Colors::Text), id.c_str());
                ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(mode_str).x - 8.0f * scale);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Colors::TextDim), mode_str);
                active_count++;
            }
        }
        if (active_count == 0) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Colors::TextDim), "no active binds");
        }
        
        ImGui::End();
        ImGui::PopStyleColor(1);
        ImGui::PopStyleVar(3);
    }

    // 2. Spectator List Widget
    if (s.ind_spectators) {
        ImGui::SetNextWindowSize(ImVec2(180.0f * scale, 0.0f)); // auto height
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * scale, 8.0f * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Colors::Bg);
        
        ImGui::Begin("##spectators_widget", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);
        
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wpos = ImGui::GetWindowPos();
        ImVec2 wsize = ImGui::GetWindowSize();
        
        // Centered lowercase title
        float text_width = ImGui::CalcTextSize("spectators").x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - text_width) * 0.5f);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Colors::TextBright), "spectators");
        
        // Separator line underneath
        ImGui::Dummy(ImVec2(0.0f, 3.0f * scale));
        ImVec2 cursor_pos = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(
            ImVec2(wpos.x, cursor_pos.y),
            ImVec2(wpos.x + wsize.x, cursor_pos.y + 1.5f * scale),
            Colors::Accent
        );
        ImGui::Dummy(ImVec2(0.0f, 6.0f * scale));
        
        const char* mock_spectators[] = { "Joshie", "Gamer123", "Admin" };
        for (int i = 0; i < 3; i++) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Colors::Text), mock_spectators[i]);
            ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize("spec").x - 8.0f * scale);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(Colors::TextDim), "spec");
        }
        
        ImGui::End();
        ImGui::PopStyleColor(1);
        ImGui::PopStyleVar(3);
    }
}
