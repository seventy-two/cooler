// Protocol and curve tests. Build anywhere:
//   g++ -std=c++17 -Isrc tests/tests.cpp src/protocol.cpp -o tests && ./tests
#include <cstdio>
#include <string>

#include "protocol.h"

using namespace cooler;

static int failures = 0;
#define CHECK(x)                                                         \
    do {                                                                 \
        if (!(x)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);     \
            failures++;                                                  \
        }                                                                \
    } while (0)

static Packet reply(std::initializer_list<uint8_t> bytes) {
    Packet p{};
    size_t i = 0;
    for (uint8_t b : bytes) p[i++] = b;
    return p;
}

int main() {
    // Requests: 0x08, command, data.
    {
        Packet p = Request(cmd::kOpen, ep::kCurves.id);
        CHECK(p[0] == 0x08 && p[1] == 0x0d && p[2] == 0x00 && p[3] == 0x62 && p[4] == 0x6d && p[5] == 0);
        CHECK(Answers(reply({0x00, 0x0d}), cmd::kOpen));
        CHECK(!Answers(reply({0x00, 0x08}), cmd::kOpen));
        CHECK(!Answers(reply({0x01, 0x0d}), cmd::kOpen));
    }
    // Endpoint reads join three packets after checking the type.
    {
        Packet a = reply({0x00, 0x08, 0x00, 0x06, 0x00, 0x03, 0x34, 0x09, 0x2a, 0x03});
        Packet b{}, c{};
        auto p = Payload(a, b, c, ep::kSpeeds.type);
        CHECK(p && p->size() == (kPacket - 5) + 2 * (kPacket - 3));
        auto speeds = ParseSpeeds(*p);
        CHECK(speeds.size() == 3 && speeds[0] == 0x0934 && speeds[1] == 0x032a && speeds[2] == 0);
        CHECK(!Payload(a, b, c, ep::kTemps.type));
    }
    // Temperatures: status 0 is connected, tenths of a degree.
    {
        Bytes p = {2, 0x00, 0x66, 0x01, 0x01, 0x00, 0x00};
        auto t = ParseTemps(p);
        CHECK(t.size() == 2 && t[0] && *t[0] == 35.8 && !t[1]);
        auto conn = ParseConnected(Bytes{3, 0x07, 0x07, 0x01});
        CHECK(conn.size() == 3 && conn[0] && conn[1] && !conn[2]);
    }
    // Curves round-trip.
    {
        std::vector<Curve> cs = {{0, {{25, 50}, {40, 100}}}, {0, {{28.5, 20}, {35, 40}, {45, 100}}}};
        Bytes enc = EncodeCurves(cs);
        CHECK(enc[0] == 2 && enc[1] == 0 && enc[2] == 2 && enc[3] == 250 && enc[4] == 0 && enc[5] == 50);
        auto back = ParseCurves(enc);
        CHECK(back && back->size() == 2 && (*back)[0] == cs[0] && (*back)[1] == cs[1]);
        CHECK(!ParseCurves(Bytes{1, 0, 5, 1}));  // short
        auto fixed = ParseFixed(EncodeFixed({75, 30, 130}));
        CHECK(fixed.size() == 3 && fixed[0] == 75 && fixed[2] == 100);
        auto modes = ParseModes(EncodeModes({kModeCurve, kModeFixed}));
        CHECK(modes.size() == 2 && modes[0] == kModeCurve && modes[1] == kModeFixed);
    }
    // Writes: the first packet carries length and type.
    {
        Bytes data(200, 0xAB);
        auto ps = WritePackets(ep::kCurves.type, data);
        CHECK(ps.size() == 3);
        CHECK(ps[0][0] == 0x08 && ps[0][1] == 0x06 && ps[0][2] == 0x00);
        CHECK(ps[0][3] == 202 && ps[0][4] == 0 && ps[0][5] == 0 && ps[0][6] == 0 && ps[0][7] == 0x05 && ps[0][8] == 0x00);
        CHECK(ps[0][9] == 0xAB && ps[0][kPacket - 1] == 0xAB);
        CHECK(ps[1][1] == 0x07 && ps[1][3] == 0xAB);
        size_t carried = (kPacket - 9) + (kPacket - 3);
        CHECK(ps[2][3 + (200 - carried) - 1] == 0xAB && ps[2][3 + (200 - carried)] == 0);
        auto small = WritePackets(ep::kModes.type, EncodeModes({2, 2}));
        CHECK(small.size() == 1 && small[0][3] == 5);
    }
    // Every preset keeps the rules.
    for (const auto& p : Presets()) {
        CHECK(Problem(p.pump, true).empty());
        CHECK(Problem(p.fans, false).empty());
        CHECK(Sanitise(p.pump, true) == p.pump);
        CHECK(Sanitise(p.fans, false) == p.fans);
    }
    // Sanitise mends bad curves.
    {
        Curve c{3, {{45, 60}, {30, 20}, {30.4, 25}, {35, 10}}};
        Curve s = Sanitise(c, true);
        CHECK(Problem(s, true).empty());
        CHECK(s.points.front().temp == 30 && s.points.front().duty == 50);
        CHECK(s.points.back().temp == 50 && s.points.back().duty == 100);
        Curve hot{0, {{30, 20}, {55, 60}}};
        Curve h = Sanitise(hot, false);
        CHECK(Problem(h, false).empty() && h.points.size() == 2 && h.points[1].temp == 50);
        Curve one{0, {{49.5, 40}}};
        Curve o = Sanitise(one, false);
        CHECK(Problem(o, false).empty());
        Curve eight{0, {{20, 10}, {25, 15}, {30, 20}, {33, 25}, {36, 30}, {39, 40}, {42, 50}, {45, 60}}};
        CHECK(Problem(Sanitise(eight, false), false).empty());
        CHECK(Problem(Curve{0, {{30, 40}, {40, 30}, {45, 100}}}, false) == "slows down as it warms");
        CHECK(!Problem(Curve{0, {{30, 40}, {45, 90}}}, false).empty());
        CHECK(!Problem(Curve{0, {{30, 40}, {45, 100}}}, true).empty());
    }
    // Reading a curve.
    {
        Curve c{0, {{30, 20}, {40, 60}, {50, 100}}};
        CHECK(DutyAt(c, 20) == 20 && DutyAt(c, 35) == 40 && DutyAt(c, 45) == 80 && DutyAt(c, 70) == 100);
    }
    if (failures) {
        std::printf("%d failed\n", failures);
        return 1;
    }
    std::printf("tests passed\n");
    return 0;
}
