#pragma once
#include "../imgui/imgui.h"
#include "colors.h"

namespace UI {

inline void DrawGradientLine(ImDrawList* dl, float x, float y, float width,
                              float height = 1.0f, ImU32 col = Colors::Accent) {
    ImU32 zero = IM_COL32(0, 0, 0, 0);
    float cx   = x + width * 0.5f;
    dl->AddRectFilledMultiColor(ImVec2(x,  y), ImVec2(cx,         y + height), zero, col,  col,  zero);
    dl->AddRectFilledMultiColor(ImVec2(cx, y), ImVec2(x + width,  y + height), col,  zero, zero, col);
}

bool IsMouseOverOverlay();

inline int _blocked_click_frame = -1;

}
