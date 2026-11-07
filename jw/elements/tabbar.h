#pragma once
#include "../imgui/imgui.h"
#include "../imgui/imgui_internal.h"
#include "colors.h"
#include "utils.h"
#include <unordered_map>
#include <string>

namespace UI {

inline int TabBar(const char** tabs, int count, int current,
                  ImVec2 pos, float width, float height) {
    ImDrawList* fdl   = ImGui::GetForegroundDrawList();
    ImVec2      mouse = ImGui::GetIO().MousePos;
    int         result = current;

    fdl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), Colors::TabBg);
    UI::DrawGradientLine(fdl, pos.x, pos.y, width, 1.0f);

    float tab_w = width / (float)count;

    static std::unordered_map<std::string, float> hover_anims;

    for (int i = 0; i < count; i++) {
        ImVec2 tp0 = ImVec2(pos.x + (float)i * tab_w, pos.y);
        ImVec2 tp1 = ImVec2(tp0.x + tab_w, pos.y + height);

        bool hov    = (mouse.x >= tp0.x && mouse.x <= tp1.x &&
                       mouse.y >= tp0.y && mouse.y <= tp1.y);
        bool active = (i == current);

        if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) result = i;

        float& ha = hover_anims[tabs[i]];
        ha += ((hov ? 1.0f : 0.0f) - ha) * ImGui::GetIO().DeltaTime * 12.0f;

        ImVec2 ts  = ImGui::CalcTextSize(tabs[i]);
        ImVec2 ttp = ImVec2(tp0.x + (tab_w - ts.x) * 0.5f,
                            tp0.y + (height  - ts.y) * 0.5f);

        ImU32 tc;
        if (active) {
            tc = Colors::TextBright;
        } else {

            ImVec4 dim = ImGui::ColorConvertU32ToFloat4(Colors::TextDim);
            ImVec4 lit = ImGui::ColorConvertU32ToFloat4(Colors::Text);
            tc = ImGui::ColorConvertFloat4ToU32(ImLerp(dim, lit, ha));
        }

        fdl->AddText(ttp, tc, tabs[i]);

        if (i < count - 1) {
            float sx = tp1.x;
            float sy0 = pos.y + height * 0.25f;
            float sy1 = pos.y + height * 0.75f;
            fdl->AddLine(ImVec2(sx, sy0), ImVec2(sx, sy1),
                         IM_COL32(32, 30, 38, 255), 1.0f);
        }
    }

    return result;
}

}
