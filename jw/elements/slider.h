#pragma once
#include "../imgui/imgui.h"
#include "../imgui/imgui_internal.h"
#include "colors.h"
#include <string>
#include <unordered_map>

namespace UI {

inline bool SliderFloat(const char* label, float* v, float vmin, float vmax,
                        const char* suffix = "", const char* fmt = "%.0f",
                        float col_w = -1.f) {
    ImDrawList* dl      = ImGui::GetWindowDrawList();
    ImVec2      pos     = ImGui::GetCursorScreenPos();
    float       avail_w = (col_w > 0.f) ? col_w : ImGui::GetContentRegionAvail().x;

    float scale = ImGui::GetIO().FontGlobalScale;
    const float lbl_h   = ImGui::GetTextLineHeight();
    const float track_h = 4.0f * scale;
    const float val_w   = 42.0f * scale;
    const float minus_w = 14.0f * scale;
    const float plus_w  = 14.0f * scale;
    const float gap     = 6.0f * scale;
    const float track_w = avail_w - val_w - minus_w - plus_w - gap * 3;
    const float total_h = lbl_h + 3.0f * scale + track_h + 3.0f * scale;

    // Hover state animation tracking
    bool is_hov = ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + avail_w, pos.y + total_h));
    static std::unordered_map<const void*, float> hover_anims;
    float& hov_anim = hover_anims[v];
    hov_anim += ((is_hov ? 1.0f : 0.0f) - hov_anim) * ImGui::GetIO().DeltaTime * 12.0f;
    if (fabsf(hov_anim - (is_hov ? 1.0f : 0.0f)) < 0.001f) hov_anim = is_hov ? 1.0f : 0.0f;

    ImVec4 text_dim = ImGui::ColorConvertU32ToFloat4(Colors::TextDim);
    ImVec4 text_normal = ImGui::ColorConvertU32ToFloat4(Colors::Text);
    ImU32 text_col = ImGui::ColorConvertFloat4ToU32(ImLerp(text_dim, text_normal, hov_anim));
    dl->AddText(pos, text_col, label);

    ImVec2 tp0 = ImVec2(pos.x + minus_w + gap, pos.y + lbl_h + 3.0f * scale);
    ImVec2 tp1 = ImVec2(tp0.x + track_w, tp0.y + track_h);

    std::string id = std::string("##sl") + label;
    ImGui::SetCursorScreenPos(ImVec2(tp0.x, tp0.y - 3.0f * scale));
    ImGui::InvisibleButton(id.c_str(), ImVec2(track_w, track_h + 6.0f * scale));
    ImGuiID item_id = ImGui::GetID(id.c_str());
    bool held    = ImGui::IsItemActive();
    bool changed = false;
 
    static ImGuiID allowed_drag_id = 0;
    if (held) {
        if (ImGui::IsItemClicked()) {
            if (!UI::IsMouseOverOverlay() && ImGui::GetFrameCount() != UI::_blocked_click_frame) {
                allowed_drag_id = item_id;
            }
        }
        if (allowed_drag_id == item_id) {
            float mx = ImGui::GetIO().MousePos.x;
            float t  = ImClamp((mx - tp0.x) / track_w, 0.0f, 1.0f);
            float nv = vmin + t * (vmax - vmin);
            if (fabsf(nv - *v) > 1e-5f) { *v = nv; changed = true; }
        }
    } else {
        if (allowed_drag_id == item_id) {
            allowed_drag_id = 0;
        }
    }

    float t = (vmax > vmin) ? ImClamp((*v - vmin) / (vmax - vmin), 0.0f, 1.0f) : 0.0f;
 
    static std::unordered_map<std::string, float> anims;
    float& anim_t = anims[label];
    anim_t += (t - anim_t) * ImGui::GetIO().DeltaTime * 12.0f;
    if (fabsf(anim_t - t) < 0.0001f) anim_t = t;
 
    ImU32 track_col0 = ImGui::ColorConvertFloat4ToU32(ImLerp(ImVec4(16.f/255.f, 16.f/255.f, 16.f/255.f, 1.f), ImVec4(24.f/255.f, 24.f/255.f, 24.f/255.f, 1.f), hov_anim));
    ImU32 track_col1 = ImGui::ColorConvertFloat4ToU32(ImLerp(ImVec4(23.f/255.f, 23.f/255.f, 23.f/255.f, 1.f), ImVec4(32.f/255.f, 32.f/255.f, 32.f/255.f, 1.f), hov_anim));
    dl->AddRectFilledMultiColor(tp0, tp1, track_col0, track_col0, track_col1, track_col1);
 
    if (anim_t > 0.001f) {
        ImVec2 fe = ImVec2(tp0.x + anim_t * track_w, tp1.y);
 
        ImVec4 acc  = ImGui::ColorConvertU32ToFloat4(Colors::Accent);
        ImVec4 dark = ImVec4(acc.x * 0.62f, acc.y * 0.62f, acc.z * 0.62f, 1.0f);
        ImU32  dim  = ImGui::ColorConvertFloat4ToU32(dark);
 
        dl->AddRectFilledMultiColor(tp0, fe,
            dim,           Colors::Accent,
            Colors::Accent, dim);
    }
 
    char buf[32];
    snprintf(buf, sizeof(buf), fmt, *v);
    char full[48];
    snprintf(full, sizeof(full), "%s%s", buf, suffix);
 
    float vy = tp0.y + (track_h - ImGui::GetTextLineHeight()) * 0.5f;
    dl->AddText(ImVec2(pos.x, vy), Colors::TextBind, "-");
 
    ImVec2 text_size = ImGui::CalcTextSize(full);
    float text_x = tp0.x + (track_w * anim_t) - (text_size.x * 0.5f);
    text_x = ImClamp(text_x, tp0.x, tp0.x + track_w - text_size.x);
    float text_y = vy + 2.0f * scale;

    dl->AddText(ImVec2(text_x - 1 * scale, text_y), IM_COL32(0, 0, 0, 180), full);
    dl->AddText(ImVec2(text_x + 1 * scale, text_y), IM_COL32(0, 0, 0, 180), full);
    dl->AddText(ImVec2(text_x, text_y - 1 * scale), IM_COL32(0, 0, 0, 180), full);
    dl->AddText(ImVec2(text_x, text_y + 1 * scale), IM_COL32(0, 0, 0, 180), full);
    dl->AddText(ImVec2(text_x, text_y), Colors::TextBright, full);
 
    dl->AddText(ImVec2(tp0.x + track_w + gap, vy), Colors::TextBind, "+");
 
    std::string minus_id = std::string("##minus") + label;
    std::string plus_id = std::string("##plus") + label;
 
    ImGui::SetCursorScreenPos(ImVec2(pos.x, vy - 3.0f * scale));
    ImGui::InvisibleButton(minus_id.c_str(), ImVec2(minus_w, lbl_h + 6.0f * scale));
    if (ImGui::IsItemClicked() && !UI::IsMouseOverOverlay() && ImGui::GetFrameCount() != UI::_blocked_click_frame) {
        float step = (vmax - vmin) * 0.01f;
        *v = ImClamp(*v - step, vmin, vmax);
        changed = true;
    }
 
    ImGui::SetCursorScreenPos(ImVec2(tp0.x + track_w + gap, vy - 3.0f * scale));
    ImGui::InvisibleButton(plus_id.c_str(), ImVec2(plus_w + 8.0f * scale, lbl_h + 6.0f * scale));
    if (ImGui::IsItemClicked() && !UI::IsMouseOverOverlay() && ImGui::GetFrameCount() != UI::_blocked_click_frame) {
        float step = (vmax - vmin) * 0.01f;
        *v = ImClamp(*v + step, vmin, vmax);
        changed = true;
    }
 
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + total_h + 1.0f * scale));
    return changed;
}

}
