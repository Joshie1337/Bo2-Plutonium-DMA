#pragma once
#include "../imgui/imgui.h"
#include "../imgui/imgui_internal.h"
#include "colors.h"
#include <string>

namespace UI {

inline int SubTabBar(const char** labels, int count, int active,
                     const ImVec2& pos, float start_x) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 mouse = ImGui::GetIO().MousePos;

    const float lh = ImGui::GetTextLineHeight();
    const float gap = 20.0f;

    float cx = pos.x + start_x;

    int new_active = active;

    for (int i = 0; i < count; i++) {
        ImVec2 ts = ImGui::CalcTextSize(labels[i]);
        ImVec2 tp = ImVec2(cx, pos.y);

        ImVec2 btn_min = ImVec2(tp.x, tp.y - 4.0f);
        ImVec2 btn_max = ImVec2(tp.x + ts.x, tp.y + lh + 4.0f);

        bool hov = (mouse.x >= btn_min.x && mouse.x <= btn_max.x &&
                    mouse.y >= btn_min.y && mouse.y <= btn_max.y);
        bool clicked = hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left);

        if (clicked) {
            new_active = i;
        }

        ImU32 col = (i == active) ? Colors::Accent :
                    (hov ? Colors::Text : Colors::TextDim);

        dl->AddText(tp, col, labels[i]);

        cx += ts.x + gap;
    }

    return new_active;
}

}
