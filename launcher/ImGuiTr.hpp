// Client.cpp's ImGui, translated (Idioma.hpp): included after imgui.h, it makes every ImGui:: in that file
// RangaUi::. RangaUi has the functions that show text, each translating its label, text or format string first, and
// everything else is ImGui's own (the using directive: a name RangaUi declares hides ImGui's, any other is found there).
//
// Labels keep the Portuguese id (Idioma::Label), so the same button has the same id in every language. Popups are not
// translated: their name is also their id, opened elsewhere. A "%s" text translates its argument (text the code built).
#pragma once

#include "imgui.h"
#include "Idioma.hpp"

#include <cstdarg>
#include <cstring>
#include <vector>

namespace RangaUi {
using namespace ::ImGui;
using Idioma::Label;
using Idioma::Tr;

namespace detail {
// "%s" alone: the argument is the text; anything else: the format is.
template <typename F> void Formatted(F show, const char* fmt, va_list args) {
    if (std::strcmp(fmt, "%s") == 0) {
        const char* text = va_arg(args, const char*);
        show("%s", Tr(text));
        return;
    }
    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), Tr(fmt), args);
    // What went into the format is translated too (parts and pieces the code put in).
    show("%s", Tr(buffer));
}
}

inline void Text(const char* fmt, ...) IM_FMTARGS(1);
inline void Text(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    detail::Formatted([](const char* f, const char* s) { ::ImGui::Text(f, s); }, fmt, args);
    va_end(args);
}
inline void TextDisabled(const char* fmt, ...) IM_FMTARGS(1);
inline void TextDisabled(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    detail::Formatted([](const char* f, const char* s) { ::ImGui::TextDisabled(f, s); }, fmt, args);
    va_end(args);
}
inline void TextWrapped(const char* fmt, ...) IM_FMTARGS(1);
inline void TextWrapped(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    detail::Formatted([](const char* f, const char* s) { ::ImGui::TextWrapped(f, s); }, fmt, args);
    va_end(args);
}
inline void BulletText(const char* fmt, ...) IM_FMTARGS(1);
inline void BulletText(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    detail::Formatted([](const char* f, const char* s) { ::ImGui::BulletText(f, s); }, fmt, args);
    va_end(args);
}
inline void SetTooltip(const char* fmt, ...) IM_FMTARGS(1);
inline void SetTooltip(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    detail::Formatted([](const char* f, const char* s) { ::ImGui::SetTooltip(f, s); }, fmt, args);
    va_end(args);
}
inline void TextColored(const ImVec4& color, const char* fmt, ...) IM_FMTARGS(2);
inline void TextColored(const ImVec4& color, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    detail::Formatted([&color](const char* f, const char* s) { ::ImGui::TextColored(color, f, s); }, fmt, args);
    va_end(args);
}

inline void TextUnformatted(const char* text, const char* text_end = nullptr) {
    if (text_end == nullptr) ::ImGui::TextUnformatted(Tr(text));
    else {
        const std::string piece = Tr(std::string(text, text_end));
        ::ImGui::TextUnformatted(piece.c_str(), piece.c_str() + piece.size());
    }
}
inline ImVec2 CalcTextSize(const char* text, const char* text_end = nullptr, bool hide_text_after_double_hash = false, float wrap_width = -1.0f) {
    if (text_end != nullptr) {
        const std::string piece = Tr(std::string(text, text_end));
        return ::ImGui::CalcTextSize(piece.c_str(), piece.c_str() + piece.size(), hide_text_after_double_hash, wrap_width);
    }
    return ::ImGui::CalcTextSize(hide_text_after_double_hash ? Label(text) : Tr(text), nullptr, hide_text_after_double_hash, wrap_width);
}

inline bool Button(const char* label, const ImVec2& size = ImVec2(0, 0)) { return ::ImGui::Button(Label(label), size); }
inline bool SmallButton(const char* label) { return ::ImGui::SmallButton(Label(label)); }
inline bool Checkbox(const char* label, bool* v) { return ::ImGui::Checkbox(Label(label), v); }
inline bool RadioButton(const char* label, bool active) { return ::ImGui::RadioButton(Label(label), active); }
inline bool RadioButton(const char* label, int* v, int v_button) { return ::ImGui::RadioButton(Label(label), v, v_button); }
inline bool Selectable(const char* label, bool selected = false, ImGuiSelectableFlags flags = 0, const ImVec2& size = ImVec2(0, 0)) {
    return ::ImGui::Selectable(Label(label), selected, flags, size);
}
inline bool Selectable(const char* label, bool* p_selected, ImGuiSelectableFlags flags = 0, const ImVec2& size = ImVec2(0, 0)) {
    return ::ImGui::Selectable(Label(label), p_selected, flags, size);
}
inline bool BeginTabItem(const char* label, bool* p_open = nullptr, ImGuiTabItemFlags flags = 0) { return ::ImGui::BeginTabItem(Label(label), p_open, flags); }
inline bool BeginMenu(const char* label, bool enabled = true) { return ::ImGui::BeginMenu(Label(label), enabled); }
inline bool MenuItem(const char* label, const char* shortcut = nullptr, bool selected = false, bool enabled = true) {
    return ::ImGui::MenuItem(Label(label), shortcut, selected, enabled);
}
inline bool CollapsingHeader(const char* label, ImGuiTreeNodeFlags flags = 0) { return ::ImGui::CollapsingHeader(Label(label), flags); }
inline void TableSetupColumn(const char* label, ImGuiTableColumnFlags flags = 0, float init_width_or_weight = 0.0f, ImGuiID user_id = 0) {
    ::ImGui::TableSetupColumn(Tr(label), flags, init_width_or_weight, user_id);
}
inline void ProgressBar(float fraction, const ImVec2& size_arg = ImVec2(-FLT_MIN, 0), const char* overlay = nullptr) {
    ::ImGui::ProgressBar(fraction, size_arg, overlay != nullptr ? Tr(overlay) : nullptr);
}
inline bool SliderInt(const char* label, int* v, int v_min, int v_max, const char* format = "%d", ImGuiSliderFlags flags = 0) {
    return ::ImGui::SliderInt(Label(label), v, v_min, v_max, Tr(format), flags);
}
inline bool Combo(const char* label, int* current_item, const char* const items[], int items_count, int popup_max_height_in_items = -1) {
    std::vector<const char*> shown(items, items + items_count);
    for (const char*& item : shown) item = Tr(item);
    return ::ImGui::Combo(Label(label), current_item, shown.data(), items_count, popup_max_height_in_items);
}
inline bool InputText(const char* label, char* buf, size_t buf_size, ImGuiInputTextFlags flags = 0, ImGuiInputTextCallback callback = nullptr,
                      void* user_data = nullptr) {
    return ::ImGui::InputText(Label(label), buf, buf_size, flags, callback, user_data);
}
inline bool InputTextWithHint(const char* label, const char* hint, char* buf, size_t buf_size, ImGuiInputTextFlags flags = 0,
                              ImGuiInputTextCallback callback = nullptr, void* user_data = nullptr) {
    return ::ImGui::InputTextWithHint(Label(label), Tr(hint), buf, buf_size, flags, callback, user_data);
}
inline bool InputTextMultiline(const char* label, char* buf, size_t buf_size, const ImVec2& size = ImVec2(0, 0), ImGuiInputTextFlags flags = 0,
                               ImGuiInputTextCallback callback = nullptr, void* user_data = nullptr) {
    return ::ImGui::InputTextMultiline(Label(label), buf, buf_size, size, flags, callback, user_data);
}

}

#define ImGui RangaUi
