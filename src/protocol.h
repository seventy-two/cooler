// The Corsair Commander Core's USB protocol (the hub on Elite Capellix and
// Elite LCD coolers), as documented by liquidctl and FanControl.CorsairLink,
// plus the fan curves cooler writes to it. Pure functions only, so it tests
// anywhere; hid.cpp does the talking.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cooler {

// Packets are 96 bytes after the report ID. Requests start 0x08, then the
// command and its data.
constexpr size_t kPacket = 96;
using Packet = std::array<uint8_t, kPacket>;
using Bytes = std::vector<uint8_t>;

namespace cmd {
// Software mode: the host is in charge (and the lights go dark until it
// lets go). Hardware mode: the controller runs what's saved in it.
const Bytes kSoftwareMode = {0x01, 0x03, 0x00, 0x02};
const Bytes kHardwareMode = {0x01, 0x03, 0x00, 0x01};
const Bytes kFirmware = {0x02, 0x13};
const Bytes kOpen = {0x0d, 0x00};
const Bytes kClose = {0x05, 0x01, 0x00};
const Bytes kRead1 = {0x08, 0x00, 0x01};
const Bytes kRead2 = {0x08, 0x00, 0x02};
const Bytes kRead3 = {0x08, 0x00, 0x03};
const Bytes kWrite = {0x06, 0x00};
const Bytes kWriteMore = {0x07, 0x00};
}  // namespace cmd

// An endpoint to read or write, and the data type it answers with.
struct Endpoint {
    Bytes id;
    std::array<uint8_t, 2> type;
};
namespace ep {
const Endpoint kSpeeds = {{0x17}, {0x06, 0x00}};
const Endpoint kConnected = {{0x1a}, {0x09, 0x00}};
const Endpoint kTemps = {{0x21}, {0x10, 0x00}};
const Endpoint kModes = {{0x60, 0x6d}, {0x03, 0x00}};   // saved: each channel's mode
const Endpoint kFixed = {{0x61, 0x6d}, {0x04, 0x00}};   // saved: fixed percentages
const Endpoint kCurves = {{0x62, 0x6d}, {0x05, 0x00}};  // saved: curves
}  // namespace ep

// Channel 0 is the pump on coolers; fans are 1-6.
constexpr int kPump = 0;
constexpr uint8_t kModeFixed = 0x00;
constexpr uint8_t kModeCurve = 0x02;

// Request builds an outgoing packet: 0x08, the command, then data.
Packet Request(const Bytes& command, const Bytes& data = {});

// Answers checks a reply is to this command (and not something left over).
bool Answers(const Packet& reply, const Bytes& command);

// Payload joins the three packets of an endpoint read, or nothing if the
// first isn't the expected data type.
std::optional<Bytes> Payload(const Packet& a, const Packet& b, const Packet& c, const std::array<uint8_t, 2>& type);

// WritePackets splits an endpoint write into its packets: the first carries
// the length and data type, the rest follow on with kWriteMore.
std::vector<Packet> WritePackets(const std::array<uint8_t, 2>& type, const Bytes& data);

std::string FirmwareText(const Packet& reply);

// Parsing what endpoints return. Each starts with a count of channels.
std::vector<int> ParseSpeeds(const Bytes& p);
std::vector<bool> ParseConnected(const Bytes& p);
std::vector<std::optional<double>> ParseTemps(const Bytes& p);
std::vector<uint8_t> ParseModes(const Bytes& p);
std::vector<int> ParseFixed(const Bytes& p);

struct Point {
    double temp = 0;  // liquid, °C (sent in tenths)
    int duty = 0;     // percent
};

struct Curve {
    uint8_t sensor = 0;  // 0 is the liquid temperature
    std::vector<Point> points;
    bool operator==(const Curve& o) const;
};

std::optional<std::vector<Curve>> ParseCurves(const Bytes& p);

Bytes EncodeModes(const std::vector<uint8_t>& modes);
Bytes EncodeFixed(const std::vector<int>& duties);
Bytes EncodeCurves(const std::vector<Curve>& curves);

// Safety rules every curve cooler writes keeps to, since the controller runs
// it with nothing watching: 2-7 points, rising temperatures within
// kMinTemp-kMaxTemp, duty never falling as it warms, full speed by
// kFullBy at the latest, and the pump never under kPumpFloor.
constexpr int kMaxPoints = 7;
constexpr double kMinTemp = 20;
constexpr double kMaxTemp = 60;
constexpr double kFullBy = 50;
constexpr int kPumpFloor = 50;
constexpr int kFanFloor = 0;

// Sanitise makes a curve keep the rules (sorting, clamping, raising the
// floor, adding a full-speed point) and returns it.
Curve Sanitise(Curve c, bool pump);
// Problem says what breaks the rules, or "" if nothing.
std::string Problem(const Curve& c, bool pump);

// DutyAt reads a curve at a temperature, straight lines between points (as
// the controller does), flat beyond the ends.
double DutyAt(const Curve& c, double temp);

struct Preset {
    const char* name;
    const char* about;
    Curve pump, fans;
};
const std::vector<Preset>& Presets();

}  // namespace cooler
