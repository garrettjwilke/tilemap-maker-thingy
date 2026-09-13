#include "theme.h"

#include "imgui.h"

namespace tmm {

void apply_theme(bool dark, float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowPadding = ImVec2(10, 8);
    s.FramePadding = ImVec2(8, 4);
    s.ItemSpacing = ImVec2(8, 5);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.CellPadding = ImVec2(6, 4);
    s.IndentSpacing = 18.0f;
    s.ScrollbarSize = 16.0f;
    s.GrabMinSize = 14.0f;
    s.FrameRounding = 5.0f;
    s.GrabRounding = 4.0f;
    s.ChildRounding = 6.0f;
    s.PopupRounding = 6.0f;
    s.TabRounding = 5.0f;
    s.WindowRounding = 6.0f;
    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 1.0f;
    s.FrameBorderSize = 1.0f;
    s.PopupBorderSize = 1.0f;
    s.SeparatorTextBorderSize = 1.0f;
    if (dark) {
        ImGui::StyleColorsDark(&s);
        s.Colors[ImGuiCol_WindowBg] = ImVec4(0.11f, 0.12f, 0.14f, 1.0f);
        s.Colors[ImGuiCol_ChildBg] = ImVec4(0.14f, 0.15f, 0.17f, 1.0f);
        s.Colors[ImGuiCol_PopupBg] = ImVec4(0.13f, 0.14f, 0.16f, 1.0f);
        s.Colors[ImGuiCol_Border] = ImVec4(0.32f, 0.34f, 0.38f, 1.0f);
        s.Colors[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.19f, 0.22f, 1.0f);
        s.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.26f, 0.30f, 1.0f);
        s.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.28f, 0.31f, 0.36f, 1.0f);
        s.Colors[ImGuiCol_TitleBg] = ImVec4(0.09f, 0.10f, 0.12f, 1.0f);
        s.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.14f, 0.18f, 1.0f);
        s.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.14f, 0.15f, 0.18f, 1.0f);
        s.Colors[ImGuiCol_Button] = ImVec4(0.26f, 0.32f, 0.42f, 1.0f);
        s.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.34f, 0.42f, 0.55f, 1.0f);
        s.Colors[ImGuiCol_ButtonActive] = ImVec4(0.42f, 0.52f, 0.68f, 1.0f);
        s.Colors[ImGuiCol_Header] = ImVec4(0.24f, 0.32f, 0.44f, 1.0f);
        s.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.32f, 0.42f, 0.56f, 1.0f);
        s.Colors[ImGuiCol_Separator] = ImVec4(0.40f, 0.43f, 0.48f, 1.0f);
        s.Colors[ImGuiCol_Text] = ImVec4(0.93f, 0.94f, 0.96f, 1.0f);
        s.Colors[ImGuiCol_CheckMark] = ImVec4(0.55f, 0.82f, 1.0f, 1.0f);
        s.Colors[ImGuiCol_SliderGrab] = ImVec4(0.45f, 0.70f, 0.95f, 1.0f);
        s.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.60f, 0.82f, 1.0f, 1.0f);
    } else {
        ImGui::StyleColorsLight(&s);
        s.Colors[ImGuiCol_WindowBg] = ImVec4(0.94f, 0.95f, 0.97f, 1.0f);
        s.Colors[ImGuiCol_ChildBg] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        s.Colors[ImGuiCol_PopupBg] = ImVec4(0.98f, 0.98f, 0.99f, 1.0f);
        s.Colors[ImGuiCol_Border] = ImVec4(0.55f, 0.58f, 0.64f, 1.0f);
        s.Colors[ImGuiCol_FrameBg] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        s.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.90f, 0.93f, 0.98f, 1.0f);
        s.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.82f, 0.88f, 0.96f, 1.0f);
        s.Colors[ImGuiCol_TitleBg] = ImVec4(0.82f, 0.85f, 0.90f, 1.0f);
        s.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.72f, 0.78f, 0.88f, 1.0f);
        s.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.88f, 0.90f, 0.93f, 1.0f);
        s.Colors[ImGuiCol_Button] = ImVec4(0.78f, 0.84f, 0.93f, 1.0f);
        s.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.68f, 0.77f, 0.92f, 1.0f);
        s.Colors[ImGuiCol_ButtonActive] = ImVec4(0.55f, 0.68f, 0.88f, 1.0f);
        s.Colors[ImGuiCol_Header] = ImVec4(0.75f, 0.83f, 0.94f, 1.0f);
        s.Colors[ImGuiCol_Separator] = ImVec4(0.58f, 0.62f, 0.68f, 1.0f);
        s.Colors[ImGuiCol_Text] = ImVec4(0.10f, 0.12f, 0.16f, 1.0f);
        s.Colors[ImGuiCol_CheckMark] = ImVec4(0.10f, 0.35f, 0.72f, 1.0f);
        s.Colors[ImGuiCol_SliderGrab] = ImVec4(0.22f, 0.45f, 0.78f, 1.0f);
        s.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.14f, 0.34f, 0.66f, 1.0f);
    }
    if (scale != 1.0f) {
        s.ScaleAllSizes(scale);
    }
}

Rgb background_clear_color(bool dark) {
    if (dark) {
        return Rgb{static_cast<uint8_t>(0.10f * 255), static_cast<uint8_t>(0.11f * 255), static_cast<uint8_t>(0.13f * 255)};
    } else {
        return Rgb{static_cast<uint8_t>(0.90f * 255), static_cast<uint8_t>(0.91f * 255), static_cast<uint8_t>(0.93f * 255)};
    }
}

} // namespace tmm
