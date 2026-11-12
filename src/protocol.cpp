#include "protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace cooler {
namespace {

int u16(const Bytes& p, size_t at) { return at + 1 < p.size() ? p[at] | (p[at + 1] << 8) : 0; }

void put16(Bytes& out, int v) {
    out.push_back(uint8_t(v & 0xFF));
    out.push_back(uint8_t((v >> 8) & 0xFF));
}

}  // namespace

Packet Request(const Bytes& command, const Bytes& data) {
    Packet p{};
    p[0] = 0x08;
    size_t i = 1;
    for (uint8_t b : command)
        if (i < kPacket) p[i++] = b;
    for (uint8_t b : data)
        if (i < kPacket) p[i++] = b;
    return p;
}

bool Answers(const Packet& reply, const Bytes& command) {
    return reply[0] == 0x00 && !command.empty() && reply[1] == command[0];
}

std::optional<Bytes> Payload(const Packet& a, const Packet& b, const Packet& c, const std::array<uint8_t, 2>& type) {
    if (a[3] != type[0] || a[4] != type[1]) return std::nullopt;
    Bytes out(a.begin() + 5, a.end());
    out.insert(out.end(), b.begin() + 3, b.end());
    out.insert(out.end(), c.begin() + 3, c.end());
    return out;
}

std::vector<Packet> WritePackets(const std::array<uint8_t, 2>& type, const Bytes& data) {
    std::vector<Packet> out;
    // First: command (2) + length (2) + two zeros + type (2) after the 0x08.
    const size_t first = kPacket - 9;
    const size_t more = kPacket - 3;
    size_t n = std::min(first, data.size());
    Bytes head;
    put16(head, int(data.size() + type.size()));
    head.push_back(0);
    head.push_back(0);
    head.push_back(type[0]);
    head.push_back(type[1]);
    head.insert(head.end(), data.begin(), data.begin() + n);
    out.push_back(Request(cmd::kWrite, head));
    for (size_t at = n; at < data.size(); at += more) {
        size_t m = std::min(more, data.size() - at);
        out.push_back(Request(cmd::kWriteMore, Bytes(data.begin() + at, data.begin() + at + m)));
    }
    return out;
}

std::string FirmwareText(const Packet& r) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d.%d.%d", r[3], r[4], r[5] | (r[6] << 8));
    return buf;
}

std::vector<int> ParseSpeeds(const Bytes& p) {
    std::vector<int> out;
    if (p.empty()) return out;
    for (int i = 0; i < p[0] && 1 + i * 2 + 1 < int(p.size()); i++) out.push_back(u16(p, 1 + i * 2));
    return out;
}

std::vector<bool> ParseConnected(const Bytes& p) {
    std::vector<bool> out;
    if (p.empty()) return out;
    for (int i = 0; i < p[0] && 1 + i < int(p.size()); i++) out.push_back(p[1 + i] == 0x07);
    return out;
}

std::vector<std::optional<double>> ParseTemps(const Bytes& p) {
    std::vector<std::optional<double>> out;
    if (p.empty()) return out;
    for (int i = 0; i < p[0] && 1 + i * 3 + 2 < int(p.size()); i++) {
        size_t at = 1 + i * 3;
        if (p[at] == 0x00) out.push_back(u16(p, at + 1) / 10.0);
        else out.push_back(std::nullopt);
    }
    return out;
}

std::vector<uint8_t> ParseModes(const Bytes& p) {
    std::vector<uint8_t> out;
    if (p.empty()) return out;
    for (int i = 0; i < p[0] && 1 + i < int(p.size()); i++) out.push_back(p[1 + i]);
    return out;
}

std::vector<int> ParseFixed(const Bytes& p) {
    std::vector<int> out;
    if (p.empty()) return out;
    for (int i = 0; i < p[0] && 1 + i * 2 + 1 < int(p.size()); i++) out.push_back(u16(p, 1 + i * 2));
    return out;
}

bool Curve::operator==(const Curve& o) const {
    if (sensor != o.sensor || points.size() != o.points.size()) return false;
    for (size_t i = 0; i < points.size(); i++)
        if (std::lround(points[i].temp * 10) != std::lround(o.points[i].temp * 10) || points[i].duty != o.points[i].duty)
            return false;
    return true;
}

std::optional<std::vector<Curve>> ParseCurves(const Bytes& p) {
    if (p.empty()) return std::nullopt;
    std::vector<Curve> out;
    size_t at = 1;
    for (int i = 0; i < p[0]; i++) {
        if (at + 2 > p.size()) return std::nullopt;
        Curve c;
        c.sensor = p[at];
        int n = p[at + 1];
        at += 2;
        if (n > 16 || at + size_t(n) * 4 > p.size()) return std::nullopt;
        for (int k = 0; k < n; k++, at += 4) c.points.push_back({u16(p, at) / 10.0, u16(p, at + 2)});
        out.push_back(c);
    }
    return out;
}

Bytes EncodeModes(const std::vector<uint8_t>& modes) {
    Bytes out{uint8_t(modes.size())};
    out.insert(out.end(), modes.begin(), modes.end());
    return out;
}

Bytes EncodeFixed(const std::vector<int>& duties) {
    Bytes out{uint8_t(duties.size())};
    for (int d : duties) put16(out, std::clamp(d, 0, 100));
    return out;
}

Bytes EncodeCurves(const std::vector<Curve>& curves) {
    Bytes out{uint8_t(curves.size())};
    for (const auto& c : curves) {
        out.push_back(c.sensor);
        out.push_back(uint8_t(c.points.size()));
        for (const auto& pt : c.points) {
            put16(out, int(std::lround(pt.temp * 10)));
            put16(out, std::clamp(pt.duty, 0, 100));
        }
    }
    return out;
}

Curve Sanitise(Curve c, bool pump) {
    const int floor = pump ? kPumpFloor : kFanFloor;
    std::sort(c.points.begin(), c.points.end(), [](const Point& a, const Point& b) { return a.temp < b.temp; });
    for (auto& p : c.points) {
        p.temp = std::clamp(std::round(p.temp * 10) / 10, kMinTemp, kMaxTemp);
        p.duty = std::clamp(p.duty, floor, 100);
    }
    // Keep temperatures at least a degree apart.
    std::vector<Point> kept;
    for (const auto& p : c.points)
        if (kept.empty() || p.temp >= kept.back().temp + 1) kept.push_back(p);
    c.points = kept;
    // Never slower as it warms.
    for (size_t i = 1; i < c.points.size(); i++) c.points[i].duty = std::max(c.points[i].duty, c.points[i - 1].duty);
    // Full speed by kFullBy: drop anything past it, then end at 100%.
    while (!c.points.empty() && c.points.back().temp > kFullBy) c.points.pop_back();
    if (c.points.empty() || c.points.back().duty < 100) {
        if (!c.points.empty() && c.points.back().temp > kFullBy - 1) {
            c.points.back().duty = 100;
        } else {
            if (c.points.size() >= size_t(kMaxPoints)) c.points.pop_back();
            c.points.push_back({kFullBy, 100});
        }
    }
    if (c.points.size() == 1) c.points.insert(c.points.begin(), {kMinTemp, std::max(floor, std::min(100, c.points[0].duty))});
    while (c.points.size() > size_t(kMaxPoints)) c.points.erase(c.points.end() - 2);
    c.sensor = 0;
    return c;
}

std::string Problem(const Curve& c, bool pump) {
    if (c.points.size() < 2) return "needs at least two points";
    if (c.points.size() > size_t(kMaxPoints)) return "has more than seven points";
    if (c.sensor != 0) return "doesn't follow the liquid temperature";
    for (size_t i = 0; i < c.points.size(); i++) {
        const auto& p = c.points[i];
        if (p.temp < kMinTemp || p.temp > kMaxTemp) return "has a point outside 20-60\xC2\xB0" "C";
        if (p.duty < (pump ? kPumpFloor : kFanFloor) || p.duty > 100)
            return pump ? "goes under the pump's 50% floor" : "has a speed outside 0-100%";
        if (i && p.temp < c.points[i - 1].temp + 1) return "has points closer than a degree";
        if (i && p.duty < c.points[i - 1].duty) return "slows down as it warms";
    }
    bool full = false;
    for (const auto& p : c.points) full |= p.duty >= 100 && p.temp <= kFullBy;
    if (!full) return "doesn't reach full speed by 50\xC2\xB0" "C";
    return "";
}

double DutyAt(const Curve& c, double temp) {
    if (c.points.empty()) return 0;
    if (temp <= c.points.front().temp) return c.points.front().duty;
    for (size_t i = 1; i < c.points.size(); i++) {
        const auto& a = c.points[i - 1];
        const auto& b = c.points[i];
        if (temp <= b.temp) return a.duty + (b.duty - a.duty) * (temp - a.temp) / (b.temp - a.temp);
    }
    return c.points.back().duty;
}

const std::vector<Preset>& Presets() {
    // Liquid temperature, not CPU: it moves slowly, so fans don't rev with
    // every burst. Idle liquid sits a few degrees over the room and warms a
    // few more under load. For comparison, iCUE's own "Quiet" saves the pump
    // fixed at 75% and fans reaching 100% by 42 C.
    static const std::vector<Preset> presets = {
        {"Silent", "Fans and pump at their lowest until the liquid is properly warm.",
         {0, {{25, 50}, {38, 50}, {42, 65}, {45, 85}, {48, 100}}},
         {0, {{25, 20}, {34, 22}, {38, 35}, {42, 60}, {45, 85}, {48, 100}}}},
        {"Quiet", "Low and steady for everyday use; rises gently under load.",
         {0, {{25, 55}, {36, 60}, {40, 75}, {44, 90}, {47, 100}}},
         {0, {{25, 25}, {33, 28}, {37, 40}, {41, 65}, {44, 85}, {47, 100}}}},
        {"Balanced", "Cooler under load, still calm at idle.",
         {0, {{25, 65}, {35, 70}, {39, 85}, {42, 100}}},
         {0, {{25, 30}, {32, 38}, {36, 55}, {40, 75}, {43, 100}}}},
        {"Performance", "Pump flat out; fans keep the liquid as cool as they can.",
         {0, {{25, 100}, {45, 100}}},
         {0, {{25, 45}, {30, 60}, {35, 80}, {39, 100}}}},
    };
    return presets;
}

}  // namespace cooler
