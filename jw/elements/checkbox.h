#pragma once
#include "../imgui/imgui.h"
#include "../imgui/imgui_internal.h"
#include "colors.h"
#include "bind.h"
#include <string>
#include <unordered_map>
#include <cmath>

namespace UI {

inline bool Checkbox(const char* label, bool* v,
                     const char* bind_id      = nullptr,
                     const char* default_bind = nullptr,
                     float       col_w        = -1.f) {
    ImDrawList* dl    = ImGui::GetWindowDrawList();
    ImVec2      pos   = ImGui::GetCursorScreenPos();
    float       scale = ImGui::GetIO().FontGlobalScale;
    float       width = (col_w > 0.f) ? col_w : ImGui::GetContentRegionAvail().x;

    const float row_h = 20.0f * scale;
    const float cb    = 13.0f * scale;

    if (bind_id) UI::InitBind(bind_id, default_bind);
    if (bind_id) UI::ProcessBindState(bind_id, v);

    float bind_w = 0.f;
    if (bind_id) {
        const char* bd = UI::GetBindDisplay(bind_id);
        bind_w = ImGui::CalcTextSize(bd).x + 6.f * scale;
    }

    float cb_row_w = width - bind_w;
    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton(label, ImVec2(cb_row_w, row_h));
    bool hov     = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    if (clicked && !UI::IsMouseOverOverlay() && ImGui::GetFrameCount() != UI::_blocked_click_frame) *v = !(*v);

    static std::unordered_map<const void*, float> anims;
    float& anim = anims[v];
    anim += ((*v ? 1.0f : 0.0f) - anim) * ImGui::GetIO().DeltaTime * 12.0f;
    if (fabsf(anim - (*v ? 1.0f : 0.0f)) < 0.001f) anim = *v ? 1.0f : 0.0f;

    static std::unordered_map<const void*, float> hover_anims;
    float& hov_anim = hover_anims[v];
    hov_anim += ((hov ? 1.0f : 0.0f) - hov_anim) * ImGui::GetIO().DeltaTime * 12.0f;
    if (fabsf(hov_anim - (hov ? 1.0f : 0.0f)) < 0.001f) hov_anim = hov ? 1.0f : 0.0f;

    ImVec2 cp0 = ImVec2(pos.x,      pos.y + (row_h - cb) * 0.5f);
    ImVec2 cp1 = ImVec2(pos.x + cb, cp0.y + cb);

    dl->AddRectFilled(cp0, cp1, Colors::CbBg, 1.5f * scale);

    ImVec4 acc4  = ImGui::ColorConvertU32ToFloat4(Colors::Accent);
    if (anim > 0.02f) {
        float pad = 0.0f;
        ImVec4 bg  = ImGui::ColorConvertU32ToFloat4(Colors::CbBg);
        ImVec4 acc = ImGui::ColorConvertU32ToFloat4(Colors::Accent);
        ImU32 fill = ImGui::ColorConvertFloat4ToU32(ImLerp(bg, acc, anim));
        dl->AddRectFilled(
            ImVec2(cp0.x + 1.f * scale + pad, cp0.y + 1.f * scale + pad),
            ImVec2(cp1.x - 1.f * scale - pad, cp1.y - 1.f * scale - pad),
            fill, 1.0f * scale);
    }

    ImVec4 border_normal = ImGui::ColorConvertU32ToFloat4(Colors::CbBorder);
    ImVec4 border_hover = ImGui::ColorConvertU32ToFloat4(Colors::CbBorderHov);
    ImVec4 border_active = ImVec4(acc4.x, acc4.y, acc4.z, 0.72f);
    
    ImVec4 target_border = ImLerp(border_normal, border_active, anim);
    ImVec4 final_border = ImLerp(target_border, border_hover, hov_anim);
    ImU32 bord = ImGui::ColorConvertFloat4ToU32(final_border);
    dl->AddRect(cp0, cp1, bord, 1.5f * scale, 0, 1.0f * scale);

    if (anim > 0.02f) {
        ImVec2 grad_top = ImVec2(cp0.x + 1.f * scale, cp0.y + 1.f * scale);
        ImVec2 grad_bot = ImVec2(cp1.x - 1.f * scale, cp0.y + 1.f * scale + (cb - 2.f * scale) * 0.35f);
        dl->AddRectFilledMultiColor(
            grad_top, grad_bot,
            IM_COL32(255, 255, 255, 20), IM_COL32(255, 255, 255, 20),
            IM_COL32(255, 255, 255, 60), IM_COL32(255, 255, 255, 60));
    }

    float  tly = pos.y + (row_h - ImGui::GetTextLineHeight()) * 0.5f;
    ImVec4 text_normal = ImGui::ColorConvertU32ToFloat4(Colors::Text);
    ImVec4 text_hover = ImGui::ColorConvertU32ToFloat4(Colors::TextBright);
    ImU32 text_col = ImGui::ColorConvertFloat4ToU32(ImLerp(text_normal, text_hover, hov_anim));
    dl->AddText(ImVec2(pos.x + cb + 6.f * scale, tly), text_col, label);

    if (bind_id) {
        bool    waiting  = UI::IsBindWaiting(bind_id);
        const char* bd   = UI::GetBindDisplay(bind_id);
        ImVec2  bts      = ImGui::CalcTextSize(bd);
        float   bx       = pos.x + width - bts.x;

        std::string btn_id = std::string("##bnd_") + bind_id;
        ImGui::SetCursorScreenPos(ImVec2(bx - 3.f * scale, pos.y));
        ImGui::InvisibleButton(btn_id.c_str(), ImVec2(bts.x + 6.f * scale, row_h));
        bool bhov     = ImGui::IsItemHovered();
        bool bclicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        bool bright_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);

        if (bclicked && !UI::IsMouseOverOverlay() && ImGui::GetFrameCount() != UI::_blocked_click_frame) {
            if (waiting) UI::g_binds[bind_id].waiting = false;
            else         UI::StartBindWaiting(bind_id);
        }

        std::string popup_id = std::string("##popup_") + bind_id;
        if (bright_clicked) {
            ImGui::OpenPopup(popup_id.c_str());
        }

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.f * scale, 6.f * scale));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(15, 15, 15, 255));
        ImGui::PushStyleColor(ImGuiCol_Border, Colors::SectionBorder);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(32, 32, 32, 255));
        ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(24, 24, 24, 255));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(28, 28, 28, 255));

        if (ImGui::BeginPopup(popup_id.c_str())) {
            const char* modes[] = { "disabled", "hold", "hold off", "toggle", "always on" };
            for (int m = 0; m < 5; m++) {
                bool selected = (UI::g_binds[bind_id].mode == m);
                if (selected) {
                    ImGui::PushStyleColor(ImGuiCol_Text, Colors::Accent);
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, Colors::TextDim);
                }
                
                if (ImGui::Selectable(modes[m], selected)) {
                    UI::g_binds[bind_id].mode = m;
                }
                
                ImGui::PopStyleColor();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(5);
        ImGui::PopStyleVar();

        static std::unordered_map<std::string, float> pulse;
        float& ph = pulse[bind_id];
        if (waiting) ph = fmodf(ph + ImGui::GetIO().DeltaTime * 3.f, 6.2832f);

        ImU32 bc = waiting
            ? ImGui::ColorConvertFloat4ToU32(ImVec4(
                acc4.x, acc4.y, acc4.z,
                0.55f + 0.45f * sinf(ph)))
            : (bhov ? Colors::Text : Colors::TextBind);

        dl->AddText(ImVec2(bx, tly), bc, bd);
    }

    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + row_h + 1.0f * scale));
    return clicked;
}

}
