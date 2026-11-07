#pragma once
#include "../imgui/imgui.h"
#include "../imgui/imgui_internal.h"
#include "colors.h"
#include "../menu.h"
#include <string>

namespace UI {

namespace _cp {
    inline ImGuiID open_id = 0;
    inline int     open_frame = -1;
    inline ImVec2  open_pos = {};
    inline ImVec4* editing_color = nullptr;
    inline float   hue = 0.0f;
    inline float   sat = 1.0f;
    inline float   val = 1.0f;
    inline float   alpha = 1.0f;
    inline ImVec2  menu_pos_when_opened = {};
    inline bool    dragging_inside = false;
}

inline bool ColorPicker(const char* label, ImVec4* color, float right_edge, float offset_from_right = 0.0f) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 cursor_pos = ImGui::GetCursorScreenPos();
    float scale = ImGui::GetIO().FontGlobalScale;

    const float picker_w = 18.0f * scale;
    const float picker_h = 12.0f * scale;
    const float spacing = 4.0f * scale;

    float pos_x = right_edge - picker_w - offset_from_right;
    ImVec2 pos = ImVec2(pos_x, cursor_pos.y);

    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton(label, ImVec2(picker_w, picker_h));
    bool hov = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

    ImGuiID my_id = ImGui::GetID(label);

    if (clicked && ImGui::GetFrameCount() != UI::_blocked_click_frame) {
        if (_cp::open_id == my_id) {
            _cp::open_id = 0;
        } else {
            _cp::open_id = my_id;
            _cp::open_frame = ImGui::GetFrameCount();
            _cp::open_pos = ImVec2(pos.x + picker_w * 0.5f, pos.y + picker_h * 0.5f);
            _cp::editing_color = color;
            _cp::menu_pos_when_opened = ImGui::GetWindowPos();

            ImGui::ColorConvertRGBtoHSV(color->x, color->y, color->z, _cp::hue, _cp::sat, _cp::val);
            _cp::alpha = color->w;
        }
    }

    if (_cp::open_id == my_id) {
        _cp::open_pos = ImVec2(pos.x + picker_w * 0.5f, pos.y + picker_h * 0.5f);
    }

    ImVec2 p1 = ImVec2(pos.x + picker_w, pos.y + picker_h);

    dl->AddRectFilled(pos, p1, IM_COL32(16, 16, 16, 255), 2.0f * scale);

    ImU32 col_tl = ImGui::ColorConvertFloat4ToU32(*color);
    ImVec4 darker = ImVec4(color->x * 0.5f, color->y * 0.5f, color->z * 0.5f, 1.0f);
    ImU32 col_br = ImGui::ColorConvertFloat4ToU32(darker);

    dl->AddRectFilledMultiColor(
        ImVec2(pos.x + 1.0f * scale, pos.y + 1.0f * scale),
        ImVec2(p1.x - 1.0f * scale, p1.y - 1.0f * scale),
        col_tl, col_tl,
        col_br, col_br);

    if (hov) {
        dl->AddRect(pos, p1, Colors::Accent, 2.0f * scale, 0, 1.5f * scale);
    }

    ImGui::SetCursorScreenPos(ImVec2(cursor_pos.x, cursor_pos.y));

    return false;
}

inline void RenderOpenColorPicker() {
    if (_cp::open_id == 0) return;
    if (_cp::editing_color == nullptr) {
        _cp::open_id = 0;
        return;
    }

    float scale = ImGui::GetIO().FontGlobalScale;

    const float picker_w = 220.0f * scale;
    const float picker_h = 220.0f * scale;
    const float sv_size = 180.0f * scale;
    const float hue_w = 14.0f * scale;
    const float alpha_h = 14.0f * scale;
    const float gap = 8.0f * scale;

    ImVec2 p0 = _cp::open_pos;
    ImVec2 p1 = ImVec2(p0.x + picker_w, p0.y + picker_h);

    ImVec2 mouse = ImGui::GetIO().MousePos;

    ImGui::SetNextWindowPos(p0);
    ImGui::SetNextWindowSize(ImVec2(picker_w, picker_h));
    
    if (ImGui::GetFrameCount() == _cp::open_frame) {
        ImGui::SetNextWindowFocus();
    }
    
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | 
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | 
                             ImGuiWindowFlags_NoSavedSettings;

    if (ImGui::Begin("##color_picker_popup", nullptr, flags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        
        bool win_hov = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
        bool mouse_clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);

        if (mouse_clicked && (ImGui::GetFrameCount() > _cp::open_frame) && !win_hov) {
            _cp::open_id = 0;
            UI::_blocked_click_frame = ImGui::GetFrameCount();
            ImGui::End();
            ImGui::PopStyleColor(1);
            ImGui::PopStyleVar(3);
            return;
        }

        ImVec4 shadow_v4 = ImGui::ColorConvertU32ToFloat4(IM_COL32(0, 0, 0, 60));
        shadow_v4.w *= s.bg_opacity;
        ImU32 shadow_col = ImGui::ColorConvertFloat4ToU32(shadow_v4);

        ImVec4 bg_v4 = ImGui::ColorConvertU32ToFloat4(Colors::Bg);
        bg_v4.w *= s.bg_opacity;
        ImU32 bg_col = ImGui::ColorConvertFloat4ToU32(bg_v4);

        ImVec4 border_v4 = ImGui::ColorConvertU32ToFloat4(IM_COL32(22, 22, 22, 255));
        border_v4.w *= s.bg_opacity;
        ImU32 border_col = ImGui::ColorConvertFloat4ToU32(border_v4);

        dl->AddRectFilled(ImVec2(p0.x + 2 * scale, p0.y + 2 * scale), ImVec2(p1.x + 2 * scale, p1.y + 2 * scale),
                           shadow_col, 4.0f * scale);
        dl->AddRectFilled(p0, p1, bg_col, 4.0f * scale);
        dl->AddRect(p0, p1, border_col, 4.0f * scale, 0, 1.5f * scale);

        ImVec2 sv_pos = ImVec2(p0.x + 10.0f * scale, p0.y + 10.0f * scale);
        ImVec2 sv_end = ImVec2(sv_pos.x + sv_size, sv_pos.y + sv_size);

        ImVec4 hue_col;
        ImGui::ColorConvertHSVtoRGB(_cp::hue, 1.0f, 1.0f, hue_col.x, hue_col.y, hue_col.z);
        ImU32 hue_u32 = ImGui::ColorConvertFloat4ToU32(ImVec4(hue_col.x, hue_col.y, hue_col.z, 1.0f));

        dl->AddRectFilledMultiColor(sv_pos, sv_end,
            IM_COL32(255, 255, 255, 255), hue_u32,
            hue_u32, IM_COL32(0, 0, 0, 255));

        dl->AddRectFilledMultiColor(sv_pos, sv_end,
            IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
            IM_COL32(0, 0, 0, 255), IM_COL32(0, 0, 0, 255));

        ImGui::SetCursorScreenPos(sv_pos);
        ImGui::InvisibleButton("##sv_box", ImVec2(sv_size, sv_size));
        bool sv_active = ImGui::IsItemActive();
        
        if (sv_active) {
            _cp::sat = ImClamp((mouse.x - sv_pos.x) / sv_size, 0.0f, 1.0f);
            _cp::val = 1.0f - ImClamp((mouse.y - sv_pos.y) / sv_size, 0.0f, 1.0f);

            if (_cp::editing_color != nullptr) {
                ImGui::ColorConvertHSVtoRGB(_cp::hue, _cp::sat, _cp::val,
                    _cp::editing_color->x, _cp::editing_color->y, _cp::editing_color->z);
                _cp::editing_color->w = _cp::alpha;
            }
        }

        float cursor_x = sv_pos.x + _cp::sat * sv_size;
        float cursor_y = sv_pos.y + (1.0f - _cp::val) * sv_size;
        dl->AddCircleFilled(ImVec2(cursor_x, cursor_y), 4.0f * scale, IM_COL32(255, 255, 255, 255), 12);
        dl->AddCircle(ImVec2(cursor_x, cursor_y), 4.0f * scale, IM_COL32(0, 0, 0, 255), 12, 1.5f * scale);

        ImVec2 hue_pos = ImVec2(sv_end.x + gap, sv_pos.y);
        ImVec2 hue_end = ImVec2(hue_pos.x + hue_w, sv_end.y);

        for (int i = 0; i < 6; i++) {
            float y0 = hue_pos.y + (sv_size / 6.0f) * i;
            float y1 = hue_pos.y + (sv_size / 6.0f) * (i + 1);

            ImVec4 c0, c1;
            ImGui::ColorConvertHSVtoRGB(i / 6.0f, 1.0f, 1.0f, c0.x, c0.y, c0.z);
            ImGui::ColorConvertHSVtoRGB((i + 1) / 6.0f, 1.0f, 1.0f, c1.x, c1.y, c1.z);

            ImU32 col0 = ImGui::ColorConvertFloat4ToU32(ImVec4(c0.x, c0.y, c0.z, 1.0f));
            ImU32 col1 = ImGui::ColorConvertFloat4ToU32(ImVec4(c1.x, c1.y, c1.z, 1.0f));

            dl->AddRectFilledMultiColor(
                ImVec2(hue_pos.x, y0), ImVec2(hue_end.x, y1),
                col0, col0, col1, col1);
        }

        ImGui::SetCursorScreenPos(hue_pos);
        ImGui::InvisibleButton("##hue_slider", ImVec2(hue_w, sv_size));
        bool hue_active = ImGui::IsItemActive();

        if (hue_active) {
            _cp::hue = ImClamp((mouse.y - hue_pos.y) / sv_size, 0.0f, 1.0f);

            if (_cp::editing_color != nullptr) {
                ImGui::ColorConvertHSVtoRGB(_cp::hue, _cp::sat, _cp::val,
                    _cp::editing_color->x, _cp::editing_color->y, _cp::editing_color->z);
                _cp::editing_color->w = _cp::alpha;
            }
        }

        float hue_cursor_y = hue_pos.y + _cp::hue * sv_size;
        dl->AddRectFilled(
            ImVec2(hue_pos.x - 2 * scale, hue_cursor_y - 2 * scale),
            ImVec2(hue_end.x + 2 * scale, hue_cursor_y + 2 * scale),
            IM_COL32(255, 255, 255, 255), 1.0f);
        dl->AddRect(
            ImVec2(hue_pos.x - 2 * scale, hue_cursor_y - 2 * scale),
            ImVec2(hue_end.x + 2 * scale, hue_cursor_y + 2 * scale),
            IM_COL32(0, 0, 0, 255), 1.0f, 0, 1.5f * scale);

        ImVec2 alpha_pos = ImVec2(sv_pos.x, sv_end.y + gap);
        ImVec2 alpha_end = ImVec2(sv_end.x, alpha_pos.y + alpha_h);

        ImVec4 current_rgb;
        ImGui::ColorConvertHSVtoRGB(_cp::hue, _cp::sat, _cp::val, current_rgb.x, current_rgb.y, current_rgb.z);
        ImU32 col_opaque = ImGui::ColorConvertFloat4ToU32(ImVec4(current_rgb.x, current_rgb.y, current_rgb.z, 1.0f));
        ImU32 col_transparent = ImGui::ColorConvertFloat4ToU32(ImVec4(current_rgb.x, current_rgb.y, current_rgb.z, 0.0f));

        dl->AddRectFilledMultiColor(alpha_pos, alpha_end,
            col_transparent, col_opaque,
            col_opaque, col_transparent);

        ImGui::SetCursorScreenPos(alpha_pos);
        ImGui::InvisibleButton("##alpha_slider", ImVec2(sv_size, alpha_h));
        bool alpha_active = ImGui::IsItemActive();

        if (alpha_active) {
            _cp::alpha = ImClamp((mouse.x - alpha_pos.x) / sv_size, 0.0f, 1.0f);
            if (_cp::editing_color != nullptr) {
                _cp::editing_color->w = _cp::alpha;
            }
        }

        float alpha_cursor_x = alpha_pos.x + _cp::alpha * sv_size;
        dl->AddRectFilled(
            ImVec2(alpha_cursor_x - 2 * scale, alpha_pos.y - 2 * scale),
            ImVec2(alpha_cursor_x + 2 * scale, alpha_end.y + 2 * scale),
            IM_COL32(255, 255, 255, 255), 1.0f);
        dl->AddRect(
            ImVec2(alpha_cursor_x - 2 * scale, alpha_pos.y - 2 * scale),
            ImVec2(alpha_cursor_x + 2 * scale, alpha_end.y + 2 * scale),
            IM_COL32(0, 0, 0, 255), 1.0f, 0, 1.5f * scale);
        
        ImGui::End();
    }
    ImGui::PopStyleColor(1);
    ImGui::PopStyleVar(3);
}

inline bool IsMouseOverOverlay() {
    ImVec2 mouse = ImGui::GetIO().MousePos;
    float scale = ImGui::GetIO().FontGlobalScale;
    
    if (_dd::open_id != 0) {
        ImVec2 p0 = _dd::open_pos;
        ImVec2 p1 = ImVec2(p0.x + _dd::open_w, p0.y + (float)_dd::count * 18.0f * scale + 4.0f * scale);
        if (mouse.x >= p0.x && mouse.x <= p1.x && mouse.y >= p0.y && mouse.y <= p1.y) {
            return true;
        }
    }
    
    if (_mdd::open_id != 0) {
        ImVec2 p0 = _mdd::open_pos;
        ImVec2 p1 = ImVec2(p0.x + _mdd::open_w, p0.y + (float)_mdd::count * 18.0f * scale + 4.0f * scale);
        if (mouse.x >= p0.x && mouse.x <= p1.x && mouse.y >= p0.y && mouse.y <= p1.y) {
            return true;
        }
    }
    
    if (_cp::open_id != 0) {
        ImVec2 p0 = _cp::open_pos;
        ImVec2 p1 = ImVec2(p0.x + 220.0f * scale, p0.y + 220.0f * scale);
        if (mouse.x >= p0.x && mouse.x <= p1.x && mouse.y >= p0.y && mouse.y <= p1.y) {
            return true;
        }
    }
    
    return false;
}

}
