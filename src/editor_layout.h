#pragma once

#include "imgui.h"

// The renderer currently presents directly to the swapchain. Keep the center
// clear for the scene while placing editor controls in predictable side rails.
struct EditorLayout {
    float width;
    float height;
    float toolbar = 52.f;
    float status = 32.f;
    float left;
    float right;

    static EditorLayout current() {
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        const float usable = size.x > 0.f ? size.x : 1280.f;
        EditorLayout layout{};
        layout.width = usable;
        layout.height = size.y > 0.f ? size.y : 720.f;
        layout.left = usable < 1100.f ? 250.f : 300.f;
        layout.right = usable < 1100.f ? 310.f : 380.f;
        return layout;
    }

    float panel_height() const { return height - toolbar - status; }
    float viewport_left() const { return left; }
    float viewport_width() const { return width - left - right; }
};

constexpr ImGuiWindowFlags EditorPanelFlags =
    ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoResize |
    ImGuiWindowFlags_NoCollapse |
    ImGuiWindowFlags_NoSavedSettings;
