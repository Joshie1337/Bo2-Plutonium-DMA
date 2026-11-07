#pragma once
#include "imgui/imgui.h"
#include <vector>
#include <string>

struct State {
    bool  aimbot          = true;
    bool  backtrack       = false;
    bool  triggerbot      = false;
    bool  magnet          = false;
    bool  penetration     = false;
    bool  override_shared = false;
    bool  target_types[2] = { false, true };
    bool  hitboxes[5]     = { true, false, false, false, false };
    int   hitgroup_sel    = 0;
    int   weapon_sel      = 0;
    float tb_delay        = 0.0f;    // triggerbot reaction delay (ms)
    float fov             = 90.0f;
    bool  draw_fov        = true;
    ImVec4 fov_color      = ImVec4(1.0f, 1.0f, 1.0f, 0.4f);
    float smooth          = 0.0f;
    bool  seed_pred       = false;
    float delay           = 0.0f;
    float hitchance       = 0.0f;
    int   active_tab      = 0;
    int   visuals_subtab  = 0;
    // ── Enemy ESP settings ──
    bool   box             = true;
    ImVec4 box_color       = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    bool   health          = true;
    ImVec4 health_color    = ImVec4(1.0f, 0.4f, 0.7f, 1.0f);
    ImVec4 health_color2   = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    bool   name            = true;
    ImVec4 name_color      = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    bool   weapon          = true;
    ImVec4 weapon_color    = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    bool   ammo            = false;
    ImVec4 ammo_color      = ImVec4(1.0f, 0.4f, 0.7f, 1.0f);
    ImVec4 ammo_color2     = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    bool   out_of_view     = false;
    ImVec4 oov_color       = ImVec4(0.3f, 0.5f, 1.0f, 1.0f);
    bool   skeleton        = false;
    ImVec4 skeleton_color  = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    bool   flags_esp       = false;
    ImVec4 flags_color1    = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    ImVec4 flags_color2    = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    ImVec4 flags_color3    = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

    bool   history         = false;
    bool   glow            = false;
    ImVec4 glow_color1     = ImVec4(0.85f, 0.1f, 0.1f, 1.0f);
    ImVec4 glow_color2     = ImVec4(0.85f, 0.1f, 0.1f, 1.0f);

    // ── Friendly ESP settings (separate from enemy) ──
    bool   f_box             = false;
    ImVec4 f_box_color       = ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
    bool   f_health          = false;
    ImVec4 f_health_color    = ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
    bool   f_name            = false;
    ImVec4 f_name_color      = ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
    bool   f_weapon          = false;
    ImVec4 f_weapon_color    = ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
    bool   f_ammo            = false;
    ImVec4 f_ammo_color      = ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
    bool   f_skeleton        = false;
    ImVec4 f_skeleton_color  = ImVec4(0.2f, 0.8f, 0.2f, 1.0f);

    bool   edge_jump        = false;
    bool   ladder_edge_jump = false;
    bool   long_jump        = false;
    bool   long_jump_edge   = true;
    bool   mini_jump        = false;
    bool   null_strafe      = true;
    bool   mousespeed_lim   = false;

    bool   vsat              = false;

    // ── Write tab features (memory writes — risky) ──
    bool   w_fov_changer     = false;
    float  w_fov_value       = 90.0f;
    bool   w_hide_gun        = false;
    bool   w_show_fps        = false;
    bool   w_third_person    = false;
    bool   w_tab_accepted    = false;   // user accepted the write-tab warning

    bool   chat_logs        = true;
    int    chat_logs_type   = 0;
    bool   healthshot       = false;
    bool   sounds           = true;
    int    sounds_type      = 0;
    float  sounds_volume    = 30.0f;

    float  ind_offset       = 67.0f;
    bool   ind_keybinds     = false;
    bool   ind_spectators   = false;
    ImVec4 keybinds_col1    = ImVec4(0.2f, 0.85f, 0.2f, 1.0f);
    ImVec4 keybinds_col2    = ImVec4(1.0f, 1.0f,  1.0f, 1.0f);
    bool   vel_text         = false;
    ImVec4 vel_text_col1    = ImVec4(0.9f, 0.35f, 0.4f, 1.0f);
    ImVec4 vel_text_col2    = ImVec4(1.0f, 0.7f,  0.2f, 1.0f);
    ImVec4 vel_text_col3    = ImVec4(0.2f, 0.85f, 0.2f, 1.0f);
    bool   keystrokes       = false;
    ImVec4 keystrokes_col   = ImVec4(1.0f, 1.0f,  1.0f, 1.0f);
    bool   vel_graph        = false;
    ImVec4 vel_graph_col1   = ImVec4(1.0f, 1.0f,  1.0f, 1.0f);
    ImVec4 vel_graph_col2   = ImVec4(1.0f, 1.0f,  1.0f, 1.0f);

    bool   toggle_menu      = true;
    ImVec4 menu_color       = ImVec4(0.85f, 0.1f, 0.1f, 1.0f);
    char   search_query[64] = "";
    bool   search_active    = false;
    bool   settings_open    = false;
    float  menu_scale       = 1.0f;
    int    selected_theme   = 0;
    float  bg_opacity       = 0.95f;
    float  bg_blur          = 0.5f;
    bool   esp_builder      = false;
    int    selected_monitor = 0;
    bool   auto_load_config = true;
    int    navigation_style = 0;

    // MAKCU options ported from BO6
    int   aimbot_curve    = 0;     // 0: Linear, 1: Ease-Out, 2: Cubic
    int   aimbot_filter   = 0;     // 0: Closest (to crosshair), 1: Lowest HP
    float aim_speed       = 0.35f; // exponential fraction per frame (0.05=slow/legit, 0.9=snap)
    float aim_deadzone    = 1.2f;  // pixels from center — don't move inside this radius
    bool  aim_pred        = true;  // velocity prediction (lead moving targets)
    float aim_pred_ms     = 60.0f; // prediction lookahead in milliseconds

    float tb_burst        = 0.0f;  // triggerbot burst duration (ms)
    bool  tb_require_aim  = false; // only triggerbot when aim key held
};

extern State s;

namespace Menu {
    void Render();
    void RenderWidgets();
}
