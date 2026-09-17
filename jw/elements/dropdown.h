#pragma once
#include "../imgui/imgui.h"
#include "../imgui/imgui_internal.h"
#include "colors.h"
#include "../menu.h"
#include <string>
#include <cmath>
#include <unordered_map>

namespace UI {

namespace _dd {
    inline ImGuiID      open_id    = 0;
    inline int          open_frame = -1;
    inline ImVec2       open_pos   = {};
    inline float        open_w     = 0.f;
    inline const char** items      = nullptr;
    inline int          count      = 0;
    inline int*         selected   = nullptr;
    inline ImGuiID      anim_id    = 0;
    inline float        anim_factor = 0.0f;
}

inline bool Dropdown(const char* label, int* sel, const char** items, int count, float col_w = -1.f) {
    ImDrawList* dl  = ImGui::GetWindowDrawList();
    ImVec2      pos = ImGui::GetCursorScreenPos();
    float       scale = ImGui::GetIO().FontGlobalScale;

    const float h = 20.0f * scale;
    const float w = (col_w > 0.f) ? col_w : ImGui::GetContentRegionAvail().x;

    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton(label, ImVec2(w, h));
    bool hov     = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

    ImGuiID my_id = ImGui::GetID(label);

    if (clicked && !UI::IsMouseOverOverlay() && ImGui::GetFrameCount() != UI::_blocked_click_frame) {
        if (_dd::open_id == my_id) {
            _dd::open_id = 0;
        } else {
            _dd::open_id    = my_id;
            _dd::anim_id    = my_id;
            _dd::open_frame = ImGui::GetFrameCount();
            _dd::open_pos   = ImVec2(pos.x, pos.y + h);
            _dd::open_w     = w;
            _dd::items      = items;
            _dd::count      = count;
            _dd::selected   = sel;
        }
    }

    bool is_open = (_dd::open_id == my_id);
    if (is_open) {
        _dd::open_pos = ImVec2(pos.x, pos.y + h);
        _dd::open_w   = w;
    }

    // Hover animation
    static std::unordered_map<const void*, float> hover_anims;
    float& hov_anim = hover_anims[sel];
    hov_anim += ((hov ? 1.0f : 0.0f) - hov_anim) * ImGui::GetIO().DeltaTime * 12.0f;
    if (fabsf(hov_anim - (hov ? 1.0f : 0.0f)) < 0.001f) hov_anim = hov ? 1.0f : 0.0f;

    // Open/rotation animation
    static std::unordered_map<const void*, float> open_anims;
    float& open_anim = open_anims[sel];
    open_anim += ((is_open ? 1.0f : 0.0f) - open_anim) * ImGui::GetIO().DeltaTime * 14.0f;
    if (fabsf(open_anim - (is_open ? 1.0f : 0.0f)) < 0.001f) open_anim = is_open ? 1.0f : 0.0f;

    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), Colors::Bg, 2.0f * scale);

    ImVec2 grad_top = pos;
    ImVec2 grad_bot = ImVec2(pos.x + w, pos.y + h);
    dl->AddRectFilledMultiColor(
        grad_top, grad_bot,
        IM_COL32(20, 20, 20, 255), IM_COL32(20, 20, 20, 255),
        IM_COL32(9, 9, 9, 255), IM_COL32(9, 9, 9, 255));

    const char* cur = (*sel >= 0 && *sel < count) ? items[*sel] : "";
    float ty = pos.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
    
    // Smoothly color dropdown selection text on hover/active
    ImVec4 text_normal = ImGui::ColorConvertU32ToFloat4(Colors::Text);
    ImVec4 text_hover = ImGui::ColorConvertU32ToFloat4(Colors::TextBright);
    ImU32 text_col = ImGui::ColorConvertFloat4ToU32(ImLerp(text_normal, text_hover, hov_anim));
    dl->AddText(ImVec2(pos.x + 7.0f * scale, ty), text_col, cur);

    float ax = pos.x + w - 16.0f * scale;
    float ay = pos.y + h * 0.5f;
    
    // Rotating Arrow Triangle calculation
    float angle = open_anim * 3.14159265f;
    float cx = ax + 3.0f * scale;
    float cy = ay;
    
    auto rotate_pt = [&](float px, float py) {
        float dx = px - cx;
        float dy = py - cy;
        float rx = dx * cosf(angle) - dy * sinf(angle);
        float ry = dx * sinf(angle) + dy * cosf(angle);
        return ImVec2(cx + rx, cy + ry);
    };
    
    ImVec2 p0 = rotate_pt(ax,        ay - 2.5f * scale);
    ImVec2 p1 = rotate_pt(ax + 6.0f * scale, ay - 2.5f * scale);
    ImVec2 p2 = rotate_pt(ax + 3.0f * scale, ay + 2.5f * scale);
    
    ImVec4 arrow_normal = ImGui::ColorConvertU32ToFloat4(Colors::TextBind);
    ImVec4 arrow_active = ImGui::ColorConvertU32ToFloat4(Colors::Accent);
    ImU32 arrow_color = ImGui::ColorConvertFloat4ToU32(ImLerp(arrow_normal, arrow_active, open_anim));
    
    dl->AddTriangleFilled(p0, p1, p2, arrow_color);

    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + h + 1.0f * scale));
    return false;
}

inline void RenderOpenDropdown() {
    float scale = ImGui::GetIO().FontGlobalScale;
    float target = (_dd::open_id == _dd::anim_id && _dd::open_id != 0) ? 1.0f : 0.0f;
    _dd::anim_factor += (target - _dd::anim_factor) * ImGui::GetIO().DeltaTime * 14.0f;
    if (fabsf(_dd::anim_factor - target) < 0.001f) _dd::anim_factor = target;
    
    if (_dd::anim_factor < 0.005f) {
        _dd::anim_id = 0;
        return;
    }

    ImDrawList* fdl    = ImGui::GetForegroundDrawList();
    const float item_h = 18.0f * scale;
    float       total_h = ((float)_dd::count * item_h + 4.0f * scale) * _dd::anim_factor;
    ImVec2      p0     = _dd::open_pos;
    ImVec2      p1     = ImVec2(p0.x + _dd::open_w, p0.y + total_h);
    ImVec2      mouse  = ImGui::GetIO().MousePos;

    // Apply alpha fade to all colors
    ImU32 shadow_col = IM_COL32(0, 0, 0, (int)(60.0f * _dd::anim_factor * s.bg_opacity));
    ImU32 bg_col = Colors::Bg;
    ImVec4 bg_v4 = ImGui::ColorConvertU32ToFloat4(bg_col);
    bg_v4.w *= _dd::anim_factor * s.bg_opacity;
    bg_col = ImGui::ColorConvertFloat4ToU32(bg_v4);

    fdl->AddRectFilled(ImVec2(p0.x + 2 * scale, p0.y + 2 * scale), ImVec2(p1.x + 2 * scale, p1.y + 2 * scale), shadow_col, 3.0f * scale);
    fdl->AddRectFilled(p0, p1, bg_col, 3.0f * scale);

    fdl->PushClipRect(p0, p1, true);

    int  click_sel  = -1;
    bool click_any  = ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                   && (ImGui::GetFrameCount() > _dd::open_frame)
                   && (_dd::open_id != 0);

    for (int i = 0; i < _dd::count; i++) {
        ImVec2 ip0 = ImVec2(p0.x + 1.0f * scale, p0.y + 2.0f * scale + (float)i * item_h);
        ImVec2 ip1 = ImVec2(p1.x - 1.0f * scale, ip0.y + item_h);

        bool hov_i = (mouse.x >= ip0.x && mouse.x <= ip1.x &&
                      mouse.y >= ip0.y && mouse.y <= ip1.y);
        bool sel_i = (*_dd::selected == i);

        if (hov_i) {
            ImU32 hov_col = IM_COL32(35, 32, 44, (int)(255.0f * _dd::anim_factor));
            fdl->AddRectFilled(ip0, ip1, hov_col, 2.0f * scale);
        }

        float ty = ip0.y + (item_h - ImGui::GetTextLineHeight()) * 0.5f;
        
        ImVec4 text_col_v4 = ImGui::ColorConvertU32ToFloat4(sel_i ? Colors::Accent : (hov_i ? Colors::TextBright : Colors::Text));
        text_col_v4.w *= _dd::anim_factor;
        ImU32 tc = ImGui::ColorConvertFloat4ToU32(text_col_v4);
        
        fdl->AddText(ImVec2(ip0.x + 7.0f * scale, ty), tc, _dd::items[i]);

        if (hov_i && click_any) click_sel = i;
    }

    fdl->PopClipRect();

    if (click_sel >= 0) {
        *_dd::selected = click_sel;
        _dd::open_id   = 0;
        UI::_blocked_click_frame = ImGui::GetFrameCount();
        return;
    }

    if (click_any) {
        bool inside = (mouse.x >= p0.x && mouse.x <= p1.x &&
                       mouse.y >= p0.y && mouse.y <= p0.y + ((float)_dd::count * item_h + 4.0f * scale));
        if (!inside) {
            _dd::open_id = 0;
            UI::_blocked_click_frame = ImGui::GetFrameCount();
        }
    }
}

namespace _mdd {
    inline ImGuiID      open_id    = 0;
    inline int          open_frame = -1;
    inline ImVec2       open_pos   = {};
    inline float        open_w     = 0.f;
    inline const char** items      = nullptr;
    inline int          count      = 0;
    inline bool*        selected   = nullptr;
    inline ImGuiID      anim_id    = 0;
    inline float        anim_factor = 0.0f;
}

inline bool MultiDropdown(const char* label, bool* selected, const char** items, int count, float col_w = -1.f) {
    ImDrawList* dl  = ImGui::GetWindowDrawList();
    ImVec2      pos = ImGui::GetCursorScreenPos();
    float       scale = ImGui::GetIO().FontGlobalScale;

    const float h = 20.0f * scale;
    const float w = (col_w > 0.f) ? col_w : ImGui::GetContentRegionAvail().x;

    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton(label, ImVec2(w, h));
    bool hov     = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

    ImGuiID my_id = ImGui::GetID(label);

    if (clicked && !UI::IsMouseOverOverlay() && ImGui::GetFrameCount() != UI::_blocked_click_frame) {
        if (_mdd::open_id == my_id) {
            _mdd::open_id = 0;
        } else {
            _dd::open_id = 0; // Close single dropdown
            _mdd::open_id    = my_id;
            _mdd::anim_id    = my_id;
            _mdd::open_frame = ImGui::GetFrameCount();
            _mdd::open_pos   = ImVec2(pos.x, pos.y + h);
            _mdd::open_w     = w;
            _mdd::items      = items;
            _mdd::count      = count;
            _mdd::selected   = selected;
        }
    }

    bool is_open = (_mdd::open_id == my_id);
    if (is_open) {
        _mdd::open_pos = ImVec2(pos.x, pos.y + h);
        _mdd::open_w   = w;
    }

    // Hover animation
    static std::unordered_map<const void*, float> hover_anims;
    float& hov_anim = hover_anims[selected];
    hov_anim += ((hov ? 1.0f : 0.0f) - hov_anim) * ImGui::GetIO().DeltaTime * 12.0f;
    if (fabsf(hov_anim - (hov ? 1.0f : 0.0f)) < 0.001f) hov_anim = hov ? 1.0f : 0.0f;

    // Open/rotation animation
    static std::unordered_map<const void*, float> open_anims;
    float& open_anim = open_anims[selected];
    open_anim += ((is_open ? 1.0f : 0.0f) - open_anim) * ImGui::GetIO().DeltaTime * 14.0f;
    if (fabsf(open_anim - (is_open ? 1.0f : 0.0f)) < 0.001f) open_anim = is_open ? 1.0f : 0.0f;

    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), Colors::Bg, 2.0f * scale);

    ImVec2 grad_top = pos;
    ImVec2 grad_bot = ImVec2(pos.x + w, pos.y + h);
    dl->AddRectFilledMultiColor(
        grad_top, grad_bot,
        IM_COL32(20, 20, 20, 255), IM_COL32(20, 20, 20, 255),
        IM_COL32(9, 9, 9, 255), IM_COL32(9, 9, 9, 255));

    std::string preview = "";
    int sel_count = 0;
    for (int i = 0; i < count; i++) {
        if (selected[i]) {
            if (!preview.empty()) preview += ", ";
            preview += items[i];
            sel_count++;
        }
    }
    if (preview.empty()) {
        preview = "none";
    } else if (preview.length() > 14) {
        size_t comma = preview.find_last_of(',', 13);
        if (comma != std::string::npos) {
            preview = preview.substr(0, comma) + "...";
        } else {
            preview = preview.substr(0, 11) + "...";
        }
    }

    float ty = pos.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
    
    // Smoothly color multi-dropdown text on hover/active
    ImVec4 text_normal = ImGui::ColorConvertU32ToFloat4(Colors::Text);
    ImVec4 text_hover = ImGui::ColorConvertU32ToFloat4(Colors::TextBright);
    ImU32 text_col = ImGui::ColorConvertFloat4ToU32(ImLerp(text_normal, text_hover, hov_anim));
    dl->AddText(ImVec2(pos.x + 7.0f * scale, ty), text_col, preview.c_str());

    float ax = pos.x + w - 16.0f * scale;
    float ay = pos.y + h * 0.5f;

    // Rotating Arrow Triangle calculation
    float angle = open_anim * 3.14159265f;
    float cx = ax + 3.0f * scale;
    float cy = ay;
    
    auto rotate_pt = [&](float px, float py) {
        float dx = px - cx;
        float dy = py - cy;
        float rx = dx * cosf(angle) - dy * sinf(angle);
        float ry = dx * sinf(angle) + dy * cosf(angle);
        return ImVec2(cx + rx, cy + ry);
    };

    ImVec2 p0 = rotate_pt(ax,        ay - 2.5f * scale);
    ImVec2 p1 = rotate_pt(ax + 6.0f * scale, ay - 2.5f * scale);
    ImVec2 p2 = rotate_pt(ax + 3.0f * scale, ay + 2.5f * scale);
    
    ImVec4 arrow_normal = ImGui::ColorConvertU32ToFloat4(Colors::TextBind);
    ImVec4 arrow_active = ImGui::ColorConvertU32ToFloat4(Colors::Accent);
    ImU32 arrow_color = ImGui::ColorConvertFloat4ToU32(ImLerp(arrow_normal, arrow_active, open_anim));
    
    dl->AddTriangleFilled(p0, p1, p2, arrow_color);

    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + h + 1.0f * scale));
    return false;
}

inline void RenderOpenMultiDropdown() {
    float scale = ImGui::GetIO().FontGlobalScale;
    float target = (_mdd::open_id == _mdd::anim_id && _mdd::open_id != 0) ? 1.0f : 0.0f;
    _mdd::anim_factor += (target - _mdd::anim_factor) * ImGui::GetIO().DeltaTime * 14.0f;
    if (fabsf(_mdd::anim_factor - target) < 0.001f) _mdd::anim_factor = target;
    
    if (_mdd::anim_factor < 0.005f) {
        _mdd::anim_id = 0;
        return;
    }

    ImDrawList* fdl    = ImGui::GetForegroundDrawList();
    const float item_h = 18.0f * scale;
    float       total_h = ((float)_mdd::count * item_h + 4.0f * scale) * _mdd::anim_factor;
    ImVec2      p0     = _mdd::open_pos;
    ImVec2      p1     = ImVec2(p0.x + _mdd::open_w, p0.y + total_h);
    ImVec2      mouse  = ImGui::GetIO().MousePos;

    // Apply alpha fade to all colors
    ImU32 shadow_col = IM_COL32(0, 0, 0, (int)(60.0f * _mdd::anim_factor * s.bg_opacity));
    ImU32 bg_col = Colors::Bg;
    ImVec4 bg_v4 = ImGui::ColorConvertU32ToFloat4(bg_col);
    bg_v4.w *= _mdd::anim_factor * s.bg_opacity;
    bg_col = ImGui::ColorConvertFloat4ToU32(bg_v4);

    fdl->AddRectFilled(ImVec2(p0.x + 2 * scale, p0.y + 2 * scale), ImVec2(p1.x + 2 * scale, p1.y + 2 * scale), shadow_col, 3.0f * scale);
    fdl->AddRectFilled(p0, p1, bg_col, 3.0f * scale);

    fdl->PushClipRect(p0, p1, true);

    bool click_any  = ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                   && (ImGui::GetFrameCount() > _mdd::open_frame)
                   && (_mdd::open_id != 0);

    bool clicked_inside = false;
    for (int i = 0; i < _mdd::count; i++) {
        ImVec2 ip0 = ImVec2(p0.x + 1.0f * scale, p0.y + 2.0f * scale + (float)i * item_h);
        ImVec2 ip1 = ImVec2(p1.x - 1.0f * scale, ip0.y + item_h);

        bool hov_i = (mouse.x >= ip0.x && mouse.x <= ip1.x &&
                      mouse.y >= ip0.y && mouse.y <= ip1.y);
        bool sel_i = _mdd::selected[i];

        if (hov_i) {
            ImU32 hov_col = IM_COL32(35, 32, 44, (int)(255.0f * _mdd::anim_factor));
            fdl->AddRectFilled(ip0, ip1, hov_col, 2.0f * scale);
        }

        float ty = ip0.y + (item_h - ImGui::GetTextLineHeight()) * 0.5f;
        
        ImVec4 text_col_v4 = ImGui::ColorConvertU32ToFloat4(sel_i ? Colors::Accent : (hov_i ? Colors::TextBright : Colors::Text));
        text_col_v4.w *= _mdd::anim_factor;
        ImU32 tc = ImGui::ColorConvertFloat4ToU32(text_col_v4);
        
        fdl->AddText(ImVec2(ip0.x + 7.0f * scale, ty), tc, _mdd::items[i]);

        if (hov_i && click_any) {
            _mdd::selected[i] = !_mdd::selected[i];
            clicked_inside = true;
        }
    }

    fdl->PopClipRect();

    if (click_any && !clicked_inside) {
        bool inside = (mouse.x >= p0.x && mouse.x <= p1.x &&
                       mouse.y >= p0.y && mouse.y <= p0.y + ((float)_mdd::count * item_h + 4.0f * scale));
        if (!inside) {
            _mdd::open_id = 0;
            UI::_blocked_click_frame = ImGui::GetFrameCount();
        }
    }
}

}
