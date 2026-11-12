#include "controller.h"

#include <cstdio>

namespace cooler {
namespace {

std::string hex(const Bytes& b, size_t max = 64) {
    std::string s;
    char buf[4];
    for (size_t i = 0; i < b.size() && i < max; i++) {
        std::snprintf(buf, sizeof(buf), "%02x ", b[i]);
        s += buf;
    }
    return s;
}

// Session puts the controller in software mode for its lifetime and hands
// it back to hardware mode (its saved settings) at the end, always.
struct Session {
    Link& link;
    bool woke = false;
    explicit Session(Link& l) : link(l) { woke = link.Send(cmd::kSoftwareMode); }
    ~Session() {
        if (woke) link.Send(cmd::kHardwareMode);
    }
};

bool readAll(Link& link, Reading& r) {
    Packet fw;
    if (link.Send(cmd::kFirmware, {}, fw)) r.firmware = FirmwareText(fw);
    auto conn = link.ReadEndpoint(ep::kConnected);
    auto speeds = link.ReadEndpoint(ep::kSpeeds);
    auto temps = link.ReadEndpoint(ep::kTemps);
    auto modes = link.ReadEndpoint(ep::kModes);
    auto fixed = link.ReadEndpoint(ep::kFixed);
    auto curves = link.ReadEndpoint(ep::kCurves);
    if (!conn || !speeds || !temps || !modes || !fixed || !curves) {
        r.error = link.Error().empty() ? "The controller didn't answer" : link.Error();
        return false;
    }
    r.raw = "connected: " + hex(*conn, 16) + "\nspeeds:    " + hex(*speeds, 16) + "\ntemps:     " + hex(*temps, 24) +
            "\nmodes:     " + hex(*modes, 16) + "\nfixed:     " + hex(*fixed, 16) + "\ncurves:    " + hex(*curves, 240) + "\n";
    r.connected = ParseConnected(*conn);
    r.rpm = ParseSpeeds(*speeds);
    auto t = ParseTemps(*temps);
    if (!t.empty() && r.hasPump) r.liquid = t[0];
    r.modes = ParseModes(*modes);
    r.fixed = ParseFixed(*fixed);
    auto c = ParseCurves(*curves);
    if (!c) {
        r.error = "Couldn't make sense of the saved curves";
        return false;
    }
    r.curves = *c;
    r.ok = true;
    return true;
}

}  // namespace

Reading Read() {
    Reading r;
    if (IcueRunning()) {
        r.error = "iCUE is running. Close it first: two programs driving the controller trip each other up.";
        return r;
    }
    auto found = FindCoolers();
    if (found.empty()) {
        r.error = "No Corsair Commander Core found.";
        return r;
    }
    r.name = found[0].name;
    r.hasPump = found[0].hasPump;
    Link link(found[0].path);
    if (!link.Open()) {
        r.error = link.Error();
        return r;
    }
    Session s(link);
    if (!s.woke) {
        r.error = link.Error();
        return r;
    }
    readAll(link, r);
    return r;
}

std::string Apply(const Curve& pumpIn, const Curve& fansIn, Reading& after) {
    if (IcueRunning()) return "iCUE is running. Close it first.";
    Curve pump = Sanitise(pumpIn, true), fans = Sanitise(fansIn, false);
    if (auto p = Problem(pump, true); !p.empty()) return "The pump curve " + p;
    if (auto p = Problem(fans, false); !p.empty()) return "The fan curve " + p;
    auto found = FindCoolers();
    if (found.empty()) return "No Corsair Commander Core found.";
    Link link(found[0].path);
    if (!link.Open()) return link.Error();
    std::string err;
    {
        Session s(link);
        if (!s.woke) return link.Error();
        auto connNow = link.ReadEndpoint(ep::kConnected);
        auto modesNow = link.ReadEndpoint(ep::kModes);
        auto curvesNow = link.ReadEndpoint(ep::kCurves);
        if (!connNow || !modesNow || !curvesNow) return "Couldn't read the saved settings: " + link.Error();
        auto modes = ParseModes(*modesNow);
        auto curves = ParseCurves(*curvesNow);
        if (modes.empty() || !curves || curves->size() != modes.size()) return "The saved settings didn't look as expected, so nothing was changed.";
        const bool hasPump = found[0].hasPump;
        auto connected = ParseConnected(*connNow);
        // Only what's plugged in: empty ports keep whatever they had.
        for (size_t ch = 0; ch < modes.size(); ch++) {
            bool isPump = hasPump && ch == size_t(kPump);
            if (!isPump && (ch >= connected.size() || !connected[ch])) continue;
            (*curves)[ch] = isPump ? pump : fans;
            modes[ch] = kModeCurve;
        }
        // Curves first, so a channel never switches to curve mode with an
        // old curve.
        if (!link.WriteEndpoint(ep::kCurves, EncodeCurves(*curves))) err = "Writing the curves failed: " + link.Error();
        else if (!link.WriteEndpoint(ep::kModes, EncodeModes(modes))) err = "Writing the modes failed: " + link.Error();
        after.name = found[0].name;
        after.hasPump = hasPump;
        readAll(link, after);
    }
    if (!err.empty()) return err;
    if (!after.ok) return "Saved, but reading back failed: " + after.error;
    for (size_t ch = 0; ch < after.curves.size(); ch++) {
        bool isPump = after.hasPump && ch == size_t(kPump);
        if (!isPump && (ch >= after.connected.size() || !after.connected[ch])) continue;
        if (!(after.curves[ch] == (isPump ? pump : fans)) || after.modes[ch] != kModeCurve)
            return "The controller didn't keep what was sent (channel " + std::to_string(ch) + ").";
    }
    return "";
}

}  // namespace cooler
