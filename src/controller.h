// What the controller reports and has saved, read and written in one
// session each (the lights blink once per session: see the README).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "hid.h"
#include "protocol.h"

namespace cooler {

struct Reading {
    bool ok = false;
    std::string error;
    std::string name, firmware;
    bool hasPump = true;
    std::vector<int> rpm;                 // per channel; channel 0 the pump
    std::vector<bool> connected;          // per channel
    std::optional<double> liquid;         // °C
    std::vector<uint8_t> modes;           // saved mode per channel
    std::vector<int> fixed;               // saved fixed percent per channel
    std::vector<Curve> curves;            // saved curve per channel
    std::string raw;                      // hex of each endpoint, for -dump
};

// Read opens the first controller found and reads everything.
Reading Read();

// Apply saves curves on the controller: the pump's to channel 0, the
// fans' to every connected fan, all in curve mode. Both are sanitised first.
// Returns "" or what went wrong; then reads back into `after`.
std::string Apply(const Curve& pump, const Curve& fans, Reading& after);

}  // namespace cooler
