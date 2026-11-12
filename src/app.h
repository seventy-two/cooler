// The window's contents: what the cooler reports, presets, and the pump
// and fan curves to drag about. Drawn with Dear ImGui each frame.
#pragma once

#include <windows.h>

#include <functional>
#include <future>
#include <string>
#include <vector>

#include "controller.h"
#include "imgui.h"

namespace cooler {

class App {
public:
    App(HWND hwnd, std::function<void()> wake);
    ~App();

    void Frame();
    void Pump();  // picks up finished reads and saves; call every loop

    bool InCaption(float x, float y) const;
    void SetScale(float scale);
    void SetDark(bool dark);
    bool FontsDirty() const { return fontsDirty_; }
    void ReloadFonts();
    bool Busy() const { return job_.valid(); }

    bool Place(RECT& r) const;
    void SavePlace(const RECT& r);

private:
    struct Job {
        bool save = false;
        Reading reading;
        std::string error;  // a save's
    };

    void startRead();
    void startSave();
    void took(const Reading& r);  // a fresh reading: maybe reset the editor
    void drawTitle(float width);
    void drawStatus(float width);
    void drawPresets(float width);
    void drawCurve(const char* id, const char* title, Curve& c, const Curve* saved, bool pump, float w, float h);
    bool chip(const char* label, bool on, float minW = 0);
    bool iconButton(const char* icon, const char* tip, bool danger = false);
    bool edited() const;
    void loadConfig();
    void saveConfig();

    HWND hwnd_;
    std::function<void()> wake_;
    std::future<Job> job_;
    ULONGLONG rereadAt_ = 0;  // a second look after saving, once speeds settle
    bool icue_ = false;
    ULONGLONG icueAt_ = 0;    // next check for iCUE

    Reading reading_;
    bool read_ = false;
    SYSTEMTIME readAt_{};
    std::string message_;
    bool messageBad_ = false;

    // The editor, and what's saved on the cooler (as curves).
    Curve pump_, fans_;
    Curve savedPump_, savedFans_;
    bool haveSaved_ = false;
    std::string savedNote_;  // e.g. the pump was fixed

    int dragging_ = -1;
    std::string dragId_;

    std::string dir_;
    bool placed_ = false;
    RECT place_{};

    float scale_ = 1;
    bool fontsDirty_ = true;
    ImFont* body_ = nullptr;
    ImFont* small_ = nullptr;
    ImFont* title_ = nullptr;
    ImFont* label_ = nullptr;
    ImFont* big_ = nullptr;

    ImVec2 barMax_;
    std::vector<std::pair<ImVec2, ImVec2>> hot_;
};

}  // namespace cooler
