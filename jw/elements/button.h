#pragma once
#include "../imgui/imgui.h"
#include "../imgui/imgui_internal.h"
#include "colors.h"
#include "../menu.h"

namespace UI {

inline bool Button(const char* label, float w, float h = 18.0f) {
    ImDrawList* dl  = ImGui::GetWindowDrawList();
    ImVec2      pos = ImGui::GetCursorScreenPos();
    ImVec2      p1  = ImVec2(pos.x + w, pos.y + h);

    ImGui::InvisibleButton(label, ImVec2(w, h));
    bool hov     = ImGui::IsItemHovered();
    bool held    = ImGui::IsItemActive();
    bool clicked = ImGui::IsItemClicked();

    ImU32 col_top, col_bot;
    if (held) {
        col_top = IM_COL32(6, 5, 7, (int)(s.bg_opacity * 255.f));
        col_bot = IM_COL32(12, 10, 14, (int)(s.bg_opacity * 255.f));
    } else if (hov) {
        col_top = IM_COL32(30, 27, 35, (int)(s.bg_opacity * 255.f));
        col_bot = IM_COL32(17, 15, 20, (int)(s.bg_opacity * 255.f));
    } else {
        col_top = IM_COL32(22, 20, 26, (int)(s.bg_opacity * 255.f));
        col_bot = IM_COL32(10, 9, 11, (int)(s.bg_opacity * 255.f));
    }

    dl->AddRectFilled(pos, p1, col_top, 2.0f);
    dl->AddRectFilledMultiColor(pos, p1, col_top, col_top, col_bot, col_bot);

    if (hov) {

        dl->AddRect(pos, p1, IM_COL32(42, 38, 50, (int)(s.bg_opacity * 255.f)), 2.0f, 0, 1.0f);

        ImVec4 ac = ImGui::ColorConvertU32ToFloat4(Colors::Accent);
        ImU32  ac2 = ImGui::ColorConvertFloat4ToU32(ImVec4(ac.x, ac.y, ac.z, 0.6f));
        dl->AddLine(ImVec2(pos.x + 1, pos.y + 3), ImVec2(pos.x + 1, p1.y - 3), ac2, 1.5f);
    }

    ImVec2 ts = ImGui::CalcTextSize(label);
    float  tx = pos.x + (w - ts.x) * 0.5f;
    float  ty = pos.y + (h - ts.y) * 0.5f;
    dl->AddText(ImVec2(tx, ty), hov ? Colors::TextBright : Colors::Text, label);

    return clicked;
}

}
