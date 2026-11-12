#include "app.h"

#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "nlohmann/json.hpp"

namespace cooler {
namespace {

ImVec4 hex(unsigned rgb, float a = 1.0f) {
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

// kitchensink's colours.
struct Palette {
    ImVec4 bg, card, border, text, muted, accent, hover, frame, accentText, warn, good, bad;
};
const Palette kDark = {hex(0x15171C), hex(0x1E2128), hex(0x2B2F38), hex(0xE6E8EC), hex(0x8C929D), hex(0x5AA8FF),
                       hex(0x30353F), hex(0x262A33), hex(0xFFFFFF), hex(0xE3A73B), hex(0x4CC38A), hex(0xE5534B)};
const Palette kLight = {hex(0xF2F3F5), hex(0xFFFFFF), hex(0xD5D9DF), hex(0x1F2328), hex(0x656D76), hex(0x0969DA),
                        hex(0xE4E7EB), hex(0xEEF0F3), hex(0xFFFFFF), hex(0x9A6700), hex(0x1A7F37), hex(0xCF222E)};
const Palette* pal = &kDark;

ImU32 col(const ImVec4& v, float a = 1) { return ImGui::ColorConvertFloat4ToU32(ImVec4(v.x, v.y, v.z, v.w * a)); }

namespace icon {
const char* Refresh = "\xEE\x9C\xAC";   // U+E72C
const char* Minimise = "\xEE\xA4\xA1";  // U+E921
const char* Close = "\xEE\xA2\xBB";     // U+E8BB
}  // namespace icon

const char* kDeg = "\xC2\xB0" "C";

std::string fontPath(const char* file) {
    char dir[MAX_PATH];
    GetWindowsDirectoryA(dir, MAX_PATH);
    return std::string(dir) + "\\Fonts\\" + file;
}

bool exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// asCurve shows a channel's saved setting as a curve; a fixed speed is a
// flat line (that still reaches full speed at 50°C, as every curve here
// does: in practice the liquid never gets there).
Curve asCurve(const Reading& r, size_t ch, bool pump, std::string& note) {
    if (ch < r.modes.size() && r.modes[ch] == kModeCurve && ch < r.curves.size() && r.curves[ch].points.size() >= 2)
        return r.curves[ch];
    int f = ch < r.fixed.size() ? r.fixed[ch] : 100;
    if (ch < r.modes.size() && r.modes[ch] == kModeFixed) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s was fixed at %d%%.", pump ? "The pump" : "The fans", f);
        if (!note.empty()) note += " ";
        note += buf;
    } else {
        if (!note.empty()) note += " ";
        note += pump ? "The pump's saved mode isn't one this app knows." : "The fans' saved mode isn't one this app knows.";
    }
    f = std::clamp(f, pump ? kPumpFloor : kFanFloor, 100);
    if (f >= 100) return Curve{0, {{25, 100}, {50, 100}}};
    return Curve{0, {{25, f}, {49, f}, {50, 100}}};
}

std::string timeText(const SYSTEMTIME& t) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", t.wHour, t.wMinute);
    return buf;
}

}  // namespace

App::App(HWND hwnd, std::function<void()> wake) : hwnd_(hwnd), wake_(std::move(wake)) {
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)))
        dir_ = (std::filesystem::path(p) / "cooler").u8string();
    CoTaskMemFree(p);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(dir_), ec);
    loadConfig();
    const auto& quiet = Presets()[1];
    pump_ = quiet.pump;
    fans_ = quiet.fans;
    startRead();
}

App::~App() {
    if (job_.valid()) job_.wait();  // never leave a save half done
}

void App::loadConfig() {
    std::ifstream in(std::filesystem::u8path(dir_) / "config.json");
    if (!in) return;
    try {
        nlohmann::json j = nlohmann::json::parse(in);
        if (j.contains("window")) {
            auto w = j["window"];
            place_ = RECT{w.value("x", 0), w.value("y", 0), w.value("x", 0) + w.value("w", 0), w.value("y", 0) + w.value("h", 0)};
            placed_ = place_.right > place_.left && place_.bottom > place_.top;
        }
    } catch (...) {
    }
}

void App::saveConfig() {
    nlohmann::json j;
    if (placed_)
        j["window"] = {{"x", place_.left}, {"y", place_.top}, {"w", place_.right - place_.left}, {"h", place_.bottom - place_.top}};
    std::ofstream out(std::filesystem::u8path(dir_) / "config.json");
    out << j.dump(2) << "\n";
}

bool App::Place(RECT& r) const {
    if (!placed_) return false;
    r = place_;
    return true;
}

void App::SavePlace(const RECT& r) {
    placed_ = true;
    place_ = r;
    saveConfig();
}

void App::startRead() {
    if (job_.valid()) return;
    auto wake = wake_;
    job_ = std::async(std::launch::async, [wake] {
        Job j;
        j.reading = Read();
        wake();
        return j;
    });
}

void App::startSave() {
    if (job_.valid()) return;
    Curve pump = pump_, fans = fans_;
    auto wake = wake_;
    message_ = "Saving\xE2\x80\xA6";
    messageBad_ = false;
    job_ = std::async(std::launch::async, [wake, pump, fans] {
        Job j;
        j.save = true;
        j.error = Apply(pump, fans, j.reading);
        wake();
        return j;
    });
}

bool App::edited() const { return !haveSaved_ || !(pump_ == savedPump_ && fans_ == savedFans_); }

void App::took(const Reading& r) {
    const bool wasEdited = haveSaved_ && edited();
    reading_ = r;
    read_ = true;
    GetLocalTime(&readAt_);
    if (!r.ok) return;
    std::string note;
    savedPump_ = r.hasPump ? asCurve(r, kPump, true, note) : Presets()[1].pump;
    // The fans: the first one plugged in stands for them all.
    size_t first = 0;
    for (size_t ch = r.hasPump ? 1 : 0; ch < r.connected.size(); ch++)
        if (r.connected[ch]) {
            first = ch;
            break;
        }
    savedFans_ = first ? asCurve(r, first, false, note) : Presets()[1].fans;
    for (size_t ch = first + 1; ch < r.connected.size(); ch++)
        if (r.connected[ch] && !(ch < r.curves.size() && r.curves[ch] == r.curves[first] && r.modes[ch] == r.modes[first])) {
            note += " The fans had different settings; this shows fan port " + std::to_string(first) + "'s.";
            break;
        }
    savedNote_ = note;
    haveSaved_ = true;
    if (!wasEdited) {
        pump_ = Sanitise(savedPump_, true);
        fans_ = Sanitise(savedFans_, false);
    }
}

void App::Pump() {
    if (job_.valid() && job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        Job j = job_.get();
        if (j.save) {
            if (j.reading.ok) {
                // Take what was saved as the new baseline.
                haveSaved_ = false;
                took(j.reading);
            }
            if (j.error.empty()) {
                message_ = "Saved on the cooler. It keeps to these curves with this app closed, and after restarts.";
                messageBad_ = false;
                rereadAt_ = GetTickCount64() + 12000;
            } else {
                message_ = j.error;
                messageBad_ = true;
            }
        } else {
            took(j.reading);
        }
    }
    if (GetTickCount64() >= icueAt_) {
        icueAt_ = GetTickCount64() + 3000;
        icue_ = IcueRunning();
    }
    if (rereadAt_ && GetTickCount64() >= rereadAt_ && !job_.valid()) {
        rereadAt_ = 0;
        startRead();
    }
}

bool App::InCaption(float x, float y) const {
    if (x >= barMax_.x || y >= barMax_.y) return false;
    for (const auto& [a, b] : hot_)
        if (x >= a.x && y >= a.y && x < b.x && y < b.y) return false;
    return true;
}

void App::SetScale(float scale) {
    scale_ = scale;
    fontsDirty_ = true;
}

void App::SetDark(bool dark) { pal = dark ? &kDark : &kLight; }

void App::ReloadFonts() {
    fontsDirty_ = false;
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    static const ImWchar text[] = {0x0020, 0x024F, 0x2000, 0x206F, 0x2190, 0x21FF, 0x25A0, 0x25FF, 0};
    static const ImWchar icons[] = {0xE700, 0xF8FF, 0};
    std::string regular = fontPath("segoeui.ttf"), semibold = fontPath("seguisb.ttf");
    std::string iconFont = fontPath("SegoeIcons.ttf");
    if (!exists(iconFont)) iconFont = fontPath("segmdl2.ttf");
    if (!exists(semibold)) semibold = regular;
    auto add = [&](const std::string& path, float px, bool withIcons) -> ImFont* {
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        ImFont* f = exists(path) ? io.Fonts->AddFontFromFileTTF(path.c_str(), px * scale_, &cfg, text) : nullptr;
        if (!f) {
            ImFontConfig def;
            def.SizePixels = px * scale_;
            return io.Fonts->AddFontDefault(&def);
        }
        if (withIcons && exists(iconFont)) {
            ImFontConfig merge;
            merge.MergeMode = true;
            merge.GlyphOffset = ImVec2(0, 1.5f * scale_);
            io.Fonts->AddFontFromFileTTF(iconFont.c_str(), px * scale_ * 0.9f, &merge, icons);
        }
        return f;
    };
    body_ = add(regular, 15, true);
    small_ = add(regular, 13, true);
    title_ = add(semibold, 17, false);
    label_ = add(semibold, 12, false);
    big_ = add(semibold, 30, false);
    io.FontDefault = body_;

    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowPadding = ImVec2(0, 0);
    s.FramePadding = ImVec2(10, 5);
    s.ItemSpacing = ImVec2(6, 6);
    s.FrameRounding = 0;
    s.GrabRounding = 0;
    s.PopupRounding = 0;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.PopupBorderSize = 1;
    s.ScrollbarSize = 8;
    s.ScrollbarRounding = 0;
    s.ScaleAllSizes(scale_);
}

bool App::iconButton(const char* glyph, const char* tip, bool danger) {
    const Palette& p = *pal;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, danger ? hex(0xC42B1C) : p.hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, danger ? hex(0xC42B1C) : p.hover);
    ImGui::PushStyleColor(ImGuiCol_Text, p.muted);
    ImGui::PushFont(small_);
    bool pressed = ImGui::Button(glyph, ImVec2(30 * scale_, 26 * scale_));
    ImGui::PopFont();
    ImGui::PopStyleColor(4);
    hot_.push_back({ImGui::GetItemRectMin(), ImGui::GetItemRectMax()});
    if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", tip);
    return pressed;
}

bool App::chip(const char* label, bool on, float minW) {
    const Palette& p = *pal;
    ImGui::PushStyleColor(ImGuiCol_Button, on ? p.accent : p.frame);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? p.accent : p.hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, on ? p.accent : p.hover);
    ImGui::PushStyleColor(ImGuiCol_Text, on ? p.accentText : p.text);
    bool pressed = ImGui::Button(label, ImVec2(std::max(minW, ImGui::CalcTextSize(label).x + 20 * scale_), 0));
    ImGui::PopStyleColor(4);
    return pressed;
}

void App::Frame() {
    hot_.clear();
    const Palette& p = *pal;
    ImGuiIO& io = ImGui::GetIO();
    ImGuiStyle& st = ImGui::GetStyle();
    st.Colors[ImGuiCol_WindowBg] = p.bg;
    st.Colors[ImGuiCol_Text] = p.text;
    st.Colors[ImGuiCol_TextDisabled] = p.muted;
    st.Colors[ImGuiCol_PopupBg] = p.card;
    st.Colors[ImGuiCol_Border] = p.border;
    st.Colors[ImGuiCol_FrameBg] = p.frame;
    st.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_ScrollbarGrab] = p.border;
    st.Colors[ImGuiCol_ScrollbarGrabHovered] = p.muted;
    st.Colors[ImGuiCol_ScrollbarGrabActive] = p.muted;

    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) startRead();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("cooler", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    drawTitle(io.DisplaySize.x);

    const float k = scale_;
    const float pad = 14 * k;
    ImGui::SetCursorPos(ImVec2(0, barMax_.y));
    ImGui::BeginChild("body", ImVec2(io.DisplaySize.x, io.DisplaySize.y - barMax_.y), ImGuiChildFlags_None,
                      dragging_ >= 0 ? ImGuiWindowFlags_NoScrollWithMouse : 0);
    ImGui::SetCursorPos(ImVec2(pad, 4 * k));
    ImGui::BeginGroup();
    const float width = io.DisplaySize.x - 2 * pad - 8 * k;

    drawStatus(width);
    ImGui::Dummy(ImVec2(0, 6 * k));
    if (haveSaved_) {
        drawPresets(width);
        ImGui::Dummy(ImVec2(0, 6 * k));
        const bool side = width >= 760 * k;
        const float gw = side ? (width - 12 * k) / 2 : width;
        const float gh = std::clamp((io.DisplaySize.y - ImGui::GetCursorScreenPos().y - 110 * k) / (side ? 1 : 2), 200 * k, 420 * k);
        drawCurve("pump", "PUMP", pump_, &savedPump_, true, gw, gh);
        if (side) ImGui::SameLine(0, 12 * k);
        drawCurve("fans", "FANS", fans_, &savedFans_, false, gw, gh);

        ImGui::PushFont(small_);
        ImGui::TextColored(p.muted, "Drag a point to move it. Double-click to add one (up to 7), right-click to remove one. "
                                    "Both follow the liquid temperature.");
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 4 * k));

        // Save, undo, and how it went.
        const bool can = edited() && !Busy() && reading_.ok && !icue_;
        ImGui::BeginDisabled(!can);
        if (chip("Save to cooler", can)) startSave();
        ImGui::EndDisabled();
        ImGui::SameLine(0, 6 * k);
        ImGui::BeginDisabled(!edited() || Busy());
        if (chip("Undo changes", false)) {
            pump_ = Sanitise(savedPump_, true);
            fans_ = Sanitise(savedFans_, false);
            message_.clear();
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0, 12 * k);
        ImGui::PushFont(small_);
        ImGui::AlignTextToFramePadding();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - ImGui::GetCursorPosX() + pad);
        if (!message_.empty()) ImGui::TextColored(messageBad_ ? p.bad : p.good, "%s", message_.c_str());
        else if (edited()) ImGui::TextColored(p.warn, "Not saved yet: the cooler is still on its old curves.");
        else ImGui::TextColored(p.muted, "This is what the cooler has saved.");
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
    }
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, 8 * k));
    ImGui::EndChild();
    ImGui::End();
}

void App::drawTitle(float width) {
    const Palette& p = *pal;
    const float k = scale_;
    const float h = 38 * k;
    barMax_ = ImVec2(width, h);
    ImGui::SetCursorPos(ImVec2(14 * k, 11 * k));
    ImGui::PushFont(label_);
    ImGui::TextColored(p.accent, "COOLER");
    ImGui::PopFont();
    ImGui::SameLine(0, 8 * k);
    ImGui::PushFont(small_);
    ImGui::SetCursorPosY(10 * k);
    const char* sub = !Busy() ? "Corsair pump and fan curves, without iCUE"
                     : (message_ == "Saving\xE2\x80\xA6" ? "saving\xE2\x80\xA6" : "reading\xE2\x80\xA6");
    ImGui::TextColored(p.muted, "%s", sub);
    ImGui::PopFont();

    ImGui::SetCursorPos(ImVec2(width - 3 * 30 * k - 6 * k, 6 * k));
    if (iconButton(icon::Refresh, "Read the cooler again (F5). Its lights blink once each time.")) startRead();
    ImGui::SameLine(0, 0);
    if (iconButton(icon::Minimise, "Minimise")) ShowWindow(hwnd_, SW_MINIMIZE);
    ImGui::SameLine(0, 0);
    if (iconButton(icon::Close, "Close", true)) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

void App::drawStatus(float width) {
    const Palette& p = *pal;
    const float k = scale_;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImDrawListSplitter split;
    split.Split(dl, 2);
    split.SetCurrentChannel(dl, 1);
    const ImVec2 top = ImGui::GetCursorScreenPos();
    const float pad = 14 * k;
    ImGui::SetCursorScreenPos(ImVec2(top.x + pad, top.y + 10 * k));
    ImGui::BeginGroup();
    if (!read_) {
        ImGui::TextColored(p.muted, "Reading the cooler\xE2\x80\xA6");
    } else if (!reading_.ok) {
        ImGui::PushTextWrapPos(top.x + width - pad);
        ImGui::TextColored(p.warn, "%s", reading_.error.c_str());
        ImGui::PopTextWrapPos();
        if (chip("Try again", false)) startRead();
    } else {
        const Reading& r = reading_;
        // Liquid temperature, big, then pump and fans.
        ImGui::BeginGroup();
        ImGui::PushFont(label_);
        ImGui::TextColored(p.muted, "LIQUID");
        ImGui::PopFont();
        ImGui::PushFont(big_);
        if (r.liquid) ImGui::Text("%.1f%s", *r.liquid, kDeg);
        else ImGui::TextColored(p.muted, "\xE2\x80\x94");
        ImGui::PopFont();
        ImGui::EndGroup();

        auto stat = [&](const char* label, int rpm, double duty) {
            ImGui::SameLine(0, 34 * k);
            ImGui::BeginGroup();
            ImGui::PushFont(label_);
            ImGui::TextColored(p.muted, "%s", label);
            ImGui::PopFont();
            ImGui::PushFont(title_);
            ImGui::Text("%d rpm", rpm);
            ImGui::PopFont();
            if (duty >= 0) {
                ImGui::PushFont(small_);
                ImGui::TextColored(p.muted, "%.0f%% now", duty);
                ImGui::PopFont();
            }
            ImGui::EndGroup();
        };
        const double t = r.liquid.value_or(-1);
        if (r.hasPump && !r.rpm.empty()) stat("PUMP", r.rpm[0], t > 0 ? DutyAt(savedPump_, t) : -1);
        for (size_t ch = r.hasPump ? 1 : 0; ch < r.rpm.size(); ch++) {
            if (ch >= r.connected.size() || !r.connected[ch]) continue;
            std::string label = "FAN " + std::to_string(ch);
            stat(label.c_str(), r.rpm[ch], t > 0 ? DutyAt(savedFans_, t) : -1);
        }

        ImGui::PushFont(small_);
        std::string facts = r.name + "   \xC2\xB7   firmware " + r.firmware + "   \xC2\xB7   read at " + timeText(readAt_);
        ImGui::TextColored(p.muted, "%s", facts.c_str());
        if (!savedNote_.empty()) {
            ImGui::PushTextWrapPos(top.x + width - pad);
            ImGui::TextColored(p.muted, "%s", savedNote_.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::PopFont();
    }
    if (icue_) {
        ImGui::PushFont(small_);
        ImGui::TextColored(p.warn, "iCUE is running: close it before saving here.");
        ImGui::PopFont();
    }
    ImGui::EndGroup();
    const ImVec2 bottom(top.x + width, ImGui::GetItemRectMax().y + 12 * k);
    split.SetCurrentChannel(dl, 0);
    dl->AddRectFilled(top, bottom, col(p.card));
    split.Merge(dl);
    ImGui::SetCursorScreenPos(ImVec2(top.x, bottom.y));
    ImGui::Dummy(ImVec2(width, 0));
}

void App::drawPresets(float width) {
    const Palette& p = *pal;
    const float k = scale_;
    ImGui::PushFont(label_);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(p.muted, "PRESETS");
    ImGui::PopFont();
    const Preset* hovered = nullptr;
    bool any = false;
    for (const auto& pr : Presets()) {
        ImGui::SameLine(0, pr.name == Presets()[0].name ? 12 * k : 4 * k);
        bool on = pr.pump == pump_ && pr.fans == fans_;
        any |= on;
        if (chip(pr.name, on, 96 * k)) {
            pump_ = pr.pump;
            fans_ = pr.fans;
            message_.clear();
        }
        if (ImGui::IsItemHovered()) hovered = &pr;
    }
    ImGui::SameLine(0, 4 * k);
    ImGui::BeginDisabled();
    chip("Custom", !any, 96 * k);
    ImGui::EndDisabled();
    ImGui::SameLine(0, 12 * k);
    ImGui::PushFont(small_);
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(100.0f, width - ImGui::GetCursorPosX()));
    if (hovered) ImGui::TextColored(p.muted, "%s", hovered->about);
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
}

// drawCurve draws one curve's graph and lets its points be dragged.
void App::drawCurve(const char* id, const char* title, Curve& c, const Curve* saved, bool pump, float w, float h) {
    const Palette& p = *pal;
    const float k = scale_;
    const double xMin = 20, xMax = 60;
    const int floor = pump ? kPumpFloor : kFanFloor;
    ImGui::BeginGroup();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 top = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(top, ImVec2(top.x + w, top.y + h), col(p.card));

    // Title, and the value at the current liquid temperature.
    ImGui::SetCursorScreenPos(ImVec2(top.x + 14 * k, top.y + 10 * k));
    ImGui::PushFont(label_);
    ImGui::TextColored(p.muted, "%s", title);
    ImGui::PopFont();
    const std::optional<double> liquid = reading_.ok ? reading_.liquid : std::nullopt;

    const ImVec2 a(top.x + 44 * k, top.y + 34 * k);       // plot top-left
    const ImVec2 b(top.x + w - 16 * k, top.y + h - 30 * k);  // plot bottom-right
    auto X = [&](double t) { return float(a.x + (t - xMin) / (xMax - xMin) * (b.x - a.x)); };
    auto Y = [&](double d) { return float(b.y - d / 100.0 * (b.y - a.y)); };
    auto T = [&](float x) { return xMin + (x - a.x) / (b.x - a.x) * (xMax - xMin); };
    auto D = [&](float y) { return (b.y - y) / (b.y - a.y) * 100.0; };

    // Past 50°C everything runs flat out.
    dl->AddRectFilled(ImVec2(X(kFullBy), a.y), b, col(p.warn, 0.06f));
    // Under the pump's floor.
    if (floor > 0) dl->AddRectFilled(ImVec2(a.x, Y(floor)), ImVec2(b.x, b.y), col(p.muted, 0.06f));
    ImGui::PushFont(small_);
    for (int d = 0; d <= 100; d += 25) {
        dl->AddLine(ImVec2(a.x, Y(d)), ImVec2(b.x, Y(d)), col(p.border));
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d%%", d);
        ImVec2 sz = ImGui::CalcTextSize(buf);
        dl->AddText(ImVec2(a.x - sz.x - 6 * k, Y(d) - sz.y / 2), col(p.muted), buf);
    }
    for (int t = 20; t <= 60; t += 5) {
        dl->AddLine(ImVec2(X(t), a.y), ImVec2(X(t), b.y), col(p.border, t % 10 == 0 ? 1.0f : 0.5f));
        if (t % 10) continue;
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d%s", t, kDeg);
        ImVec2 sz = ImGui::CalcTextSize(buf);
        dl->AddText(ImVec2(X(t) - sz.x / 2, b.y + 6 * k), col(p.muted), buf);
    }

    auto path = [&](const Curve& cv, ImU32 colour, float thick) {
        if (cv.points.empty()) return;
        std::vector<ImVec2> pts;
        pts.push_back(ImVec2(a.x, Y(cv.points.front().duty)));
        for (const auto& pt : cv.points) pts.push_back(ImVec2(X(pt.temp), Y(pt.duty)));
        pts.push_back(ImVec2(b.x, Y(cv.points.back().duty)));
        dl->AddPolyline(pts.data(), int(pts.size()), colour, 0, thick);
    };
    // What's saved on the cooler, if the editor differs.
    if (saved && !(*saved == c)) path(*saved, col(p.muted, 0.8f), 1.5f * k);
    path(c, col(p.accent), 2.5f * k);

    // The liquid now, and where the curve puts it.
    if (liquid && *liquid > xMin && *liquid < xMax) {
        float x = X(*liquid);
        dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), col(p.good, 0.7f), 1.5f * k);
        double d = DutyAt(c, *liquid);
        dl->AddCircleFilled(ImVec2(x, Y(d)), 4 * k, col(p.good));
        char buf[48];
        std::snprintf(buf, sizeof(buf), "now %.1f%s \xE2\x86\x92 %.0f%%", *liquid, kDeg, d);
        ImVec2 sz = ImGui::CalcTextSize(buf);
        float tx = std::min(x + 6 * k, b.x - sz.x);
        dl->AddText(ImVec2(tx, a.y - sz.y - 2 * k), col(p.good), buf);
    }

    // Interaction: one invisible button over the plot.
    ImGui::SetCursorScreenPos(ImVec2(a.x - 8 * k, a.y - 8 * k));
    ImGui::InvisibleButton(id, ImVec2(b.x - a.x + 16 * k, b.y - a.y + 16 * k),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hoveredPlot = ImGui::IsItemHovered();
    const ImVec2 m = ImGui::GetIO().MousePos;
    int nearest = -1;
    float best = 12 * k;
    for (size_t i = 0; i < c.points.size(); i++) {
        float dx = m.x - X(c.points[i].temp), dy = m.y - Y(c.points[i].duty);
        float dist = std::sqrt(dx * dx + dy * dy);
        if (dist < best) {
            best = dist;
            nearest = int(i);
        }
    }
    const bool mine = dragId_ == id;
    if (dragging_ >= 0 && mine && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        dragging_ = -1;
        dragId_.clear();
        c = Sanitise(c, pump);
    }
    if (hoveredPlot && nearest >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        dragging_ = nearest;
        dragId_ = id;
    }
    const int n = int(c.points.size());
    if (dragging_ >= 0 && dragId_ == id && dragging_ < n) {
        int i = dragging_;
        const bool last = i == n - 1;
        double lo = i == 0 ? xMin : c.points[i - 1].temp + 1;
        double hi = last ? kFullBy : c.points[i + 1].temp - 1;
        double t = std::round(T(m.x));
        c.points[i].temp = std::clamp(t, lo, std::max(lo, hi));
        if (last) {
            c.points[i].duty = 100;
        } else {
            int dlo = std::max(floor, i == 0 ? floor : c.points[i - 1].duty);
            int dhi = c.points[i + 1].duty;
            int d = int(std::lround(D(m.y)));
            c.points[i].duty = std::clamp(d, dlo, std::max(dlo, dhi));
        }
        message_.clear();
    }
    if (hoveredPlot && dragging_ < 0) {
        if (nearest >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && n > 2 && nearest != n - 1) {
            c.points.erase(c.points.begin() + nearest);
            c = Sanitise(c, pump);
            message_.clear();
            nearest = -1;
        } else if (nearest < 0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && n < kMaxPoints) {
            double t = std::round(T(m.x));
            if (t < c.points.back().temp) {
                bool clear = true;
                size_t at = 0;
                while (at < c.points.size() && c.points[at].temp < t) at++;
                for (const auto& pt : c.points) clear &= std::abs(pt.temp - t) >= 1;
                if (clear && t >= xMin) {
                    int dlo = std::max(floor, at ? c.points[at - 1].duty : floor);
                    int dhi = c.points[at].duty;
                    int d = std::clamp(int(std::lround(D(m.y))), dlo, std::max(dlo, dhi));
                    c.points.insert(c.points.begin() + at, Point{t, d});
                    c = Sanitise(c, pump);
                    message_.clear();
                }
            }
        }
    }

    // The points, the dragged or hovered one bigger, with its values.
    for (int i = 0; i < int(c.points.size()); i++) {
        ImVec2 at(X(c.points[i].temp), Y(c.points[i].duty));
        bool hot = (dragging_ == i && mine) || (dragging_ < 0 && hoveredPlot && nearest == i);
        dl->AddCircleFilled(at, (hot ? 6.5f : 5) * k, col(p.accent));
        dl->AddCircle(at, (hot ? 6.5f : 5) * k, col(p.card), 0, 1.5f * k);
        if (hot) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.0f%s  %d%%", c.points[i].temp, kDeg, c.points[i].duty);
            ImVec2 sz = ImGui::CalcTextSize(buf);
            ImVec2 tp(std::clamp(at.x - sz.x / 2, a.x, b.x - sz.x), at.y - sz.y - 10 * k);
            if (tp.y < a.y) tp.y = at.y + 10 * k;
            dl->AddRectFilled(ImVec2(tp.x - 4 * k, tp.y - 2 * k), ImVec2(tp.x + sz.x + 4 * k, tp.y + sz.y + 2 * k), col(p.bg));
            dl->AddText(tp, col(p.text), buf);
        }
    }
    if (hoveredPlot && dragging_ < 0 && nearest == int(c.points.size()) - 1)
        ImGui::SetTooltip("The last point is always full speed, at 50%s or cooler.", kDeg);
    ImGui::PopFont();

    ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + h));
    ImGui::Dummy(ImVec2(w, 6 * k));
    ImGui::EndGroup();
}

}  // namespace cooler
